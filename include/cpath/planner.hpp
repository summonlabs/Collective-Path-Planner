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

// Epistemic category of a denial. This is the difference between "there is no
// such mapping" and "this planner did not find one within its declared bounds",
// and a caller must be able to tell them apart without reading prose.
//
//  kInvalidRequest       the request itself is malformed or self-contradictory
//  kStaleInput           the request's generations do not authorise this plan
//  kProvenInfeasible     no mapping exists for the declared problem class, and
//                        that has been proven, not merely unobserved
//  kSearchLimitReached   the search stopped at a declared bound before proving
//                        or finding anything: the result is INDETERMINATE
//  kUnsupportedConstraint the policy asks for a mode this planner does not
//                        implement, so no claim is made either way
enum class DenialKind : std::uint8_t {
  kInvalidRequest = 0,
  kStaleInput = 1,
  kProvenInfeasible = 2,
  kSearchLimitReached = 3,
  kUnsupportedConstraint = 4,
};

const char* to_string(DenialKind value) noexcept;

// A precise, bounded explanation of why a mapping was refused. A denial is
// terminal for the request: the planner never returns a partial plan alongside
// a denial.
struct DenialDetail {
  ErrorCode code{ErrorCode::kInternalError};
  DenialKind kind{DenialKind::kProvenInfeasible};
  ConflictKind conflict{ConflictKind::kNone};
  LogicalEdgeId logical_edge{};  // 1-based; invalid when the denial is global
  ParticipantId participant{};
  std::string message{};
  // What the constraint was evaluated against, when the planner got that far.
  std::size_t candidates_considered{0};
  std::size_t paths_enumerated{0};
  bool enumeration_exhaustive{false};
  // Bounded witness sets (each at most kMaxDenialWitness entries).
  std::vector<NodeId> nodes{};
  std::vector<EdgeId> edges{};
  std::vector<FailureDomainId> failure_domains{};
  std::vector<TierId> tiers{};
};

inline constexpr std::size_t kMaxDenialWitness = 8;
inline constexpr std::size_t kMaxDenials = 64;

// Search bounds. Every one of them, when reached, produces
// DenialKind::kSearchLimitReached rather than an infeasibility claim.
struct PlannerLimits {
  // Paths retained per logical edge when the enumeration was not exhaustive.
  std::size_t max_candidate_paths{8};
  // Enumeration stops once this many feasible simple paths are known, and the
  // path set is then no longer exhaustive.
  std::size_t max_exhaustive_paths{64};
  // Depth-first steps spent enumerating one logical edge's simple paths. When
  // this is reached the enumeration is incomplete and says so. Exhaustion costs
  // proof strength, never feasibility: the planner falls back to the heuristic
  // pool and still returns a plan when one exists.
  std::size_t max_enumeration_steps{512};
  // Total enumeration steps across the whole request. Charged separately from
  // max_search_expansions, which bounds relaxed-edge expansions only, so that
  // enumerating early logical edges cannot starve the search for later ones.
  std::size_t max_total_enumeration_steps{1u << 21};
  // Budget for enumerating the k-subsets of one logical edge's path set.
  std::size_t max_combination_steps{200000};
  // Per-logical-edge alternatives retained for the global capacity search.
  std::size_t max_set_alternatives{8};
  // Budget for the global capacity-coherent assignment search.
  std::size_t max_global_nodes{200000};
  // Budget for relaxed-edge expansions across the whole request.
  std::size_t max_search_expansions{1u << 18};
  // When false, the planner returns the first denial immediately instead of
  // collecting a bounded set of them.
  bool collect_all_denials{true};
};

struct PlanningOutcome {
  std::optional<Plan> plan{};
  std::vector<DenialDetail> denials{};
  std::size_t search_expansions{0};    // relaxed-edge expansions
  std::size_t enumeration_steps{0};    // simple-path enumeration steps
  std::size_t paths_enumerated{0};     // feasible simple paths found
  std::size_t combination_steps{0};    // candidate sets considered
  std::size_t global_search_nodes{0};
  // True when every logical edge's feasible path set was enumerated in full and
  // the selection over them was exhaustive, so the chosen mapping is provably
  // optimal for the declared objective. False means "valid and deterministic,
  // but there may exist a better mapping outside the search bounds".
  bool optimal{false};

  bool ok() const noexcept { return plan.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
  ErrorCode primary_code() const noexcept;
  DenialKind primary_kind() const noexcept;
  // True when the outcome claims that no mapping exists. Only ever set for a
  // denial whose kind is kProvenInfeasible.
  bool proven_infeasible() const noexcept;
  // True when the outcome is INDETERMINATE: the search stopped at a declared
  // bound and no claim is made about existence.
  bool indeterminate() const noexcept;
};

// Deterministic planning entry point.
//
// Contract, in full:
//  * every emitted path is a SIMPLE path - no physical node and no physical
//    edge appears twice in one path;
//  * the objective is lexicographic: (1) satisfy every hard constraint,
//    (2) minimise the total path cost, (3) minimise the number of sibling pairs
//    that are not failure-domain independent, (4) minimise the largest single
//    path cost, (5) maximise the smallest remaining verified capacity headroom,
//    (6) break remaining ties by the canonical path encoding. Partial order
//    comparisons use exactly the same total order, so the result never depends
//    on container iteration, hash order, thread scheduling or the order in
//    which candidates happened to be discovered;
//  * identical canonical requests produce byte-identical plans;
//  * a denial claiming infeasibility (DenialKind::kProvenInfeasible) is only
//    emitted when infeasibility was actually proven, and never carries a plan;
//  * when the search stops at a declared bound, the result is
//    DenialKind::kSearchLimitReached, which is INDETERMINATE and is not an
//    infeasibility claim.
PlanningOutcome plan_collective(const PlanningRequest& request, const PlannerLimits& limits = {});

// Single-path convenience used by tests and tooling. Returns the cheapest
// simple path under the policy, or a Status describing the failure.
Result<std::vector<Hop>> find_path(const PlanningRequest& request,
                                   const NodeId& source,
                                   const NodeId& destination,
                                   const PlannerLimits& limits = {});

// True when the hop sequence visits no physical node and no physical edge twice.
// Exposed because it is part of the plan contract, not an internal detail.
bool hops_are_simple(const std::vector<Hop>& hops);

}  // namespace cpath

#endif  // CPATH_PLANNER_HPP
