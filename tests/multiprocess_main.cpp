// Collective Path Planner - multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every proof in this suite drives real operating-system processes: the cpathd
// coordinator is started with CreateProcess (Windows) or fork+exec (POSIX),
// killed with TerminateProcess/SIGKILL rather than a graceful shutdown,
// restarted against the same durable store, and driven over real loopback TCP
// connections. Nothing here simulates a process, a socket or a crash in-process.
//
// Process-startup synchronisation uses cpath_test::wait_for_file on the port
// file cpathd publishes once it is listening: the coordinator is started with
// --port 0 and --quiet, so the file is the only way to learn the ephemeral port.
// That is coordination, not a test timeout. The single bounded wait that is not
// startup coordination is the session-retirement probe (probe_further_reply),
// which observes the *absence* of a reply from a session the coordinator has
// already retired; it is documented at its definition.
//
// Every test owns a distinct temporary directory (cpath_test::temporary_directory)
// and every process it starts is joined or killed before the test returns, so a
// daemon can never leak the port into the next proof.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "cpath/buffer.hpp"
#include "cpath/digest.hpp"
#include "cpath/dsl.hpp"
#include "cpath/persistence.hpp"
#include "cpath/plan.hpp"
#include "cpath/request.hpp"
#include "cpath/server.hpp"
#include "cpath/status.hpp"
#include "cpath/wire.hpp"

#include "fixtures.hpp"
#include "process.hpp"
#include "test_framework.hpp"

namespace {

namespace fs = std::filesystem;

// Startup coordination budget for wait_for_file: how long a freshly spawned
// daemon may take to publish its port file. This bounds process startup only.
constexpr unsigned kStartupBudgetMillis = 30000u;
// Budget for the absence-of-reply probe (see probe_further_reply).
constexpr unsigned kSilenceBudgetMillis = 5000u;
constexpr unsigned kPollStepMillis = 10u;

std::string join_path(const std::string& directory, const std::string& name) {
  return (fs::path(directory) / name).string();
}

std::string executable_name(const char* stem) {
#ifdef _WIN32
  return std::string(stem) + ".exe";
#else
  return std::string(stem);
#endif
}

// Absolute path of the running test executable, used to find its siblings.
std::string self_executable_path() {
#ifdef _WIN32
  char buffer[MAX_PATH] = {};
  const DWORD written = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (written == 0 || written >= static_cast<DWORD>(sizeof(buffer))) {
    return std::string();
  }
  return std::string(buffer, buffer + written);
#else
  char buffer[4096] = {};
  const ssize_t written = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1u);
  if (written <= 0) {
    return std::string();
  }
  return std::string(buffer, buffer + static_cast<std::size_t>(written));
#endif
}

std::string sibling_of_test_executable(const std::string& name) {
  const std::string self = self_executable_path();
  if (self.empty()) {
    return name;
  }
  return join_path(fs::path(self).parent_path().string(), name);
}

// CMake passes an absolute path for a normal build. Without the definition the
// executables are expected next to this test binary, which is how the
// verification build places cpathd.exe, cpath.exe and this suite in one folder.
const std::string& daemon_executable() {
#ifdef CPATH_DAEMON_PATH
  static const std::string path{CPATH_DAEMON_PATH};
#else
  static const std::string path = sibling_of_test_executable(executable_name("cpathd"));
#endif
  return path;
}

const std::string& cli_executable() {
#ifdef CPATH_CLI_PATH
  static const std::string path{CPATH_CLI_PATH};
#else
  static const std::string path = sibling_of_test_executable(executable_name("cpath"));
#endif
  return path;
}

// One test's private scratch area: a base directory holding the durable store
// and the port file. Removed when the test ends, passed or failed.
struct Workspace {
  std::string root;
  std::string store;
  std::string port_file;

  explicit Workspace(const std::string& tag)
      : root(cpath_test::temporary_directory(tag)),
        store(join_path(root, "store")),
        port_file(join_path(root, "port")) {
    (void)cpath_test::make_directory(store);
  }
  ~Workspace() { (void)cpath_test::remove_directory_tree(root); }

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
  Workspace(Workspace&&) = delete;
  Workspace& operator=(Workspace&&) = delete;
};

// A real cpathd process plus the ephemeral port it published on disk.
struct DaemonProcess {
  cpath_test::ChildProcess child{};
  std::string store{};
  std::string port_file{};
  std::uint16_t port{0};

