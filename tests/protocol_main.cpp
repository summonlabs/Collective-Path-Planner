// Collective Path Planner - wire codec and live transport proofs.
//
// The fabrics used here are SYNTHETIC: every planning request comes from the
// hand-written fixtures in tests/support/fixtures.hpp and no measurement from a
// real device is involved.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cpath/dsl.hpp"
#include "cpath/limits.hpp"
#include "cpath/persistence.hpp"
#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/server.hpp"
#include "cpath/status.hpp"
#include "cpath/wire.hpp"
#include "fixtures.hpp"
#include "test_framework.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

// Byte offsets inside an encoded frame header. The layout is owned by
// src/wire.cpp: magic(4) version(2) type(2) flags(4) sequence(8) epoch(8)
// payload_length(4) future_word(4) header_crc(4) session_id(8) request_id(4)
// reserved(4).
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kFlagsOffset = 8;
constexpr std::size_t kPayloadLengthOffset = 28;
constexpr std::size_t kHeaderCrcOffset = 36;
constexpr std::size_t kReservedOffset = 52;
// The header checksum covers every field up to and including the future word.
constexpr std::size_t kHeaderChecksumCoverageBytes = 36;
// Label bound the wire decoders apply to client/server labels and denial
// participants; src/wire.cpp owns the value.
constexpr std::size_t kMaxLabelBytes = 128;

std::string repeated(char value, std::size_t count) { return std::string(count, value); }

std::vector<cpath::FrameType> all_frame_types() {
  return {cpath::FrameType::kHello,          cpath::FrameType::kHelloAck,
          cpath::FrameType::kPlanRequest,    cpath::FrameType::kPlanResponse,
          cpath::FrameType::kGetPlanRequest, cpath::FrameType::kGetPlanResponse,
          cpath::FrameType::kStorePutRequest, cpath::FrameType::kStorePutResponse,
          cpath::FrameType::kListPlansRequest, cpath::FrameType::kListPlansResponse,
          cpath::FrameType::kRevalidateRequest, cpath::FrameType::kRevalidateResponse,
          cpath::FrameType::kPing,           cpath::FrameType::kPong,
          cpath::FrameType::kError,          cpath::FrameType::kShutdownRequest,
          cpath::FrameType::kShutdownAck,    cpath::FrameType::kBye};
}

