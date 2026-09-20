// Collective Path Planner - planning policy model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/policy.hpp"

#include <algorithm>

namespace cpath {
namespace {

template <class T>
void sort_unique(std::vector<T>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

template <class T>
Status require_sorted_unique(const std::vector<T>& values, const char* name) {
  for (std::size_t index = 1; index < values.size(); ++index) {
    if (!(values[index - 1] < values[index])) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           std::string(name) + " is not sorted and de-duplicated");
    }
  }
  return Status::ok();
}

}  // namespace

const char* to_string(Disjointness value) noexcept {
  switch (value) {
    case Disjointness::kNone:
      return "none";
    case Disjointness::kEdge:
      return "edge";
    case Disjointness::kNode:
      return "node";
  }
  return "none";
}

bool parse_disjointness(std::string_view text, Disjointness& out) noexcept {
  if (text == "none") {
    out = Disjointness::kNone;
    return true;
  }
  if (text == "edge") {
    out = Disjointness::kEdge;
    return true;
  }
  if (text == "node") {
    out = Disjointness::kNode;
    return true;
  }
  return false;
}

const char* to_string(DomainDiversity value) noexcept {
  switch (value) {
    case DomainDiversity::kNone:
      return "none";
    case DomainDiversity::kPreferred:
      return "preferred";
    case DomainDiversity::kRequired:
      return "required";
  }
  return "none";
}

bool parse_domain_diversity(std::string_view text, DomainDiversity& out) noexcept {
  if (text == "none") {
    out = DomainDiversity::kNone;
    return true;
  }
  if (text == "preferred") {
    out = DomainDiversity::kPreferred;
    return true;
  }
  if (text == "required") {
    out = DomainDiversity::kRequired;
    return true;
  }
  return false;
}

const char* to_string(EvidenceRequirement value) noexcept {
  switch (value) {
    case EvidenceRequirement::kAny:
      return "any";
    case EvidenceRequirement::kSyntheticOrBetter:
      return "synthetic-or-better";
    case EvidenceRequirement::kMeasured:
      return "measured";
  }
  return "any";
}

bool parse_evidence_requirement(std::string_view text, EvidenceRequirement& out) noexcept {
  if (text == "any") {
    out = EvidenceRequirement::kAny;
    return true;
  }
  if (text == "synthetic-or-better" || text == "synthetic") {
    out = EvidenceRequirement::kSyntheticOrBetter;
    return true;
  }
  if (text == "measured") {
    out = EvidenceRequirement::kMeasured;
    return true;
  }
  return false;
}

void Policy::canonicalize() {
  sort_unique(forbidden_nodes);
  sort_unique(forbidden_tiers);
  sort_unique(forbidden_failure_domains);
  sort_unique(allowed_path_tiers);
}

Status Policy::validate() const {
  if (!generation.valid()) {
    return Status::error(ErrorCode::kMissingRequiredField, "policy generation must be set");
  }
  if (max_hops < 1 || max_hops > kMaxHops) {
    return Status::error(ErrorCode::kValueOutOfRange,
                         "max-hop limit must be between 1 and " + std::to_string(kMaxHops));
  }
  if (paths_per_logical_edge < kMinPathsPerLogicalEdge || paths_per_logical_edge > kMaxPathsPerLogicalEdge) {
    return Status::error(ErrorCode::kPathCountUnsupported,
                         "paths per logical edge must be between " + std::to_string(kMinPathsPerLogicalEdge) +
                             " and " + std::to_string(kMaxPathsPerLogicalEdge));
  }
  if (latency_weight_milli > kMaxScoringWeightMilli || congestion_weight_milli > kMaxScoringWeightMilli) {
    return Status::error(ErrorCode::kValueOutOfRange, "scoring weight exceeds the model ceiling");
  }
  if (max_path_cost == 0 || max_path_cost > kCostCeiling) {
    return Status::error(ErrorCode::kValueOutOfRange, "max path cost must be between 1 and the cost ceiling");
  }
  if (forbidden_nodes.size() > kMaxPolicySetEntries || forbidden_tiers.size() > kMaxPolicySetEntries ||
      forbidden_failure_domains.size() > kMaxPolicySetEntries ||
      allowed_path_tiers.size() > kMaxPolicySetEntries) {
    return Status::error(ErrorCode::kTooManyItems, "policy set exceeds the configured bound");
  }
  if (max_search_expansions == 0 || max_search_expansions > kMaxSearchExpansions) {
    return Status::error(ErrorCode::kValueOutOfRange,
                         "search budget must be between 1 and " + std::to_string(kMaxSearchExpansions));
  }
  for (const NodeId& node : forbidden_nodes) {
    if (!is_valid_identifier(node.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "forbidden node id is not a valid identifier");
    }
  }
  for (const TierId& tier : forbidden_tiers) {
    if (!is_valid_tier_name(tier.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "forbidden tier is not a valid tier name");
    }
  }
  for (const FailureDomainId& domain : forbidden_failure_domains) {
    if (!is_valid_identifier(domain.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "forbidden failure domain is not a valid identifier");
    }
  }
  for (const TierId& tier : allowed_path_tiers) {
    if (!is_valid_tier_name(tier.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "allowed tier is not a valid tier name");
    }
    if (std::find(forbidden_tiers.begin(), forbidden_tiers.end(), tier) != forbidden_tiers.end()) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "tier " + tier.value() + " is both allowed and forbidden");
    }
  }
  if (Status status = require_sorted_unique(forbidden_nodes, "forbidden node set"); !status.is_ok()) {
    return status;
  }
  if (Status status = require_sorted_unique(forbidden_tiers, "forbidden tier set"); !status.is_ok()) {
    return status;
  }
  if (Status status = require_sorted_unique(forbidden_failure_domains, "forbidden failure domain set");
      !status.is_ok()) {
    return status;
  }
  if (Status status = require_sorted_unique(allowed_path_tiers, "allowed tier set"); !status.is_ok()) {
    return status;
  }
  return Status::ok();
}

void Policy::encode_canonical(ByteWriter& writer) const {
  writer.tagged("policy.v1");
  writer.u64(generation.value());
  writer.u64(static_cast<std::uint64_t>(max_hops));
  writer.u64(static_cast<std::uint64_t>(paths_per_logical_edge));
  writer.u8(static_cast<std::uint8_t>(disjointness));
  writer.u8(static_cast<std::uint8_t>(domain_diversity));
  writer.u8(static_cast<std::uint8_t>(domain_diversity_level));
  writer.u8(static_cast<std::uint8_t>(evidence));
  writer.u64(latency_weight_milli);
  writer.u64(congestion_weight_milli);
  writer.u64(max_path_cost);
  writer.boolean(allow_unverified_capacity);
  writer.u64(static_cast<std::uint64_t>(max_search_expansions));

  writer.u32(static_cast<std::uint32_t>(forbidden_nodes.size()));
  for (const NodeId& node : forbidden_nodes) {
    writer.string(node.value());
  }
  writer.u32(static_cast<std::uint32_t>(forbidden_tiers.size()));
  for (const TierId& tier : forbidden_tiers) {
    writer.string(tier.value());
  }
  writer.u32(static_cast<std::uint32_t>(forbidden_failure_domains.size()));
  for (const FailureDomainId& domain : forbidden_failure_domains) {
    writer.string(domain.value());
  }
  writer.u32(static_cast<std::uint32_t>(allowed_path_tiers.size()));
  for (const TierId& tier : allowed_path_tiers) {
    writer.string(tier.value());
  }
}

}  // namespace cpath