  ~DaemonProcess() { retire(); }

  DaemonProcess() = default;
  DaemonProcess(const DaemonProcess&) = delete;
  DaemonProcess& operator=(const DaemonProcess&) = delete;

  bool spawn(const std::string& store_root, const std::string& port_path, std::string& error) {
    store = store_root;
    port_file = port_path;
    const std::vector<std::string> arguments{"--bind", "127.0.0.1", "--port", "0",
                                            "--store", store, "--port-file", port_file, "--quiet"};
    if (!child.spawn(daemon_executable(), arguments, error)) {
      return false;
    }
    std::string content;
    if (!cpath_test::wait_for_file(port_file, kStartupBudgetMillis, content)) {
      error = "cpathd did not publish a port at " + port_file;
      return false;
    }
    try {
      const unsigned long value = std::stoul(content);
      if (value == 0ul || value > 65535ul) {
        error = "cpathd published an out-of-range port: " + content;
        return false;
      }
      port = static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
      error = "cpathd published a non-numeric port: " + content;
      return false;
    }
    return true;
  }

  bool alive() const { return child.running(); }

  // Terminates the process if it is still alive and reaps it. Called from the
  // destructor as well, so no proof can leave a daemon holding a port.
  void retire() {
    if (child.running()) {
      child.kill();
    }
    (void)child.join();
  }
};

bool open_session(cpath::ServiceClient& client, std::uint16_t port, const char* label,
                  cpath::HelloAckPayload& ack, std::string& error) {
  const cpath::Status connected = client.connect("127.0.0.1", port);
  if (!connected.is_ok()) {
    error = "connect failed: " + connected.to_string();
    return false;
  }
  auto handshake = client.handshake(label);
  if (!handshake.has_value()) {
    error = "handshake failed: " + handshake.status().to_string();
    return false;
  }
  ack = handshake.value();
  return true;
}

std::vector<std::uint8_t> build_frame(cpath::FrameType type, std::uint64_t sequence, std::uint64_t epoch,
                                      std::uint64_t session_id, std::span<const std::uint8_t> payload) {
  cpath::FrameHeader header;
  header.type = type;
  header.sequence = sequence;
  header.epoch = epoch;
  header.session_id = session_id;
  return cpath::encode_frame(header, payload);
}

cpath::ErrorCode error_code_of(const cpath::Frame& frame) {
  cpath::ByteReader reader(frame.payload);
  cpath::ErrorPayload payload;
  if (!cpath::decode_error(reader, payload).is_ok()) {
    return cpath::ErrorCode::kOk;
  }
  return static_cast<cpath::ErrorCode>(payload.code);
}

struct StoreScan {
  std::size_t records{0};
  bool store_meta{false};
};

StoreScan scan_store(const std::string& root) {
  StoreScan scan;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(root, error)) {
    const std::string name = entry.path().filename().string();
    if (name == "store.meta") {
      scan.store_meta = true;
    }
    if (name.rfind("plan-", 0) == 0 && entry.path().extension() == ".rec") {
      ++scan.records;
    }
  }
  return scan;
}

bool list_contains(const cpath::ListPlansResponsePayload& payload, const cpath::Digest& id) {
  return std::any_of(payload.plans.begin(), payload.plans.end(),
                     [&id](const cpath::PlanSummaryPayload& item) { return item.plan_id == id; });
}

// What happened to a session after the coordinator stopped serving it?
enum class SessionFate {
  kAnswered,  // the coordinator sent another frame on that session
  kClosed,    // the coordinator released the peer: the client read end-of-stream
  kSilent,    // nothing arrived inside the budget and the socket stayed open
};

// The coordinator answers loopback requests in microseconds, so the only reason
// a session stays silent for the whole budget is that the coordinator stopped
// serving it. Both the client and the result slot must be heap-owned by the
// caller: on kSilent a detached reader thread is still blocked in recv holding
// the client's mutex, so the caller must abandon (never destroy) the client --
// its destructor would call close() and wait on that mutex forever. Leaking one
// rejected session inside a test process is the price of observing closure.
SessionFate probe_further_reply(cpath::ServiceClient* client, cpath::Result<cpath::Frame>* slot,
                                unsigned budget_millis) {
  auto* answered = new std::atomic<bool>(false);
  std::thread reader([client, slot, answered] {
    *slot = client->receive_frame();
    answered->store(true);
  });
  for (unsigned elapsed = 0; elapsed <= budget_millis && !answered->load(); elapsed += kPollStepMillis) {
    cpath_test::sleep_millis(kPollStepMillis);
  }
  if (answered->load()) {
    reader.join();
    delete answered;
    // A failure with no frame is a release, not a reply.
    return slot->has_value() ? SessionFate::kAnswered : SessionFate::kClosed;
  }
  reader.detach();
  return SessionFate::kSilent;
}