std::uint32_t load_u32_le(const Bytes& bytes, std::size_t offset) {
  CPATH_REQUIRE(offset + 4u <= bytes.size());
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
         (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

void store_u32_le(Bytes& bytes, std::size_t offset, std::uint32_t value) {
  CPATH_REQUIRE(offset + 4u <= bytes.size());
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
  bytes[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
  bytes[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
}

void store_u16_le(Bytes& bytes, std::size_t offset, std::uint16_t value) {
  CPATH_REQUIRE(offset + 2u <= bytes.size());
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
}

// Recomputes the header checksum after a test has edited a covered field.
void refresh_header_checksum(Bytes& bytes) {
  CPATH_REQUIRE(bytes.size() >= cpath::kFrameHeaderBytes);
  store_u32_le(bytes, kHeaderCrcOffset,
               cpath::crc32(bytes.data(), kHeaderChecksumCoverageBytes));
}

// A header with deliberately wrong magic/version/checksum/reserved/payload
// length values. Encoding must canonicalise all of them.
cpath::FrameHeader sample_header(cpath::FrameType type, std::uint64_t sequence) {
  cpath::FrameHeader header;
  header.magic = cpath::kFrameMagic;
  header.version = cpath::kWireProtocolVersion;
  header.type = type;
  header.flags = 0xA5A5A5A5u;
  header.sequence = sequence;
  header.epoch = 7u;
  header.payload_length = 0x00FFFFFFu;  // the encoder owns this field
  header.header_crc = 0xDEADBEEFu;      // the encoder owns this field
  header.session_id = 0x1122334455667788ull;
  header.request_id = 4242u;
  header.reserved = 0xFFFFFFFFu;  // the format requires zero
  return header;
}

cpath::FrameHeader canonical_header(cpath::FrameType type, std::uint64_t sequence,
                                    std::size_t payload_bytes) {
  cpath::FrameHeader header = sample_header(type, sequence);
  header.payload_length = static_cast<std::uint32_t>(payload_bytes);
  header.reserved = 0;
  return header;
}

// Compares every field the encoder is required to preserve.
bool same_header_fields(const cpath::FrameHeader& lhs, const cpath::FrameHeader& rhs) {
  return lhs.magic == rhs.magic && lhs.version == rhs.version && lhs.type == rhs.type &&
         lhs.flags == rhs.flags && lhs.sequence == rhs.sequence && lhs.epoch == rhs.epoch &&
         lhs.payload_length == rhs.payload_length && lhs.session_id == rhs.session_id &&
         lhs.request_id == rhs.request_id && lhs.reserved == rhs.reserved;
}

Bytes encode_or_fail(const cpath::FrameHeader& header, const Bytes& payload) {
  Bytes bytes = cpath::encode_frame(header, payload);
  CPATH_REQUIRE(!bytes.empty());
  return bytes;
}

cpath::ErrorCode decode_code(const Bytes& bytes) {
  auto decoded = cpath::decode_frame(bytes);
  return decoded.has_value() ? cpath::ErrorCode::kOk : decoded.status().code();
}

template <class T, class Encode, class Decode>
struct PayloadRoundTrip {
  bool decoded_ok{false};
  T decoded{};
  Bytes encoded{};
  Bytes reencoded{};
};

template <class T, class Encode, class Decode>
PayloadRoundTrip<T, Encode, Decode> round_trip_payload(const T& value, Encode encode, Decode decode) {
  PayloadRoundTrip<T, Encode, Decode> result;
  cpath::ByteWriter writer;
  encode(value, writer);
  result.encoded = writer.bytes();
  cpath::ByteReader reader(result.encoded);
  result.decoded_ok = decode(reader, result.decoded).is_ok();
  if (result.decoded_ok) {
    cpath::ByteWriter again;
    encode(result.decoded, again);
    result.reencoded = again.bytes();
  }
  return result;
}

// Appends one byte after a well-formed payload; every decoder must refuse it.
template <class T, class Encode, class Decode>
cpath::ErrorCode decode_with_trailing_byte(const T& value, Encode encode, Decode decode) {
  cpath::ByteWriter writer;
  encode(value, writer);
  Bytes bytes = writer.bytes();
  bytes.push_back(0x5Au);
  cpath::ByteReader reader(bytes);
  T out{};
  const cpath::Status status = decode(reader, out);
  return status.is_ok() ? cpath::ErrorCode::kOk : status.code();
}

template <class T, class Decode>
cpath::ErrorCode decode_payload(const Bytes& bytes, Decode decode) {
  cpath::ByteReader reader(bytes);
  T out{};
  const cpath::Status status = decode(reader, out);
  return status.is_ok() ? cpath::ErrorCode::kOk : status.code();
}

cpath::ServerConfig loopback_config() {
  cpath::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.label = "cpath-test";
  return config;
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

struct SecondClientOutcome {
  bool connect_ok{false};
  bool handshake_ok{false};
  bool ping_ok{false};
  bool plan_ok{false};
  bool has_plan{false};
  std::uint64_t session_id{0};
  cpath::Status failure{};
};

SecondClientOutcome run_second_client(std::uint16_t port, const std::string& request_text) {
  SecondClientOutcome outcome;
  cpath::ServiceClient client;
  outcome.failure = client.connect("127.0.0.1", port);
  outcome.connect_ok = outcome.failure.is_ok();
  if (!outcome.connect_ok) {
    return outcome;
  }
  auto ack = client.handshake("second-client");
  outcome.handshake_ok = ack.has_value();
  if (!ack.has_value()) {
    outcome.failure = ack.status();
    return outcome;
  }
  outcome.session_id = ack.value().session_id;
  outcome.ping_ok = client.ping().is_ok();
  auto response = client.plan(request_text, false);
  outcome.plan_ok = response.has_value();
  if (response.has_value()) {
    outcome.has_plan = response.value().has_plan;
  } else {
    outcome.failure = response.status();
  }
  client.close();
  return outcome;
}

}  // namespace

CPATH_TEST(protocol_wire, frame_round_trip_covers_every_frame_type) {
  for (const cpath::FrameType type : all_frame_types()) {
    const Bytes payload = {0x01u, 0x02u, 0x03u, 0x04u, 0xFEu, 0xFFu};
    const std::uint64_t sequence = 1000u + static_cast<std::uint64_t>(type);
    const Bytes bytes = encode_or_fail(sample_header(type, sequence), payload);
    CPATH_CHECK_EQ(bytes.size(),
                   cpath::kFrameHeaderBytes + payload.size() + cpath::kFrameTrailerBytes);

    auto decoded = cpath::decode_frame(bytes);
    CPATH_REQUIRE(decoded.has_value());
    CPATH_CHECK(same_header_fields(decoded.value().header,
                                   canonical_header(type, sequence, payload.size())));
    CPATH_CHECK(decoded.value().header.type == type);
    CPATH_CHECK(decoded.value().payload == payload);
    // The encoder computed a real checksum over the covered prefix and zeroed
    // the reserved word the caller tried to set.
    CPATH_CHECK_EQ(decoded.value().header.header_crc, load_u32_le(bytes, kHeaderCrcOffset));
    CPATH_CHECK_EQ(decoded.value().header.reserved, 0u);
    CPATH_CHECK_EQ(load_u32_le(bytes, kReservedOffset), 0u);
    CPATH_CHECK_EQ(load_u32_le(bytes, kMagicOffset), cpath::kFrameMagic);
  }
}

CPATH_TEST(protocol_wire, frame_round_trip_at_payload_bounds) {
  const cpath::FrameHeader header = sample_header(cpath::FrameType::kPlanResponse, 9u);

  const Bytes empty;
  const Bytes empty_frame = encode_or_fail(header, empty);
  CPATH_CHECK_EQ(empty_frame.size(), cpath::kFrameHeaderBytes + cpath::kFrameTrailerBytes);
  auto empty_decoded = cpath::decode_frame(empty_frame);
  CPATH_REQUIRE(empty_decoded.has_value());
  CPATH_CHECK_EQ(empty_decoded.value().header.payload_length, 0u);
  CPATH_CHECK(empty_decoded.value().payload.empty());

  const Bytes single = {0x7Fu};
  auto single_decoded = cpath::decode_frame(encode_or_fail(header, single));
  CPATH_REQUIRE(single_decoded.has_value());
  CPATH_CHECK_EQ(single_decoded.value().header.payload_length, 1u);
  CPATH_CHECK(single_decoded.value().payload == single);

  // One megabyte, allocated once.
  Bytes full(cpath::kMaxFramePayloadBytes, 0u);
  for (std::size_t index = 0; index < full.size(); ++index) {
    full[index] = static_cast<std::uint8_t>(index * 31u + 7u);
  }
  const Bytes full_frame = encode_or_fail(header, full);
  CPATH_CHECK_EQ(full_frame.size(), cpath::kFrameHeaderBytes + cpath::kMaxFramePayloadBytes +
                                        cpath::kFrameTrailerBytes);
  auto full_decoded = cpath::decode_frame(full_frame);
  CPATH_REQUIRE(full_decoded.has_value());
  CPATH_CHECK_EQ(full_decoded.value().header.payload_length,
                 static_cast<std::uint32_t>(cpath::kMaxFramePayloadBytes));
  CPATH_CHECK(full_decoded.value().payload == full);

  // One byte past the ceiling is refused by the encoder rather than truncated.
  Bytes oversize(cpath::kMaxFramePayloadBytes + 1u, 0u);
  CPATH_CHECK(cpath::encode_frame(header, oversize).empty());
}

CPATH_TEST(protocol_wire, decode_rejects_with_exact_codes) {
  const Bytes payload = {0x10u, 0x20u, 0x30u, 0x40u};
  const Bytes good = encode_or_fail(sample_header(cpath::FrameType::kPlanRequest, 11u), payload);
  CPATH_CHECK(decode_code(good) == cpath::ErrorCode::kOk);

  Bytes bad_magic = good;
  bad_magic[kMagicOffset] ^= 0xFFu;
  CPATH_CHECK(decode_code(bad_magic) == cpath::ErrorCode::kBadMagic);

  Bytes bad_version = good;
  store_u16_le(bad_version, kVersionOffset, static_cast<std::uint16_t>(cpath::kWireProtocolVersion + 1u));
  refresh_header_checksum(bad_version);
  CPATH_CHECK(decode_code(bad_version) == cpath::ErrorCode::kBadProtocolVersion);

  Bytes unknown_type = good;
  store_u16_le(unknown_type, kTypeOffset, 0xFFFFu);
  refresh_header_checksum(unknown_type);
  CPATH_CHECK(decode_code(unknown_type) == cpath::ErrorCode::kUnexpectedFrameType);

  Bytes flipped_header_byte = good;
  flipped_header_byte[kFlagsOffset] ^= 0x01u;  // covered by the checksum, not recomputed
  CPATH_CHECK(decode_code(flipped_header_byte) == cpath::ErrorCode::kBadHeaderChecksum);

  Bytes flipped_payload_byte = good;
  flipped_payload_byte[cpath::kFrameHeaderBytes] ^= 0x01u;
  CPATH_CHECK(decode_code(flipped_payload_byte) == cpath::ErrorCode::kBadPayloadDigest);

  Bytes oversized_length = good;
  store_u32_le(oversized_length, kPayloadLengthOffset,
               static_cast<std::uint32_t>(cpath::kMaxFramePayloadBytes + 1u));
  refresh_header_checksum(oversized_length);
  CPATH_CHECK(decode_code(oversized_length) == cpath::ErrorCode::kFrameTooLarge);

  const Bytes truncated(good.begin(), good.end() - 1);
  CPATH_CHECK(decode_code(truncated) == cpath::ErrorCode::kTruncatedRecord);

  Bytes trailing = good;
  trailing.push_back(0x5Au);
  CPATH_CHECK(decode_code(trailing) == cpath::ErrorCode::kTrailingGarbage);
}

CPATH_TEST(protocol_wire, decode_never_crashes_on_adversarial_input) {
  const Bytes empty;
  CPATH_CHECK(decode_code(empty) == cpath::ErrorCode::kTruncatedRecord);
  std::size_t consumed = 99u;
  CPATH_CHECK(!cpath::decode_frame_header(empty, consumed).has_value());
  CPATH_CHECK_EQ(consumed, static_cast<std::size_t>(0));

  const Bytes one_byte = {0x00u};
  CPATH_CHECK(decode_code(one_byte) == cpath::ErrorCode::kTruncatedRecord);

  const Bytes all_ff_header(cpath::kFrameHeaderBytes, 0xFFu);
  CPATH_CHECK(decode_code(all_ff_header) == cpath::ErrorCode::kBadMagic);
  Bytes all_ff_frame(200u, static_cast<std::uint8_t>(0xFFu));
  CPATH_CHECK(decode_code(all_ff_frame) == cpath::ErrorCode::kBadMagic);

  // A well-formed header that claims a 0xFFFFFFFF payload: the length ceiling is
  // checked before anything is allocated or read.
  Bytes hostile = encode_or_fail(sample_header(cpath::FrameType::kPlanRequest, 3u), empty);
  store_u32_le(hostile, kPayloadLengthOffset, 0xFFFFFFFFu);
  refresh_header_checksum(hostile);
  const Bytes hostile_header_only(hostile.begin(), hostile.begin() + static_cast<std::ptrdiff_t>(cpath::kFrameHeaderBytes));
  CPATH_CHECK(decode_code(hostile_header_only) == cpath::ErrorCode::kFrameTooLarge);
  Bytes hostile_with_trailer = hostile_header_only;
  hostile_with_trailer.resize(cpath::kFrameHeaderBytes + cpath::kFrameTrailerBytes, 0u);
  CPATH_CHECK(decode_code(hostile_with_trailer) == cpath::ErrorCode::kFrameTooLarge);
  consumed = 77u;
  CPATH_CHECK(!cpath::decode_frame_header(hostile_header_only, consumed).has_value());
  CPATH_CHECK_EQ(consumed, static_cast<std::size_t>(0));
}

CPATH_TEST(protocol_wire, header_decode_needs_the_whole_header) {
  const Bytes payload = {0xAAu, 0xBBu};
  const Bytes frame = encode_or_fail(sample_header(cpath::FrameType::kPing, 5u), payload);

  for (std::size_t size = 0; size < cpath::kFrameHeaderBytes; ++size) {
    std::size_t consumed = 12345u;
    auto partial =
        cpath::decode_frame_header(std::span<const std::uint8_t>(frame.data(), size), consumed);
    CPATH_CHECK(!partial.has_value());
    CPATH_CHECK_EQ(consumed, static_cast<std::size_t>(0));
    CPATH_CHECK(partial.status().code() == cpath::ErrorCode::kTruncatedRecord);
  }

  std::size_t consumed = 0;
  auto complete = cpath::decode_frame_header(frame, consumed);
  CPATH_REQUIRE(complete.has_value());
  CPATH_CHECK_EQ(consumed, cpath::kFrameHeaderBytes);
  CPATH_CHECK(same_header_fields(complete.value(),
                                 canonical_header(cpath::FrameType::kPing, 5u, payload.size())));
  // A header-only buffer decodes as a header and is still not a complete frame.
  const Bytes header_only(frame.begin(),
                          frame.begin() + static_cast<std::ptrdiff_t>(cpath::kFrameHeaderBytes));
  consumed = 0;
  CPATH_CHECK(cpath::decode_frame_header(header_only, consumed).has_value());
  CPATH_CHECK_EQ(consumed, cpath::kFrameHeaderBytes);
  CPATH_CHECK(decode_code(header_only) == cpath::ErrorCode::kTruncatedRecord);
}

CPATH_TEST(protocol_wire, hello_payload_round_trips) {
  cpath::HelloPayload value;
  value.protocol_version = cpath::kWireProtocolVersion;
  value.client_label = repeated('c', kMaxLabelBytes);
  value.observed_epoch = 0x0123456789ABCDEFull;
  const auto trip = round_trip_payload(value, cpath::encode_hello, cpath::decode_hello);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.protocol_version, value.protocol_version);
  CPATH_CHECK_EQ(trip.decoded.client_label, value.client_label);
  CPATH_CHECK_EQ(trip.decoded.observed_epoch, value.observed_epoch);

  cpath::HelloPayload bare;
  bare.client_label.clear();
  const auto bare_trip = round_trip_payload(bare, cpath::encode_hello, cpath::decode_hello);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.client_label.empty());
  CPATH_CHECK_EQ(bare_trip.encoded.size(), static_cast<std::size_t>(2u + 4u + 8u));
}

CPATH_TEST(protocol_wire, hello_ack_payload_round_trips) {
  cpath::HelloAckPayload value;
  value.protocol_version = cpath::kWireProtocolVersion;
  value.session_id = 0xFFFFFFFFFFFFFFFFull;
  value.epoch = 77u;
  value.incarnation = 78u;
  value.server_label = repeated('s', kMaxLabelBytes);
  value.max_frame_payload = static_cast<std::uint32_t>(cpath::kMaxFramePayloadBytes);
  value.max_pending_requests = static_cast<std::uint64_t>(cpath::kMaxPendingRequestsPerSession);
  const auto trip = round_trip_payload(value, cpath::encode_hello_ack, cpath::decode_hello_ack);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.session_id, value.session_id);
  CPATH_CHECK_EQ(trip.decoded.epoch, value.epoch);
  CPATH_CHECK_EQ(trip.decoded.incarnation, value.incarnation);
  CPATH_CHECK_EQ(trip.decoded.server_label, value.server_label);
  CPATH_CHECK_EQ(trip.decoded.max_frame_payload, value.max_frame_payload);
  CPATH_CHECK_EQ(trip.decoded.max_pending_requests, value.max_pending_requests);

  cpath::HelloAckPayload bare;
  bare.server_label.clear();
  const auto bare_trip = round_trip_payload(bare, cpath::encode_hello_ack, cpath::decode_hello_ack);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.server_label.empty());
}

