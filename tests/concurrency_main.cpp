// Collective Path Planner - coordinator lifecycle, locking and race proofs.
//
// The fabrics used here are SYNTHETIC: every planning request comes from the
// hand-written fixtures in tests/support/fixtures.hpp and no measurement from a
// real device is involved.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpath/server.hpp"
#include "cpath/status.hpp"
#include "cpath/wire.hpp"
#include "fixtures.hpp"
#include "process.hpp"
#include "test_framework.hpp"

namespace {

// Releases N workers at the same moment. Every worker arrives even when it
// failed earlier, so one failure cannot strand the others here.
class StartBarrier {
 public:
  explicit StartBarrier(std::size_t expected) : expected_(expected) {}

  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ >= expected_) {
      lock.unlock();
      ready_.notify_all();
      return;
    }
    ready_.wait(lock, [this] { return arrived_ >= expected_; });
  }

 private:
  std::mutex mutex_{};
  std::condition_variable ready_{};
  std::size_t arrived_{0};
  std::size_t expected_{0};
};

struct SessionOutcome {
  bool connect_ok{false};
  bool handshake_ok{false};
  bool plan_ok{false};
  bool has_plan{false};
  std::uint64_t session_id{0};
  cpath::Status failure{};
};

cpath::ServerConfig loopback_config() {
  cpath::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.label = "cpath-concurrency";
  return config;
}

// The failure set a client can see once the coordinator has torn a session
// down: the transport reports a definite breakage, never stale data.
bool is_transport_failure(cpath::ErrorCode code) {
  return code == cpath::ErrorCode::kSessionClosed || code == cpath::ErrorCode::kSendFailure ||
         code == cpath::ErrorCode::kReceiveFailure || code == cpath::ErrorCode::kPeerClosed ||
         code == cpath::ErrorCode::kSocketFailure || code == cpath::ErrorCode::kConnectionRefused ||
         code == cpath::ErrorCode::kServiceStopping;
}

// Bounded convergence wait. A cancelled socket is noticed by the coordinator's
// own reader thread, so the test waits for that observation to land instead of
// assuming a fixed delay. The budget is a failed-proof guard, not a timeout: the
// test always returns and reports.
template <class Predicate>
bool wait_until(Predicate predicate, unsigned budget_millis) {
  for (unsigned elapsed = 0; elapsed <= budget_millis; ++elapsed) {
    if (predicate()) {
      return true;
    }
    cpath_test::sleep_millis(1);
  }
  return predicate();
}

SessionOutcome run_threaded_session(std::uint16_t port, const char* label, const std::string& text,
                                    StartBarrier& barrier) {
  SessionOutcome outcome;
  cpath::ServiceClient client;
  outcome.failure = client.connect("127.0.0.1", port);
  outcome.connect_ok = outcome.failure.is_ok();
  if (outcome.connect_ok) {
    auto ack = client.handshake(label);
    outcome.handshake_ok = ack.has_value();
    if (ack.has_value()) {
      outcome.session_id = ack.value().session_id;
    } else {
      outcome.failure = ack.status();
    }
  }
  barrier.arrive_and_wait();
  if (outcome.handshake_ok) {
    auto response = client.plan(text, false);
    outcome.plan_ok = response.has_value();
    if (response.has_value()) {
      outcome.has_plan = response.value().has_plan;
    } else {
      outcome.failure = response.status();
    }
  }
  client.close();
  return outcome;
}

cpath::Result<cpath::ErrorPayload> error_from(const cpath::Result<cpath::Frame>& reply) {
  if (!reply.has_value()) {
    return cpath::Result<cpath::ErrorPayload>::failure(reply.status());
  }
  if (reply.value().header.type != cpath::FrameType::kError) {
    return cpath::Result<cpath::ErrorPayload>::failure(
        cpath::ErrorCode::kUnexpectedFrameType, "the coordinator did not answer with an error frame");
  }
  cpath::ByteReader reader(reply.value().payload);
  cpath::ErrorPayload payload;
  const cpath::Status status = cpath::decode_error(reader, payload);
  if (!status.is_ok()) {
    return cpath::Result<cpath::ErrorPayload>::failure(status);
  }
  return cpath::Result<cpath::ErrorPayload>::success(payload);
}

