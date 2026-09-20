// Collective Path Planner - coordinator service and blocking client.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// LOCK ORDER, identical everywhere in this file:
//   1. Impl::state_mutex
//   2. Session::mutex
//   3. the PlanStore's internal mutex
// A thread never acquires a lower-numbered lock while holding a higher-numbered
// one, never calls back into user code while holding Session::mutex, and never
// touches the store while holding Session::mutex.
//
// SESSION THREAD LIFETIME: a session's reader and worker threads are the only
// threads that touch that session's queue, and both exit before teardown()
// returns. The store pointer is written in start() before any session exists and
// cleared in teardown() after every session thread has been joined, so session
// threads read it without a lock.
#include "cpath/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cpath/dsl.hpp"
#include "cpath/explain.hpp"
#include "cpath/planner.hpp"
#include "net.hpp"

namespace cpath {
namespace {

std::uint64_t unix_micros_now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Marks the calling thread as belonging to a session, so that a self-initiated
// shutdown never tries to join itself.
thread_local bool g_is_session_thread = false;

Status read_exact(const internal::Socket& socket, std::span<std::uint8_t> buffer) {
  std::size_t received = 0;
  while (received < buffer.size()) {
    auto chunk = internal::receive_some(socket, buffer.subspan(received));
    if (!chunk.has_value()) {
      return chunk.status();
    }
    if (chunk.value() == 0) {
      return Status::error(ErrorCode::kPeerClosed, "peer closed the session mid-frame");
    }
    received += chunk.value();
  }
  return Status::ok();
}

Result<Frame> read_frame(const internal::Socket& socket) {
  std::vector<std::uint8_t> frame_bytes(kFrameHeaderBytes, 0u);
  if (Status status = read_exact(socket, frame_bytes); !status.is_ok()) {
    return Result<Frame>::failure(status);
  }
  std::size_t consumed = 0;
  auto header = decode_frame_header(frame_bytes, consumed);
  if (!header.has_value()) {
    return Result<Frame>::failure(header.status());
  }
  if (consumed != kFrameHeaderBytes) {
    return Result<Frame>::failure(ErrorCode::kTruncatedRecord, "frame header was not fully decoded");
  }
  const std::size_t offset = frame_bytes.size();
  const std::size_t tail = static_cast<std::size_t>(header.value().payload_length) + kFrameTrailerBytes;
  frame_bytes.resize(offset + tail);
  if (Status status = read_exact(socket, std::span<std::uint8_t>(frame_bytes.data() + offset, tail));
      !status.is_ok()) {
    return Result<Frame>::failure(status);
  }
  return decode_frame(frame_bytes);
}

Status write_frame(const internal::Socket& socket, const FrameHeader& header,
                   std::span<const std::uint8_t> payload) {
  const std::vector<std::uint8_t> bytes = encode_frame(header, payload);
  return internal::send_all(socket, bytes);
}

// A reopened plan is served from storage without its original request bytes. The
// record's request-digest field is a copy of the plan's canonical input digest,
// which is exactly what a request carrying only that digest would produce.
PlanningRequest digest_only_request(const Plan& plan) {
  PlanningRequest request;
  request.canonical_digest = plan.input_digest;
  return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

struct Coordinator::Impl {
  struct Session {
    SessionId id{};
    internal::Socket socket{};
    internal::WakeupPair wakeup{};
    std::mutex mutex{};
    std::condition_variable not_empty{};
    std::condition_variable not_full{};
    std::deque<Frame> inbox{};
    bool reader_done{false};
    bool handshaken{false};
    bool has_sequence{false};
    std::uint64_t last_sequence{0};
    std::uint32_t in_flight{0};
    std::thread reader{};
    std::thread worker{};
  };

  explicit Impl(ServerConfig value) : config(std::move(value)) {}

  ServerConfig config{};
  StoreConfig store_config{};
  internal::Socket listener{};
  internal::WakeupPair acceptor_wakeup{};
  std::unique_ptr<PlanStore> store{};
  std::vector<std::unique_ptr<Session>> sessions{};
  std::thread acceptor{};
  std::thread reaper{};
  std::atomic<std::thread::id> reaper_id{};
  std::atomic<bool> stopping{false};
  std::atomic<bool> running{false};
  std::atomic<bool> finished{false};
  std::atomic<std::uint64_t> next_session_id{1};
  std::atomic<std::uint64_t> epoch{0};
  std::atomic<std::uint64_t> incarnation{0};
  std::atomic<std::uint16_t> bound_port{0};
  mutable std::mutex state_mutex{};
  std::condition_variable state_changed{};
  ServerStats stats{};

  // Only ever called while state_mutex is held.
  void bump_locked(std::uint64_t amount, std::uint64_t ServerStats::*member) { stats.*member += amount; }

  // Never called while Session::mutex is held.
  void bump(std::uint64_t amount, std::uint64_t ServerStats::*member) {
    std::lock_guard<std::mutex> lock(state_mutex);
    bump_locked(amount, member);
  }

  void observe_queue_depth(std::size_t depth) {
    std::lock_guard<std::mutex> lock(state_mutex);
    stats.queued_frames = depth;
    if (depth > stats.max_queue_depth_observed) {
      stats.max_queue_depth_observed = depth;
    }
  }

  bool stopping_now() const { return stopping.load(std::memory_order_acquire); }

  void request_stop();
  void accept_loop();
  void start_session_threads(Session& session);
  void reader_loop(Session& session);
  void worker_loop(Session& session);
  Status handle_frame(Session& session, const Frame& frame, bool& request_shutdown);
  void teardown();
  void run_reaper();
};

void Coordinator::Impl::request_stop() {
  if (!stopping.exchange(true, std::memory_order_acq_rel)) {
    (void)acceptor_wakeup.signal();
    state_changed.notify_all();
  }
}

void Coordinator::Impl::accept_loop() {
  while (!stopping_now()) {
    auto ready = internal::wait_readable(listener, &acceptor_wakeup.reader());
    if (!ready.has_value()) {
      if (stopping_now()) {
        break;
      }
      bump(1u, &ServerStats::errors);
      continue;
    }
    if (ready.value() == internal::WaitOutcome::kSecond) {
      (void)acceptor_wakeup.drain();
      continue;
    }
    auto accepted = listener.accept();
    if (!accepted.has_value()) {
      if (stopping_now()) {
        break;
      }
      bump(1u, &ServerStats::errors);
      continue;
    }

    auto session = std::make_unique<Session>();
    session->socket = std::move(accepted.value());
    if (Status status = session->wakeup.open(); !status.is_ok()) {
      bump(1u, &ServerStats::errors);
      session->socket.shutdown_both();
      continue;
    }

    bool refused = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (sessions.size() >= config.max_sessions || stopping_now()) {
        bump_locked(1u, &ServerStats::sessions_rejected);
        bump_locked(1u, &ServerStats::clients_turned_away);
        refused = true;
      } else {
        session->id = SessionId{next_session_id.fetch_add(1u)};
        bump_locked(1u, &ServerStats::sessions_accepted);
        sessions.push_back(std::move(session));
      }
    }

    if (refused) {
      // Answer the refused client with a deterministic reason before closing.
      FrameHeader header;
      header.type = FrameType::kError;
      header.epoch = epoch.load();
      ErrorPayload payload;
      payload.code = static_cast<std::uint16_t>(ErrorCode::kSessionLimitExceeded);
      payload.message = "the coordinator is at its concurrent session limit";
      ByteWriter writer;
      encode_error(payload, writer);
      (void)write_frame(session->socket, header, writer.bytes());
      session->socket.shutdown_both();
      session->socket.close();
      session->wakeup.close();
      continue;
    }

    start_session_threads(*sessions.back());
  }
}

void Coordinator::Impl::start_session_threads(Session& session) {
  session.reader = std::thread([this, &session] {
    g_is_session_thread = true;
    reader_loop(session);
  });
  session.worker = std::thread([this, &session] {
    g_is_session_thread = true;
    worker_loop(session);
  });
}

void Coordinator::Impl::reader_loop(Session& session) {
  while (!stopping_now()) {
    auto ready = internal::wait_readable(session.socket, &session.wakeup.reader());
    if (!ready.has_value()) {
      break;
    }
    if (ready.value() == internal::WaitOutcome::kSecond) {
      (void)session.wakeup.drain();
      continue;
    }
    auto frame = read_frame(session.socket);
    if (!frame.has_value()) {
      if (frame.status().code() == ErrorCode::kPeerClosed ||
          frame.status().code() == ErrorCode::kReceiveFailure) {
        // An orderly client close (or a socket retired by teardown) is not a
        // rejected frame; the session simply ends.
        break;
      }
      // A malformed frame is answered exactly once with a deterministic code,
      // then the session is closed.
      FrameHeader header;
      header.type = FrameType::kError;
      header.epoch = epoch.load();
      header.session_id = session.id.value();
      ErrorPayload payload;
      payload.code = static_cast<std::uint16_t>(frame.status().code());
      payload.message = frame.status().detail();
      ByteWriter writer;
      encode_error(payload, writer);
      (void)write_frame(session.socket, header, writer.bytes());
      bump(1u, &ServerStats::frames_rejected);
      break;
    }
    bump(1u, &ServerStats::frames_received);
    std::size_t depth = 0;
    {
      std::unique_lock<std::mutex> lock(session.mutex);
      session.not_full.wait(lock, [&] {
        return session.inbox.size() < config.max_queue_depth || stopping_now();
      });
      if (stopping_now()) {
        break;
      }
      session.inbox.push_back(std::move(frame.value()));
      depth = session.inbox.size();
    }
    observe_queue_depth(depth);
    session.not_empty.notify_one();
  }
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    session.reader_done = true;
  }
  session.not_empty.notify_all();
  session.not_full.notify_all();
  if (!stopping_now()) {
    // A retired session releases its peer immediately: the client sees an
    // orderly close instead of silence. The handle itself is closed during
    // teardown, after both threads have exited.
    (void)session.socket.shutdown_both();
  }
}

void Coordinator::Impl::worker_loop(Session& session) {
  while (true) {
    Frame frame;
    std::size_t remaining_depth = 0;
    {
      std::unique_lock<std::mutex> lock(session.mutex);
      session.not_empty.wait(lock, [&] {
        return !session.inbox.empty() || session.reader_done || stopping_now();
      });
      if (session.inbox.empty()) {
        if (session.reader_done || stopping_now()) {
          break;
        }
        continue;
      }
      frame = std::move(session.inbox.front());
      session.inbox.pop_front();
      ++session.in_flight;
      remaining_depth = session.inbox.size();
    }
    session.not_full.notify_one();
    observe_queue_depth(remaining_depth);

    bool request_shutdown = false;
    const Status status = handle_frame(session, frame, request_shutdown);
    {
      std::lock_guard<std::mutex> lock(session.mutex);
      if (session.in_flight > 0) {
        --session.in_flight;
      }
    }
    if (!status.is_ok() || request_shutdown || stopping_now()) {
      if (request_shutdown) {
        request_stop();
      }
      break;
    }
  }
  session.not_empty.notify_all();
  session.not_full.notify_all();
}

Status Coordinator::Impl::handle_frame(Session& session, const Frame& frame, bool& request_shutdown) {
  const std::uint64_t current_epoch = epoch.load();

  const auto reply_header_for = [&](FrameType type) {
    FrameHeader header = frame.header;
    header.type = type;
    header.version = kWireProtocolVersion;
    header.payload_length = 0;
    header.header_crc = 0;
    header.reserved = 0;
    header.flags = 0;
    header.epoch = current_epoch;
    header.session_id = session.id.value();
    return header;
  };

  const auto send_error = [&](ErrorCode code, const std::string& message) {
    FrameHeader header = reply_header_for(FrameType::kError);
    ErrorPayload payload;
    payload.code = static_cast<std::uint16_t>(code);
    payload.request_id = frame.header.request_id;
    payload.message = message;
    ByteWriter writer;
    encode_error(payload, writer);
    return write_frame(session.socket, header, writer.bytes());
  };

  // Replay and reordering handling: sequence numbers advance strictly inside a
  // session. A repeat or a regression is refused deterministically.
  bool replay = false;
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    if (session.has_sequence && frame.header.sequence <= session.last_sequence) {
      replay = true;
    } else {
      session.has_sequence = true;
      session.last_sequence = frame.header.sequence;
    }
  }
  if (replay) {
    bump(1u, &ServerStats::replays_refused);
    return send_error(ErrorCode::kSequenceMismatch, "the frame sequence did not advance in this session");
  }