// Runs a *separate* CLI process and captures what it printed. Used where the
// proof depends on the text of the output (the ping line carries the epoch).
// The child is reaped by _pclose/pclose, so no helper process is leaked.
struct CommandOutcome {
  int exit_code{-1};
  std::string output{};
};

CommandOutcome run_capturing(const std::string& executable, const std::vector<std::string>& arguments) {
  std::string command_line = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command_line += " \"";
    command_line += argument;
    command_line += "\"";
  }
  command_line += " 2>&1";
#ifdef _WIN32
  // cmd.exe removes the outermost quote pair when /C is followed by a quoted
  // command, which would split a program path containing spaces; one more level
  // of quoting keeps the executable path intact.
  command_line = "\"" + command_line + "\"";
#endif
  CommandOutcome outcome;
#ifdef _WIN32
  std::FILE* pipe = ::_popen(command_line.c_str(), "r");
#else
  std::FILE* pipe = ::popen(command_line.c_str(), "r");
#endif
  if (pipe == nullptr) {
    outcome.output = "cannot start " + command_line;
    return outcome;
  }
  char buffer[512];
  while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
    outcome.output += buffer;
  }
#ifdef _WIN32
  outcome.exit_code = ::_pclose(pipe);
#else
  outcome.exit_code = ::pclose(pipe);
#endif
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. fresh start: a real daemon, an ephemeral port learned from its port file,
//    a real TCP session, a handshake and a ping.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, fresh_start_publishes_port_and_answers_ping) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-fresh");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  CPATH_CHECK(daemon.alive());
  CPATH_CHECK(daemon.port != 0);
  CPATH_CHECK(fs::exists(workspace.port_file));
  CPATH_CHECK(fs::exists(workspace.store));

  cpath::ServiceClient client;
  cpath::HelloAckPayload ack{};
  if (!open_session(client, daemon.port, "multiprocess-fresh", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }
  CPATH_CHECK(ack.session_id != 0);
  // A real incarnation: the coordinator reports the fence it holds from its
  // store (a fresh store starts at the first incarnation).
  CPATH_CHECK(ack.incarnation != 0);
  CPATH_CHECK(ack.epoch != 0);
  CPATH_CHECK(ack.protocol_version != 0);
  CPATH_CHECK_EQ(std::string(ack.server_label), std::string("cpathd"));
  CPATH_CHECK(client.connected());
  CPATH_CHECK(client.ping().is_ok());
  std::cout << "[multiprocess] fresh start: port " << daemon.port << " session " << ack.session_id
            << " epoch " << ack.epoch << " incarnation " << ack.incarnation << "\n";
  client.close();
}

// ---------------------------------------------------------------------------
// 2. real planning over TCP, validated against the parsed request.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, plan_over_tcp_matches_request_digest) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-plan");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  cpath::ServiceClient client;
  cpath::HelloAckPayload ack{};
  if (!open_session(client, daemon.port, "multiprocess-plan", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }

  const std::string text = cpath_test::four_node_ring_text();
  auto request = cpath::parse_request_text(text);
  CPATH_REQUIRE(request.has_value());

  // The explanation is requested as well, so the reply carries the plan record
  // and the coordinator's variable-length narrative in one frame.
  auto response = client.plan(text, true);
  if (!response.has_value()) {
    CPATH_FAIL("plan request failed over TCP: " + response.status().to_string());
  }
  if (!response.value().has_plan) {
    std::string denials;
    for (const cpath::DenialPayload& denial : response.value().denials) {
      denials += " [" + std::to_string(denial.code) + " " + denial.message + "]";
    }
    CPATH_FAIL("the coordinator denied the four-node ring request:" + denials);
  }
  CPATH_REQUIRE(!response.value().plan_record.empty());
  CPATH_CHECK(!response.value().explanation.empty());

  cpath::StoredPlanSummary summary{};
  auto plan = cpath::decode_plan_record(response.value().plan_record, &summary);
  CPATH_REQUIRE(plan.has_value());
  // The plan validates against the request it was produced for.
  CPATH_CHECK(cpath::validate_plan(plan.value(), request.value()).is_ok());
  // Stable identity: the plan id is the canonical request digest.
  CPATH_CHECK(plan.value().id.value() == request.value().canonical_digest);
  CPATH_CHECK(plan.value().input_digest == request.value().canonical_digest);
  CPATH_CHECK(summary.id.value() == request.value().canonical_digest);
  CPATH_CHECK(plan.value().stats.path_count > 0u);
  CPATH_CHECK(plan.value().stats.hop_count > 0u);
  std::cout << "[multiprocess] plan over TCP: id " << plan.value().id.value().hex() << " paths "
            << plan.value().stats.path_count << " hops " << plan.value().stats.hop_count
            << " canonical-digest " << request.value().canonical_digest.hex() << "\n";
  client.close();
}