CPATH_TEST(protocol_wire, error_payload_round_trips) {
  cpath::ErrorPayload value;
  value.code = static_cast<std::uint16_t>(cpath::ErrorCode::kStaleEpoch);
  value.request_id = 0xDEADBEEFu;
  value.message = repeated('m', cpath::kMaxDetailBytes);
  const auto trip = round_trip_payload(value, cpath::encode_error, cpath::decode_error);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.code, value.code);
  CPATH_CHECK_EQ(trip.decoded.request_id, value.request_id);
  CPATH_CHECK_EQ(trip.decoded.message, value.message);

  cpath::ErrorPayload bare;
  bare.code = static_cast<std::uint16_t>(cpath::ErrorCode::kOk);
  const auto bare_trip = round_trip_payload(bare, cpath::encode_error, cpath::decode_error);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.message.empty());
}

CPATH_TEST(protocol_wire, plan_request_payload_round_trips) {
  cpath::PlanRequestPayload value;
  value.request_id = 1u;
  value.include_explanation = true;
  value.request_text = repeated('r', cpath::kMaxFramePayloadBytes);
  const auto trip = round_trip_payload(value, cpath::encode_plan_request, cpath::decode_plan_request);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.request_id, value.request_id);
  CPATH_CHECK_EQ(trip.decoded.include_explanation, true);
  CPATH_CHECK(trip.decoded.request_text == value.request_text);
  CPATH_CHECK_EQ(trip.decoded.request_text.size(), cpath::kMaxFramePayloadBytes);

  cpath::PlanRequestPayload bare;
  bare.include_explanation = false;
  const auto bare_trip = round_trip_payload(bare, cpath::encode_plan_request, cpath::decode_plan_request);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.request_text.empty());
  CPATH_CHECK_EQ(bare_trip.decoded.include_explanation, false);
}

