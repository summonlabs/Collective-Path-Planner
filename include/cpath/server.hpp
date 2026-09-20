// Collective Path Planner - coordinator service (planning authority boundary).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_SERVER_HPP
#define CPATH_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "cpath/limits.hpp"
#include "cpath/persistence.hpp"
#include "cpath/status.hpp"
#include "cpath/wire.hpp"

namespace cpath {

struct ServerConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};  // 0 selects an ephemeral port; read it back with port()
  std::size_t max_sessions{kMaxSessions};
  std::size_t max_queue_depth{kMaxSessionQueueDepth};
  std::size_t max_pending_requests{kMaxPendingRequestsPerSession};
  // Empty means no durable store: the coordinator answers planning requests but
  // offers no persistence surface.
  std::string store_root{};
  std::string label{"cpathd"};
  // Bound on how long a session may stay idle before the coordinator retires
  // it. Zero disables retirement. This is a resource bound, not a correctness
  // mechanism: no protocol decision depends on it.
  std::uint64_t idle_timeout_ms{0};
};

struct ServerStats {
  std::uint64_t sessions_accepted{0};
  std::uint64_t sessions_rejected{0};
  std::uint64_t sessions_closed{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_sent{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t handshakes_completed{0};
  std::uint64_t handshakes_refused{0};
  std::uint64_t plans_produced{0};
  std::uint64_t plan_denials{0};
  std::uint64_t revalidations_current{0};
  std::uint64_t revalidations_stale{0};
  std::uint64_t store_commits{0};
  std::uint64_t store_reads{0};
  std::uint64_t replays_refused{0};
  std::uint64_t errors{0};
  std::uint64_t queued_frames{0};
  std::uint64_t max_queue_depth_observed{0};
  std::uint64_t clients_turned_away{0};
};

// The coordinator owns a planning epoch and a process incarnation. Both are
// reported to clients and bound into every plan it produces. A restart always
// advances the incarnation, so a client holding an identifier from a previous
// process cannot use it as authority.
//
// Session lifetime rules, which the protocol tests assert directly:
//  * a frame that fails to decode closes its session: a peer that is emitting
//    corrupt bytes is not given a second chance on that connection;
//  * a protocol misuse on a well-formed frame (a request before the handshake, a
//    repeated handshake, a sequence that does not advance) is answered with a
//    deterministic error frame and leaves the session usable, so a client that
//    raced its own handshake can recover;
//  * a shutdown request carrying the wrong epoch is refused and the coordinator
//    keeps running.
class Coordinator {
 public:
  explicit Coordinator(ServerConfig config);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  Status start();
  Status stop();
  bool running() const noexcept;
  std::uint16_t port() const noexcept;
  const std::string& bind_address() const noexcept;
  Epoch epoch() const noexcept;
  Incarnation incarnation() const noexcept;
  ServerStats stats() const noexcept;
  const StoreConfig& store_config() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Blocking client used by the CLI and by the multiprocess tests. It performs a
// real TCP connection, a framed handshake, and synchronous request/response
// exchanges with strict integrity checking on every frame.
class ServiceClient {
 public:
  ServiceClient();
  ~ServiceClient();

  ServiceClient(const ServiceClient&) = delete;
  ServiceClient& operator=(const ServiceClient&) = delete;

  Status connect(const std::string& host, std::uint16_t port);
  void close();
  bool connected() const noexcept;

  Result<HelloAckPayload> handshake(std::string_view label);
  Status ping();
  Result<PlanResponsePayload> plan(std::string_view request_text, bool include_explanation);
  Result<ListPlansResponsePayload> list_plans();
  Result<GetPlanResponsePayload> get_plan(const Digest& plan_id);
  Result<RevalidateResponsePayload> revalidate(const Digest& plan_id, std::string_view request_text);
  Result<ShutdownRequestPayload> shutdown(std::uint64_t expected_epoch);

  // Raw frame exchange, for adversarial protocol tests.
  Result<Frame> exchange(const FrameHeader& header, std::span<const std::uint8_t> payload);
  Status send_raw(std::span<const std::uint8_t> bytes);
  Result<Frame> receive_frame();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// "host:port" parser. Rejects anything malformed rather than guessing.
Result<std::uint16_t> parse_endpoint(std::string_view text, std::string& host_out);

}  // namespace cpath

#endif  // CPATH_SERVER_HPP