// ---------------------------------------------------------------------------
// 3. the daemon commits durably, and a second client can list the plan.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, daemon_commits_durably_and_second_client_lists_plan) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-durable");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  const std::string text = cpath_test::four_node_ring_text();
  auto request = cpath::parse_request_text(text);
  CPATH_REQUIRE(request.has_value());
  const cpath::Digest digest = request.value().canonical_digest;

  cpath::ServiceClient first;
  cpath::HelloAckPayload ack{};
  if (!open_session(first, daemon.port, "multiprocess-durable", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }
  auto response = first.plan(text, false);
  CPATH_REQUIRE(response.has_value());
  CPATH_REQUIRE(response.value().has_plan);

  // The commit is durable before the response is written, so the record is on
  // disk now. Asserted with std::filesystem against the store directory.
  const StoreScan scan = scan_store(workspace.store);
  CPATH_CHECK(scan.store_meta);
  CPATH_CHECK_EQ(scan.records, static_cast<std::size_t>(1));
  CPATH_CHECK(fs::exists(join_path(workspace.store, "plan-" + digest.hex() + ".rec")));
  std::cout << "[multiprocess] durable store: " << scan.records << " plan record(s), store.meta "
            << (scan.store_meta ? "present" : "missing") << ", plan-" << digest.hex() << ".rec\n";

  // A second client, started afterwards, sees the plan the first one created.
  cpath::ServiceClient second;
  cpath::HelloAckPayload second_ack{};
  if (!open_session(second, daemon.port, "multiprocess-durable-second", second_ack, error)) {
    CPATH_FAIL("cannot open the second session against cpathd: " + error);
  }
  auto listed = second.list_plans();
  CPATH_REQUIRE(listed.has_value());
  CPATH_CHECK(list_contains(listed.value(), digest));
  auto fetched = second.get_plan(digest);
  CPATH_REQUIRE(fetched.has_value());
  CPATH_CHECK(fetched.value().has_plan);
  std::cout << "[multiprocess] second client sees " << listed.value().plans.size()
            << " plan(s); get-plan has_plan " << (fetched.value().has_plan ? 1 : 0) << "\n";
  first.close();
  second.close();
}

