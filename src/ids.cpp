// Collective Path Planner - strongly typed identities and generations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/ids.hpp"

namespace cpath {
namespace {

// Conservative ASCII grammar. The first character must be alphanumeric or an
// underscore; the remainder may also contain '-', '.' and ':'. This keeps every
// identity safe to embed in the text DSL, in log lines and in CLI output
// without quoting, and makes canonical byte order equal to human sort order.
bool valid_identifier(std::string_view text, std::size_t max_bytes) noexcept {
  if (text.empty() || text.size() > max_bytes) {
    return false;
  }
  const auto first = static_cast<unsigned char>(text.front());
  const bool first_ok = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                        (first >= '0' && first <= '9') || first == '_';
  if (!first_ok) {
    return false;
  }
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-' || ch == '.' || ch == ':';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool is_valid_identifier(std::string_view text) noexcept {
  return valid_identifier(text, kMaxIdentifierBytes);
}

bool is_valid_tier_name(std::string_view text) noexcept {
  return valid_identifier(text, kMaxTierNameBytes);
}

Result<std::string> make_identifier(std::string_view text) {
  if (!is_valid_identifier(text)) {
    if (text.empty()) {
      return Result<std::string>::failure(ErrorCode::kMissingRequiredField, "identifier must not be empty");
    }
    if (text.size() > kMaxIdentifierBytes) {
      return Result<std::string>::failure(ErrorCode::kStringTooLong, "identifier exceeds 64 bytes");
    }
    return Result<std::string>::failure(ErrorCode::kInvalidCharacter, "identifier contains an unsupported character");
  }
  return Result<std::string>::success(std::string(text));
}

Result<std::string> make_tier_name(std::string_view text) {
  if (!is_valid_tier_name(text)) {
    if (text.empty()) {
      return Result<std::string>::failure(ErrorCode::kMissingRequiredField, "tier name must not be empty");
    }
    if (text.size() > kMaxTierNameBytes) {
      return Result<std::string>::failure(ErrorCode::kStringTooLong, "tier name exceeds 32 bytes");
    }
    return Result<std::string>::failure(ErrorCode::kInvalidCharacter, "tier name contains an unsupported character");
  }
  return Result<std::string>::success(std::string(text));
}

std::string to_string(const NodeId& id) { return id.value(); }
std::string to_string(const EdgeId& id) { return id.value(); }
std::string to_string(const ParticipantId& id) { return id.value(); }
std::string to_string(const LogicalEdgeId& id) { return std::to_string(id.value()); }
std::string to_string(const PathId& id) { return std::to_string(id.value()); }
std::string to_string(const CollectiveId& id) { return id.value(); }
std::string to_string(const GroupId& id) { return id.value(); }
std::string to_string(const FailureDomainId& id) { return id.value(); }
std::string to_string(const TierId& id) { return id.value(); }
std::string to_string(const CollectivePlanId& id) { return id.value().hex(); }
std::string to_string(const SessionId& id) { return std::to_string(id.value()); }

}  // namespace cpath