CPATH_TEST(protocol_wire, plan_response_payload_round_trips) {
  cpath::PlanResponsePayload bare;
  bare.request_id = 2u;
  bare.code = static_cast<std::uint16_t>(cpath::ErrorCode::kNoPath);
  bare.has_plan = false;
  const auto bare_trip = round_trip_payload(bare, cpath::encode_plan_response, cpath::decode_plan_response);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.denials.empty());  // empty collection
  CPATH_CHECK(bare_trip.decoded.plan_record.empty());
  CPATH_CHECK_EQ(bare_trip.decoded.explanation, std::string());

  cpath::PlanResponsePayload value;
  value.request_id = 3u;
  value.code = static_cast<std::uint16_t>(cpath::ErrorCode::kInsufficientDisjointPaths);
  value.has_plan = true;
  value.plan_record.resize(4096u);
  for (std::size_t index = 0; index < value.plan_record.size(); ++index) {
    value.plan_record[index] = static_cast<std::uint8_t>(index * 7u + 3u);
  }
  value.explanation = repeated('e', cpath::kMaxDetailBytes);
  value.search_expansions = 0x123456789ABCDEFull;
  for (std::size_t index = 0; index < cpath::kMaxDenials; ++index) {
    cpath::DenialPayload denial;
    denial.code = static_cast<std::uint16_t>(cpath::ErrorCode::kNoPath);
    denial.conflict = static_cast<std::uint8_t>(cpath::ConflictKind::kDisjointness);
    denial.logical_edge = index + 1u;
    denial.participant = repeated('p', kMaxLabelBytes);
    denial.message = repeated('d', cpath::kMaxDetailBytes);
    value.denials.push_back(denial);
  }
  const auto trip = round_trip_payload(value, cpath::encode_plan_response, cpath::decode_plan_response);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.request_id, value.request_id);
  CPATH_CHECK_EQ(trip.decoded.code, value.code);
  CPATH_CHECK_EQ(trip.decoded.has_plan, true);
  CPATH_CHECK(trip.decoded.plan_record == value.plan_record);
  CPATH_CHECK_EQ(trip.decoded.explanation, value.explanation);
  CPATH_CHECK_EQ(trip.decoded.search_expansions, value.search_expansions);
  CPATH_REQUIRE_EQ(trip.decoded.denials.size(), cpath::kMaxDenials);
  for (std::size_t index = 0; index < trip.decoded.denials.size(); ++index) {
    CPATH_CHECK_EQ(trip.decoded.denials[index].code, value.denials[index].code);
    CPATH_CHECK_EQ(trip.decoded.denials[index].conflict, value.denials[index].conflict);
    CPATH_CHECK_EQ(trip.decoded.denials[index].logical_edge, value.denials[index].logical_edge);
    CPATH_CHECK_EQ(trip.decoded.denials[index].participant, value.denials[index].participant);
    CPATH_CHECK_EQ(trip.decoded.denials[index].message, value.denials[index].message);
  }
}

