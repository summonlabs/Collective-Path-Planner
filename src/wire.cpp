// Collective Path Planner - framed, versioned, integrity-checked transport.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/digest.hpp"
#include "cpath/limits.hpp"
#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/status.hpp"

namespace cpath {
namespace {

// The canonical header fields up to payload_length occupy the first 32 bytes.
// One 32-bit word follows them, reserved for a future protocol revision, which
// places header_crc at byte offset 36: the checksum covers the 36 bytes that
// precede it, and the trailing 20 bytes (CRC, session id, request id, reserved)
// complete the 56-byte header.
constexpr std::size_t kHeaderCrcCoverageBytes = 36;

// Bounds for the length-prefixed and counted payload fields. Denials are
// bounded by the planner's kMaxDenials and plan summaries by kMaxStoreRecords.
constexpr std::size_t kMaxLabelBytes = 128;
constexpr std::uint8_t kMaxFreshness = static_cast<std::uint8_t>(PlanFreshness::kUnverified);
constexpr std::uint8_t kMaxConflict = static_cast<std::uint8_t>(ConflictKind::kStructure);

// ErrorCode bands are dense inside each documented range, so membership is a
// set of range tests rather than a table.
bool is_known_error_code(std::uint32_t raw) noexcept {
  return raw <= 17u || (raw >= 100u && raw <= 111u) || (raw >= 200u && raw <= 220u) ||
         (raw >= 300u && raw <= 312u) || (raw >= 400u && raw <= 418u) || (raw >= 900u && raw <= 904u);
}

Status unknown_enum(const char* field, std::uint32_t value) {
  return Status::error(ErrorCode::kInvalidSyntax,
                       std::string(field) + " value " + std::to_string(value) +
                           " is not a known enumerator");
}

Status read_error_code(ByteReader& reader, std::uint16_t& out) {
  std::uint16_t value = 0;
  if (Status status = reader.u16(value); !status.is_ok()) {
    return status;
  }
  if (!is_known_error_code(value)) {
    return unknown_enum("error code", value);
  }
  out = value;
  return Status::ok();
}

Status read_freshness(ByteReader& reader, std::uint8_t& out) {
  std::uint8_t value = 0;
  if (Status status = reader.u8(value); !status.is_ok()) {
    return status;
  }
  if (value > kMaxFreshness) {
    return unknown_enum("freshness", value);
  }
  out = value;
  return Status::ok();
}

Status read_conflict(ByteReader& reader, std::uint8_t& out) {
  std::uint8_t value = 0;
  if (Status status = reader.u8(value); !status.is_ok()) {
    return status;
  }
  if (value > kMaxConflict) {
    return unknown_enum("conflict kind", value);
  }
  out = value;
  return Status::ok();
}

// Byte blobs travel as a u32 length followed by that many bytes, exactly like
// strings, with their own explicit ceiling.
void write_bytes(ByteWriter& writer, const std::vector<std::uint8_t>& value) {
  writer.u32(static_cast<std::uint32_t>(value.size()));
  writer.raw(value.data(), value.size());
}

Status read_bytes(ByteReader& reader, std::vector<std::uint8_t>& out, std::size_t max_bytes) {
  std::uint32_t length = 0;
  if (Status status = reader.u32(length); !status.is_ok()) {
    return status;
  }
  const std::size_t size = static_cast<std::size_t>(length);
  if (size > max_bytes) {
    return Status::error(ErrorCode::kStringTooLong,
                         "encoded byte string length " + std::to_string(size) + " exceeds limit " +
                             std::to_string(max_bytes));
  }
  std::span<const std::uint8_t> view;
  if (Status status = reader.raw(size, view); !status.is_ok()) {
    return status;
  }
  out.assign(view.begin(), view.end());
  return Status::ok();
}

void encode_denial(const DenialPayload& value, ByteWriter& writer) {
  writer.u16(value.code);
  writer.u8(value.conflict);
  writer.u64(value.logical_edge);
  writer.string(value.participant);
  writer.string(value.message);
}

Status decode_denial(ByteReader& reader, DenialPayload& out) {
  DenialPayload value;
  if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
    return status;
  }
  if (Status status = read_conflict(reader, value.conflict); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.u64(value.logical_edge); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.string(value.participant, kMaxLabelBytes); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.string(value.message, kMaxDetailBytes); !status.is_ok()) {
    return status;
  }
  out = std::move(value);
  return Status::ok();
}

void encode_plan_summary(const PlanSummaryPayload& value, ByteWriter& writer) {
  writer.digest(value.plan_id);
  writer.u64(value.generation);
  writer.string(value.collective);
  writer.u32(value.kind);
  writer.u64(value.stored_sequence);
  writer.u64(value.record_bytes);
  writer.u8(value.freshness);
}

Status decode_plan_summary(ByteReader& reader, PlanSummaryPayload& out) {
  PlanSummaryPayload value;
  if (Status status = reader.digest(value.plan_id); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.u64(value.generation); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.string(value.collective, kMaxLabelBytes); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.u32(value.kind); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.u64(value.stored_sequence); !status.is_ok()) {
    return status;
  }
  if (Status status = reader.u64(value.record_bytes); !status.is_ok()) {
    return status;
  }
  if (Status status = read_freshness(reader, value.freshness); !status.is_ok()) {
    return status;
  }
  out = std::move(value);
  return Status::ok();
}

}  // namespace

