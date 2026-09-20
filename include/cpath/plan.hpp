// Collective Path Planner - plan model, freshness and invariant checks.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_PLAN_HPP
#define CPATH_PLAN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/collective.hpp"
#include "cpath/digest.hpp"
#include "cpath/fabric.hpp"
#include "cpath/ids.hpp"
#include "cpath/limits.hpp"
#include "cpath/request.hpp"
#include "cpath/status.hpp"

namespace cpath {

// A single physical hop chosen for a logical edge.
struct Hop {
  EdgeId edge{};
  NodeId from{};
  NodeId to{};

  friend bool operator==(const Hop&, const Hop&) = default;
};

// One physical path carrying one logical edge.
struct PathPlan {
  PathId id{};                     // dense, assigned in canonical order
  LogicalEdgeId logical_edge{};    // 1-based index into Collective::logical_edges
  ParticipantId src{};
  ParticipantId dst{};
  std::uint32_t stage{0};
  std::vector<Hop> hops{};
  std::uint64_t cost{0};
  std::uint64_t bottleneck_mbps{0};
  // Failure domains this path adds at the policy's diversity level, sorted and
  // de-duplicated, EXCLUDING the attachment domains of its two endpoints. Every
  // sibling path necessarily sits in the source and destination domains, so a
  // failure there takes the endpoint down whichever route was chosen and those
  // domains carry no discriminating information. This is the value diversity is
  // checked against.
  std::vector<FailureDomainId> domain_signature{};
  EvidenceClass weakest_evidence{EvidenceClass::kUnknown};

  friend bool operator==(const PathPlan&, const PathPlan&) = default;
};

// A planning stage. Stage boundaries come from the declared hierarchy levels of
// a hierarchical collective; a flat collective has exactly one stage.
struct Stage {
  std::uint32_t index{0};
  std::vector<LogicalEdgeId> logical_edges{};
  std::vector<PathId> paths{};

  friend bool operator==(const Stage&, const Stage&) = default;
};

// Capacity this plan would consume from each physical edge, in canonical edge
// order. This is a proposal: it is not admission and carries no authority.
struct Allocation {
  EdgeId edge{};
  std::uint64_t planned_mbps{0};

  friend bool operator==(const Allocation&, const Allocation&) = default;
};

struct PlanStats {
  std::uint64_t total_cost{0};
  std::uint64_t max_path_cost{0};
  std::size_t path_count{0};
  std::size_t hop_count{0};
  std::size_t logical_edge_count{0};
  std::uint64_t bottleneck_mbps{0};

  friend bool operator==(const PlanStats&, const PlanStats&) = default;
};

// Freshness of a plan with respect to a set of current generations. Only
// kCurrent means "this plan was produced from exactly these generations"; every
// other value means the plan must be regenerated or revalidated before use.
enum class PlanFreshness : std::uint8_t {
  kCurrent = 0,
  kStaleTopology = 1,
  kStaleFailureDomains = 2,
  kStaleCapacityEvidence = 3,
  kStalePolicy = 4,
  kStalePlanGeneration = 5,
  kUnverified = 6,  // reopened from storage and not yet revalidated
};

const char* to_string(PlanFreshness value) noexcept;

struct Plan {
  CollectivePlanId id{};        // equal to input_digest; stable identity
  Digest input_digest{};        // canonical request digest
  CollectivePlanGeneration generation{};
  GenerationBindings bindings{};
  CollectiveId collective{};
  CollectiveKind kind{CollectiveKind::kRing};
  std::uint32_t stage_count{1};
  std::vector<Stage> stages{};
  std::vector<PathPlan> paths{};
  std::vector<Allocation> allocations{};
  PlanStats stats{};
  EvidenceClass weakest_evidence{EvidenceClass::kUnknown};

  std::vector<std::uint8_t> encode() const;
  static Result<Plan> decode(std::span<const std::uint8_t> bytes);
  Digest body_digest() const;
};

// Freshness assessment against the generations reported by a live request.
PlanFreshness assess_freshness(const Plan& plan, const GenerationBindings& current) noexcept;
const char* describe_freshness(PlanFreshness freshness, ErrorCode& code) noexcept;

// Structural proof obligations for a produced plan. Every property the planner
// promises is re-checked here against the inputs it claims to have used:
//  - every hop references an existing physical edge;
//  - consecutive hops connect head-to-tail;
//  - the first and last hop endpoints equal the bound participant endpoints;
//  - no hop uses a forbidden node, tier or failure domain;
//  - hop count, path count and cost stay inside the policy limits;
//  - disjointness and domain diversity actually hold between sibling paths;
//  - the plan digest equals the canonical request digest.
Status validate_plan(const Plan& plan, const PlanningRequest& request);

// Replan and compare. Returns a fresh plan when the inputs still produce one.
Result<Plan> replan(const PlanningRequest& request);

}  // namespace cpath

#endif  // CPATH_PLAN_HPP