CPATH_TEST(protocol_wire, get_plan_payloads_round_trip) {
  cpath::GetPlanRequestPayload request;
  request.request_id = 4u;
  request.plan_id.bytes.fill(0xABu);
  const auto request_trip =
      round_trip_payload(request, cpath::encode_get_plan_request, cpath::decode_get_plan_request);
  CPATH_REQUIRE(request_trip.decoded_ok);
  CPATH_CHECK(request_trip.encoded == request_trip.reencoded);
  CPATH_CHECK_EQ(request_trip.decoded.request_id, request.request_id);
  CPATH_CHECK(request_trip.decoded.plan_id == request.plan_id);

  cpath::GetPlanRequestPayload zero_request;
  const auto zero_trip =
      round_trip_payload(zero_request, cpath::encode_get_plan_request, cpath::decode_get_plan_request);
  CPATH_REQUIRE(zero_trip.decoded_ok);
  CPATH_CHECK(zero_trip.decoded.plan_id.is_zero());

  cpath::GetPlanResponsePayload response;
  response.request_id = 5u;
  response.code = static_cast<std::uint16_t>(cpath::ErrorCode::kNotFound);
  response.has_plan = true;
  response.plan_record.resize(1024u, static_cast<std::uint8_t>(0x5Au));
  response.freshness = static_cast<std::uint8_t>(cpath::PlanFreshness::kUnverified);
  const auto response_trip =
      round_trip_payload(response, cpath::encode_get_plan_response, cpath::decode_get_plan_response);
  CPATH_REQUIRE(response_trip.decoded_ok);
  CPATH_CHECK(response_trip.encoded == response_trip.reencoded);
  CPATH_CHECK_EQ(response_trip.decoded.code, response.code);
  CPATH_CHECK_EQ(response_trip.decoded.has_plan, true);
  CPATH_CHECK(response_trip.decoded.plan_record == response.plan_record);
  CPATH_CHECK_EQ(response_trip.decoded.freshness, response.freshness);

  cpath::GetPlanResponsePayload bare;
  const auto bare_trip =
      round_trip_payload(bare, cpath::encode_get_plan_response, cpath::decode_get_plan_response);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.plan_record.empty());
}

CPATH_TEST(protocol_wire, list_plans_payloads_round_trip) {
  cpath::ListPlansRequestPayload request;
  request.request_id = 6u;
  const auto request_trip = round_trip_payload(request, cpath::encode_list_plans_request,
                                               cpath::decode_list_plans_request);
  CPATH_REQUIRE(request_trip.decoded_ok);
  CPATH_CHECK(request_trip.encoded == request_trip.reencoded);
  CPATH_CHECK_EQ(request_trip.decoded.request_id, request.request_id);

  cpath::ListPlansResponsePayload bare;
  bare.code = static_cast<std::uint16_t>(cpath::ErrorCode::kStoreNotOpen);
  const auto bare_trip = round_trip_payload(bare, cpath::encode_list_plans_response,
                                            cpath::decode_list_plans_response);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK(bare_trip.encoded == bare_trip.reencoded);
  CPATH_CHECK(bare_trip.decoded.plans.empty());  // empty collection

  cpath::ListPlansResponsePayload value;
  value.request_id = 7u;
  value.code = static_cast<std::uint16_t>(cpath::ErrorCode::kOk);
  for (std::size_t index = 0; index < 2u; ++index) {
    cpath::PlanSummaryPayload summary;
    summary.plan_id.bytes.fill(static_cast<std::uint8_t>(0x10u + index));
    summary.generation = 100u + index;
    summary.collective = repeated('g', kMaxLabelBytes);
    summary.kind = static_cast<std::uint32_t>(index);
    summary.stored_sequence = 1000u + index;
    summary.record_bytes = 2048u + index;
    summary.freshness = static_cast<std::uint8_t>(cpath::PlanFreshness::kCurrent);
    value.plans.push_back(summary);
  }
  const auto trip =
      round_trip_payload(value, cpath::encode_list_plans_response, cpath::decode_list_plans_response);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_REQUIRE_EQ(trip.decoded.plans.size(), static_cast<std::size_t>(2u));
  for (std::size_t index = 0; index < trip.decoded.plans.size(); ++index) {
    CPATH_CHECK(trip.decoded.plans[index].plan_id == value.plans[index].plan_id);
    CPATH_CHECK_EQ(trip.decoded.plans[index].generation, value.plans[index].generation);
    CPATH_CHECK_EQ(trip.decoded.plans[index].collective, value.plans[index].collective);
    CPATH_CHECK_EQ(trip.decoded.plans[index].kind, value.plans[index].kind);
    CPATH_CHECK_EQ(trip.decoded.plans[index].stored_sequence, value.plans[index].stored_sequence);
    CPATH_CHECK_EQ(trip.decoded.plans[index].record_bytes, value.plans[index].record_bytes);
    CPATH_CHECK_EQ(trip.decoded.plans[index].freshness, value.plans[index].freshness);
  }
}

