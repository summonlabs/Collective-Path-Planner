// Collective Path Planner - deterministic constrained path planner.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_PLANNER_HPP
#define CPATH_PLANNER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cpath/ids.hpp"
#include "cpath/limits.hpp"
#include "cpath/plan.hpp"
#include "cpath/request.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Which constraint class caused a denial. Reported separately from the error
// code so that a caller can react to the constraint without parsing prose.
enum class ConflictKind : std::uint8_t {
  kNone = 0,
  kMissingBinding,
  kUnknownNode,
  kIneligibleEndpoint,
  kEndpointConstraint,
  kForbiddenNode,
  kForbiddenTier,
  kForbiddenFailureDomain,
  kHopLimit,
  kCostLimit,
  kDisconnected,
  kNoVerifiedCapacity,
  kInsufficientCapacity,
  kEvidenceInsufficient,
  kDisjointness,
  kDomainDiversity,
  kSearchBudget,
  kStructure,
};

const char* to_string(ConflictKind value) noexcept;

// A precise, bounded explanation of why a mapping was refused. A denial is
// terminal for the request: the planner never returns a partial plan alongside
// a denial.
struct DenialDetail {
  ErrorCode code{ErrorCode::kInternalError};
  ConflictKind conflict{ConflictKind::kNone};
  LogicalEdgeId logical_edge{};  // 1-based; invalid when the denial is global
  ParticipantId participant{};
  std::string message{};
  // Bounded witness sets (each at most kMaxDenialWitness entries).
  std::vector<NodeId> nodes{};
  std::vector<EdgeId> edges{};
  std::vector<FailureDomainId> failure_domains{};
  std::vector<TierId> tiers{};
};

inline constexpr std::size_t kMaxDenialWitness = 8;
inline constexpr std::size_t kMaxDenials = 64;

struct PlannerLimits {
  // Candidate paths retained per logical edge before constraint selection.
  std::size_t max_candidate_paths{8};
  // Total relaxed-edge expansions across the whole request.
  std::size_t max_search_expansions{1u << 18};
  // When false, the planner returns the first denial immediately instead of
  // collecting a bounded set of them.
  bool collect_all_denials{true};
};

struct PlanningOutcome {
  std::optional<Plan> plan{};
  std::vector<DenialDetail> denials{};
  std::size_t search_expansions{0};

  bool ok() const noexcept { return plan.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
  ErrorCode primary_code() const noexcept;
};

// Deterministic planning entry point. Identical canonical inputs produce
// byte-identical plans.
PlanningOutcome plan_collective(const PlanningRequest& request, const PlannerLimits& limits = {});

// Single-path convenience used by tests and tooling.
Result<std::vector<Hop>> find_path(const PlanningRequest& request,
                                   const NodeId& source,
                                   const NodeId& destination,
                                   const PlannerLimits& limits = {});

}  // namespace cpath

#endif  // CPATH_PLANNER_HPP
