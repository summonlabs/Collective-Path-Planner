// Collective Path Planner - canonical byte encoding primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/buffer.hpp"

#include <cstring>

namespace cpath {
namespace {

Status truncated(std::size_t needed, std::size_t available) {
  return Status::error(ErrorCode::kTruncatedRecord,
                       "need " + std::to_string(needed) + " bytes but only " + std::to_string(available) +
                           " remain");
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  u16(static_cast<std::uint16_t>(value & 0xFFFFu));
  u16(static_cast<std::uint16_t>((value >> 16u) & 0xFFFFu));
}

void ByteWriter::u64(std::uint64_t value) {
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
  u32(static_cast<std::uint32_t>((value >> 32u) & 0xFFFFFFFFu));
}

void ByteWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void ByteWriter::raw(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return;
  }
  const std::size_t offset = buffer_.size();
  buffer_.resize(offset + size);
  std::memcpy(buffer_.data() + offset, data, size);
}

void ByteWriter::raw(std::span<const std::uint8_t> data) { raw(data.data(), data.size()); }

void ByteWriter::string(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

void ByteWriter::digest(const Digest& value) { raw(value.bytes.data(), value.bytes.size()); }

void ByteWriter::tagged(std::string_view tag) { string(tag); }

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

ByteReader::ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data.data()), size_(data.size()) {}

ByteReader::ByteReader(std::string_view data) noexcept
    : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}

Status ByteReader::fail(Status status) noexcept {
  if (status_.is_ok()) {
    status_ = std::move(status);
  }
  return status_;
}

Status ByteReader::need(std::size_t size) noexcept {
  if (!status_.is_ok()) {
    return status_;
  }
  if (size > remaining()) {
    return fail(truncated(size, remaining()));
  }
  return Status::ok();
}

Status ByteReader::u8(std::uint8_t& out) {
  if (!status_.is_ok()) {
    return status_;
  }
  if (remaining() < 1u) {
    return fail(truncated(1u, remaining()));
  }
  out = data_[offset_];
  offset_ += 1u;
  return Status::ok();
}

Status ByteReader::u16(std::uint16_t& out) {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (Status status = u8(low); !status.is_ok()) {
    return status;
  }
  if (Status status = u8(high); !status.is_ok()) {
    return status;
  }
  out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(high) << 8u));
  return Status::ok();
}

Status ByteReader::u32(std::uint32_t& out) {
  std::uint16_t low = 0;
  std::uint16_t high = 0;
  if (Status status = u16(low); !status.is_ok()) {
    return status;
  }
  if (Status status = u16(high); !status.is_ok()) {
    return status;
  }
  out = static_cast<std::uint32_t>(static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16u));
  return Status::ok();
}

Status ByteReader::u64(std::uint64_t& out) {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (Status status = u32(low); !status.is_ok()) {
    return status;
  }
  if (Status status = u32(high); !status.is_ok()) {
    return status;
  }
  out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
  return Status::ok();
}

Status ByteReader::boolean(bool& out) {
  std::uint8_t raw = 0;
  if (Status status = u8(raw); !status.is_ok()) {
    return status;
  }
  if (raw > 1u) {
    return fail(Status::error(ErrorCode::kInvalidSyntax, "boolean field must be 0 or 1"));
  }
  out = raw == 1u;
  return Status::ok();
}

Status ByteReader::raw(std::size_t size, std::span<const std::uint8_t>& out) {
  if (Status status = need(size); !status.is_ok()) {
    return status;
  }
  out = std::span<const std::uint8_t>(data_ + offset_, size);
  offset_ += size;
  return Status::ok();
}

Status ByteReader::string(std::string& out, std::size_t max_bytes) {
  std::uint32_t length = 0;
  if (Status status = u32(length); !status.is_ok()) {
    return status;
  }
  if (static_cast<std::size_t>(length) > max_bytes) {
    return fail(Status::error(ErrorCode::kStringTooLong,
                              "encoded string length " + std::to_string(length) + " exceeds limit " +
                                  std::to_string(max_bytes)));
  }
  std::span<const std::uint8_t> view;
  if (Status status = raw(static_cast<std::size_t>(length), view); !status.is_ok()) {
    return status;
  }
  out.assign(reinterpret_cast<const char*>(view.data()), view.size());
  return Status::ok();
}

Status ByteReader::digest(Digest& out) {
  std::span<const std::uint8_t> view;
  if (Status status = raw(kDigestBytes, view); !status.is_ok()) {
    return status;
  }
  std::memcpy(out.bytes.data(), view.data(), kDigestBytes);
  return Status::ok();
}

Status ByteReader::count(std::uint32_t& out, std::size_t max_count) {
  std::uint32_t value = 0;
  if (Status status = u32(value); !status.is_ok()) {
    return status;
  }
  if (static_cast<std::size_t>(value) > max_count) {
    return fail(Status::error(ErrorCode::kTooManyItems,
                              "encoded count " + std::to_string(value) + " exceeds limit " +
                                  std::to_string(max_count)));
  }
  out = value;
  return Status::ok();
}

Status ByteReader::require_end() {
  if (!status_.is_ok()) {
    return status_;
  }
  if (!at_end()) {
    return fail(Status::error(ErrorCode::kTrailingGarbage,
                              std::to_string(remaining()) + " unexpected trailing bytes"));
  }
  return Status::ok();
}

bool fits_u32(std::size_t value) noexcept { return value <= 0xFFFFFFFFu; }

Status checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling, std::uint64_t& out) noexcept {
  if (a > ceiling || b > ceiling || a > ceiling - b) {
    out = ceiling;
    return Status::error(ErrorCode::kOverflow, "accumulator exceeded the configured ceiling");
  }
  out = a + b;
  return Status::ok();
}

}  // namespace cpath