CPATH_TEST(protocol_wire, revalidate_payloads_round_trip) {
  cpath::RevalidateRequestPayload request;
  request.request_id = 8u;
  request.plan_id.bytes.fill(0x33u);
  request.request_text = repeated('t', cpath::kMaxFramePayloadBytes);
  const auto request_trip =
      round_trip_payload(request, cpath::encode_revalidate_request, cpath::decode_revalidate_request);
  CPATH_REQUIRE(request_trip.decoded_ok);
  CPATH_CHECK(request_trip.encoded == request_trip.reencoded);
  CPATH_CHECK(request_trip.decoded.plan_id == request.plan_id);
  CPATH_CHECK(request_trip.decoded.request_text == request.request_text);

  cpath::RevalidateRequestPayload bare;
  const auto bare_request_trip =
      round_trip_payload(bare, cpath::encode_revalidate_request, cpath::decode_revalidate_request);
  CPATH_REQUIRE(bare_request_trip.decoded_ok);
  CPATH_CHECK(bare_request_trip.decoded.request_text.empty());

  cpath::RevalidateResponsePayload response;
  response.request_id = 9u;
  response.code = static_cast<std::uint16_t>(cpath::ErrorCode::kStalePlanGeneration);
  response.still_valid = true;
  response.freshness = static_cast<std::uint8_t>(cpath::PlanFreshness::kStaleTopology);
  response.current_input_digest.bytes.fill(0x77u);
  response.detail = repeated('x', cpath::kMaxDetailBytes);
  const auto response_trip = round_trip_payload(response, cpath::encode_revalidate_response,
                                                cpath::decode_revalidate_response);
  CPATH_REQUIRE(response_trip.decoded_ok);
  CPATH_CHECK(response_trip.encoded == response_trip.reencoded);
  CPATH_CHECK_EQ(response_trip.decoded.code, response.code);
  CPATH_CHECK_EQ(response_trip.decoded.still_valid, true);
  CPATH_CHECK_EQ(response_trip.decoded.freshness, response.freshness);
  CPATH_CHECK(response_trip.decoded.current_input_digest == response.current_input_digest);
  CPATH_CHECK_EQ(response_trip.decoded.detail, response.detail);

  cpath::RevalidateResponsePayload empty_response;
  empty_response.still_valid = false;
  const auto empty_trip = round_trip_payload(empty_response, cpath::encode_revalidate_response,
                                             cpath::decode_revalidate_response);
  CPATH_REQUIRE(empty_trip.decoded_ok);
  CPATH_CHECK(empty_trip.decoded.detail.empty());
  CPATH_CHECK(empty_trip.decoded.current_input_digest.is_zero());
}

CPATH_TEST(protocol_wire, shutdown_request_payload_round_trips) {
  cpath::ShutdownRequestPayload value;
  value.request_id = 10u;
  value.expected_epoch = 0xFFFFFFFFFFFFFFFFull;
  const auto trip =
      round_trip_payload(value, cpath::encode_shutdown_request, cpath::decode_shutdown_request);
  CPATH_REQUIRE(trip.decoded_ok);
  CPATH_CHECK(trip.encoded == trip.reencoded);
  CPATH_CHECK_EQ(trip.decoded.request_id, value.request_id);
  CPATH_CHECK_EQ(trip.decoded.expected_epoch, value.expected_epoch);
  CPATH_CHECK_EQ(trip.encoded.size(), static_cast<std::size_t>(12u));

  cpath::ShutdownRequestPayload bare;
  const auto bare_trip =
      round_trip_payload(bare, cpath::encode_shutdown_request, cpath::decode_shutdown_request);
  CPATH_REQUIRE(bare_trip.decoded_ok);
  CPATH_CHECK_EQ(bare_trip.decoded.expected_epoch, 0u);
}

CPATH_TEST(protocol_wire, payload_decoders_refuse_trailing_garbage) {
  CPATH_CHECK(decode_with_trailing_byte(cpath::HelloPayload{}, cpath::encode_hello, cpath::decode_hello) ==
              cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::HelloAckPayload{}, cpath::encode_hello_ack,
                                        cpath::decode_hello_ack) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::ErrorPayload{}, cpath::encode_error, cpath::decode_error) ==
              cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::PlanRequestPayload{}, cpath::encode_plan_request,
                                        cpath::decode_plan_request) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::PlanResponsePayload{}, cpath::encode_plan_response,
                                        cpath::decode_plan_response) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::GetPlanRequestPayload{}, cpath::encode_get_plan_request,
                                        cpath::decode_get_plan_request) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::GetPlanResponsePayload{}, cpath::encode_get_plan_response,
                                        cpath::decode_get_plan_response) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::ListPlansRequestPayload{}, cpath::encode_list_plans_request,
                                        cpath::decode_list_plans_request) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::ListPlansResponsePayload{},
                                        cpath::encode_list_plans_response,
                                        cpath::decode_list_plans_response) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::RevalidateRequestPayload{},
                                        cpath::encode_revalidate_request,
                                        cpath::decode_revalidate_request) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::RevalidateResponsePayload{},
                                        cpath::encode_revalidate_response,
                                        cpath::decode_revalidate_response) == cpath::ErrorCode::kTrailingGarbage);
  CPATH_CHECK(decode_with_trailing_byte(cpath::ShutdownRequestPayload{},
                                        cpath::encode_shutdown_request,
                                        cpath::decode_shutdown_request) == cpath::ErrorCode::kTrailingGarbage);
}