const char* to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::kHello:
      return "hello";
    case FrameType::kHelloAck:
      return "hello_ack";
    case FrameType::kPlanRequest:
      return "plan_request";
    case FrameType::kPlanResponse:
      return "plan_response";
    case FrameType::kGetPlanRequest:
      return "get_plan_request";
    case FrameType::kGetPlanResponse:
      return "get_plan_response";
    case FrameType::kStorePutRequest:
      return "store_put_request";
    case FrameType::kStorePutResponse:
      return "store_put_response";
    case FrameType::kListPlansRequest:
      return "list_plans_request";
    case FrameType::kListPlansResponse:
      return "list_plans_response";
    case FrameType::kRevalidateRequest:
      return "revalidate_request";
    case FrameType::kRevalidateResponse:
      return "revalidate_response";
    case FrameType::kPing:
      return "ping";
    case FrameType::kPong:
      return "pong";
    case FrameType::kError:
      return "error";
    case FrameType::kShutdownRequest:
      return "shutdown_request";
    case FrameType::kShutdownAck:
      return "shutdown_ack";
    case FrameType::kBye:
      return "bye";
  }
  return "unknown";
}

bool is_known_frame_type(std::uint16_t raw) noexcept {
  switch (static_cast<FrameType>(raw)) {
    case FrameType::kHello:
    case FrameType::kHelloAck:
    case FrameType::kPlanRequest:
    case FrameType::kPlanResponse:
    case FrameType::kGetPlanRequest:
    case FrameType::kGetPlanResponse:
    case FrameType::kStorePutRequest:
    case FrameType::kStorePutResponse:
    case FrameType::kListPlansRequest:
    case FrameType::kListPlansResponse:
    case FrameType::kRevalidateRequest:
    case FrameType::kRevalidateResponse:
    case FrameType::kPing:
    case FrameType::kPong:
    case FrameType::kError:
    case FrameType::kShutdownRequest:
    case FrameType::kShutdownAck:
    case FrameType::kBye:
      return true;
  }
  return false;
}

std::vector<std::uint8_t> encode_frame(const FrameHeader& header, std::span<const std::uint8_t> payload) {
  // An oversized payload fails closed: truncating it would emit a well-formed
  // frame carrying something other than what the caller supplied. An empty
  // vector is never a valid frame, so callers cannot mistake it for one.
  if (payload.size() > kMaxFramePayloadBytes) {
    return std::vector<std::uint8_t>{};
  }
  const std::size_t payload_bytes = payload.size();
  try {
    FrameHeader canonical = header;
    canonical.magic = kFrameMagic;
    canonical.version = kWireProtocolVersion;
    canonical.payload_length = static_cast<std::uint32_t>(payload_bytes);
    canonical.reserved = 0;  // the format requires zero; encoding enforces it

    ByteWriter writer;
    writer.u32(canonical.magic);
    writer.u16(canonical.version);
    writer.u16(static_cast<std::uint16_t>(canonical.type));
    writer.u32(canonical.flags);
    writer.u64(canonical.sequence);
    writer.u64(canonical.epoch);
    writer.u32(canonical.payload_length);
    writer.u32(0u);  // word reserved for a future protocol revision
    canonical.header_crc = crc32(writer.bytes().data(), writer.size());
    writer.u32(canonical.header_crc);
    writer.u64(canonical.session_id);
    writer.u32(canonical.request_id);
    writer.u32(canonical.reserved);

    std::vector<std::uint8_t> frame;
    frame.reserve(kFrameHeaderBytes + payload_bytes + kFrameTrailerBytes);
    frame.insert(frame.end(), writer.bytes().begin(), writer.bytes().end());
    const std::span<const std::uint8_t> body = payload.first(payload_bytes);
    frame.insert(frame.end(), body.begin(), body.end());
    const Digest trailer = Sha256::hash(frame.data(), frame.size());
    frame.insert(frame.end(), trailer.bytes.begin(), trailer.bytes.end());
    return frame;
  } catch (...) {
    return std::vector<std::uint8_t>{};
  }
}