// ---------------------------------------------------------------------------
// 4 + 5. abort the daemon with an OS-level kill, restart it against the same
//        store, and prove the fences advanced and the old plan is not current.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, abrupt_kill_then_restart_advances_fences_and_stales_plan) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-restart");
  const std::string text = cpath_test::four_node_ring_text();
  auto request = cpath::parse_request_text(text);
  CPATH_REQUIRE(request.has_value());

  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  cpath::ServiceClient before;
  cpath::HelloAckPayload ack_before{};
  if (!open_session(before, daemon.port, "multiprocess-before-kill", ack_before, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }
  const std::uint64_t epoch_before = ack_before.epoch;
  const std::uint64_t incarnation_before = ack_before.incarnation;
  auto response = before.plan(text, false);
  CPATH_REQUIRE(response.has_value());
  CPATH_REQUIRE(response.value().has_plan);
  cpath::StoredPlanSummary summary{};
  auto committed = cpath::decode_plan_record(response.value().plan_record, &summary);
  CPATH_REQUIRE(committed.has_value());
  const cpath::Digest plan_id = committed.value().id.value();
  CPATH_CHECK(summary.written_incarnation.value() == incarnation_before);
  before.close();

  // --- proof 4: the SAME daemon is killed abruptly, with no graceful shutdown.
  CPATH_REQUIRE(daemon.alive());
  daemon.child.kill();  // TerminateProcess on Windows, SIGKILL on POSIX
  CPATH_CHECK(!daemon.child.running());
  CPATH_CHECK(daemon.child.exited());
  const int kill_code = daemon.child.join();
  // A killed process never reports a clean exit: Windows reports the
  // TerminateProcess code 0xDEAD (57005), POSIX reports no exit status (-1).
  CPATH_CHECK(kill_code != 0);
  CPATH_CHECK(!daemon.alive());
  std::cout << "[multiprocess] abrupt kill: TerminateProcess/SIGKILL exit code " << kill_code
            << ", running() false, join() returned\n";

  // The killed process never wrote a new port file; the stale one still holds
  // the old port, so it is removed before the restart or wait_for_file would
  // return the previous port immediately.
  CPATH_CHECK(cpath_test::remove_file(workspace.port_file));

  // --- proof 5: restart against the same store, same port file path.
  DaemonProcess restarted;
  if (!restarted.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot restart cpathd against the surviving store: " + error);
  }
  cpath::ServiceClient after;
  cpath::HelloAckPayload ack_after{};
  if (!open_session(after, restarted.port, "multiprocess-after-kill", ack_after, error)) {
    CPATH_FAIL("cannot open a session against the restarted cpathd: " + error);
  }
  CPATH_CHECK(after.ping().is_ok());
  std::cout << "[multiprocess] restart: epoch " << epoch_before << " -> " << ack_after.epoch
            << ", incarnation " << incarnation_before << " -> " << ack_after.incarnation << "\n";
  CPATH_CHECK(ack_after.epoch > epoch_before);
  CPATH_CHECK(ack_after.incarnation > incarnation_before);

  // The record the dead process committed is still readable...
  auto listed = after.list_plans();
  CPATH_REQUIRE(listed.has_value());
  CPATH_CHECK(list_contains(listed.value(), plan_id));
  const auto entry = std::find_if(listed.value().plans.begin(), listed.value().plans.end(),
                                  [&plan_id](const cpath::PlanSummaryPayload& item) {
                                    return item.plan_id == plan_id;
                                  });
  CPATH_REQUIRE(entry != listed.value().plans.end());
  // ...but a plan read back from storage is never presented as current.
  CPATH_CHECK_EQ(static_cast<unsigned>(entry->freshness),
                 static_cast<unsigned>(cpath::PlanFreshness::kUnverified));
  auto fetched = after.get_plan(plan_id);
  CPATH_REQUIRE(fetched.has_value());
  CPATH_CHECK(fetched.value().has_plan);
  CPATH_CHECK_EQ(fetched.value().plan_record.size(), response.value().plan_record.size());
  CPATH_CHECK_EQ(static_cast<unsigned>(fetched.value().freshness),
                 static_cast<unsigned>(cpath::PlanFreshness::kUnverified));

  // Revalidation after the crash. This build derives still_valid purely from the
  // input digest and the generation bindings (see PlanStore::revalidate), so the
  // SAME request text re-hashes to the SAME digest and still_valid comes back
  // true; the crash is reported in the detail, which names the incarnation the
  // record was written under and the incarnation the store now runs under. The
  // suite encodes exactly that case and asserts the incarnation really differs,
  // so the incarnation caveat -- not a same-incarnation shortcut -- is what is
  // under test:
  //   * still_valid is true only because the inputs are unchanged;
  //   * the plan is never offered as current: freshness is kUnverified above and
  //     revalidate names the dead incarnation in its detail.
  CPATH_CHECK(ack_after.incarnation != incarnation_before);
  auto revalidated = after.revalidate(plan_id, text);
  CPATH_REQUIRE(revalidated.has_value());
  CPATH_CHECK(revalidated.value().still_valid);
  CPATH_CHECK(revalidated.value().freshness ==
              static_cast<std::uint8_t>(cpath::PlanFreshness::kCurrent));
  CPATH_CHECK(revalidated.value().detail.find("incarnation") != std::string::npos);
  CPATH_CHECK(revalidated.value().detail.find(std::to_string(incarnation_before)) != std::string::npos);
  CPATH_CHECK(revalidated.value().detail.find(std::to_string(ack_after.incarnation)) != std::string::npos);
  std::cout << "[multiprocess] revalidation after restart: still_valid "
            << (revalidated.value().still_valid ? 1 : 0) << " freshness "
            << static_cast<unsigned>(revalidated.value().freshness) << " detail "
            << revalidated.value().detail << "\n";
  after.close();
}

