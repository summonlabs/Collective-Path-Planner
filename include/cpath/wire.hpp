// Collective Path Planner - framed, versioned, integrity-checked transport.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_WIRE_HPP
#define CPATH_WIRE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/limits.hpp"
#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/request.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Frame magic: 'C' 'P' 'P' '1' read as a little-endian u32.
//
// A frame is a 56-byte header, then the payload, then a 32-byte SHA-256 over
// (header || payload). The header layout, in order, is:
//
//   0  u32 magic            4  u16 version        6  u16 type
//   8  u32 flags           12  u64 sequence      20  u64 epoch
//  28  u32 payload_length  32  u32 alignment word (must be zero)
//  36  u32 header_crc      40  u64 session_id    48  u32 request_id
//  52  u32 reserved (must be zero)
//
// The alignment word pads the header to 56 bytes so that the payload begins on
// an eight-byte boundary; it is covered by header_crc together with everything
// before it. The trailing reserved word is covered by the payload digest.
inline constexpr std::uint32_t kFrameMagic = 0x31505043u;
inline constexpr std::size_t kFrameHeaderBytes = 56;
inline constexpr std::size_t kFrameTrailerBytes = 32;

enum class FrameType : std::uint16_t {
  kHello = 1,
  kHelloAck = 2,
  kPlanRequest = 3,
  kPlanResponse = 4,
  kGetPlanRequest = 5,
  kGetPlanResponse = 6,
  kStorePutRequest = 7,
  kStorePutResponse = 8,
  kListPlansRequest = 9,
  kListPlansResponse = 10,
  kRevalidateRequest = 11,
  kRevalidateResponse = 12,
  kPing = 13,
  kPong = 14,
  kError = 15,
  kShutdownRequest = 16,
  kShutdownAck = 17,
  kBye = 18,
};

const char* to_string(FrameType type) noexcept;
bool is_known_frame_type(std::uint16_t raw) noexcept;

struct FrameHeader {
  std::uint32_t magic{kFrameMagic};
  std::uint16_t version{kWireProtocolVersion};
  FrameType type{FrameType::kPing};
  std::uint32_t flags{0};
  std::uint64_t sequence{0};
  std::uint64_t epoch{0};
  std::uint32_t payload_length{0};
  std::uint32_t header_crc{0};
  std::uint64_t session_id{0};
  std::uint32_t request_id{0};
  std::uint32_t reserved{0};
};

// A decoded frame. The payload digest has already been verified.
struct Frame {
  FrameHeader header{};
  std::vector<std::uint8_t> payload{};
};

// Pure codec, safe to fuzz. encode_frame produces a well-formed frame, forces
// the magic, protocol version and reserved words, recomputes the header
// checksum and appends the trailer digest.
//
// It returns an EMPTY vector when the payload exceeds kMaxFramePayloadBytes or
// when allocation fails. An empty vector is never a valid frame, and the codec
// never truncates a payload: a caller that asked for bytes it cannot carry gets
// nothing rather than a frame carrying something else.
std::vector<std::uint8_t> encode_frame(const FrameHeader& header, std::span<const std::uint8_t> payload);

// Strict decode of exactly one frame occupying the whole input. Rejects bad
// magic, unknown version, unknown frame type, bad header checksum, oversized
// payload, truncated input, trailing bytes, and payload digest mismatch.
Result<Frame> decode_frame(std::span<const std::uint8_t> bytes);

// Decodes a frame header from the first kFrameHeaderBytes bytes of a stream, so
// a reader can learn the payload length before allocating. The consumed
// out-parameter receives the number of bytes taken from the input (0 when more
// header bytes are needed).
Result<FrameHeader> decode_frame_header(std::span<const std::uint8_t> bytes, std::size_t& consumed);

// --- Typed payloads --------------------------------------------------------

struct HelloPayload {
  std::uint16_t protocol_version{kWireProtocolVersion};
  std::string client_label{};
  std::uint64_t observed_epoch{0};
};

struct HelloAckPayload {
  std::uint16_t protocol_version{kWireProtocolVersion};
  std::uint64_t session_id{0};
  std::uint64_t epoch{0};
  std::uint64_t incarnation{0};
  std::string server_label{};
  std::uint32_t max_frame_payload{static_cast<std::uint32_t>(kMaxFramePayloadBytes)};
  std::uint64_t max_pending_requests{static_cast<std::uint64_t>(kMaxPendingRequestsPerSession)};
};

struct ErrorPayload {
  std::uint16_t code{0};
  std::uint32_t request_id{0};
  std::string message{};
};