  switch (frame.header.type) {
    case FrameType::kHello: {
      ByteReader reader(frame.payload);
      HelloPayload payload;
      if (Status status = decode_hello(reader, payload); !status.is_ok()) {
        bump(1u, &ServerStats::handshakes_refused);
        return send_error(status.code(), status.detail());
      }
      if (payload.protocol_version != kWireProtocolVersion) {
        bump(1u, &ServerStats::handshakes_refused);
        return send_error(ErrorCode::kBadProtocolVersion,
                          "the client speaks protocol version " + std::to_string(payload.protocol_version));
      }
      bool already = false;
      {
        std::lock_guard<std::mutex> lock(session.mutex);
        already = session.handshaken;
        session.handshaken = true;
      }
      if (already) {
        bump(1u, &ServerStats::handshakes_refused);
        return send_error(ErrorCode::kSequenceMismatch, "this session already completed its handshake");
      }
      HelloAckPayload ack;
      ack.protocol_version = kWireProtocolVersion;
      ack.session_id = session.id.value();
      ack.epoch = current_epoch;
      ack.incarnation = incarnation.load();
      ack.server_label = config.label;
      ack.max_frame_payload = static_cast<std::uint32_t>(kMaxFramePayloadBytes);
      ack.max_pending_requests = static_cast<std::uint64_t>(config.max_pending_requests);
      ByteWriter writer;
      encode_hello_ack(ack, writer);
      if (Status status = write_frame(session.socket, reply_header_for(FrameType::kHelloAck), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::handshakes_completed);
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    case FrameType::kPing: {
      if (Status status = write_frame(session.socket, reply_header_for(FrameType::kPong), {}); !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    default:
      break;
  }

  bool handshaken = false;
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    handshaken = session.handshaken;
  }
  if (!handshaken) {
    return send_error(ErrorCode::kSessionHandshakeRequired, "send a hello frame before any other request");
  }

  switch (frame.header.type) {
    case FrameType::kPlanRequest: {
      ByteReader reader(frame.payload);
      PlanRequestPayload payload;
      if (Status status = decode_plan_request(reader, payload); !status.is_ok()) {
        return send_error(status.code(), status.detail());
      }
      PlanResponsePayload response;
      response.request_id = payload.request_id;
      auto parsed = parse_request_text(payload.request_text);
      if (!parsed.has_value()) {
        response.code = static_cast<std::uint16_t>(parsed.status().code());
        DenialPayload denial;
        denial.code = static_cast<std::uint16_t>(parsed.status().code());
        denial.kind = static_cast<std::uint8_t>(
            category_of(parsed.status().code()) == ErrorCategory::kAuthority ? DenialKind::kStaleInput
                                                                            : DenialKind::kInvalidRequest);
        denial.message = parsed.status().detail();
        response.denials.push_back(std::move(denial));
        if (payload.include_explanation) {
          response.explanation = "request rejected: " + parsed.status().to_string() + "\n";
        }
        bump(1u, &ServerStats::plan_denials);
      } else {
        const PlanningOutcome outcome = plan_collective(parsed.value());
        response.search_expansions = outcome.search_expansions;
        if (outcome.ok()) {
          const Plan& plan = *outcome.plan;
          response.has_plan = true;
          response.code = static_cast<std::uint16_t>(ErrorCode::kOk);
          std::uint64_t sequence = 0;
          {
            std::lock_guard<std::mutex> lock(state_mutex);
            bump_locked(1u, &ServerStats::store_commits);
            sequence = stats.store_commits;
          }
          response.plan_record =
              encode_plan_record(plan, parsed.value(), Incarnation{incarnation.load()}, sequence,
                                 unix_micros_now());
          if (store != nullptr) {
            auto stored = store->put(plan, parsed.value());
            if (!stored.has_value()) {
              DenialPayload persist_failure;
              persist_failure.code = static_cast<std::uint16_t>(stored.status().code());
              persist_failure.kind = static_cast<std::uint8_t>(DenialKind::kUnsupportedConstraint);
              persist_failure.message =
                  "the plan was produced but could not be persisted: " + stored.status().to_string();
              response.denials.push_back(std::move(persist_failure));
              bump(1u, &ServerStats::errors);
            }
          }
          if (payload.include_explanation) {
            response.explanation = explain_plan(plan, &parsed.value());
          }
          bump(1u, &ServerStats::plans_produced);
        } else {
          response.code = static_cast<std::uint16_t>(outcome.primary_code());
          response.has_plan = false;
          const std::size_t limit = std::min<std::size_t>(outcome.denials.size(), kMaxDenials);
          for (std::size_t index = 0; index < limit; ++index) {
            const DenialDetail& denial = outcome.denials[index];
            DenialPayload entry;
            entry.code = static_cast<std::uint16_t>(denial.code);
            entry.kind = static_cast<std::uint8_t>(denial.kind);
            entry.conflict = static_cast<std::uint8_t>(denial.conflict);
            entry.logical_edge = denial.logical_edge.value();
            entry.participant = denial.participant.value();
            entry.message = denial.message;
            response.denials.push_back(std::move(entry));
          }
          if (payload.include_explanation) {
            response.explanation = explain_denials(outcome);
          }
          bump(1u, &ServerStats::plan_denials);
        }
      }
      ByteWriter writer;
      encode_plan_response(response, writer);
      if (Status status = write_frame(session.socket, reply_header_for(FrameType::kPlanResponse), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    case FrameType::kGetPlanRequest: {
      ByteReader reader(frame.payload);
      GetPlanRequestPayload payload;
      if (Status status = decode_get_plan_request(reader, payload); !status.is_ok()) {
        return send_error(status.code(), status.detail());
      }
      GetPlanResponsePayload response;
      response.request_id = payload.request_id;
      if (store == nullptr) {
        response.code = static_cast<std::uint16_t>(ErrorCode::kStoreNotOpen);
      } else {
        auto plan = store->get(CollectivePlanId{payload.plan_id});
        if (!plan.has_value()) {
          response.code = static_cast<std::uint16_t>(plan.status().code());
        } else {
          response.has_plan = true;
          response.code = static_cast<std::uint16_t>(ErrorCode::kOk);
          const PlanningRequest digest_binding = digest_only_request(plan.value());
          response.plan_record = encode_plan_record(plan.value(), digest_binding, Incarnation{incarnation.load()},
                                                    0u, unix_micros_now());
          // A plan read back from storage is never reported as current.
          response.freshness = static_cast<std::uint8_t>(PlanFreshness::kUnverified);
        }
        bump(1u, &ServerStats::store_reads);
      }
      ByteWriter writer;
      encode_get_plan_response(response, writer);
      if (Status status =
              write_frame(session.socket, reply_header_for(FrameType::kGetPlanResponse), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    case FrameType::kListPlansRequest: {
      ByteReader reader(frame.payload);
      ListPlansRequestPayload payload;
      if (Status status = decode_list_plans_request(reader, payload); !status.is_ok()) {
        return send_error(status.code(), status.detail());
      }
      ListPlansResponsePayload response;
      response.request_id = payload.request_id;
      if (store == nullptr) {
        response.code = static_cast<std::uint16_t>(ErrorCode::kStoreNotOpen);
      } else {
        auto summaries = store->list();
        if (!summaries.has_value()) {
          response.code = static_cast<std::uint16_t>(summaries.status().code());
        } else {
          response.code = static_cast<std::uint16_t>(ErrorCode::kOk);
          for (const StoredPlanSummary& entry : summaries.value()) {
            PlanSummaryPayload item;
            item.plan_id = entry.id.value();
            item.generation = entry.generation.value();
            item.collective = entry.collective.value();
            item.kind = static_cast<std::uint32_t>(entry.kind);
            item.stored_sequence = entry.stored_sequence;
            item.record_bytes = entry.record_bytes;
            item.freshness = static_cast<std::uint8_t>(PlanFreshness::kUnverified);
            response.plans.push_back(std::move(item));
          }
        }
        bump(1u, &ServerStats::store_reads);
      }
      ByteWriter writer;
      encode_list_plans_response(response, writer);
      if (Status status =
              write_frame(session.socket, reply_header_for(FrameType::kListPlansResponse), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    case FrameType::kRevalidateRequest: {
      ByteReader reader(frame.payload);
      RevalidateRequestPayload payload;
      if (Status status = decode_revalidate_request(reader, payload); !status.is_ok()) {
        return send_error(status.code(), status.detail());
      }
      RevalidateResponsePayload response;
      response.request_id = payload.request_id;
      auto parsed = parse_request_text(payload.request_text);
      if (!parsed.has_value()) {
        response.code = static_cast<std::uint16_t>(parsed.status().code());
        response.detail = parsed.status().detail();
      } else if (store == nullptr) {
        response.code = static_cast<std::uint16_t>(ErrorCode::kStoreNotOpen);
      } else {
        auto result = store->revalidate(CollectivePlanId{payload.plan_id}, parsed.value());
        if (!result.has_value()) {
          response.code = static_cast<std::uint16_t>(result.status().code());
          response.detail = result.status().detail();
        } else {
          response.code = static_cast<std::uint16_t>(result.value().code);
          response.still_valid = result.value().still_valid;
          response.freshness = static_cast<std::uint8_t>(result.value().freshness);
          response.current_input_digest = result.value().current_input_digest;
          response.detail = result.value().detail;
          if (result.value().still_valid) {
            bump(1u, &ServerStats::revalidations_current);
          } else {
            bump(1u, &ServerStats::revalidations_stale);
          }
        }
      }
      ByteWriter writer;
      encode_revalidate_response(response, writer);
      if (Status status =
              write_frame(session.socket, reply_header_for(FrameType::kRevalidateResponse), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      return Status::ok();
    }
    case FrameType::kShutdownRequest: {
      ByteReader reader(frame.payload);
      ShutdownRequestPayload payload;
      if (Status status = decode_shutdown_request(reader, payload); !status.is_ok()) {
        return send_error(status.code(), status.detail());
      }
      if (payload.expected_epoch != current_epoch) {
        return send_error(ErrorCode::kStaleEpoch,
                          "shutdown was requested for epoch " + std::to_string(payload.expected_epoch) +
                              " but the coordinator holds epoch " + std::to_string(current_epoch));
      }
      ByteWriter writer;
      encode_shutdown_request(payload, writer);
      if (Status status =
              write_frame(session.socket, reply_header_for(FrameType::kShutdownAck), writer.bytes());
          !status.is_ok()) {
        return status;
      }
      bump(1u, &ServerStats::frames_sent);
      request_shutdown = true;
      return Status::ok();
    }
    default:
      return send_error(ErrorCode::kUnexpectedFrameType, "this frame type is not accepted on a session");
  }
}

void Coordinator::Impl::teardown() {
  // The acceptor is joined first so that no new session can appear while the
  // existing ones are being retired.
  if (acceptor.joinable()) {
    acceptor.join();
  }
  std::vector<Session*> snapshot;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    snapshot.reserve(sessions.size());
    for (const std::unique_ptr<Session>& session : sessions) {
      snapshot.push_back(session.get());
    }
  }
  for (Session* session : snapshot) {
    (void)session->wakeup.signal();
    session->socket.shutdown_both();
  }
  for (Session* session : snapshot) {
    if (session->reader.joinable()) {
      session->reader.join();
    }
  }
  for (Session* session : snapshot) {
    session->not_empty.notify_all();
    session->not_full.notify_all();
  }
  for (Session* session : snapshot) {
    if (session->worker.joinable()) {
      session->worker.join();
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (Session* session : snapshot) {
      session->socket.close();
      session->wakeup.close();
    }
    bump_locked(static_cast<std::uint64_t>(snapshot.size()), &ServerStats::sessions_closed);
    sessions.clear();
  }
  if (store != nullptr) {
    (void)store->close();
    store.reset();
  }
  listener.close();
  acceptor_wakeup.close();
}

void Coordinator::Impl::run_reaper() {
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    state_changed.wait(lock, [this] { return stopping.load(std::memory_order_acquire); });
  }
  teardown();
  finished.store(true, std::memory_order_release);
  state_changed.notify_all();
}

Coordinator::Coordinator(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Coordinator::~Coordinator() { (void)stop(); }

Status Coordinator::start() {
  if (impl_->reaper.joinable()) {
    // A previous run stopped itself (for example through a shutdown request) and
    // was never collected. Collect it before starting again.
    (void)stop();
  }
  if (impl_->running.load()) {
    return Status::error(ErrorCode::kStoreAlreadyOpen, "the coordinator is already running");
  }
  if (Status status = internal::net_initialize(); !status.is_ok()) {
    return status;
  }

  const int backlog = static_cast<int>(std::max<std::size_t>(impl_->config.max_sessions, 8u));
  auto listener = internal::Socket::listen_on(impl_->config.bind_address, impl_->config.port, backlog);
  if (!listener.has_value()) {
    return listener.status();
  }
  impl_->listener = std::move(listener.value());
  if (Status status = impl_->acceptor_wakeup.open(); !status.is_ok()) {
    impl_->listener.close();
    return status;
  }
  impl_->bound_port.store(impl_->listener.local_port());

  impl_->store_config = StoreConfig{};
  impl_->store_config.root = impl_->config.store_root;
  impl_->store_config.max_records = kMaxStoreRecords;
  impl_->store_config.max_record_bytes = kMaxPlanRecordBytes;
  impl_->store_config.max_total_bytes = kMaxStoreTotalBytes;
  impl_->store_config.durable_commit = true;

  if (!impl_->config.store_root.empty()) {
    auto store = std::make_unique<PlanStore>(impl_->store_config);
    if (Status status = store->open(); !status.is_ok()) {
      impl_->listener.close();
      impl_->acceptor_wakeup.close();
      return status;
    }
    impl_->epoch.store(store->epoch().value());
    impl_->incarnation.store(store->incarnation().value());
    impl_->store = std::move(store);
  } else {
    impl_->epoch.store(1u);
    impl_->incarnation.store(1u);
  }

  impl_->stopping.store(false);
  impl_->finished.store(false);
  impl_->running.store(true);
  impl_->acceptor = std::thread([this] { impl_->accept_loop(); });
  impl_->reaper = std::thread([this] { impl_->run_reaper(); });
  impl_->reaper_id.store(impl_->reaper.get_id());
  return Status::ok();
}

Status Coordinator::stop() {
  if (!impl_->running.load() && impl_->finished.load() && !impl_->reaper.joinable()) {
    return Status::ok();
  }
  impl_->request_stop();
  const std::thread::id self = std::this_thread::get_id();
  if (g_is_session_thread || self == impl_->reaper_id.load()) {
    // Called from inside a session, or from the reaper itself: signal and let the
    // reaper finish. Joining here would deadlock.
    return Status::ok();
  }
  if (impl_->reaper.joinable()) {
    impl_->reaper.join();
    impl_->reaper_id.store(std::thread::id{});
  }
  impl_->running.store(false);
  return Status::ok();
}

bool Coordinator::running() const noexcept {
  return impl_->running.load() && !impl_->stopping.load();
}

std::uint16_t Coordinator::port() const noexcept { return impl_->bound_port.load(); }

const std::string& Coordinator::bind_address() const noexcept { return impl_->config.bind_address; }

Epoch Coordinator::epoch() const noexcept { return Epoch{impl_->epoch.load()}; }

Incarnation Coordinator::incarnation() const noexcept { return Incarnation{impl_->incarnation.load()}; }

ServerStats Coordinator::stats() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->stats;
}

const StoreConfig& Coordinator::store_config() const noexcept { return impl_->store_config; }

// ---------------------------------------------------------------------------
// ServiceClient
// ---------------------------------------------------------------------------

struct ServiceClient::Impl {
  internal::Socket socket{};
  // The client serialises its exchanges. The mutex is recursive so that the
  // convenience methods can compose with exchange() without an unlock dance.
  std::recursive_mutex mutex{};
  std::uint64_t next_sequence{1};
  std::uint64_t session_id{0};
  std::uint64_t epoch{0};
  bool connected{false};
};

ServiceClient::ServiceClient() : impl_(std::make_unique<Impl>()) {}

ServiceClient::~ServiceClient() { close(); }

Status ServiceClient::connect(const std::string& host, std::uint16_t port) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->connected) {
    return Status::error(ErrorCode::kSocketFailure, "the client is already connected");
  }
  auto socket = internal::Socket::connect_to(host, port);
  if (!socket.has_value()) {
    return socket.status();
  }
  impl_->socket = std::move(socket.value());
  impl_->next_sequence = 1;
  impl_->session_id = 0;
  impl_->connected = true;
  return Status::ok();
}

void ServiceClient::close() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  impl_->socket.shutdown_both();
  impl_->socket.close();
  impl_->connected = false;
}

bool ServiceClient::connected() const noexcept { return impl_->connected && impl_->socket.is_open(); }

Result<Frame> ServiceClient::exchange(const FrameHeader& header, std::span<const std::uint8_t> payload) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->connected || !impl_->socket.is_open()) {
    return Result<Frame>::failure(ErrorCode::kSocketFailure, "the client is not connected");
  }
  FrameHeader outbound = header;
  outbound.sequence = impl_->next_sequence++;
  outbound.epoch = impl_->epoch;
  outbound.session_id = impl_->session_id;
  if (Status status = write_frame(impl_->socket, outbound, payload); !status.is_ok()) {
    return Result<Frame>::failure(status);
  }
  auto frame = read_frame(impl_->socket);
  if (!frame.has_value()) {
    return frame;
  }
  if (frame.value().header.sequence != outbound.sequence) {
    return Result<Frame>::failure(ErrorCode::kSequenceMismatch,
                                  "the reply sequence does not match the request sequence");
  }
  return frame;
}

Status ServiceClient::send_raw(std::span<const std::uint8_t> bytes) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->connected || !impl_->socket.is_open()) {
    return Status::error(ErrorCode::kSocketFailure, "the client is not connected");
  }
  return internal::send_all(impl_->socket, bytes);
}

Result<Frame> ServiceClient::receive_frame() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->connected || !impl_->socket.is_open()) {
    return Result<Frame>::failure(ErrorCode::kSocketFailure, "the client is not connected");
  }
  return read_frame(impl_->socket);
}

Result<HelloAckPayload> ServiceClient::handshake(std::string_view label) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->connected) {
    return Result<HelloAckPayload>::failure(ErrorCode::kSocketFailure, "the client is not connected");
  }
  HelloPayload hello;
  hello.protocol_version = kWireProtocolVersion;
  hello.client_label.assign(label);
  hello.observed_epoch = impl_->epoch;
  ByteWriter writer;
  encode_hello(hello, writer);

  FrameHeader header;
  header.type = FrameType::kHello;
  header.sequence = impl_->next_sequence++;
  header.epoch = impl_->epoch;
  const std::vector<std::uint8_t> bytes = encode_frame(header, writer.bytes());
  if (Status status = internal::send_all(impl_->socket, bytes); !status.is_ok()) {
    return Result<HelloAckPayload>::failure(status);
  }
  auto frame = read_frame(impl_->socket);
  if (!frame.has_value()) {
    return Result<HelloAckPayload>::failure(frame.status());
  }
  if (frame.value().header.sequence != header.sequence) {
    return Result<HelloAckPayload>::failure(ErrorCode::kSequenceMismatch,
                                            "the handshake reply sequence does not match");
  }
  if (frame.value().header.type == FrameType::kError) {
    ByteReader reader(frame.value().payload);
    ErrorPayload payload_out;
    if (Status status = decode_error(reader, payload_out); status.is_ok()) {
      return Result<HelloAckPayload>::failure(static_cast<ErrorCode>(payload_out.code), payload_out.message);
    }
    return Result<HelloAckPayload>::failure(ErrorCode::kBadProtocolVersion, "the handshake was refused");
  }
  if (frame.value().header.type != FrameType::kHelloAck) {
    return Result<HelloAckPayload>::failure(ErrorCode::kUnexpectedFrameType,
                                            "the coordinator did not answer the handshake");
  }
  ByteReader reader(frame.value().payload);
  HelloAckPayload ack;
  if (Status status = decode_hello_ack(reader, ack); !status.is_ok()) {
    return Result<HelloAckPayload>::failure(status);
  }
  if (ack.protocol_version != kWireProtocolVersion) {
    return Result<HelloAckPayload>::failure(ErrorCode::kBadProtocolVersion,
                                            "the coordinator speaks a different protocol version");
  }
  impl_->session_id = ack.session_id;
  impl_->epoch = ack.epoch;
  return Result<HelloAckPayload>::success(std::move(ack));
}

Status ServiceClient::ping() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  FrameHeader header;
  header.type = FrameType::kPing;
  auto frame = exchange(header, {});
  if (!frame.has_value()) {
    return frame.status();
  }
  if (frame.value().header.type != FrameType::kPong) {
    return Status::error(ErrorCode::kUnexpectedFrameType, "the coordinator did not answer the ping");
  }
  return Status::ok();
}

Result<PlanResponsePayload> ServiceClient::plan(std::string_view request_text, bool include_explanation) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  PlanRequestPayload request;
  request.request_id = 1;
  request.include_explanation = include_explanation;
  request.request_text.assign(request_text);
  ByteWriter writer;
  encode_plan_request(request, writer);
  FrameHeader header;
  header.type = FrameType::kPlanRequest;
  header.request_id = request.request_id;
  auto frame = exchange(header, writer.bytes());
  if (!frame.has_value()) {
    return Result<PlanResponsePayload>::failure(frame.status());
  }
  if (frame.value().header.type != FrameType::kPlanResponse) {
    return Result<PlanResponsePayload>::failure(ErrorCode::kUnexpectedFrameType,
                                                "the coordinator did not answer the plan request");
  }
  ByteReader reader(frame.value().payload);
  PlanResponsePayload response;
  if (Status status = decode_plan_response(reader, response); !status.is_ok()) {
    return Result<PlanResponsePayload>::failure(status);
  }
  return Result<PlanResponsePayload>::success(std::move(response));
}