// ---------------------------------------------------------------------------
// 6. a byte-for-byte replay of an already-sent frame is refused.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, stale_frame_replay_is_refused) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-replay");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  // Heap-owned: if the coordinator retires this session the closure probe below
  // abandons the client on purpose (see probe_further_reply).
  cpath::ServiceClient* session = new cpath::ServiceClient();
  cpath::HelloAckPayload ack{};
  if (!open_session(*session, daemon.port, "multiprocess-replay", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }

  // The handshake consumed sequence 1, so this is exactly the ping frame the
  // client itself would send next. It is built once and sent twice.
  const std::vector<std::uint8_t> frame =
      build_frame(cpath::FrameType::kPing, 2u, ack.epoch, ack.session_id, {});
  CPATH_CHECK(session->send_raw(frame).is_ok());
  auto pong = session->receive_frame();
  CPATH_REQUIRE(pong.has_value());
  CPATH_CHECK(pong.value().header.type == cpath::FrameType::kPong);

  // Byte-for-byte replay of the frame the coordinator already answered.
  CPATH_CHECK(session->send_raw(frame).is_ok());
  auto refusal = session->receive_frame();
  CPATH_REQUIRE(refusal.has_value());
  CPATH_CHECK(refusal.value().header.type == cpath::FrameType::kError);
  CPATH_CHECK_EQ(static_cast<unsigned>(error_code_of(refusal.value())),
                 static_cast<unsigned>(cpath::ErrorCode::kSequenceMismatch));
  std::cout << "[multiprocess] stale replay: refused with kSequenceMismatch on sequence 2\n";

  // A stale frame never takes effect. What the coordinator does next is either
  // to refuse the identical copy again (the session stays fenced) or to retire
  // the session (no further frame at all); both are observed here, and neither
  // lets the replayed frame through.
  (void)session->send_raw(frame);
  auto* slot = new cpath::Result<cpath::Frame>(cpath::Result<cpath::Frame>::failure(
      cpath::ErrorCode::kSocketFailure, "no reply from the coordinator"));
  const SessionFate fate = probe_further_reply(session, slot, kSilenceBudgetMillis);
  if (fate == SessionFate::kAnswered) {
    if (slot->has_value()) {
      CPATH_CHECK(slot->value().header.type == cpath::FrameType::kError);
      CPATH_CHECK_EQ(static_cast<unsigned>(error_code_of(slot->value())),
                     static_cast<unsigned>(cpath::ErrorCode::kSequenceMismatch));
      // The refusal is about the replay, not about a broken session: a
      // legitimate frame whose sequence advanced past the replayed one is still
      // served on the same session.
      CPATH_CHECK(session
                      ->send_raw(build_frame(cpath::FrameType::kPing, 3u, ack.epoch, ack.session_id, {}))
                      .is_ok());
      auto advanced = session->receive_frame();
      CPATH_REQUIRE(advanced.has_value());
      CPATH_CHECK(advanced.value().header.type == cpath::FrameType::kPong);
      std::cout << "[multiprocess] stale replay: refused again on the same session while sequence 3 "
                   "was still served; the replayed frame never applies\n";
    } else {
      std::cout << "[multiprocess] stale replay: the coordinator closed the fenced session ("
                << slot->status().to_string() << ")\n";
    }
    delete slot;
    delete session;
  } else {
    std::cout << "[multiprocess] stale replay: the coordinator retired the session after refusing "
                 "the replay\n";
  }

  // A fresh session is unaffected by the refused replay.
  cpath::ServiceClient fresh;
  cpath::HelloAckPayload fresh_ack{};
  if (!open_session(fresh, daemon.port, "multiprocess-replay-fresh", fresh_ack, error)) {
    CPATH_FAIL("a fresh session after the refused replay did not work: " + error);
  }
  CPATH_CHECK(fresh.ping().is_ok());
  CPATH_CHECK(daemon.alive());
  fresh.close();
}