struct PlanRequestPayload {
  std::uint32_t request_id{0};
  bool include_explanation{false};
  // Canonical text encoding of the request (cpath DSL).
  std::string request_text{};
};

struct DenialPayload {
  std::uint16_t code{0};
  // DenialKind: whether this denial is a proof that no mapping exists, an
  // invalid request, or an INDETERMINATE search-limit result. A consumer must
  // not have to re-derive that distinction from the code.
  std::uint8_t kind{0};
  std::uint8_t conflict{0};
  std::uint64_t logical_edge{0};
  std::string participant{};
  std::string message{};
};

struct PlanResponsePayload {
  std::uint32_t request_id{0};
  std::uint16_t code{0};  // kOk when a plan is present
  bool has_plan{false};
  std::vector<std::uint8_t> plan_record{};
  std::vector<DenialPayload> denials{};
  std::string explanation{};
  std::uint64_t search_expansions{0};
};

struct GetPlanRequestPayload {
  std::uint32_t request_id{0};
  Digest plan_id{};
};

struct GetPlanResponsePayload {
  std::uint32_t request_id{0};
  std::uint16_t code{0};
  bool has_plan{false};
  std::vector<std::uint8_t> plan_record{};
  std::uint8_t freshness{0};
};

struct ListPlansRequestPayload {
  std::uint32_t request_id{0};
};

struct PlanSummaryPayload {
  Digest plan_id{};
  std::uint64_t generation{0};
  std::string collective{};
  std::uint32_t kind{0};
  std::uint64_t stored_sequence{0};
  std::uint64_t record_bytes{0};
  std::uint8_t freshness{0};
};

struct ListPlansResponsePayload {
  std::uint32_t request_id{0};
  std::uint16_t code{0};
  std::vector<PlanSummaryPayload> plans{};
};

struct RevalidateRequestPayload {
  std::uint32_t request_id{0};
  Digest plan_id{};
  std::string request_text{};
};

struct RevalidateResponsePayload {
  std::uint32_t request_id{0};
  std::uint16_t code{0};
  bool still_valid{false};
  std::uint8_t freshness{0};
  Digest current_input_digest{};
  std::string detail{};
};

struct ShutdownRequestPayload {
  std::uint32_t request_id{0};
  std::uint64_t expected_epoch{0};
};

// Payload codecs. Every decoder is bounded and sticky-error.
void encode_hello(const HelloPayload& value, ByteWriter& writer);
Status decode_hello(ByteReader& reader, HelloPayload& out);
void encode_hello_ack(const HelloAckPayload& value, ByteWriter& writer);
Status decode_hello_ack(ByteReader& reader, HelloAckPayload& out);
void encode_error(const ErrorPayload& value, ByteWriter& writer);
Status decode_error(ByteReader& reader, ErrorPayload& out);
void encode_plan_request(const PlanRequestPayload& value, ByteWriter& writer);
Status decode_plan_request(ByteReader& reader, PlanRequestPayload& out);
void encode_plan_response(const PlanResponsePayload& value, ByteWriter& writer);
Status decode_plan_response(ByteReader& reader, PlanResponsePayload& out);
void encode_get_plan_request(const GetPlanRequestPayload& value, ByteWriter& writer);
Status decode_get_plan_request(ByteReader& reader, GetPlanRequestPayload& out);
void encode_get_plan_response(const GetPlanResponsePayload& value, ByteWriter& writer);
Status decode_get_plan_response(ByteReader& reader, GetPlanResponsePayload& out);
void encode_list_plans_request(const ListPlansRequestPayload& value, ByteWriter& writer);
Status decode_list_plans_request(ByteReader& reader, ListPlansRequestPayload& out);
void encode_list_plans_response(const ListPlansResponsePayload& value, ByteWriter& writer);
Status decode_list_plans_response(ByteReader& reader, ListPlansResponsePayload& out);
void encode_revalidate_request(const RevalidateRequestPayload& value, ByteWriter& writer);
Status decode_revalidate_request(ByteReader& reader, RevalidateRequestPayload& out);
void encode_revalidate_response(const RevalidateResponsePayload& value, ByteWriter& writer);
Status decode_revalidate_response(ByteReader& reader, RevalidateResponsePayload& out);
void encode_shutdown_request(const ShutdownRequestPayload& value, ByteWriter& writer);
Status decode_shutdown_request(ByteReader& reader, ShutdownRequestPayload& out);

}  // namespace cpath

#endif  // CPATH_WIRE_HPP