Result<ListPlansResponsePayload> ServiceClient::list_plans() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  ListPlansRequestPayload request;
  request.request_id = 1;
  ByteWriter writer;
  encode_list_plans_request(request, writer);
  FrameHeader header;
  header.type = FrameType::kListPlansRequest;
  header.request_id = request.request_id;
  auto frame = exchange(header, writer.bytes());
  if (!frame.has_value()) {
    return Result<ListPlansResponsePayload>::failure(frame.status());
  }
  if (frame.value().header.type != FrameType::kListPlansResponse) {
    return Result<ListPlansResponsePayload>::failure(ErrorCode::kUnexpectedFrameType,
                                                     "the coordinator did not answer the list request");
  }
  ByteReader reader(frame.value().payload);
  ListPlansResponsePayload response;
  if (Status status = decode_list_plans_response(reader, response); !status.is_ok()) {
    return Result<ListPlansResponsePayload>::failure(status);
  }
  return Result<ListPlansResponsePayload>::success(std::move(response));
}

Result<GetPlanResponsePayload> ServiceClient::get_plan(const Digest& plan_id) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  GetPlanRequestPayload request;
  request.request_id = 1;
  request.plan_id = plan_id;
  ByteWriter writer;
  encode_get_plan_request(request, writer);
  FrameHeader header;
  header.type = FrameType::kGetPlanRequest;
  header.request_id = request.request_id;
  auto frame = exchange(header, writer.bytes());
  if (!frame.has_value()) {
    return Result<GetPlanResponsePayload>::failure(frame.status());
  }
  if (frame.value().header.type != FrameType::kGetPlanResponse) {
    return Result<GetPlanResponsePayload>::failure(ErrorCode::kUnexpectedFrameType,
                                                   "the coordinator did not answer the get request");
  }
  ByteReader reader(frame.value().payload);
  GetPlanResponsePayload response;
  if (Status status = decode_get_plan_response(reader, response); !status.is_ok()) {
    return Result<GetPlanResponsePayload>::failure(status);
  }
  return Result<GetPlanResponsePayload>::success(std::move(response));
}

