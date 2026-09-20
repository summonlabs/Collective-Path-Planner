// Collective Path Planner - exhaustive reference solver for small instances.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// TEST INFRASTRUCTURE. This is a deliberately slow, exhaustively correct
// reference solver for SMALL instances. It shares no code with src/planner.cpp:
// it re-derives every hard constraint from the public model (PlanningRequest,
// FabricGraph, Policy, Collective, EndpointBinding) and never calls
// cpath::plan_collective.
//
// What it decides, exactly:
//   1. every simple physical path is enumerated for every logical edge
//      (depth-first with a visited set) subject to the hop limit, the cost
//      limit, static edge eligibility, the per-hop demand test and BOTH
//      endpoint bindings of the logical edge;
//   2. every k-subset (k = policy.paths_per_logical_edge) of that path set is
//      enumerated subject to the policy's disjointness, required failure-domain
//      diversity and the per-set capacity fit;
//   3. one set per logical edge is chosen by exhaustive depth-first search over
//      the whole request with capacity pruning, minimising the objective
//      lexicographically: (2) total path cost, (3) sibling pairs that are not
//      failure-domain independent, (4) the largest single path cost,
//      (5) the smallest remaining verified capacity headroom (maximised),
//      (6) the canonical path encoding (the sequence of hop edge indices,
//      compared lexicographically).
//
// Status discipline, mirroring the production planner's proof discipline:
//   kFeasible    a complete assignment was found and proved minimal;
//   kInfeasible  every enumeration and the global search ran to completion and
//                no assignment exists;
//   kLimitReached one of the declared ExactLimits was reached, so no claim is
//                made either way; detail names the bound that was hit.
//
// Level (5) is measured over the physical edges the assignment actually uses
// and that carry verified capacity: an edge nobody routes over has no
// "remaining headroom after this plan" to speak about, and including saturated
// edges that no path touches would make the level constant zero.
#ifndef CPATH_TEST_EXACT_SOLVER_HPP
#define CPATH_TEST_EXACT_SOLVER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cpath/ids.hpp"
#include "cpath/request.hpp"

namespace cpath_test {

// Bounds. Reaching one of them makes the enumeration inconclusive and produces
// ExactResult::Status::kLimitReached, never an infeasibility claim.
struct ExactLimits {
  std::size_t max_paths_per_edge{50000};
  std::size_t max_combinations_per_edge{2000000};
  std::size_t max_global_nodes{2000000};
};

struct ExactResult {
  enum class Status { kFeasible, kInfeasible, kLimitReached };

  Status status{Status::kLimitReached};
  std::uint64_t total_cost{0};        // objective level 2
  std::size_t domain_conflicts{0};    // objective level 3
  std::uint64_t max_path_cost{0};     // objective level 4
  std::size_t paths_enumerated{0};
  std::size_t combinations{0};
  std::size_t global_nodes{0};
  // The chosen simple path for every logical edge, as sequences of edge ids.
  // Logical edges appear in canonical order; each entry lists exactly
  // policy.paths_per_logical_edge paths, ordered canonically.
  std::vector<std::vector<std::vector<cpath::EdgeId>>> assignment{};
  std::string detail{};
};

// Stable name of the status, for diagnostics.
const char* to_string(ExactResult::Status status) noexcept;

// Solves the request exhaustively, or reports the bound that stopped it.
ExactResult solve_exact(const cpath::PlanningRequest& request, const ExactLimits& limits = {});

}  // namespace cpath_test

#endif  // CPATH_TEST_EXACT_SOLVER_HPP
