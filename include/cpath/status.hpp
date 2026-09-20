// Collective Path Planner - deterministic error model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_STATUS_HPP
#define CPATH_STATUS_HPP

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cpath/limits.hpp"

namespace cpath {

// Deterministic, stable error codes. Codes are part of the public contract:
// they appear in logs, CLI output and wire error frames, and they never
// change meaning within a major release.
enum class ErrorCode : std::uint16_t {
  kOk = 0,

  // 1-99: canonical input decoding and validation.
  kInvalidSyntax = 1,
  kUnknownKeyword = 2,
  kDuplicateIdentifier = 3,
  kMissingRequiredField = 4,
  kValueOutOfRange = 5,
  kTooManyItems = 6,
  kStringTooLong = 7,
  kLineTooLong = 8,
  kInputTooLarge = 9,
  kInvalidCharacter = 10,
  kUnknownReference = 11,
  kTrailingGarbage = 12,
  kUnsupportedFormatVersion = 13,
  kEmptyInput = 14,
  kMalformedNumber = 15,
  kInconsistentMetadata = 16,
  kDuplicateLogicalEdge = 17,
  kSelfLoopLogicalEdge = 18,
  kEmptyCollective = 19,
  kParticipantNotInCollective = 20,
  kUnknownEndpointNode = 21,
  kPathCountUnsupported = 22,
  kDuplicateBinding = 23,

  // 100-199: authority, generation, epoch and staleness.
  kStaleTopologyGeneration = 100,
  kStaleFailureDomainGeneration = 101,
  kStaleCapacityEvidenceGeneration = 102,
  kStalePolicyGeneration = 103,
  kStalePlanGeneration = 104,
  kStaleEpoch = 105,
  kIncarnationMismatch = 106,
  kPlanInputMismatch = 107,
  kRevalidationRequired = 108,
  kNotAuthoritative = 109,
  kPlanNotCurrent = 110,
  kGenerationRegression = 111,

  // 200-299: planning denials.
  kUnboundParticipant = 200,
  kNoPath = 202,
  kHopLimitExceeded = 203,
  kCostLimitExceeded = 204,
  kInsufficientDisjointPaths = 205,
  kDomainDiversityUnsatisfiable = 206,
  kFailureDomainForbidden = 207,
  kTierForbidden = 208,
  kNodeForbidden = 209,
  kCapacityEvidenceInsufficient = 210,
  kEdgeHasNoVerifiedCapacity = 211,
  kUnsupportedDisjointness = 215,
  kEndpointBudgetExceeded = 217,
  kSearchBudgetExceeded = 218,
  kEndpointIneligible = 219,
  kInsufficientCapacity = 221,

  // 300-399: persistence.
  kNotFound = 300,
  kCorruptRecord = 301,
  kTruncatedRecord = 302,
  kOversizedRecord = 303,
  kUnsupportedRecordVersion = 304,
  kDigestMismatch = 305,
  kStoreCapacityExceeded = 306,
  kAtomicReplaceFailed = 307,
  kStoreIoFailure = 308,
  kStoreNotOpen = 309,
  kStoreAlreadyOpen = 310,
  kRecordIncompatible = 311,
  kPathEscape = 312,

  // 400-499: wire protocol and service.
  kBadMagic = 400,
  kBadProtocolVersion = 401,
  kBadHeaderChecksum = 402,
  kBadPayloadDigest = 403,
  kFrameTooLarge = 404,
  kUnexpectedFrameType = 405,
  kSequenceMismatch = 406,
  kSessionClosed = 407,
  kSessionLimitExceeded = 408,
  kSessionHandshakeRequired = 409,
  kSocketFailure = 410,
  kConnectionRefused = 411,
  kSendFailure = 412,
  kReceiveFailure = 413,
  kServiceStopping = 414,
  kReplayDetected = 415,
  kMessageTooLarge = 416,
  kPeerClosed = 417,
  kNoRouteToService = 418,

  // 900-999: internal.
  kInternalError = 900,
  kNotImplemented = 901,
  kOverflow = 902,
  kCancelled = 903,
  kTimeout = 904,
};

// Stable, human-readable code name. Never returns nullptr.
const char* to_string(ErrorCode code) noexcept;

// Short stable symbol used in machine-readable output (for example
// "stale_topology_generation").
const char* code_symbol(ErrorCode code) noexcept;

// Error category for coarse routing of failures.
enum class ErrorCategory : std::uint8_t {
  kNone = 0,
  kInput,
  kAuthority,
  kPlanning,
  kPersistence,
  kProtocol,
  kInternal,
};

ErrorCategory category_of(ErrorCode code) noexcept;

// A deterministic error value: stable code plus a bounded human detail.
class Status {
 public:
  Status() = default;

  Status(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  static Status ok() noexcept { return Status(); }

  static Status error(ErrorCode code, std::string detail = std::string()) {
    return Status(code, std::move(detail));
  }

  bool is_ok() const noexcept { return code_ == ErrorCode::kOk; }
  explicit operator bool() const noexcept { return is_ok(); }

  ErrorCode code() const noexcept { return code_; }
  ErrorCategory category() const noexcept { return category_of(code_); }
  const std::string& detail() const noexcept { return detail_; }

  // "code_symbol: detail" (detail omitted when empty).
  std::string to_string() const;

 private:
  ErrorCode code_{ErrorCode::kOk};
  std::string detail_{};
};

inline bool operator==(const Status& lhs, const Status& rhs) noexcept {
  return lhs.code() == rhs.code() && lhs.detail() == rhs.detail();
}

// Result<T> carries either a value or a Status. It is intentionally not a
// general-purpose error-handling framework: no exceptions, no chaining.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(Status status) { return Result(std::move(status)); }
  static Result failure(ErrorCode code, std::string detail = std::string()) {
    return Result(Status::error(code, std::move(detail)));
  }

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  const T& value() const {
    assert(value_.has_value() && "Result::value() called on a failure");
    return *value_;
  }

  T& value() {
    assert(value_.has_value() && "Result::value() called on a failure");
    return *value_;
  }

  T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

  const Status& status() const noexcept { return status_; }

 private:
  std::optional<T> value_{};
  Status status_{};
};

// Result<void> specialisation.
template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  static Result success() { return Result(); }
  static Result failure(Status status) { return Result(std::move(status)); }
  static Result failure(ErrorCode code, std::string detail = std::string()) {
    return Result(Status::error(code, std::move(detail)));
  }

  bool has_value() const noexcept { return status_.is_ok(); }
  explicit operator bool() const noexcept { return has_value(); }
  const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

}  // namespace cpath

#endif  // CPATH_STATUS_HPP