Result<RevalidateResponsePayload> ServiceClient::revalidate(const Digest& plan_id,
                                                            std::string_view request_text) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  RevalidateRequestPayload request;
  request.request_id = 1;
  request.plan_id = plan_id;
  request.request_text.assign(request_text);
  ByteWriter writer;
  encode_revalidate_request(request, writer);
  FrameHeader header;
  header.type = FrameType::kRevalidateRequest;
  header.request_id = request.request_id;
  auto frame = exchange(header, writer.bytes());
  if (!frame.has_value()) {
    return Result<RevalidateResponsePayload>::failure(frame.status());
  }
  if (frame.value().header.type != FrameType::kRevalidateResponse) {
    return Result<RevalidateResponsePayload>::failure(ErrorCode::kUnexpectedFrameType,
                                                      "the coordinator did not answer the revalidate request");
  }
  ByteReader reader(frame.value().payload);
  RevalidateResponsePayload response;
  if (Status status = decode_revalidate_response(reader, response); !status.is_ok()) {
    return Result<RevalidateResponsePayload>::failure(status);
  }
  return Result<RevalidateResponsePayload>::success(std::move(response));
}

Result<ShutdownRequestPayload> ServiceClient::shutdown(std::uint64_t expected_epoch) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  ShutdownRequestPayload request;
  request.request_id = 1;
  request.expected_epoch = expected_epoch;
  ByteWriter writer;
  encode_shutdown_request(request, writer);
  FrameHeader header;
  header.type = FrameType::kShutdownRequest;
  header.request_id = request.request_id;
  auto frame = exchange(header, writer.bytes());
  if (!frame.has_value()) {
    return Result<ShutdownRequestPayload>::failure(frame.status());
  }
  if (frame.value().header.type != FrameType::kShutdownAck) {
    return Result<ShutdownRequestPayload>::failure(ErrorCode::kUnexpectedFrameType,
                                                   "the coordinator did not acknowledge the shutdown");
  }
  return Result<ShutdownRequestPayload>::success(request);
}