CPATH_TEST(protocol_wire, payload_decoders_refuse_oversized_fields) {
  // Over-long string: the length prefix alone is enough to be refused.
  cpath::ByteWriter long_label;
  long_label.u16(cpath::kWireProtocolVersion);
  long_label.u32(static_cast<std::uint32_t>(kMaxLabelBytes + 1u));
  CPATH_CHECK(decode_payload<cpath::HelloPayload>(long_label.bytes(), cpath::decode_hello) ==
              cpath::ErrorCode::kStringTooLong);

  cpath::ByteWriter long_message;
  long_message.u16(static_cast<std::uint16_t>(cpath::ErrorCode::kOk));
  long_message.u32(0u);
  long_message.u32(static_cast<std::uint32_t>(cpath::kMaxDetailBytes + 1u));
  CPATH_CHECK(decode_payload<cpath::ErrorPayload>(long_message.bytes(), cpath::decode_error) ==
              cpath::ErrorCode::kStringTooLong);

  cpath::ByteWriter long_text;
  long_text.u32(1u);
  long_text.boolean(false);
  long_text.u32(static_cast<std::uint32_t>(cpath::kMaxFramePayloadBytes + 1u));
  CPATH_CHECK(decode_payload<cpath::PlanRequestPayload>(long_text.bytes(), cpath::decode_plan_request) ==
              cpath::ErrorCode::kStringTooLong);

  cpath::ByteWriter long_record;
  long_record.u32(1u);
  long_record.u16(static_cast<std::uint16_t>(cpath::ErrorCode::kOk));
  long_record.boolean(true);
  long_record.u32(static_cast<std::uint32_t>(cpath::kMaxPlanRecordBytes + 1u));
  CPATH_CHECK(decode_payload<cpath::GetPlanResponsePayload>(long_record.bytes(),
                                                            cpath::decode_get_plan_response) ==
              cpath::ErrorCode::kStringTooLong);

  // Over-long collection count.
  cpath::ByteWriter many_denials;
  many_denials.u32(1u);
  many_denials.u16(static_cast<std::uint16_t>(cpath::ErrorCode::kOk));
  many_denials.boolean(false);
  many_denials.u32(0u);
  many_denials.u32(static_cast<std::uint32_t>(cpath::kMaxDenials + 1u));
  CPATH_CHECK(decode_payload<cpath::PlanResponsePayload>(many_denials.bytes(),
                                                         cpath::decode_plan_response) ==
              cpath::ErrorCode::kTooManyItems);

  cpath::ByteWriter many_plans;
  many_plans.u32(1u);
  many_plans.u16(static_cast<std::uint16_t>(cpath::ErrorCode::kOk));
  many_plans.u32(static_cast<std::uint32_t>(cpath::kMaxStoreRecords + 1u));
  CPATH_CHECK(decode_payload<cpath::ListPlansResponsePayload>(many_plans.bytes(),
                                                              cpath::decode_list_plans_response) ==
              cpath::ErrorCode::kTooManyItems);

  // A legal length with missing bytes is truncated, not over-long.
  cpath::ByteWriter short_text;
  short_text.u32(1u);
  short_text.boolean(false);
  short_text.u32(4u);
  short_text.u8(0x41u);
  CPATH_CHECK(decode_payload<cpath::PlanRequestPayload>(short_text.bytes(), cpath::decode_plan_request) ==
              cpath::ErrorCode::kTruncatedRecord);
}

CPATH_TEST(protocol_wire, parse_endpoint_accepts_and_rejects) {
  std::string host;
  auto port = cpath::parse_endpoint("127.0.0.1:8080", host);
  CPATH_REQUIRE(port.has_value());
  CPATH_CHECK_EQ(port.value(), static_cast<std::uint16_t>(8080u));
  CPATH_CHECK_EQ(host, std::string("127.0.0.1"));

  host.clear();
  port = cpath::parse_endpoint("localhost:1", host);
  CPATH_REQUIRE(port.has_value());
  CPATH_CHECK_EQ(port.value(), static_cast<std::uint16_t>(1u));
  CPATH_CHECK_EQ(host, std::string("localhost"));

  const auto rejects = [](std::string_view text, cpath::ErrorCode expected) {
    std::string rejected_host;
    const auto result = cpath::parse_endpoint(text, rejected_host);
    if (result.has_value()) {
      return false;
    }
    return result.status().code() == expected;
  };
  CPATH_CHECK(rejects("", cpath::ErrorCode::kInvalidSyntax));
  CPATH_CHECK(rejects("host", cpath::ErrorCode::kInvalidSyntax));
  CPATH_CHECK(rejects(":80", cpath::ErrorCode::kInvalidSyntax));
  CPATH_CHECK(rejects("host:", cpath::ErrorCode::kInvalidSyntax));
  CPATH_CHECK(rejects("host:0", cpath::ErrorCode::kValueOutOfRange));
  CPATH_CHECK(rejects("host:65536", cpath::ErrorCode::kValueOutOfRange));
  CPATH_CHECK(rejects("host:abc", cpath::ErrorCode::kMalformedNumber));
}

CPATH_TEST(protocol_transport, loopback_session_handshakes_pings_and_plans) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  CPATH_CHECK(coordinator.running());
  CPATH_CHECK_EQ(coordinator.bind_address(), std::string("127.0.0.1"));
  const std::uint16_t port = coordinator.port();
  CPATH_REQUIRE(port != 0u);
  CPATH_CHECK_EQ(coordinator.stats().sessions_accepted, 0u);

  cpath::ServiceClient client;
  CPATH_REQUIRE(client.connect("127.0.0.1", port).is_ok());
  CPATH_CHECK(client.connected());

  auto ack = client.handshake("protocol-probe");
  CPATH_REQUIRE(ack.has_value());
  CPATH_CHECK_EQ(ack.value().protocol_version, cpath::kWireProtocolVersion);
  CPATH_CHECK(ack.value().session_id != 0u);
  CPATH_CHECK_EQ(ack.value().epoch, coordinator.epoch().value());
  CPATH_CHECK_EQ(ack.value().incarnation, coordinator.incarnation().value());
  CPATH_CHECK_EQ(ack.value().server_label, std::string("cpath-test"));
  CPATH_CHECK_EQ(ack.value().max_frame_payload, static_cast<std::uint32_t>(cpath::kMaxFramePayloadBytes));
  CPATH_CHECK_EQ(ack.value().max_pending_requests,
                 static_cast<std::uint64_t>(cpath::kMaxPendingRequestsPerSession));

  CPATH_CHECK(client.ping().is_ok());

  // SYNTHETIC fabric: the shipped four-participant ring fixture.
  const std::string text = cpath_test::four_node_ring_text();
  auto response = client.plan(text, false);
  CPATH_REQUIRE(response.has_value());
  CPATH_CHECK_EQ(response.value().code, static_cast<std::uint16_t>(cpath::ErrorCode::kOk));
  CPATH_REQUIRE(response.value().has_plan);
  CPATH_CHECK(!response.value().plan_record.empty());

  cpath::StoredPlanSummary summary;
  auto decoded = cpath::decode_plan_record(response.value().plan_record, &summary);
  CPATH_REQUIRE(decoded.has_value());
  CPATH_CHECK(decoded.value().stats.path_count > 0u);
  CPATH_CHECK(summary.record_bytes == response.value().plan_record.size());

  auto parsed = cpath::parse_request_text(text);
  CPATH_REQUIRE(parsed.has_value());
  CPATH_CHECK(decoded.value().input_digest == parsed.value().canonical_digest);
  const cpath::Status valid = cpath::validate_plan(decoded.value(), parsed.value());
  CPATH_CHECK(valid.is_ok());

  client.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());
  CPATH_CHECK(!coordinator.running());
  const cpath::ServerStats stats = coordinator.stats();
  CPATH_CHECK_EQ(stats.sessions_accepted, 1u);
  CPATH_CHECK_EQ(stats.handshakes_completed, 1u);
  CPATH_CHECK_EQ(stats.plans_produced, 1u);
  CPATH_CHECK_EQ(stats.errors, 0u);
}