// ---------------------------------------------------------------------------
// 7. a frame with a valid header but a corrupted payload is refused.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, corrupt_payload_digest_is_refused_and_other_sessions_survive) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  Workspace workspace("mp-corrupt");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  // Heap-owned: the closure probe abandons this client if the coordinator
  // retires the corrupted session (see probe_further_reply).
  cpath::ServiceClient* session = new cpath::ServiceClient();
  cpath::HelloAckPayload ack{};
  if (!open_session(*session, daemon.port, "multiprocess-corrupt", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }

  cpath::PlanRequestPayload request;
  request.request_id = 7u;
  request.request_text = cpath_test::four_node_ring_text();
  cpath::ByteWriter writer;
  cpath::encode_plan_request(request, writer);
  std::vector<std::uint8_t> frame =
      build_frame(cpath::FrameType::kPlanRequest, 2u, ack.epoch, ack.session_id, writer.bytes());
  CPATH_REQUIRE(frame.size() > cpath::kFrameHeaderBytes + cpath::kFrameTrailerBytes);
  // Flip one payload byte: the header (and its checksum) stays valid, the
  // payload digest in the trailer no longer matches.
  frame[cpath::kFrameHeaderBytes] = static_cast<std::uint8_t>(frame[cpath::kFrameHeaderBytes] ^ 0x40u);
  CPATH_CHECK(session->send_raw(frame).is_ok());
  auto refusal = session->receive_frame();
  CPATH_REQUIRE(refusal.has_value());
  CPATH_CHECK(refusal.value().header.type == cpath::FrameType::kError);
  CPATH_CHECK_EQ(static_cast<unsigned>(error_code_of(refusal.value())),
                 static_cast<unsigned>(cpath::ErrorCode::kBadPayloadDigest));
  std::cout << "[multiprocess] corrupt payload: refused with kBadPayloadDigest\n";

  // The corrupted session is closed, and the client observes that as a release:
  // the coordinator answers the corrupt frame exactly once and then shuts the
  // session down, so the next read returns end-of-stream rather than hanging.
  auto* slot = new cpath::Result<cpath::Frame>(cpath::Result<cpath::Frame>::failure(
      cpath::ErrorCode::kSocketFailure, "no reply from the coordinator"));
  const SessionFate fate = probe_further_reply(session, slot, kSilenceBudgetMillis);
  CPATH_CHECK(fate == SessionFate::kClosed);
  if (fate == SessionFate::kAnswered) {
    std::cout << "[multiprocess] corrupt payload: unexpected extra frame of type "
              << (slot->has_value() ? static_cast<unsigned>(slot->value().header.type) : 0u) << "\n";
  }
  if (fate == SessionFate::kClosed) {
    CPATH_CHECK_EQ(static_cast<unsigned>(slot->status().code()),
                   static_cast<unsigned>(cpath::ErrorCode::kPeerClosed));
    std::cout << "[multiprocess] corrupt payload: session released with kPeerClosed\n";
  }

  // The coordinator keeps serving other sessions.
  cpath::ServiceClient other;
  cpath::HelloAckPayload other_ack{};
  if (!open_session(other, daemon.port, "multiprocess-corrupt-other", other_ack, error)) {
    CPATH_FAIL("the coordinator stopped serving after a corrupt frame: " + error);
  }
  CPATH_CHECK(other.ping().is_ok());
  auto planned = other.plan(cpath_test::four_node_ring_text(), false);
  CPATH_REQUIRE(planned.has_value());
  CPATH_CHECK(planned.value().has_plan);
  CPATH_CHECK(daemon.alive());
  other.close();
}

