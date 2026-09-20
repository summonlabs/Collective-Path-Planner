// Collective Path Planner - digests and integrity primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_DIGEST_HPP
#define CPATH_DIGEST_HPP

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "cpath/limits.hpp"
#include "cpath/status.hpp"

namespace cpath {

inline constexpr std::size_t kDigestBytes = 32;

// A 256-bit digest value. Trivially copyable and totally ordered so it can be
// used as a map key without allocation.
struct Digest {
  std::array<std::uint8_t, kDigestBytes> bytes{};

  friend bool operator==(const Digest&, const Digest&) = default;
  friend std::strong_ordering operator<=>(const Digest&, const Digest&) = default;

  bool is_zero() const noexcept;

  // Lower-case hex, 64 characters.
  std::string hex() const;

  // Strict decoder: exactly 64 hex characters, no prefixes, no whitespace.
  static Result<Digest> from_hex(std::string_view text);

  // First 8 bytes interpreted little-endian; used for compact display only,
  // never as an identity.
  std::uint64_t short_id() const noexcept;
};

struct DigestHash {
  std::size_t operator()(const Digest& digest) const noexcept;
};

// Incremental SHA-256. Deterministic, allocation-free apart from the caller's
// own buffers.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  }

  Digest finish() noexcept;

  static Digest hash(const std::uint8_t* data, std::size_t size) noexcept;
  static Digest hash(std::string_view text) noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::uint64_t total_bytes_{0};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
};

// CRC-32 (IEEE 802.3 polynomial, reflected). Used for wire header integrity
// only; payload integrity uses SHA-256.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

// FNV-1a 64. Used for non-cryptographic hash-table-style helpers inside the
// library; never used where integrity matters.
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept;
inline std::uint64_t fnv1a64(std::string_view text) noexcept {
  return fnv1a64(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// Saturation helpers. Every externally influenced arithmetic path in the
// planner funnels through these so that scoring can never wrap.
std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling) noexcept;
std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling) noexcept;
std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) noexcept;

}  // namespace cpath

#endif  // CPATH_DIGEST_HPP