Result<FrameHeader> decode_frame_header(std::span<const std::uint8_t> bytes, std::size_t& consumed) {
  consumed = 0;
  if (bytes.size() < kFrameHeaderBytes) {
    return Result<FrameHeader>::failure(ErrorCode::kTruncatedRecord,
                                        "frame header needs " + std::to_string(kFrameHeaderBytes) +
                                            " bytes but only " + std::to_string(bytes.size()) +
                                            " are available");
  }
  ByteReader reader(bytes.first(kFrameHeaderBytes));
  FrameHeader header;
  std::uint16_t raw_type = 0;
  std::uint32_t future_word = 0;
  if (Status status = reader.u32(header.magic); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u16(header.version); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u16(raw_type); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u32(header.flags); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u64(header.sequence); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u64(header.epoch); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u32(header.payload_length); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u32(future_word); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  static_cast<void>(future_word);  // reserved for a future protocol revision
  if (Status status = reader.u32(header.header_crc); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u64(header.session_id); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u32(header.request_id); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.u32(header.reserved); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (Status status = reader.require_end(); !status.is_ok()) {
    return Result<FrameHeader>::failure(status);
  }
  if (header.magic != kFrameMagic) {
    return Result<FrameHeader>::failure(ErrorCode::kBadMagic,
                                        "frame magic " + std::to_string(header.magic) +
                                            " does not match the expected magic");
  }
  if (header.version != kWireProtocolVersion) {
    return Result<FrameHeader>::failure(ErrorCode::kBadProtocolVersion,
                                        "frame protocol version " + std::to_string(header.version) +
                                            " is not supported");
  }
  if (!is_known_frame_type(raw_type)) {
    return Result<FrameHeader>::failure(ErrorCode::kUnexpectedFrameType,
                                        "frame type " + std::to_string(raw_type) + " is not known");
  }
  header.type = static_cast<FrameType>(raw_type);
  if (header.reserved != 0u) {
    return Result<FrameHeader>::failure(ErrorCode::kInvalidSyntax,
                                        "reserved header word must be zero");
  }
  const std::uint32_t computed_crc = crc32(bytes.data(), kHeaderCrcCoverageBytes);
  if (computed_crc != header.header_crc) {
    return Result<FrameHeader>::failure(ErrorCode::kBadHeaderChecksum,
                                        "header checksum " + std::to_string(header.header_crc) +
                                            " does not match " + std::to_string(computed_crc));
  }
  if (static_cast<std::size_t>(header.payload_length) > kMaxFramePayloadBytes) {
    return Result<FrameHeader>::failure(ErrorCode::kFrameTooLarge,
                                        "payload length " + std::to_string(header.payload_length) +
                                            " exceeds the limit " +
                                            std::to_string(kMaxFramePayloadBytes));
  }
  consumed = kFrameHeaderBytes;
  return header;
}

Result<Frame> decode_frame(std::span<const std::uint8_t> bytes) {
  std::size_t consumed = 0;
  Result<FrameHeader> header = decode_frame_header(bytes, consumed);
  if (!header.has_value()) {
    return Result<Frame>::failure(header.status());
  }
  const std::size_t payload_bytes = static_cast<std::size_t>(header.value().payload_length);
  const std::size_t body_bytes = kFrameHeaderBytes + payload_bytes;
  const std::size_t total_bytes = body_bytes + kFrameTrailerBytes;
  if (bytes.size() < total_bytes) {
    return Result<Frame>::failure(ErrorCode::kTruncatedRecord,
                                  "frame needs " + std::to_string(total_bytes) + " bytes but only " +
                                      std::to_string(bytes.size()) + " are available");
  }
  if (bytes.size() > total_bytes) {
    return Result<Frame>::failure(ErrorCode::kTrailingGarbage,
                                  std::to_string(bytes.size() - total_bytes) +
                                      " bytes follow the frame");
  }
  const Digest expected = Sha256::hash(bytes.data(), body_bytes);
  const std::span<const std::uint8_t> trailer = bytes.subspan(body_bytes, kFrameTrailerBytes);
  if (std::memcmp(trailer.data(), expected.bytes.data(), kFrameTrailerBytes) != 0) {
    return Result<Frame>::failure(ErrorCode::kBadPayloadDigest, "payload digest does not match");
  }
  try {
    Frame frame;
    frame.header = header.value();
    const std::span<const std::uint8_t> body = bytes.subspan(kFrameHeaderBytes, payload_bytes);
    frame.payload.assign(body.begin(), body.end());
    return frame;
  } catch (...) {
    return Result<Frame>::failure(ErrorCode::kInternalError, "allocation failed while decoding a frame");
  }
}

// --- Typed payloads --------------------------------------------------------
//
// Every encoder writes fields in the order its decoder reads them, and every
// decoder ends with require_end(), so a round trip is byte-identical. Payload
// encoders cannot report failure, so an allocation failure inside them is
// absorbed: the matching decoder still rejects the result, because every
// variable-length field carries its own length or count and any partial payload
// either runs out of bytes or leaves trailing garbage.

void encode_hello(const HelloPayload& value, ByteWriter& writer) {
  try {
    writer.u16(value.protocol_version);
    writer.string(value.client_label);
    writer.u64(value.observed_epoch);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_hello(ByteReader& reader, HelloPayload& out) {
  try {
    HelloPayload value;
    if (Status status = reader.u16(value.protocol_version); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.client_label, kMaxLabelBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.observed_epoch); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError, "allocation failed while decoding a hello payload");
  }
}

void encode_hello_ack(const HelloAckPayload& value, ByteWriter& writer) {
  try {
    writer.u16(value.protocol_version);
    writer.u64(value.session_id);
    writer.u64(value.epoch);
    writer.u64(value.incarnation);
    writer.string(value.server_label);
    writer.u32(value.max_frame_payload);
    writer.u64(value.max_pending_requests);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_hello_ack(ByteReader& reader, HelloAckPayload& out) {
  try {
    HelloAckPayload value;
    if (Status status = reader.u16(value.protocol_version); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.session_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.epoch); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.incarnation); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.server_label, kMaxLabelBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u32(value.max_frame_payload); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.max_pending_requests); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a hello acknowledgement payload");
  }
}

void encode_error(const ErrorPayload& value, ByteWriter& writer) {
  try {
    writer.u16(value.code);
    writer.u32(value.request_id);
    writer.string(value.message);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_error(ByteReader& reader, ErrorPayload& out) {
  try {
    ErrorPayload value;
    if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.message, kMaxDetailBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError, "allocation failed while decoding an error payload");
  }
}

void encode_plan_request(const PlanRequestPayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.boolean(value.include_explanation);
    writer.string(value.request_text);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_plan_request(ByteReader& reader, PlanRequestPayload& out) {
  try {
    PlanRequestPayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.boolean(value.include_explanation); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.request_text, kMaxFramePayloadBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a plan request payload");
  }
}

void encode_plan_response(const PlanResponsePayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.u16(value.code);
    writer.boolean(value.has_plan);
    write_bytes(writer, value.plan_record);
    writer.u32(static_cast<std::uint32_t>(value.denials.size()));
    for (const DenialPayload& denial : value.denials) {
      encode_denial(denial, writer);
    }
    writer.string(value.explanation);
    writer.u64(value.search_expansions);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_plan_response(ByteReader& reader, PlanResponsePayload& out) {
  try {
    PlanResponsePayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.boolean(value.has_plan); !status.is_ok()) {
      return status;
    }
    if (Status status = read_bytes(reader, value.plan_record, kMaxPlanRecordBytes); !status.is_ok()) {
      return status;
    }
    std::uint32_t denial_count = 0;
    if (Status status = reader.count(denial_count, kMaxDenials); !status.is_ok()) {
      return status;
    }
    value.denials.reserve(denial_count);
    for (std::uint32_t index = 0; index < denial_count; ++index) {
      DenialPayload denial;
      if (Status status = decode_denial(reader, denial); !status.is_ok()) {
        return status;
      }
      value.denials.push_back(std::move(denial));
    }
    // An explanation is a rendered plan, so it is bounded by the frame payload
    // rather than by the short-message bound: the coordinator legitimately sends
    // several kilobytes here, and the frame length already caps it.
    if (Status status = reader.string(value.explanation, kMaxFramePayloadBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.search_expansions); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a plan response payload");
  }
}

void encode_get_plan_request(const GetPlanRequestPayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.digest(value.plan_id);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_get_plan_request(ByteReader& reader, GetPlanRequestPayload& out) {
  try {
    GetPlanRequestPayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.digest(value.plan_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a get-plan request payload");
  }
}

void encode_get_plan_response(const GetPlanResponsePayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.u16(value.code);
    writer.boolean(value.has_plan);
    write_bytes(writer, value.plan_record);
    writer.u8(value.freshness);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_get_plan_response(ByteReader& reader, GetPlanResponsePayload& out) {
  try {
    GetPlanResponsePayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.boolean(value.has_plan); !status.is_ok()) {
      return status;
    }
    if (Status status = read_bytes(reader, value.plan_record, kMaxPlanRecordBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = read_freshness(reader, value.freshness); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a get-plan response payload");
  }
}

void encode_list_plans_request(const ListPlansRequestPayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_list_plans_request(ByteReader& reader, ListPlansRequestPayload& out) {
  try {
    ListPlansRequestPayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a list-plans request payload");
  }
}

void encode_list_plans_response(const ListPlansResponsePayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.u16(value.code);
    writer.u32(static_cast<std::uint32_t>(value.plans.size()));
    for (const PlanSummaryPayload& summary : value.plans) {
      encode_plan_summary(summary, writer);
    }
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_list_plans_response(ByteReader& reader, ListPlansResponsePayload& out) {
  try {
    ListPlansResponsePayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
      return status;
    }
    std::uint32_t plan_count = 0;
    if (Status status = reader.count(plan_count, kMaxStoreRecords); !status.is_ok()) {
      return status;
    }
    value.plans.reserve(plan_count);
    for (std::uint32_t index = 0; index < plan_count; ++index) {
      PlanSummaryPayload summary;
      if (Status status = decode_plan_summary(reader, summary); !status.is_ok()) {
        return status;
      }
      value.plans.push_back(std::move(summary));
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a list-plans response payload");
  }
}

void encode_revalidate_request(const RevalidateRequestPayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.digest(value.plan_id);
    writer.string(value.request_text);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_revalidate_request(ByteReader& reader, RevalidateRequestPayload& out) {
  try {
    RevalidateRequestPayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.digest(value.plan_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.request_text, kMaxFramePayloadBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a revalidate request payload");
  }
}

void encode_revalidate_response(const RevalidateResponsePayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.u16(value.code);
    writer.boolean(value.still_valid);
    writer.u8(value.freshness);
    writer.digest(value.current_input_digest);
    writer.string(value.detail);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_revalidate_response(ByteReader& reader, RevalidateResponsePayload& out) {
  try {
    RevalidateResponsePayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = read_error_code(reader, value.code); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.boolean(value.still_valid); !status.is_ok()) {
      return status;
    }
    if (Status status = read_freshness(reader, value.freshness); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.digest(value.current_input_digest); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.string(value.detail, kMaxDetailBytes); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a revalidate response payload");
  }
}

void encode_shutdown_request(const ShutdownRequestPayload& value, ByteWriter& writer) {
  try {
    writer.u32(value.request_id);
    writer.u64(value.expected_epoch);
  } catch (...) {
    // See the note above: a partial payload is always rejected by the decoder.
  }
}

Status decode_shutdown_request(ByteReader& reader, ShutdownRequestPayload& out) {
  try {
    ShutdownRequestPayload value;
    if (Status status = reader.u32(value.request_id); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.u64(value.expected_epoch); !status.is_ok()) {
      return status;
    }
    if (Status status = reader.require_end(); !status.is_ok()) {
      return status;
    }
    out = std::move(value);
    return Status::ok();
  } catch (...) {
    return Status::error(ErrorCode::kInternalError,
                         "allocation failed while decoding a shutdown request payload");
  }
}

}  // namespace cpath