Result<std::uint16_t> parse_endpoint(std::string_view text, std::string& host_out) {
  const std::size_t separator = text.rfind(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1u >= text.size()) {
    return Result<std::uint16_t>::failure(ErrorCode::kInvalidSyntax, "endpoint must be HOST:PORT");
  }
  host_out.assign(text.substr(0, separator));
  if (host_out.empty() || host_out.size() > 255u) {
    return Result<std::uint16_t>::failure(ErrorCode::kStringTooLong, "endpoint host is empty or too long");
  }
  const std::string_view port_text = text.substr(separator + 1u);
  std::uint32_t port = 0;
  for (const char ch : port_text) {
    if (ch < '0' || ch > '9') {
      return Result<std::uint16_t>::failure(ErrorCode::kMalformedNumber,
                                            "endpoint port must be a decimal number");
    }
    port = port * 10u + static_cast<std::uint32_t>(ch - '0');
    if (port > 65535u) {
      return Result<std::uint16_t>::failure(ErrorCode::kValueOutOfRange, "endpoint port is out of range");
    }
  }
  if (port == 0) {
    return Result<std::uint16_t>::failure(ErrorCode::kValueOutOfRange, "endpoint port must not be zero");
  }
  return Result<std::uint16_t>::success(static_cast<std::uint16_t>(port));
}

}  // namespace cpath