// ---------------------------------------------------------------------------
// 8. the CLI is a real second process: ping, then a graceful shutdown with the
//    epoch it observed.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, cli_ping_and_shutdown_are_real_processes) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  CPATH_REQUIRE(fs::exists(cli_executable()));
  Workspace workspace("mp-cli");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  cpath::ServiceClient client;
  cpath::HelloAckPayload ack{};
  if (!open_session(client, daemon.port, "multiprocess-cli", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }
  const std::uint64_t observed_epoch = ack.epoch;
  const std::string endpoint = "127.0.0.1:" + std::to_string(daemon.port);
  client.close();

  // rpc ping: exit code 0 and the output names the epoch the coordinator holds.
  const CommandOutcome ping =
      run_capturing(cli_executable(), {"rpc", "ping", "--endpoint", endpoint});
  std::cout << "[multiprocess] cpath rpc ping: exit " << ping.exit_code << " output " << ping.output;
  CPATH_CHECK_EQ(ping.exit_code, 0);
  CPATH_CHECK(ping.output.find("pong") != std::string::npos);
  CPATH_CHECK(ping.output.find("epoch " + std::to_string(observed_epoch)) != std::string::npos);
  CPATH_CHECK(ping.output.find("incarnation " + std::to_string(ack.incarnation)) != std::string::npos);

  // rpc shutdown with the observed epoch: acknowledged, exit code 0. Spawned as a
  // real second process; join() reaps it.
  cpath_test::ChildProcess shutdown_cli;
  if (!shutdown_cli.spawn(cli_executable(),
                          {"rpc", "shutdown", "--endpoint", endpoint, "--epoch",
                           std::to_string(observed_epoch)},
                          error)) {
    CPATH_FAIL("cannot start the cpath shutdown client: " + error);
  }
  const int shutdown_code = shutdown_cli.join();
  CPATH_CHECK_EQ(shutdown_code, 0);
  std::cout << "[multiprocess] cpath rpc shutdown --epoch " << observed_epoch << ": exit "
            << shutdown_code << "\n";

  // The daemon must leave on its own once its coordinator stops. join() blocks
  // until the process really exits and reports its exit code: no kill() and no
  // sleep-polling, so a daemon that kept idling would be a real defect.
  const int daemon_code = daemon.child.join();
  CPATH_CHECK_EQ(daemon_code, 0);
  CPATH_CHECK(daemon.child.exited());
  CPATH_CHECK(!daemon.alive());
  std::cout << "[multiprocess] cpathd exited on its own after the acknowledged shutdown with code "
            << daemon_code << "\n";
  daemon.retire();  // already exited: reaps and releases the handle without killing anything
  CPATH_CHECK(!daemon.alive());
}

// ---------------------------------------------------------------------------
// 9. a wrong-epoch shutdown is refused and leaves the daemon running.
// ---------------------------------------------------------------------------
CPATH_TEST(multiprocess, wrong_epoch_shutdown_is_refused) {
  CPATH_REQUIRE(fs::exists(daemon_executable()));
  CPATH_REQUIRE(fs::exists(cli_executable()));
  Workspace workspace("mp-wrong-epoch");
  DaemonProcess daemon;
  std::string error;
  if (!daemon.spawn(workspace.store, workspace.port_file, error)) {
    CPATH_FAIL("cannot start cpathd: " + error);
  }
  cpath::ServiceClient client;
  cpath::HelloAckPayload ack{};
  if (!open_session(client, daemon.port, "multiprocess-wrong-epoch", ack, error)) {
    CPATH_FAIL("cannot open a session against cpathd: " + error);
  }
  client.close();
  const std::string endpoint = "127.0.0.1:" + std::to_string(daemon.port);

  // A stale authority claim must be refused, not silently obeyed.
  const CommandOutcome refused =
      run_capturing(cli_executable(), {"rpc", "shutdown", "--endpoint", endpoint, "--epoch", "999999"});
  std::cout << "[multiprocess] cpath rpc shutdown --epoch 999999 (held epoch " << ack.epoch
            << "): exit " << refused.exit_code << " output " << refused.output;
  CPATH_CHECK(refused.exit_code != 0);
  // The refusal came from the coordinator, not from the argument parser: the CLI
  // really reached the daemon and reported the shutdown failure.
  CPATH_CHECK(refused.output.find("rpc shutdown") != std::string::npos);
  CPATH_CHECK(refused.output.find("an action is required") == std::string::npos);

  // The daemon is still running and still serving.
  CPATH_CHECK(daemon.alive());
  CPATH_CHECK(daemon.child.running());
  cpath::ServiceClient after;
  cpath::HelloAckPayload after_ack{};
  if (!open_session(after, daemon.port, "multiprocess-wrong-epoch-after", after_ack, error)) {
    CPATH_FAIL("the daemon stopped serving after a refused shutdown: " + error);
  }
  CPATH_CHECK(after.ping().is_ok());
  CPATH_CHECK(after_ack.epoch == ack.epoch);
  after.close();
  daemon.retire();
  CPATH_CHECK(!daemon.alive());
}

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }
