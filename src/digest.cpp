// Collective Path Planner - digests and integrity primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/digest.hpp"

#include <array>
#include <cstring>

namespace cpath {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

int hex_value(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + (ch - 'a');
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + (ch - 'A');
  }
  return -1;
}

const std::array<std::uint32_t, 256>& crc32_table() noexcept {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      result[index] = value;
    }
    return result;
  }();
  return table;
}

}  // namespace

bool Digest::is_zero() const noexcept {
  std::uint8_t accumulator = 0;
  for (const std::uint8_t byte : bytes) {
    accumulator = static_cast<std::uint8_t>(accumulator | byte);
  }
  return accumulator == 0;
}

std::string Digest::hex() const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(kDigestBytes * 2u);
  for (const std::uint8_t byte : bytes) {
    result.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
    result.push_back(kHexDigits[byte & 0x0Fu]);
  }
  return result;
}

Result<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != kDigestBytes * 2u) {
    return Result<Digest>::failure(ErrorCode::kInvalidSyntax, "digest must be 64 hex characters");
  }
  Digest result;
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    const int high = hex_value(text[index * 2u]);
    const int low = hex_value(text[index * 2u + 1u]);
    if (high < 0 || low < 0) {
      return Result<Digest>::failure(ErrorCode::kInvalidCharacter, "digest contains a non-hex character");
    }
    result.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Result<Digest>::success(result);
}

std::uint64_t Digest::short_id() const noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8u * index);
  }
  return value;
}

std::size_t DigestHash::operator()(const Digest& digest) const noexcept {
  return static_cast<std::size_t>(fnv1a64(digest.bytes.data(), digest.bytes.size()));
}

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4u]) << 24u) |
                      (static_cast<std::uint32_t>(block[index * 4u + 1u]) << 16u) |
                      (static_cast<std::uint32_t>(block[index * 4u + 2u]) << 8u) |
                      static_cast<std::uint32_t>(block[index * 4u + 3u]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3u);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10u);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + big_s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
  total_bytes_ += size;
  if (buffered_ > 0) {
    const std::size_t missing = 64u - buffered_;
    const std::size_t take = size < missing ? size : missing;
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    size -= take;
    if (buffered_ == 64u) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (size >= 64u) {
    compress(data);
    data += 64u;
    size -= 64u;
  }
  if (size > 0) {
    std::memcpy(buffer_.data(), data, size);
    buffered_ = size;
  }
}

Digest Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8u;
  const std::uint8_t marker = 0x80u;
  const std::uint8_t zero = 0x00u;
  update(&marker, 1);
  while (buffered_ != 56u) {
    update(&zero, 1);
  }
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (56u - 8u * index)) & 0xFFu);
  }
  update(length_bytes.data(), length_bytes.size());

  Digest result;
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = state_[index];
    result.bytes[index * 4u] = static_cast<std::uint8_t>((word >> 24u) & 0xFFu);
    result.bytes[index * 4u + 1u] = static_cast<std::uint8_t>((word >> 16u) & 0xFFu);
    result.bytes[index * 4u + 2u] = static_cast<std::uint8_t>((word >> 8u) & 0xFFu);
    result.bytes[index * 4u + 3u] = static_cast<std::uint8_t>(word & 0xFFu);
  }
  return result;
}

Digest Sha256::hash(const std::uint8_t* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest Sha256::hash(std::string_view text) noexcept {
  return hash(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  const std::array<std::uint32_t, 256>& table = crc32_table();
  std::uint32_t value = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < size; ++index) {
    value = table[(value ^ data[index]) & 0xFFu] ^ (value >> 8u);
  }
  return value ^ 0xFFFFFFFFu;
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint64_t value = 0xCBF29CE484222325ull;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= static_cast<std::uint64_t>(data[index]);
    value *= 0x100000001B3ull;
  }
  return value;
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling) noexcept {
  if (a >= ceiling || b >= ceiling || a > ceiling - b) {
    return ceiling;
  }
  return a + b;
}

std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a >= ceiling || b >= ceiling) {
    return ceiling;
  }
  if (a > ceiling / b) {
    return ceiling;
  }
  return a * b;
}

std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) noexcept {
  return a > b ? a - b : 0;
}

}  // namespace cpath