cpath::Result<cpath::Frame> send_list_request(cpath::ServiceClient& client, std::uint32_t request_id) {
  cpath::ListPlansRequestPayload payload;
  payload.request_id = request_id;
  cpath::ByteWriter writer;
  cpath::encode_list_plans_request(payload, writer);
  cpath::FrameHeader header;
  header.type = cpath::FrameType::kListPlansRequest;
  header.request_id = request_id;
  return client.exchange(header, writer.bytes());
}

}  // namespace

CPATH_TEST(concurrency_lifecycle, repeated_start_stop_binds_fresh_ports) {
  constexpr int kCycles = 12;  // more than the ten cycles the lifecycle proof needs
  cpath::Coordinator coordinator(loopback_config());
  std::vector<std::uint16_t> ports;
  ports.reserve(static_cast<std::size_t>(kCycles));

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    CPATH_REQUIRE(coordinator.start().is_ok());
    CPATH_CHECK(coordinator.running());
    const std::uint16_t port = coordinator.port();
    CPATH_REQUIRE(port != 0u);
    // A fresh bind each cycle: the previous listener was fully released.
    CPATH_CHECK(std::find(ports.begin(), ports.end(), port) == ports.end());
    ports.push_back(port);

    cpath::ServiceClient client;
    CPATH_REQUIRE(client.connect("127.0.0.1", port).is_ok());
    auto ack = client.handshake("cycle-client");
    CPATH_REQUIRE(ack.has_value());
    CPATH_CHECK(ack.value().session_id != 0u);
    CPATH_CHECK_EQ(ack.value().epoch, coordinator.epoch().value());
    CPATH_CHECK(client.ping().is_ok());
    client.close();

    CPATH_REQUIRE(coordinator.stop().is_ok());
    CPATH_CHECK(!coordinator.running());
    CPATH_REQUIRE(coordinator.stop().is_ok());  // stop() is idempotent
    CPATH_CHECK(!coordinator.running());

    const cpath::ServerStats stats = coordinator.stats();
    CPATH_CHECK_EQ(stats.sessions_accepted, static_cast<std::uint64_t>(cycle + 1));
    CPATH_CHECK_EQ(stats.sessions_closed, static_cast<std::uint64_t>(cycle + 1));
    CPATH_CHECK_EQ(stats.errors, 0u);
  }
  CPATH_CHECK_EQ(ports.size(), static_cast<std::size_t>(kCycles));
}