CPATH_TEST(protocol_transport, second_client_succeeds_while_the_first_is_live) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  const std::uint16_t port = coordinator.port();

  cpath::ServiceClient first;
  CPATH_REQUIRE(first.connect("127.0.0.1", port).is_ok());
  CPATH_REQUIRE(first.handshake("first-client").has_value());

  // SYNTHETIC fabric: the same four-participant ring as the first client.
  const std::string text = cpath_test::four_node_ring_text();
  SecondClientOutcome second;
  std::thread worker([&second, port, &text] { second = run_second_client(port, text); });

  auto first_response = first.plan(text, false);
  CPATH_REQUIRE(first_response.has_value());
  CPATH_CHECK(first_response.value().has_plan);
  CPATH_CHECK(first.ping().is_ok());
  worker.join();

  CPATH_CHECK(second.connect_ok);
  CPATH_CHECK(second.handshake_ok);
  CPATH_CHECK(second.ping_ok);
  CPATH_CHECK(second.plan_ok);
  CPATH_CHECK(second.has_plan);
  CPATH_CHECK(second.session_id != 0u);

  auto first_ack = first.handshake("first-client-again");
  CPATH_REQUIRE(!first_ack.has_value());  // one handshake per session
  CPATH_CHECK(first_ack.status().code() == cpath::ErrorCode::kSequenceMismatch);
  first.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());
  CPATH_CHECK_EQ(coordinator.stats().sessions_accepted, 2u);
}

CPATH_TEST(protocol_transport, refusal_carries_the_parser_error) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  cpath::ServiceClient client;
  CPATH_REQUIRE(client.connect("127.0.0.1", coordinator.port()).is_ok());
  CPATH_REQUIRE(client.handshake("refusal-probe").has_value());

  // SYNTHETIC fabric: the minimal ring fixture with trailing content appended,
  // which the canonical reader refuses. The wire answer must carry exactly the
  // parser's own error code and no plan, so a client can never mistake a
  // refusal for a partial plan.
  const std::string malformed = cpath_test::minimal_ring_text() + "unexpected trailing content\n";
  auto local = cpath::parse_request_text(malformed);
  CPATH_REQUIRE(!local.has_value());
  auto response = client.plan(malformed, false);
  CPATH_REQUIRE(response.has_value());
  CPATH_CHECK_EQ(response.value().has_plan, false);
  CPATH_CHECK(response.value().plan_record.empty());
  CPATH_CHECK_EQ(response.value().code, static_cast<std::uint16_t>(local.status().code()));
  CPATH_REQUIRE_EQ(response.value().denials.size(), static_cast<std::size_t>(1u));
  CPATH_CHECK_EQ(response.value().denials[0].code, static_cast<std::uint16_t>(local.status().code()));
  CPATH_CHECK_EQ(response.value().denials[0].message, local.status().detail());
  CPATH_CHECK(client.ping().is_ok());  // the session survives a refused request
  client.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());
  CPATH_CHECK_EQ(coordinator.stats().plan_denials, 1u);
  CPATH_CHECK_EQ(coordinator.stats().plans_produced, 0u);
}

CPATH_TEST(protocol_transport, pre_handshake_ping_is_allowed_and_other_frames_are_refused) {
  cpath::Coordinator coordinator(loopback_config());
  CPATH_REQUIRE(coordinator.start().is_ok());
  cpath::ServiceClient client;
  CPATH_REQUIRE(client.connect("127.0.0.1", coordinator.port()).is_ok());

  // A ping is a liveness check, not a request: it is answered before the hello.
  CPATH_CHECK(client.ping().is_ok());

  auto refused = error_from(send_list_request(client, 7u));
  CPATH_REQUIRE(refused.has_value());
  CPATH_CHECK_EQ(refused.value().code,
                 static_cast<std::uint16_t>(cpath::ErrorCode::kSessionHandshakeRequired));
  CPATH_CHECK_EQ(refused.value().request_id, 7u);

  // The refusal is per frame: the reader loop only ends on a transport or
  // framing failure, so the session is still usable and can complete its
  // handshake afterwards.
  CPATH_CHECK(client.ping().is_ok());
  auto ack = client.handshake("late-client");
  CPATH_REQUIRE(ack.has_value());
  CPATH_CHECK(ack.value().session_id != 0u);

  // A second hello on the same session is refused.
  auto second_hello = client.handshake("second-hello");
  CPATH_REQUIRE(!second_hello.has_value());
  CPATH_CHECK(second_hello.status().code() == cpath::ErrorCode::kSequenceMismatch);
  CPATH_CHECK(client.ping().is_ok());

  client.close();
  CPATH_REQUIRE(coordinator.stop().is_ok());
  const cpath::ServerStats stats = coordinator.stats();
  CPATH_CHECK_EQ(stats.handshakes_completed, 1u);
  CPATH_CHECK_EQ(stats.handshakes_refused, 1u);
  CPATH_CHECK_EQ(stats.errors, 0u);
}

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }
