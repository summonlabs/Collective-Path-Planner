// Collective Path Planner - canonical byte encoding primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_BUFFER_HPP
#define CPATH_BUFFER_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpath/digest.hpp"
#include "cpath/limits.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Canonical encoding rules: little-endian fixed-width integers, length-prefixed
// byte strings, no padding, no implicit terminators, no field tags. Two
// semantically identical values must produce byte-identical output. Callers are
// responsible for supplying values in canonical (sorted, de-duplicated) order;
// the model builders in this library do that.
class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value);
  void raw(const std::uint8_t* data, std::size_t size);
  void raw(std::span<const std::uint8_t> data);
  void string(std::string_view value);  // u32 byte length + bytes
  void digest(const Digest& value);
  void tagged(std::string_view tag);  // u32 length + bytes, for domain separators

  std::size_t size() const noexcept { return buffer_.size(); }
  bool empty() const noexcept { return buffer_.empty(); }
  const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
  void clear() noexcept { buffer_.clear(); }

  Digest sha256() const noexcept { return Sha256::hash(buffer_.data(), buffer_.size()); }

 private:
  std::vector<std::uint8_t> buffer_{};
};

// Bounded, sticky-error decoder. Once a read fails every later read fails with
// the same Status, so callers may perform a straight-line decode and check once.
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept;
  explicit ByteReader(std::span<const std::uint8_t> data) noexcept;
  explicit ByteReader(std::string_view data) noexcept;

  Status u8(std::uint8_t& out);
  Status u16(std::uint16_t& out);
  Status u32(std::uint32_t& out);
  Status u64(std::uint64_t& out);
  Status boolean(bool& out);
  Status raw(std::size_t size, std::span<const std::uint8_t>& out);
  Status string(std::string& out, std::size_t max_bytes = kMaxIdentifierBytes);
  Status digest(Digest& out);
  // Bounded collection count: fails with kTooManyItems when value > max_count.
  Status count(std::uint32_t& out, std::size_t max_count);

  bool ok() const noexcept { return status_.is_ok(); }
  const Status& status() const noexcept { return status_; }
  std::size_t remaining() const noexcept { return size_ - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  bool at_end() const noexcept { return offset_ == size_; }
  // Fails with kTrailingGarbage when bytes remain.
  Status require_end();

 private:
  Status fail(Status status) noexcept;
  Status need(std::size_t size) noexcept;

  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
  Status status_{};
};

// Checked conversion helpers. These never truncate silently.
bool fits_u32(std::size_t value) noexcept;
Status checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling, std::uint64_t& out) noexcept;

}  // namespace cpath

#endif  // CPATH_BUFFER_HPP