CPATH_TEST(concurrency_lifecycle, eight_clients_plan_concurrently) {
  constexpr std::size_t kClients = 8;
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  // SYNTHETIC fabric: the minimal ring fixture, planned by every worker.
  const std::string text = cpath_test::minimal_ring_text();
  StartBarrier barrier(kClients);
  std::vector<SessionOutcome> outcomes(kClients);
  std::vector<std::thread> workers;
  workers.reserve(kClients);
  for (std::size_t index = 0; index < kClients; ++index) {
    workers.emplace_back([&barrier, &outcomes, &text, port, index] {
      outcomes[index] = run_threaded_session(port, "concurrent-client", text, barrier);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  std::vector<std::uint64_t> sessions;
  sessions.reserve(kClients);
  for (std::size_t index = 0; index < kClients; ++index) {
    CPATH_CHECK(outcomes[index].connect_ok);
    CPATH_CHECK(outcomes[index].handshake_ok);
    CPATH_CHECK(outcomes[index].plan_ok);
    CPATH_CHECK(outcomes[index].has_plan);
    sessions.push_back(outcomes[index].session_id);
  }
  std::sort(sessions.begin(), sessions.end());
  CPATH_CHECK(std::adjacent_find(sessions.begin(), sessions.end()) == sessions.end());

  const cpath::ServerStats stats = coordinator.stats();
  CPATH_CHECK(stats.sessions_accepted >= kClients);
  CPATH_CHECK(stats.handshakes_completed >= kClients);
  CPATH_CHECK(stats.plans_produced >= kClients);
  CPATH_CHECK_EQ(stats.errors, 0u);
  CPATH_CHECK_EQ(stats.replays_refused, 0u);
  CPATH_REQUIRE(coordinator.stop().is_ok());
}

CPATH_TEST(concurrency_lifecycle, cancelled_handshake_does_not_disturb_the_acceptor) {
  constexpr std::size_t kCancelled = 3;
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  for (std::size_t attempt = 0; attempt < kCancelled; ++attempt) {
    cpath::ServiceClient raw;
    CPATH_REQUIRE(raw.connect("127.0.0.1", port).is_ok());
    raw.close();  // the session dies before it ever sends a hello
  }
  // A client that connects and closes sends no frame at all, so this is an
  // orderly release and must NOT be counted as a rejected frame: the rejected
  // counter is reserved for frames the decoder refused.
  CPATH_CHECK(wait_until(
      [&coordinator] { return coordinator.stats().sessions_accepted >= kCancelled; }, 2000u));
  CPATH_CHECK(coordinator.running());

  // The acceptor is untouched: a new session still handshakes, pings and plans.
  cpath::ServiceClient healthy;
  CPATH_REQUIRE(healthy.connect("127.0.0.1", port).is_ok());
  auto ack = healthy.handshake("after-cancellations");
  CPATH_REQUIRE(ack.has_value());
  CPATH_CHECK(healthy.ping().is_ok());
  // SYNTHETIC fabric: the minimal ring fixture.
  auto response = healthy.plan(cpath_test::minimal_ring_text(), false);
  CPATH_REQUIRE(response.has_value());
  CPATH_CHECK(response.value().has_plan);
  healthy.close();

  CPATH_REQUIRE(coordinator.stop().is_ok());
  const cpath::ServerStats stats = coordinator.stats();
  CPATH_CHECK_EQ(stats.sessions_accepted, static_cast<std::uint64_t>(kCancelled + 1u));
  CPATH_CHECK_EQ(stats.sessions_closed, static_cast<std::uint64_t>(kCancelled + 1u));
  CPATH_CHECK_EQ(stats.errors, 0u);
  CPATH_CHECK_EQ(stats.frames_rejected, 0u);
}

CPATH_TEST(concurrency_limits, third_session_is_refused_at_the_limit) {
  cpath::ServerConfig config = loopback_config();
  config.max_sessions = 2u;
  cpath::Coordinator coordinator(config);
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  cpath::ServiceClient first;
  cpath::ServiceClient second;
  CPATH_REQUIRE(first.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(second.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(first.handshake("limit-one").has_value());
  CPATH_REQUIRE(second.handshake("limit-two").has_value());

  // The refusal is written as soon as the third connection is accepted, so the
  // test reads it rather than racing a handshake against the coordinator.
  cpath::ServiceClient third;
  CPATH_REQUIRE(third.connect("127.0.0.1", port).is_ok());
  auto refusal = third.receive_frame();
  CPATH_REQUIRE(refusal.has_value());
  CPATH_CHECK(refusal.value().header.type == cpath::FrameType::kError);
  cpath::ByteReader reader(refusal.value().payload);
  cpath::ErrorPayload payload;
  CPATH_REQUIRE(cpath::decode_error(reader, payload).is_ok());
  CPATH_CHECK_EQ(payload.code, static_cast<std::uint16_t>(cpath::ErrorCode::kSessionLimitExceeded));
  auto closed = third.receive_frame();
  CPATH_CHECK(!closed.has_value());
  CPATH_CHECK(is_transport_failure(closed.status().code()));

  // The two admitted sessions keep working; the refusal killed nothing.
  CPATH_CHECK(first.ping().is_ok());
  CPATH_CHECK(second.ping().is_ok());
  const cpath::ServerStats before = coordinator.stats();
  CPATH_CHECK_EQ(before.sessions_accepted, 2u);
  CPATH_CHECK_EQ(before.sessions_rejected, 1u);
  CPATH_CHECK_EQ(before.clients_turned_away, 1u);

  first.close();
  second.close();
  third.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());
  const cpath::ServerStats after = coordinator.stats();
  CPATH_CHECK_EQ(after.sessions_accepted, 2u);
  CPATH_CHECK_EQ(after.sessions_closed, after.sessions_accepted);
  CPATH_CHECK_EQ(after.sessions_rejected, 1u);
}

CPATH_TEST(concurrency_shutdown, stop_with_live_clients_closes_both_sessions) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  cpath::ServiceClient first;
  cpath::ServiceClient second;
  CPATH_REQUIRE(first.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(second.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(first.handshake("live-one").has_value());
  CPATH_REQUIRE(second.handshake("live-two").has_value());
  CPATH_CHECK(first.ping().is_ok());
  CPATH_CHECK(second.ping().is_ok());
  CPATH_CHECK_EQ(coordinator.stats().sessions_accepted, 2u);

  // The main thread stops the coordinator while both sessions are still open.
  // stop() joins the acceptor, the reaper and every session thread, so a
  // deadlock in the shutdown path shows up as a hang of this test process
  // rather than as a silent pass.
  CPATH_REQUIRE(coordinator.stop().is_ok());
  CPATH_CHECK(!coordinator.running());
  CPATH_CHECK_EQ(coordinator.stats().sessions_closed, 2u);

  // Neither client may read stale data from the closed sessions.
  // SYNTHETIC fabric: the minimal ring fixture, requested after the stop.
  auto first_after = first.plan(cpath_test::minimal_ring_text(), false);
  CPATH_CHECK(!first_after.has_value());
  CPATH_CHECK(is_transport_failure(first_after.status().code()));
  const cpath::Status second_after = second.ping();
  CPATH_CHECK(!second_after.is_ok());
  CPATH_CHECK(is_transport_failure(second_after.code()));

  first.close();
  second.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());  // still idempotent after a live shutdown
  CPATH_CHECK(!coordinator.running());
}

CPATH_TEST(concurrency_shutdown, restart_advances_incarnation_and_stales_old_session) {
  const std::string store_root = cpath_test::temporary_directory("cpath-concurrency-store");
  cpath::ServerConfig config = loopback_config();
  config.store_root = store_root;
  cpath::Coordinator coordinator(config);
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint64_t incarnation_before = coordinator.incarnation().value();
  const std::uint64_t epoch_before = coordinator.epoch().value();
  const std::uint16_t port_before = coordinator.port();
  CPATH_REQUIRE(port_before != 0u);

  cpath::ServiceClient stale;
  CPATH_REQUIRE(stale.connect("127.0.0.1", port_before).is_ok());
  auto ack_before = stale.handshake("before-restart");
  CPATH_REQUIRE(ack_before.has_value());
  CPATH_CHECK_EQ(ack_before.value().incarnation, incarnation_before);
  CPATH_CHECK_EQ(ack_before.value().epoch, epoch_before);
  const std::uint64_t stale_session_id = ack_before.value().session_id;
  CPATH_CHECK(stale_session_id != 0u);

  CPATH_REQUIRE(coordinator.stop().is_ok());
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint64_t incarnation_after = coordinator.incarnation().value();
  const std::uint64_t epoch_after = coordinator.epoch().value();
  const std::uint16_t port_after = coordinator.port();
  // A restarted coordinator is a new authority, and it binds a new port.
  CPATH_CHECK(incarnation_after > incarnation_before);
  CPATH_CHECK(epoch_after > epoch_before);
  CPATH_CHECK(port_after != port_before);

  // The session that existed before the restart is gone: no stale plan.
  // SYNTHETIC fabric: the minimal ring fixture, requested after the restart.
  auto stale_plan = stale.plan(cpath_test::minimal_ring_text(), false);
  CPATH_CHECK(!stale_plan.has_value());
  CPATH_CHECK(is_transport_failure(stale_plan.status().code()));

  // Reconnecting does not carry the old identifier across: without a fresh
  // handshake the new incarnation refuses every request.
  stale.close();
  CPATH_REQUIRE(stale.connect("127.0.0.1", port_after).is_ok());
  auto refused = error_from(send_list_request(stale, 21u));
  CPATH_REQUIRE(refused.has_value());
  CPATH_CHECK_EQ(refused.value().code,
                 static_cast<std::uint16_t>(cpath::ErrorCode::kSessionHandshakeRequired));

  auto ack_after = stale.handshake("after-restart");
  CPATH_REQUIRE(ack_after.has_value());
  CPATH_CHECK_EQ(ack_after.value().incarnation, incarnation_after);
  CPATH_CHECK_EQ(ack_after.value().epoch, epoch_after);
  CPATH_CHECK(ack_after.value().session_id != stale_session_id);
  stale.close();

  CPATH_REQUIRE(coordinator.stop().is_ok());
  cpath_test::remove_directory_tree(store_root);
}

CPATH_TEST(concurrency_shutdown, self_shutdown_request_stops_the_coordinator) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  cpath::ServiceClient client;
  CPATH_REQUIRE(client.connect("127.0.0.1", coordinator.port()).is_ok());
  CPATH_REQUIRE(client.handshake("shutdown-client").has_value());

  // A shutdown request for the wrong epoch is refused and changes nothing.
  cpath::ShutdownRequestPayload wrong;
  wrong.request_id = 5u;
  wrong.expected_epoch = coordinator.epoch().value() + 41u;
  cpath::ByteWriter writer;
  cpath::encode_shutdown_request(wrong, writer);
  cpath::FrameHeader header;
  header.type = cpath::FrameType::kShutdownRequest;
  header.request_id = wrong.request_id;
  auto refused = error_from(client.exchange(header, writer.bytes()));
  CPATH_REQUIRE(refused.has_value());
  CPATH_CHECK_EQ(refused.value().code, static_cast<std::uint16_t>(cpath::ErrorCode::kStaleEpoch));
  CPATH_CHECK(coordinator.running());
  CPATH_CHECK(client.ping().is_ok());  // the refusal does not close the session

  // The current epoch is accepted, and the coordinator stops itself: this thread
  // never calls stop() before observing running() == false.
  CPATH_REQUIRE(client.shutdown(coordinator.epoch().value()).has_value());
  CPATH_CHECK(wait_until([&coordinator] { return !coordinator.running(); }, 2000u));
  CPATH_CHECK(!coordinator.running());
  // SYNTHETIC fabric: the minimal ring fixture, requested after self-shutdown.
  auto after = client.plan(cpath_test::minimal_ring_text(), false);
  CPATH_CHECK(!after.has_value());

  CPATH_REQUIRE(coordinator.stop().is_ok());  // completing afterwards is fine
  CPATH_CHECK(!coordinator.running());
  CPATH_REQUIRE(coordinator.stop().is_ok());
}

CPATH_TEST(concurrency_accounting, counters_close_after_the_final_stop) {
  cpath::ServerConfig config = loopback_config();
  config.max_queue_depth = 8u;
  cpath::Coordinator coordinator(config);
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  {
    cpath::ServiceClient first;
    CPATH_REQUIRE(first.connect("127.0.0.1", port).is_ok());
    CPATH_REQUIRE(first.handshake("accounting-one").has_value());
    // SYNTHETIC fabric: the minimal ring fixture.
    auto response = first.plan(cpath_test::minimal_ring_text(), false);
    CPATH_REQUIRE(response.has_value());
    CPATH_CHECK(response.value().has_plan);
    first.close();
  }
  {
    cpath::ServiceClient second;
    CPATH_REQUIRE(second.connect("127.0.0.1", port).is_ok());
    CPATH_REQUIRE(second.handshake("accounting-two").has_value());
    second.close();
  }
  cpath::ServiceClient open_at_stop;
  CPATH_REQUIRE(open_at_stop.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(open_at_stop.handshake("accounting-three").has_value());

  // No session is refused in this run, so the closure identity below is the
  // exact accounting of accepted, closed and rejected sessions.
  CPATH_CHECK_EQ(coordinator.stats().sessions_rejected, 0u);
  CPATH_REQUIRE(coordinator.stop().is_ok());

  const cpath::ServerStats stats = coordinator.stats();
  CPATH_CHECK_EQ(stats.sessions_accepted, stats.sessions_closed + stats.sessions_rejected);
  CPATH_CHECK_EQ(stats.sessions_accepted, 3u);
  CPATH_CHECK_EQ(stats.sessions_closed, 3u);
  CPATH_CHECK_EQ(stats.sessions_rejected, 0u);
  CPATH_CHECK_EQ(stats.handshakes_completed, 3u);
  CPATH_CHECK_EQ(stats.errors, 0u);
  CPATH_CHECK(stats.frames_received >= stats.handshakes_completed);
  // The queue counters are an enqueue-side high-water mark: the dequeue path
  // does not republish the depth, so the closure proof is that the last
  // observation never exceeds the high-water mark, and the high-water mark never
  // exceeds the configured bound.
  CPATH_CHECK(stats.queued_frames <= stats.max_queue_depth_observed);
  CPATH_CHECK(stats.max_queue_depth_observed <= config.max_queue_depth);
  CPATH_CHECK(stats.max_queue_depth_observed >= 1u);
  open_at_stop.close();
}

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }
