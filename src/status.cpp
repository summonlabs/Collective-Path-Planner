// Collective Path Planner - deterministic error model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/status.hpp"

namespace cpath {
namespace {

struct CodeEntry {
  ErrorCode code;
  const char* symbol;
  const char* display;
};

// Single source of truth for code rendering. Every enumerator of ErrorCode
// appears exactly once; to_string() falls back to a fixed string otherwise.
constexpr CodeEntry kCodeTable[] = {
    {ErrorCode::kOk, "ok", "ok"},
    {ErrorCode::kInvalidSyntax, "invalid_syntax", "invalid syntax"},
    {ErrorCode::kUnknownKeyword, "unknown_keyword", "unknown keyword"},
    {ErrorCode::kDuplicateIdentifier, "duplicate_identifier", "duplicate identifier"},
    {ErrorCode::kMissingRequiredField, "missing_required_field", "missing required field"},
    {ErrorCode::kValueOutOfRange, "value_out_of_range", "value out of range"},
    {ErrorCode::kTooManyItems, "too_many_items", "too many items"},
    {ErrorCode::kStringTooLong, "string_too_long", "string too long"},
    {ErrorCode::kLineTooLong, "line_too_long", "line too long"},
    {ErrorCode::kInputTooLarge, "input_too_large", "input too large"},
    {ErrorCode::kInvalidCharacter, "invalid_character", "invalid character"},
    {ErrorCode::kUnknownReference, "unknown_reference", "unknown reference"},
    {ErrorCode::kTrailingGarbage, "trailing_garbage", "trailing garbage"},
    {ErrorCode::kUnsupportedFormatVersion, "unsupported_format_version", "unsupported format version"},
    {ErrorCode::kEmptyInput, "empty_input", "empty input"},
    {ErrorCode::kMalformedNumber, "malformed_number", "malformed number"},
    {ErrorCode::kInconsistentMetadata, "inconsistent_metadata", "inconsistent metadata"},
    {ErrorCode::kDuplicateLogicalEdge, "duplicate_logical_edge", "duplicate logical edge"},
    {ErrorCode::kSelfLoopLogicalEdge, "self_loop_logical_edge", "self-loop logical edge"},
    {ErrorCode::kEmptyCollective, "empty_collective", "empty collective"},
    {ErrorCode::kParticipantNotInCollective, "participant_not_in_collective", "participant not in collective"},
    {ErrorCode::kUnknownEndpointNode, "unknown_endpoint_node", "unknown endpoint node"},
    {ErrorCode::kPathCountUnsupported, "path_count_unsupported", "path count unsupported"},
    {ErrorCode::kDuplicateBinding, "duplicate_binding", "duplicate binding"},
    {ErrorCode::kStaleTopologyGeneration, "stale_topology_generation", "stale topology generation"},
    {ErrorCode::kStaleFailureDomainGeneration, "stale_failure_domain_generation", "stale failure domain generation"},
    {ErrorCode::kStaleCapacityEvidenceGeneration, "stale_capacity_evidence_generation",
     "stale capacity evidence generation"},
    {ErrorCode::kStalePolicyGeneration, "stale_policy_generation", "stale policy generation"},
    {ErrorCode::kStalePlanGeneration, "stale_plan_generation", "stale plan generation"},
    {ErrorCode::kStaleEpoch, "stale_epoch", "stale epoch"},
    {ErrorCode::kIncarnationMismatch, "incarnation_mismatch", "incarnation mismatch"},
    {ErrorCode::kPlanInputMismatch, "plan_input_mismatch", "plan input mismatch"},
    {ErrorCode::kRevalidationRequired, "revalidation_required", "revalidation required"},
    {ErrorCode::kNotAuthoritative, "not_authoritative", "not authoritative"},
    {ErrorCode::kPlanNotCurrent, "plan_not_current", "plan not current"},
    {ErrorCode::kGenerationRegression, "generation_regression", "generation regression"},
    {ErrorCode::kUnboundParticipant, "unbound_participant", "unbound participant"},
    {ErrorCode::kNoPath, "no_path", "no path"},
    {ErrorCode::kHopLimitExceeded, "hop_limit_exceeded", "hop limit exceeded"},
    {ErrorCode::kCostLimitExceeded, "cost_limit_exceeded", "cost limit exceeded"},
    {ErrorCode::kInsufficientDisjointPaths, "insufficient_disjoint_paths", "insufficient disjoint paths"},
    {ErrorCode::kDomainDiversityUnsatisfiable, "domain_diversity_unsatisfiable", "domain diversity unsatisfiable"},
    {ErrorCode::kFailureDomainForbidden, "failure_domain_forbidden", "failure domain forbidden"},
    {ErrorCode::kTierForbidden, "tier_forbidden", "tier forbidden"},
    {ErrorCode::kNodeForbidden, "node_forbidden", "node forbidden"},
    {ErrorCode::kCapacityEvidenceInsufficient, "capacity_evidence_insufficient", "capacity evidence insufficient"},
    {ErrorCode::kEdgeHasNoVerifiedCapacity, "edge_has_no_verified_capacity", "edge has no verified capacity"},
    {ErrorCode::kUnsupportedDisjointness, "unsupported_disjointness", "unsupported disjointness"},
    {ErrorCode::kEndpointBudgetExceeded, "endpoint_budget_exceeded", "endpoint budget exceeded"},
    {ErrorCode::kSearchBudgetExceeded, "search_budget_exceeded", "search budget exceeded"},
    {ErrorCode::kEndpointIneligible, "endpoint_ineligible", "endpoint ineligible"},
    {ErrorCode::kInsufficientCapacity, "insufficient_capacity", "insufficient verified capacity"},
    {ErrorCode::kNotFound, "not_found", "not found"},
    {ErrorCode::kCorruptRecord, "corrupt_record", "corrupt record"},
    {ErrorCode::kTruncatedRecord, "truncated_record", "truncated record"},
    {ErrorCode::kOversizedRecord, "oversized_record", "oversized record"},
    {ErrorCode::kUnsupportedRecordVersion, "unsupported_record_version", "unsupported record version"},
    {ErrorCode::kDigestMismatch, "digest_mismatch", "digest mismatch"},
    {ErrorCode::kStoreCapacityExceeded, "store_capacity_exceeded", "store capacity exceeded"},
    {ErrorCode::kAtomicReplaceFailed, "atomic_replace_failed", "atomic replace failed"},
    {ErrorCode::kStoreIoFailure, "store_io_failure", "store io failure"},
    {ErrorCode::kStoreNotOpen, "store_not_open", "store not open"},
    {ErrorCode::kStoreAlreadyOpen, "store_already_open", "store already open"},
    {ErrorCode::kRecordIncompatible, "record_incompatible", "record incompatible"},
    {ErrorCode::kPathEscape, "path_escape", "path escape"},
    {ErrorCode::kBadMagic, "bad_magic", "bad magic"},
    {ErrorCode::kBadProtocolVersion, "bad_protocol_version", "bad protocol version"},
    {ErrorCode::kBadHeaderChecksum, "bad_header_checksum", "bad header checksum"},
    {ErrorCode::kBadPayloadDigest, "bad_payload_digest", "bad payload digest"},
    {ErrorCode::kFrameTooLarge, "frame_too_large", "frame too large"},
    {ErrorCode::kUnexpectedFrameType, "unexpected_frame_type", "unexpected frame type"},
    {ErrorCode::kSequenceMismatch, "sequence_mismatch", "sequence mismatch"},
    {ErrorCode::kSessionClosed, "session_closed", "session closed"},
    {ErrorCode::kSessionLimitExceeded, "session_limit_exceeded", "session limit exceeded"},
    {ErrorCode::kSessionHandshakeRequired, "session_handshake_required", "session handshake required"},
    {ErrorCode::kSocketFailure, "socket_failure", "socket failure"},
    {ErrorCode::kConnectionRefused, "connection_refused", "connection refused"},
    {ErrorCode::kSendFailure, "send_failure", "send failure"},
    {ErrorCode::kReceiveFailure, "receive_failure", "receive failure"},
    {ErrorCode::kServiceStopping, "service_stopping", "service stopping"},
    {ErrorCode::kReplayDetected, "replay_detected", "replay detected"},
    {ErrorCode::kMessageTooLarge, "message_too_large", "message too large"},
    {ErrorCode::kPeerClosed, "peer_closed", "peer closed"},
    {ErrorCode::kNoRouteToService, "no_route_to_service", "no route to service"},
    {ErrorCode::kInternalError, "internal_error", "internal error"},
    {ErrorCode::kNotImplemented, "not_implemented", "not implemented"},
    {ErrorCode::kOverflow, "overflow", "arithmetic overflow"},
    {ErrorCode::kCancelled, "cancelled", "cancelled"},
    {ErrorCode::kTimeout, "timeout", "timeout"},
};

}  // namespace

const char* to_string(ErrorCode code) noexcept {
  for (const CodeEntry& entry : kCodeTable) {
    if (entry.code == code) {
      return entry.display;
    }
  }
  return "unknown error code";
}

const char* code_symbol(ErrorCode code) noexcept {
  for (const CodeEntry& entry : kCodeTable) {
    if (entry.code == code) {
      return entry.symbol;
    }
  }
  return "unknown";
}

ErrorCategory category_of(ErrorCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  if (raw == 0) {
    return ErrorCategory::kNone;
  }
  if (raw < 100) {
    return ErrorCategory::kInput;
  }
  if (raw < 200) {
    return ErrorCategory::kAuthority;
  }
  if (raw < 300) {
    return ErrorCategory::kPlanning;
  }
  if (raw < 400) {
    return ErrorCategory::kPersistence;
  }
  if (raw < 500) {
    return ErrorCategory::kProtocol;
  }
  return ErrorCategory::kInternal;
}

std::string Status::to_string() const {
  if (is_ok()) {
    return std::string("ok");
  }
  std::string result(code_symbol(code_));
  if (!detail_.empty()) {
    result += ": ";
    result += detail_;
  }
  return result;
}

}  // namespace cpath
