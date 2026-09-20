// Collective Path Planner - plan explanation and comparison.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_EXPLAIN_HPP
#define CPATH_EXPLAIN_HPP

#include <string>
#include <vector>

#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/request.hpp"

namespace cpath {

// Human-readable plan explanation. When request is non-null the explanation
// also re-checks the plan against those inputs and reports any divergence.
std::string explain_plan(const Plan& plan, const PlanningRequest* request = nullptr);

// Human-readable denial explanation: one line per denial, with the constraint
// class and the bounded witness sets.
std::string explain_denials(const PlanningOutcome& outcome);

// Machine-readable rendering. Field order is fixed, strings are escaped, and
// the output is byte-stable for identical inputs.
std::string render_plan_json(const Plan& plan);
std::string render_outcome_json(const PlanningOutcome& outcome);
std::string render_freshness_json(const Plan& plan, PlanFreshness freshness);

// Difference between two plans over the same collective structure.
struct PathDifference {
  LogicalEdgeId logical_edge{};
  PathId left{};
  PathId right{};
  bool left_present{false};
  bool right_present{false};
  bool identical{false};
  std::string summary{};
};

struct PlanComparison {
  bool identical{false};
  bool same_input_digest{false};
  bool same_collective{false};
  bool left_is_replan_of_right{false};
  std::size_t changed_logical_edges{0};
  std::size_t added_paths{0};
  std::size_t removed_paths{0};
  std::size_t rewritten_paths{0};
  std::vector<PathDifference> differences{};
  std::string summary{};
};

PlanComparison compare_plans(const Plan& left, const Plan& right);

}  // namespace cpath

#endif  // CPATH_EXPLAIN_HPP
