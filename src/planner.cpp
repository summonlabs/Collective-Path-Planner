// Collective Path Planner - deterministic constrained path planner.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// ALGORITHMIC CONTRACT
// --------------------
// The planner solves one declared problem class, and it is explicit about the
// boundary of what it can prove:
//
//   Given a directed multigraph (the physical fabric), a set of logical edges,
//   an endpoint binding per participant and a policy, choose for every logical
//   edge a set of paths_per_logical_edge SIMPLE paths between its bound
//   endpoints such that every hard constraint holds:
//     - hop count at most policy.max_hops;
//     - every hop is statically eligible (fabric eligibility, forbidden nodes,
//       forbidden tiers, the allowed tier set, forbidden failure domains and
//       their descendants, the evidence floor);
//     - every hop satisfies the allowed failure domains and tiers of BOTH
//       endpoint bindings of the logical edge;
//     - every hop has at least the per-path share of demand available, where
//       availability is verified capacity minus capacity already committed by
//       an adjacent authority (a zero-capacity edge is never capacity);
//     - the chosen set as a whole does not commit more than the verified spare
//       capacity of any physical edge, in this request or across the request;
//     - sibling paths satisfy the policy's disjointness (edge or node) and,
//       when required, failure-domain independence;
//     - every path cost stays inside policy.max_path_cost.
//
// The objective is lexicographic and total:
//     (1) satisfy every hard constraint;
//     (2) minimise the total path cost;
//     (3) minimise the number of sibling pairs that are not failure-domain
//         independent (only ever non-zero when diversity is kPreferred);
//     (4) minimise the largest single path cost;
//     (5) maximise the smallest remaining verified capacity headroom;
//     (6) break remaining ties by the canonical path encoding.
// The same total order is used for partial comparisons, so nothing depends on
// container iteration, hash order, thread scheduling or discovery order.
//
// PROOF DISCIPLINE
// ----------------
// "No mapping exists" and "I did not find one" are different results and are
// reported as different DenialKind values. A cone of search that is not
// exhaustive produces kSearchLimitReached; only an exhaustive one may produce
// kProvenInfeasible. Every hard constraint is checked against evidence, never
// against a planner assumption: see validate_plan in plan.cpp, which re-derives
// the whole contract from the request.
#include "cpath/planner.hpp"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpath {
namespace {

using NodeIndex = std::uint32_t;
using EdgeIndex = std::uint32_t;

// ---------------------------------------------------------------------------
// One simple physical path.
// ---------------------------------------------------------------------------
struct Candidate {
  std::vector<EdgeIndex> edges{};  // simple: no node and no edge repeats
  std::uint64_t cost{0};
  std::uint64_t bottleneck{0};
  EvidenceClass weakest{EvidenceClass::kMeasured};
  std::vector<FailureDomainId> signature{};
  bool domain_known{true};
};

// Canonical path order. Total, so it can be used for every comparison.
bool candidate_less(const Candidate& lhs, const Candidate& rhs) {
  if (lhs.cost != rhs.cost) {
    return lhs.cost < rhs.cost;
  }
  if (lhs.edges.size() != rhs.edges.size()) {
    return lhs.edges.size() < rhs.edges.size();
  }
  return lhs.edges < rhs.edges;
}

bool same_path(const Candidate& lhs, const Candidate& rhs) { return lhs.edges == rhs.edges; }

// ---------------------------------------------------------------------------
// Generation-stamped block sets: resetting is O(1) regardless of graph size.
// ---------------------------------------------------------------------------
class BlockSet {
 public:
  void resize(std::size_t nodes, std::size_t edges) {
    node_stamp_.assign(nodes, 0);
    edge_stamp_.assign(edges, 0);
    generation_ = 1;
  }

  void reset() {
    ++generation_;
    if (generation_ == 0) {
      std::fill(node_stamp_.begin(), node_stamp_.end(), 0);
      std::fill(edge_stamp_.begin(), edge_stamp_.end(), 0);
      generation_ = 1;
    }
  }

  void block_node(NodeIndex node) { node_stamp_[node] = generation_; }
  void block_edge(EdgeIndex edge) { edge_stamp_[edge] = generation_; }
  bool node_blocked(NodeIndex node) const { return node_stamp_[node] == generation_; }
  bool edge_blocked(EdgeIndex edge) const { return edge_stamp_[edge] == generation_; }

 private:
  std::vector<std::uint32_t> node_stamp_{};
  std::vector<std::uint32_t> edge_stamp_{};
  std::uint32_t generation_{1};
};

struct Label {
  std::uint64_t cost{0};
  std::uint32_t hops{0};
  NodeIndex node{0};
};

struct LabelGreater {
  bool operator()(const Label& lhs, const Label& rhs) const noexcept {
    if (lhs.cost != rhs.cost) {
      return lhs.cost > rhs.cost;
    }
    if (lhs.hops != rhs.hops) {
      return lhs.hops > rhs.hops;
    }
    return lhs.node > rhs.node;
  }
};

std::uint64_t state_key(NodeIndex node, std::uint32_t hops) noexcept {
  return (static_cast<std::uint64_t>(node) << 32u) | static_cast<std::uint64_t>(hops);
}

struct SearchOutcome {
  bool found{false};
  bool budget_exceeded{false};
  std::uint64_t cost{0};
  std::vector<EdgeIndex> edges{};
};

struct PlannerState {
  const PlanningRequest* request{nullptr};
  const FabricGraph* fabric{nullptr};
  const Policy* policy{nullptr};
  std::vector<std::uint8_t> node_allowed{};
  std::vector<std::uint8_t> edge_allowed{};
  // Cost of each physical edge. It depends only on evidence supplied by
  // adjacent authorities, never on this request's own commitments, so candidate
  // generation for one logical edge is completely independent of every other.
  std::vector<std::uint64_t> edge_cost{};
  std::vector<std::uint64_t> spare{};
  std::uint64_t per_path_demand{0};
  std::size_t hop_limit{0};
  std::size_t expansions{0};
  std::size_t max_expansions{0};
  std::size_t enumeration_steps{0};
  std::size_t enumeration_budget{0};
  bool budget_exceeded{false};
  bool enumeration_exhausted{false};
  BlockSet blocks{};
  std::unordered_map<std::uint64_t, std::uint64_t> dist{};
  std::unordered_map<std::uint64_t, std::pair<std::uint64_t, EdgeIndex>> prev{};
  std::priority_queue<Label, std::vector<Label>, LabelGreater> frontier{};
};

bool evidence_satisfies(EvidenceClass observed, EvidenceRequirement required) noexcept {
  switch (required) {
    case EvidenceRequirement::kAny:
      return true;
    case EvidenceRequirement::kSyntheticOrBetter:
      return observed != EvidenceClass::kUnknown;
    case EvidenceRequirement::kMeasured:
      return observed == EvidenceClass::kMeasured;
  }
  return false;
}

bool tier_forbidden(const Policy& policy, const TierId& tier) noexcept {
  if (!tier.valid()) {
    return false;
  }
  return std::binary_search(policy.forbidden_tiers.begin(), policy.forbidden_tiers.end(), tier);
}

bool domain_forbidden(const FabricGraph& fabric, const Policy& policy, const FailureDomainId& domain) noexcept {
  if (!domain.valid()) {
    return false;
  }
  for (const FailureDomainId& forbidden : policy.forbidden_failure_domains) {
    if (fabric.domain_contains(forbidden, domain)) {
      return true;
    }
  }
  return false;
}

// Static eligibility: properties of the edge alone under the policy.
bool edge_statically_usable(const PlannerState& state, EdgeIndex index) {
  if (state.edge_allowed[index] == 0) {
    return false;
  }
  const Edge& edge = state.fabric->edges()[index];
  if (edge.capacity_mbps == 0) {
    return state.policy->allow_unverified_capacity && edge.reserved_mbps == 0;
  }
  return edge.reserved_mbps < edge.capacity_mbps;
}

std::uint64_t edge_spare(const PlannerState& state, EdgeIndex index) {
  const Edge& edge = state.fabric->edges()[index];
  if (edge.capacity_mbps == 0 || edge.reserved_mbps >= edge.capacity_mbps) {
    return 0;
  }
  return edge.capacity_mbps - edge.reserved_mbps;
}

// The share of the declared demand this path must carry.
bool edge_carries_demand(const PlannerState& state, EdgeIndex index) {
  if (state.per_path_demand == 0) {
    return true;
  }
  const Edge& edge = state.fabric->edges()[index];
  if (edge.capacity_mbps == 0) {
    return true;  // unverified capacity: the policy already accepted the gap
  }
  return edge_spare(state, index) >= state.per_path_demand;
}

std::uint64_t edge_cost_of(const PlannerState& state, EdgeIndex index) {
  const Edge& edge = state.fabric->edges()[index];
  const Policy& policy = *state.policy;
  const std::uint64_t latency_term =
      saturating_mul(edge.latency_micros, policy.latency_weight_milli, kCostCeiling) / 1000u;
  if (policy.congestion_weight_milli == 0 || edge.capacity_mbps == 0) {
    return latency_term;
  }
  const std::uint64_t used = edge.reserved_mbps;
  if (used >= edge.capacity_mbps) {
    return kCostCeiling;
  }
  const std::uint64_t utilisation_milli = saturating_mul(used, 1000u, kCostCeiling) / edge.capacity_mbps;
  const std::uint64_t spare_milli = 1000u - utilisation_milli;
  const std::uint64_t penalty =
      saturating_mul(policy.congestion_weight_milli, utilisation_milli, kCostCeiling) /
      (spare_milli == 0 ? 1u : spare_milli);
  return saturating_add(latency_term, penalty, kCostCeiling);
}

bool binding_allows_edge(const PlannerState& state, const EndpointBinding& binding, const Edge& edge) {
  if (!binding.allowed_failure_domains.empty()) {
    if (!edge.failure_domain.valid()) {
      return false;
    }
    bool contained = false;
    for (const FailureDomainId& allowed : binding.allowed_failure_domains) {
      if (state.fabric->domain_contains(allowed, edge.failure_domain)) {
        contained = true;
        break;
      }
    }
    if (!contained) {
      return false;
    }
  }
  if (!binding.allowed_tiers.empty()) {
    if (!edge.tier.valid() ||
        !std::binary_search(binding.allowed_tiers.begin(), binding.allowed_tiers.end(), edge.tier)) {
      return false;
    }
  }
  return true;
}

FailureDomainId domain_at_level(const FabricGraph& fabric, const FailureDomainId& domain, DomainKind level) noexcept {
  if (!domain.valid()) {
    return FailureDomainId{};
  }
  for (const FailureDomainId& entry : fabric.domain_chain(domain)) {
    const FailureDomain* record = fabric.find_failure_domain(entry);
    if (record != nullptr && record->kind == level) {
      return entry;
    }
  }
  return domain;
}

// A hop is usable by a path carrying a logical edge when every filter passes.
bool hop_usable(const PlannerState& state, EdgeIndex index, const EndpointBinding& source_binding,
                const EndpointBinding& target_binding, NodeIndex next) {
  if (state.blocks.edge_blocked(index)) {
    return false;
  }
  if (state.node_allowed[next] == 0 || state.blocks.node_blocked(next)) {
    return false;
  }
  if (!edge_statically_usable(state, index) || !edge_carries_demand(state, index)) {
    return false;
  }
  const Edge& edge = state.fabric->edges()[index];
  if (!binding_allows_edge(state, source_binding, edge) || !binding_allows_edge(state, target_binding, edge)) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Hop-limited shortest SIMPLE path.
//
// The state space is (node, hops) and the frontier order is (cost, hops, node),
// so the first time the target is popped the label is minimal in (cost, hops).
// A walk that repeats a node contains a removable cycle, and removing it keeps
// the cost the same or lower while strictly reducing the hop count, which
// contradicts minimality. The returned path is therefore always simple; the
// planner asserts that again when it materialises the candidate.
// ---------------------------------------------------------------------------
SearchOutcome shortest_path(PlannerState& state, NodeIndex source, NodeIndex target,
                            const EndpointBinding& source_binding, const EndpointBinding& target_binding) {
  SearchOutcome outcome;
  if (source == target) {
    outcome.found = true;
    return outcome;
  }
  if (state.node_allowed[source] == 0 || state.node_allowed[target] == 0) {
    return outcome;
  }

  state.dist.clear();
  state.prev.clear();
  while (!state.frontier.empty()) {
    state.frontier.pop();
  }

  const std::size_t max_hops = state.hop_limit;
  const std::uint64_t start_key = state_key(source, 0);
  state.dist.emplace(start_key, 0);
  state.frontier.push(Label{0, 0, source});

  while (!state.frontier.empty()) {
    const Label label = state.frontier.top();
    state.frontier.pop();

    const std::uint64_t key = state_key(label.node, label.hops);
    const auto known = state.dist.find(key);
    if (known == state.dist.end() || known->second < label.cost) {
      continue;
    }
    if (label.node == target) {
      outcome.found = true;
      outcome.cost = label.cost;
      std::uint64_t cursor = key;
      while (cursor != start_key) {
        const auto step = state.prev.find(cursor);
        if (step == state.prev.end()) {
          outcome.found = false;
          outcome.edges.clear();
          return outcome;
        }
        outcome.edges.push_back(step->second.second);
        cursor = step->second.first;
      }
      std::reverse(outcome.edges.begin(), outcome.edges.end());
      return outcome;
    }

    if (state.expansions >= state.max_expansions) {
      state.budget_exceeded = true;
      outcome.budget_exceeded = true;
      state.dist.clear();
      state.prev.clear();
      return outcome;
    }
    ++state.expansions;

    if (label.hops >= max_hops) {
      continue;
    }
    const std::vector<std::uint32_t>& outgoing = state.fabric->out_edges(label.node);
    for (const EdgeIndex edge_index : outgoing) {
      const Edge& edge = state.fabric->edges()[edge_index];
      const NodeIndex next = state.fabric->node_index(edge.to);
      if (next == kInvalidIndex) {
        continue;
      }
      if (!hop_usable(state, edge_index, source_binding, target_binding, next)) {
        continue;
      }
      const std::uint64_t next_cost = saturating_add(label.cost, state.edge_cost[edge_index], kCostCeiling);
      if (next_cost > state.policy->max_path_cost) {
        continue;  // costs are non-negative, so every extension would also exceed the limit
      }
      const std::uint64_t next_key = state_key(next, label.hops + 1u);
      const auto existing = state.dist.find(next_key);
      if (existing == state.dist.end() || next_cost < existing->second) {
        state.dist[next_key] = next_cost;
        state.prev[next_key] = std::make_pair(key, edge_index);
        state.frontier.push(Label{next_cost, label.hops + 1u, next});
      }
    }
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Exhaustive enumeration of the simple paths a logical edge may use.
//
// Depth first, visiting outgoing edges in canonical order, so the enumeration
// order is itself deterministic. It stops early at a declared bound, and says
// so: only an enumeration that ran to completion may be used as the basis of an
// infeasibility proof.
// ---------------------------------------------------------------------------
struct Enumeration {
  std::vector<std::vector<EdgeIndex>> paths{};
  bool exhaustive{false};
  std::size_t steps{0};
  bool step_limited{false};
};


// One depth-limited sweep: records exactly the simple paths of length
// "remaining" edges that end at the target. The target is never traversed, so a
// path can never pass through its own endpoint.
void enumerate_depth(PlannerState& state, NodeIndex node, NodeIndex target, const EndpointBinding& source_binding,
                     const EndpointBinding& target_binding, std::size_t remaining,
                     std::vector<std::uint8_t>& visited, std::vector<EdgeIndex>& current, std::uint64_t cost,
                     Enumeration& out, std::size_t max_paths, std::size_t max_steps) {
  if (remaining == 0) {
    return;
  }
  for (const EdgeIndex edge_index : state.fabric->out_edges(node)) {
    if (out.steps >= max_steps || out.paths.size() > max_paths) {
      return;
    }
    // Enumeration has its own budget, charged across the request but separate
    // from the search budget. Reaching it costs proof strength (the path set is
    // no longer exhaustive) and never costs feasibility: the heuristic pool is
    // still built afterwards.
    if (state.enumeration_steps >= state.enumeration_budget) {
      state.enumeration_exhausted = true;
      out.step_limited = true;
      return;
    }
    ++state.enumeration_steps;
    ++out.steps;
    const Edge& edge = state.fabric->edges()[edge_index];
    const NodeIndex next = state.fabric->node_index(edge.to);
    if (next == kInvalidIndex || next == node) {
      continue;
    }
    if (!hop_usable(state, edge_index, source_binding, target_binding, next)) {
      continue;
    }
    if (visited[next] != 0) {
      continue;
    }
    const std::uint64_t next_cost = saturating_add(cost, state.edge_cost[edge_index], kCostCeiling);
    if (next_cost > state.policy->max_path_cost) {
      continue;
    }
    if (next == target) {
      if (remaining == 1u) {
        current.push_back(edge_index);
        out.paths.push_back(current);
        current.pop_back();
      }
      continue;  // never route through the destination
    }
    if (remaining == 1u) {
      continue;
    }
    current.push_back(edge_index);
    visited[next] = 1;
    enumerate_depth(state, next, target, source_binding, target_binding, remaining - 1u, visited, current, next_cost,
                    out, max_paths, max_steps);
    visited[next] = 0;
    current.pop_back();
  }
}

// Enumerates by INCREASING hop count. Depth-first order would find the deepest
// routes first, so a bounded sample would consist of long paths and the cheap
// short routes would never enter the pool; deepening guarantees that shorter
// routes are found, and therefore sampled, first.
void enumerate_paths(PlannerState& state, NodeIndex source, NodeIndex target,
                     const EndpointBinding& source_binding, const EndpointBinding& target_binding,
                     Enumeration& out, std::size_t max_paths, std::size_t max_steps) {
  std::vector<std::uint8_t> visited(state.fabric->node_count(), 0u);
  std::vector<EdgeIndex> current;
  const std::size_t deepest = std::min<std::size_t>(state.hop_limit, state.fabric->node_count());
  for (std::size_t limit = 1; limit <= deepest; ++limit) {
    visited.assign(state.fabric->node_count(), 0u);
    visited[source] = 1;
    current.clear();
    enumerate_depth(state, source, target, source_binding, target_binding, limit, visited, current, 0, out,
                    max_paths, max_steps);
    if (out.steps >= max_steps || out.paths.size() > max_paths ||
        state.enumeration_steps >= state.enumeration_budget) {
      out.step_limited = out.step_limited || out.steps >= max_steps ||
                         state.enumeration_steps >= state.enumeration_budget;
      return;
    }
  }
  out.exhaustive = true;
}

// ---------------------------------------------------------------------------
// Candidate materialisation.
// ---------------------------------------------------------------------------
bool edges_form_simple_path(const FabricGraph& fabric, const std::vector<EdgeIndex>& edges, NodeIndex source) {
  // A simple path visits every node at most once and uses every edge at most
  // once. The search cannot produce anything else; this is the check that keeps
  // that promise honest if the search is ever changed.
  std::vector<NodeIndex> seen;
  seen.reserve(edges.size() + 1u);
  seen.push_back(source);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const NodeIndex next = fabric.node_index(fabric.edges()[edges[index]].to);
    if (next == kInvalidIndex) {
      return false;
    }
    if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (edges[previous] == edges[index]) {
        return false;  // the same physical edge twice
      }
    }
    seen.push_back(next);
  }
  return true;
}

Candidate make_candidate(const PlannerState& state, const std::vector<EdgeIndex>& edges, NodeIndex source, bool* ok) {
  const FabricGraph& fabric = *state.fabric;
  const Policy& policy = *state.policy;
  Candidate candidate;
  candidate.edges = edges;
  *ok = edges.size() <= policy.max_hops && edges_form_simple_path(fabric, edges, source);
  if (!*ok) {
    return candidate;
  }
  candidate.cost = 0;
  candidate.bottleneck = kCostCeiling;
  candidate.weakest = EvidenceClass::kMeasured;
  std::vector<FailureDomainId> hop_domains;
  hop_domains.reserve(edges.size());

  for (const EdgeIndex index : edges) {
    const Edge& edge = fabric.edges()[index];
    candidate.cost = saturating_add(candidate.cost, state.edge_cost[index], kCostCeiling);
    candidate.bottleneck = std::min(candidate.bottleneck, edge_spare(state, index));
    if (static_cast<std::uint8_t>(edge.evidence) < static_cast<std::uint8_t>(candidate.weakest)) {
      candidate.weakest = edge.evidence;
    }
    if (!edge.failure_domain.valid()) {
      candidate.domain_known = false;
    } else if (policy.domain_diversity != DomainDiversity::kNone) {
      const FailureDomainId level_domain =
          domain_at_level(fabric, edge.failure_domain, policy.domain_diversity_level);
      if (!level_domain.valid()) {
        candidate.domain_known = false;
      } else {
        hop_domains.push_back(level_domain);
      }
    }
  }
  if (candidate.bottleneck == kCostCeiling) {
    candidate.bottleneck = 0;
  }
  // Diversity is judged on the domains a path adds beyond the ones its endpoints
  // unavoidably sit in: both sibling paths must touch the source and destination
  // attachment domains, so those carry no discriminating information.
  if (policy.domain_diversity != DomainDiversity::kNone && !hop_domains.empty()) {
    const FailureDomainId first_domain = hop_domains.front();
    const FailureDomainId last_domain = hop_domains.back();
    bool removed_first = false;
    bool removed_last = false;
    for (const FailureDomainId& domain : hop_domains) {
      if (!removed_first && domain == first_domain) {
        removed_first = true;
        continue;
      }
      if (!removed_last && domain == last_domain) {
        removed_last = true;
        continue;
      }
      candidate.signature.push_back(domain);
    }
  }
  std::sort(candidate.signature.begin(), candidate.signature.end());
  candidate.signature.erase(std::unique(candidate.signature.begin(), candidate.signature.end()),
                            candidate.signature.end());
  return candidate;
}

bool paths_edge_disjoint(const Candidate& lhs, const Candidate& rhs) {
  for (const EdgeIndex left : lhs.edges) {
    if (std::find(rhs.edges.begin(), rhs.edges.end(), left) != rhs.edges.end()) {
      return false;
    }
  }
  return true;
}

bool paths_node_disjoint(const FabricGraph& fabric, const Candidate& lhs, const Candidate& rhs) {
  // Siblings share their source and destination by definition; only an interior
  // node in common is a diversity violation.
  for (std::size_t left = 0; left + 1u < lhs.edges.size(); ++left) {
    const NodeId& left_node = fabric.edges()[lhs.edges[left]].to;
    for (std::size_t right = 0; right + 1u < rhs.edges.size(); ++right) {
      if (left_node == fabric.edges()[rhs.edges[right]].to) {
        return false;
      }
    }
  }
  return true;
}

bool domains_independent(const Candidate& lhs, const Candidate& rhs) {
  if (!lhs.domain_known || !rhs.domain_known) {
    return false;
  }
  for (const FailureDomainId& left : lhs.signature) {
    if (std::binary_search(rhs.signature.begin(), rhs.signature.end(), left)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// A complete candidate answer for ONE logical edge: exactly
// paths_per_logical_edge sibling paths plus everything needed to compare it and
// to check its capacity footprint against the fabric.
// ---------------------------------------------------------------------------
struct PathSet {
  std::vector<Candidate> paths{};
  std::uint64_t total_cost{0};
  std::uint64_t max_path_cost{0};
  std::uint64_t headroom{kCostCeiling};
  std::size_t domain_conflicts{0};
  std::vector<std::pair<EdgeIndex, std::uint64_t>> usage{};
  std::vector<std::uint64_t> signature{};
};

// The objective, level by level. Used for partial comparisons too, so the total
// order is the same everywhere.
bool path_set_less(const PathSet& lhs, const PathSet& rhs) {
  if (lhs.total_cost != rhs.total_cost) {
    return lhs.total_cost < rhs.total_cost;
  }
  if (lhs.domain_conflicts != rhs.domain_conflicts) {
    return lhs.domain_conflicts < rhs.domain_conflicts;
  }
  if (lhs.max_path_cost != rhs.max_path_cost) {
    return lhs.max_path_cost < rhs.max_path_cost;
  }
  if (lhs.headroom != rhs.headroom) {
    return lhs.headroom > rhs.headroom;  // more remaining headroom is better
  }
  return lhs.signature < rhs.signature;
}

PathSet build_path_set(const PlannerState& state, const std::vector<Candidate>& chosen, std::uint64_t per_path_demand) {
  PathSet set;
  set.paths = chosen;
  std::sort(set.paths.begin(), set.paths.end(), candidate_less);
  std::vector<std::pair<EdgeIndex, std::uint64_t>> usage;
  for (const Candidate& path : set.paths) {
    set.total_cost = saturating_add(set.total_cost, path.cost, kCostCeiling);
    set.max_path_cost = std::max(set.max_path_cost, path.cost);
    set.signature.insert(set.signature.end(), path.edges.begin(), path.edges.end());
    for (const EdgeIndex edge : path.edges) {
      if (state.fabric->edges()[edge].capacity_mbps == 0) {
        continue;  // unverified capacity is not capacity
      }
      bool merged = false;
      for (auto& entry : usage) {
        if (entry.first == edge) {
          entry.second = saturating_add(entry.second, per_path_demand, kCostCeiling);
          merged = true;
          break;
        }
      }
      if (!merged) {
        usage.emplace_back(edge, per_path_demand);
      }
    }
  }
  std::sort(usage.begin(), usage.end());
  set.usage = std::move(usage);
  set.headroom = kCostCeiling;
  for (const auto& entry : set.usage) {
    const std::uint64_t spare = edge_spare(state, entry.first);
    const std::uint64_t left = entry.second >= spare ? 0u : spare - entry.second;
    set.headroom = std::min(set.headroom, left);
  }
  set.domain_conflicts = 0;
  if (state.policy->domain_diversity != DomainDiversity::kNone) {
    for (std::size_t i = 0; i < set.paths.size(); ++i) {
      for (std::size_t j = i + 1u; j < set.paths.size(); ++j) {
        if (!domains_independent(set.paths[i], set.paths[j])) {
          ++set.domain_conflicts;
        }
      }
    }
  }
  return set;
}

// Why a candidate set was rejected. Tracked so that a proven infeasibility can
// name the constraint that actually did the work.
enum class SetReject : std::uint8_t { kNone = 0, kDisjointness, kDomainDiversity, kCapacity };

struct SetEnumeration {
  std::vector<PathSet> best{};
  std::size_t accepted{0};
  std::size_t considered{0};
  bool exhaustive{false};
  bool step_limited{false};
  std::size_t rejected_disjointness{0};
  std::size_t rejected_domain{0};
  std::size_t rejected_capacity{0};
};

void offer_set(SetEnumeration& out, PathSet&& set, std::size_t cap) {
  ++out.accepted;
  const auto position = std::lower_bound(out.best.begin(), out.best.end(), set, path_set_less);
  out.best.insert(position, std::move(set));
  if (out.best.size() > cap) {
    out.best.pop_back();
  }
}

void enumerate_sets(const PlannerState& state, const std::vector<Candidate>& pool, std::size_t wanted,
                    std::size_t index, std::vector<const Candidate*>& chosen, SetEnumeration& out, std::size_t cap,
                    std::size_t budget) {
  if (out.considered >= budget) {
    out.step_limited = true;
    return;
  }
  if (chosen.size() == wanted) {
    ++out.considered;
    std::vector<Candidate> materialised;
    materialised.reserve(chosen.size());
    for (const Candidate* candidate : chosen) {
      materialised.push_back(*candidate);
    }
    PathSet set = build_path_set(state, materialised, state.per_path_demand);
    // Capacity footprint of the set alone, against the fabric's spare capacity.
    for (const auto& entry : set.usage) {
      if (entry.second > edge_spare(state, entry.first)) {
        ++out.rejected_capacity;
        return;
      }
    }
    offer_set(out, std::move(set), cap);
    return;
  }
  if (index >= pool.size()) {
    return;
  }
  const std::size_t remaining_needed = wanted - chosen.size();
  if (pool.size() - index < remaining_needed) {
    return;
  }
  for (std::size_t candidate_index = index; candidate_index < pool.size(); ++candidate_index) {
    if (out.considered >= budget) {
      out.step_limited = true;
      return;
    }
    const Candidate& candidate = pool[candidate_index];
    bool compatible = true;
    SetReject reject = SetReject::kNone;
    for (const Candidate* existing : chosen) {
      if (state.policy->disjointness != Disjointness::kNone &&
          !paths_edge_disjoint(*existing, candidate)) {
        compatible = false;
        reject = SetReject::kDisjointness;
        break;
      }
      if (state.policy->disjointness == Disjointness::kNode &&
          !paths_node_disjoint(*state.fabric, *existing, candidate)) {
        compatible = false;
        reject = SetReject::kDisjointness;
        break;
      }
      if (state.policy->domain_diversity == DomainDiversity::kRequired &&
          !domains_independent(*existing, candidate)) {
        compatible = false;
        reject = SetReject::kDomainDiversity;
        break;
      }
      if (state.policy->domain_diversity == DomainDiversity::kRequired && chosen.size() > 1u &&
          !candidate.domain_known) {
        compatible = false;
        reject = SetReject::kDomainDiversity;
        break;
      }
    }
    if (!compatible) {
      ++out.considered;
      if (reject == SetReject::kDisjointness) {
        ++out.rejected_disjointness;
      } else if (reject == SetReject::kDomainDiversity) {
        ++out.rejected_domain;
      }
      continue;
    }
    chosen.push_back(&candidate);
    enumerate_sets(state, pool, wanted, candidate_index + 1u, chosen, out, cap, budget);
    chosen.pop_back();
  }
}

// ---------------------------------------------------------------------------
// Unit-capacity max-flow. Used only to CERTIFY that a disjointness requirement
// is impossible, so its result is reported as a proof rather than a guess.
// ---------------------------------------------------------------------------
class MaxFlow {
 public:
  explicit MaxFlow(std::size_t nodes) : graph_(nodes) {}

  void add_arc(std::size_t from, std::size_t to, std::uint32_t capacity) {
    graph_[from].push_back(Arc{to, capacity, static_cast<std::uint32_t>(graph_[to].size())});
    graph_[to].push_back(Arc{from, 0, static_cast<std::uint32_t>(graph_[from].size() - 1u)});
  }

  std::uint32_t run(std::size_t source, std::size_t sink, std::uint32_t limit) {
    std::uint32_t flow = 0;
    while (flow < limit) {
      level_.assign(graph_.size(), kUnreachableLevel);
      std::vector<std::size_t> queue;
      queue.reserve(graph_.size());
      level_[source] = 0;
      queue.push_back(source);
      for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t node = queue[head];
        for (const Arc& arc : graph_[node]) {
          if (arc.capacity > 0 && level_[arc.to] == kUnreachableLevel) {
            level_[arc.to] = level_[node] + 1u;
            queue.push_back(arc.to);
          }
        }
      }
      if (level_[sink] == kUnreachableLevel) {
        break;
      }
      cursor_.assign(graph_.size(), 0);
      while (flow < limit) {
        const std::uint32_t pushed = augment(source, sink, limit - flow);
        if (pushed == 0) {
          break;
        }
        flow += pushed;
      }
    }
    return flow;
  }

 private:
  struct Arc {
    std::size_t to;
    std::uint32_t capacity;
    std::uint32_t reverse;
  };

  static constexpr std::uint32_t kUnreachableLevel = 0xFFFFFFFFu;

  std::uint32_t augment(std::size_t node, std::size_t sink, std::uint32_t limit) {
    if (node == sink) {
      return limit;
    }
    for (; cursor_[node] < graph_[node].size(); ++cursor_[node]) {
      Arc& arc = graph_[node][cursor_[node]];
      if (arc.capacity == 0 || level_[arc.to] != level_[node] + 1u) {
        continue;
      }
      const std::uint32_t pushed = augment(arc.to, sink, std::min(limit, arc.capacity));
      if (pushed > 0) {
        arc.capacity -= pushed;
        graph_[arc.to][arc.reverse].capacity += pushed;
        return pushed;
      }
    }
    return 0;
  }

  std::vector<std::vector<Arc>> graph_{};
  std::vector<std::uint32_t> level_{};
  std::vector<std::size_t> cursor_{};
};

std::uint32_t max_disjoint_paths(const PlannerState& state, NodeIndex source, NodeIndex target, bool node_disjoint,
                                 const EndpointBinding& source_binding, const EndpointBinding& target_binding,
                                 std::uint32_t limit) {
  const FabricGraph& fabric = *state.fabric;
  const std::size_t multiplier = node_disjoint ? 2u : 1u;
  MaxFlow flow(fabric.node_count() * multiplier);
  const auto in_index = [](NodeIndex node) { return static_cast<std::size_t>(node) * 2u; };
  const auto out_index = [](NodeIndex node) { return static_cast<std::size_t>(node) * 2u + 1u; };
  const auto plain = [](NodeIndex node) { return static_cast<std::size_t>(node); };

  if (node_disjoint) {
    for (NodeIndex node = 0; node < static_cast<NodeIndex>(fabric.node_count()); ++node) {
      if (state.node_allowed[node] == 0) {
        continue;
      }
      std::uint32_t capacity = 1;
      if (node == source || node == target) {
        capacity = limit;
      }
      flow.add_arc(in_index(node), out_index(node), capacity);
    }
  }
  for (EdgeIndex edge_index = 0; edge_index < static_cast<EdgeIndex>(fabric.edge_count()); ++edge_index) {
    if (!edge_statically_usable(state, edge_index) || !edge_carries_demand(state, edge_index)) {
      continue;
    }
    const Edge& edge = fabric.edges()[edge_index];
    if (!binding_allows_edge(state, source_binding, edge) || !binding_allows_edge(state, target_binding, edge)) {
      continue;
    }
    const NodeIndex from = fabric.node_index(edge.from);
    const NodeIndex to = fabric.node_index(edge.to);
    if (from == kInvalidIndex || to == kInvalidIndex) {
      continue;
    }
    if (state.node_allowed[from] == 0 || state.node_allowed[to] == 0) {
      continue;
    }
    const std::size_t tail = node_disjoint ? out_index(from) : plain(from);
    const std::size_t head = node_disjoint ? in_index(to) : plain(to);
    flow.add_arc(tail, head, 1);
  }
  const std::size_t flow_source = node_disjoint ? out_index(source) : plain(source);
  const std::size_t flow_sink = node_disjoint ? in_index(target) : plain(target);
  return flow.run(flow_source, flow_sink, limit);
}

// ---------------------------------------------------------------------------
// Per-logical-edge analysis.
// ---------------------------------------------------------------------------
struct Reachability {
  bool reached{false};
  std::size_t hops{0};
};

Reachability bfs_min_hops(const PlannerState& state, NodeIndex source, NodeIndex target,
                          const EndpointBinding& source_binding, const EndpointBinding& target_binding) {
  Reachability result;
  if (source == target) {
    result.reached = true;
    return result;
  }
  const std::size_t node_count = state.fabric->node_count();
  std::vector<std::uint32_t> distance(node_count, 0xFFFFFFFFu);
  std::vector<NodeIndex> queue;
  queue.reserve(node_count);
  distance[source] = 0;
  queue.push_back(source);
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const NodeIndex node = queue[head];
    for (const EdgeIndex edge_index : state.fabric->out_edges(node)) {
      const Edge& edge = state.fabric->edges()[edge_index];
      const NodeIndex next = state.fabric->node_index(edge.to);
      if (next == kInvalidIndex || distance[next] != 0xFFFFFFFFu) {
        continue;
      }
      if (!hop_usable(state, edge_index, source_binding, target_binding, next)) {
        continue;
      }
      distance[next] = distance[node] + 1u;
      if (next == target) {
        result.reached = true;
        result.hops = distance[next];
        return result;
      }
      queue.push_back(next);
    }
  }
  return result;
}

struct EdgePlan {
  LogicalEdgeId id{};
  ParticipantId src{};
  ParticipantId dst{};
  std::uint32_t stage{0};
  bool failed{false};
  ErrorCode code{ErrorCode::kNoPath};
  DenialKind kind{DenialKind::kProvenInfeasible};
  ConflictKind conflict{ConflictKind::kDisconnected};
  std::string message{};
  std::size_t candidates_considered{0};
  std::size_t paths_enumerated{0};
  bool enumeration_exhaustive{false};
  std::vector<PathSet> alternatives{};
  bool sets_exhaustive{false};
  std::vector<NodeId> witness_nodes{};
  std::vector<EdgeId> witness_edges{};
};

void diagnose_unreachable(const PlannerState& state, NodeIndex source, NodeIndex target,
                          const EndpointBinding& source_binding, const EndpointBinding& target_binding,
                          EdgePlan& plan) {
  const FabricGraph& fabric = *state.fabric;
  const Node& source_node = fabric.nodes()[source];
  const Node& target_node = fabric.nodes()[target];
  const std::string endpoints = " from " + source_node.id.value() + " to " + target_node.id.value();
  std::size_t total = 0;
  std::size_t by_node = 0;
  std::size_t by_tier = 0;
  std::size_t by_domain = 0;
  std::size_t by_evidence = 0;
  std::size_t by_capacity = 0;
  std::size_t by_binding = 0;

  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    const Edge& edge = fabric.edges()[index];
    if (!(edge.from == source_node.id)) {
      continue;
    }
    ++total;
    if (plan.witness_edges.size() < kMaxDenialWitness) {
      plan.witness_edges.push_back(edge.id);
    }
    if (!edge.eligible) {
      ++by_node;
      continue;
    }
    if (tier_forbidden(*state.policy, edge.tier) ||
        (!state.policy->allowed_path_tiers.empty() &&
         (!edge.tier.valid() ||
          !std::binary_search(state.policy->allowed_path_tiers.begin(), state.policy->allowed_path_tiers.end(),
                              edge.tier)))) {
      ++by_tier;
      continue;
    }
    if (domain_forbidden(fabric, *state.policy, edge.failure_domain)) {
      ++by_domain;
      continue;
    }
    if (!evidence_satisfies(edge.evidence, state.policy->evidence)) {
      ++by_evidence;
      continue;
    }
    if (edge.capacity_mbps == 0) {
      if (!state.policy->allow_unverified_capacity || edge.reserved_mbps != 0) {
        ++by_capacity;
        continue;
      }
    } else if (edge.reserved_mbps >= edge.capacity_mbps) {
      ++by_capacity;
      continue;
    }
    if (state.per_path_demand > 0 && edge.capacity_mbps > 0 &&
        edge_spare(state, static_cast<EdgeIndex>(index)) < state.per_path_demand) {
      ++by_capacity;
      continue;
    }
    if (!binding_allows_edge(state, source_binding, edge) ||
        !binding_allows_edge(state, target_binding, edge)) {
      ++by_binding;
      continue;
    }
    const NodeIndex next = fabric.node_index(edge.to);
    if (next == kInvalidIndex || state.node_allowed[next] == 0) {
      ++by_node;
      continue;
    }
  }

  if (total == 0) {
    plan.code = ErrorCode::kNoPath;
    plan.conflict = ConflictKind::kDisconnected;
    plan.message = "no physical edge leaves the endpoint bound to " + source_node.id.value() + endpoints;
    return;
  }
  // Most specific cause first: report the narrowest filter that removed
  // everything, not the union of all of them.
  if (by_node == total) {
    plan.code = ErrorCode::kNodeForbidden;
    plan.conflict = ConflictKind::kForbiddenNode;
    plan.message = "every edge leaving the source endpoint is ineligible or lands on a forbidden node" + endpoints;
    return;
  }
  if (by_tier + by_node == total) {
    plan.code = ErrorCode::kTierForbidden;
    plan.conflict = ConflictKind::kForbiddenTier;
    plan.message = "every edge leaving the source endpoint is excluded by the tier policy" + endpoints;
    return;
  }
  if (by_domain + by_tier + by_node == total) {
    plan.code = ErrorCode::kFailureDomainForbidden;
    plan.conflict = ConflictKind::kForbiddenFailureDomain;
    plan.message = "every edge leaving the source endpoint is excluded by the failure-domain policy" + endpoints;
    return;
  }
  if (by_evidence + by_domain + by_tier + by_node == total) {
    plan.code = ErrorCode::kCapacityEvidenceInsufficient;
    plan.conflict = ConflictKind::kEvidenceInsufficient;
    plan.message = "every edge leaving the source endpoint has evidence below the required class" + endpoints;
    return;
  }
  if (by_capacity + by_evidence + by_domain + by_tier + by_node == total) {
    plan.code = ErrorCode::kEdgeHasNoVerifiedCapacity;
    plan.conflict = ConflictKind::kNoVerifiedCapacity;
    plan.message = "every edge leaving the source endpoint lacks verified spare capacity" + endpoints;
    return;
  }
  if (by_binding + by_capacity + by_evidence + by_domain + by_tier + by_node == total) {
    plan.message =
        "every edge leaving the source endpoint is excluded by the endpoint binding constraints" + endpoints;
    plan.code = ErrorCode::kEndpointIneligible;
    plan.conflict = ConflictKind::kEndpointConstraint;
    return;
  }
  plan.code = ErrorCode::kNoPath;
  plan.conflict = ConflictKind::kDisconnected;
  plan.message = "the destination endpoint is not reachable under the policy" + endpoints;
}

// Builds the candidate pool for one logical edge. When the exhaustive
// enumeration completed, the pool IS the feasible path set. Otherwise it is a
// bounded, deliberately diverse sample, and every result derived from it records
// that it is not exhaustive.
void build_pool(PlannerState& state, NodeIndex source, NodeIndex target, const EndpointBinding& source_binding,
                const EndpointBinding& target_binding, const Enumeration& enumeration,
                const PlannerLimits& limits, std::vector<Candidate>& pool, std::size_t& enumerated_candidates) {
  const bool exhaustive = enumeration.exhaustive;
  const std::size_t cap = exhaustive ? limits.max_exhaustive_paths : limits.max_candidate_paths;
  std::vector<Candidate> candidates;
  candidates.reserve(enumeration.paths.size());
  for (const std::vector<EdgeIndex>& path : enumeration.paths) {
    bool ok = true;
    Candidate candidate = make_candidate(state, path, source, &ok);
    if (ok) {
      candidates.push_back(std::move(candidate));
    }
  }
  std::sort(candidates.begin(), candidates.end(), candidate_less);
  candidates.erase(std::unique(candidates.begin(), candidates.end(),
                               [](const Candidate& lhs, const Candidate& rhs) { return same_path(lhs, rhs); }),
                   candidates.end());
  enumerated_candidates = candidates.size();

  if (exhaustive) {
    // An exhaustive enumeration that respects the cap has every feasible path,
    // so the pool IS the feasible path set.
    pool = std::move(candidates);
    return;
  }

  const auto already_present = [&pool](const Candidate& candidate) {
    return std::any_of(pool.begin(), pool.end(),
                       [&candidate](const Candidate& entry) { return same_path(entry, candidate); });
  };
  const auto consider = [&](const SearchOutcome& found) {
    if (!found.found || pool.size() >= cap) {
      return;
    }
    bool ok = true;
    Candidate candidate = make_candidate(state, found.edges, source, &ok);
    if (!ok || already_present(candidate)) {
      return;
    }
    pool.push_back(std::move(candidate));
  };

  // The non-exhaustive sample is built in priority order: the cheapest route and
  // the greedy diversity chain come first, so that the candidates which make a
  // disjoint or domain-independent set discoverable can never be crowded out by
  // cheaper but mutually similar alternatives. Only then are the remaining
  // cheapest enumerated routes and the resource-blocking alternatives added.
  {
    state.blocks.reset();
    const SearchOutcome best = shortest_path(state, source, target, source_binding, target_binding);
    bool ok = true;
    Candidate cheapest = make_candidate(state, best.edges, source, &ok);
    if (best.found && ok) {
      pool.push_back(std::move(cheapest));
    } else if (!candidates.empty()) {
      pool.push_back(candidates.front());
    }
  }
  if (!pool.empty()) {
    std::vector<EdgeIndex> blocked_edges = pool.front().edges;
    std::vector<NodeIndex> blocked_nodes;
    const bool node_disjoint = state.policy->disjointness == Disjointness::kNode;
    if (node_disjoint) {
      for (std::size_t index = 0; index + 1u < pool.front().edges.size(); ++index) {
        blocked_nodes.push_back(state.fabric->node_index(state.fabric->edges()[pool.front().edges[index]].to));
      }
    }
    const auto search_blocked = [&](const std::vector<EdgeIndex>& edges, const std::vector<NodeIndex>& nodes) {
      state.blocks.reset();
      for (const EdgeIndex edge : edges) {
        state.blocks.block_edge(edge);
      }
      for (const NodeIndex node : nodes) {
        state.blocks.block_node(node);
      }
      return shortest_path(state, source, target, source_binding, target_binding);
    };
    for (std::size_t step = 0; step + 1u < state.policy->paths_per_logical_edge && pool.size() < cap; ++step) {
      const SearchOutcome next = search_blocked(blocked_edges, blocked_nodes);
      if (!next.found) {
        break;
      }
      blocked_edges.insert(blocked_edges.end(), next.edges.begin(), next.edges.end());
      if (node_disjoint) {
        for (std::size_t index = 0; index + 1u < next.edges.size(); ++index) {
          blocked_nodes.push_back(state.fabric->node_index(state.fabric->edges()[next.edges[index]].to));
        }
      }
      consider(next);
    }
    std::vector<Candidate> extra;
    for (const EdgeIndex edge : pool.front().edges) {
      const SearchOutcome found = search_blocked({edge}, {});
      if (!found.found) {
        continue;
      }
      bool ok = true;
      Candidate candidate = make_candidate(state, found.edges, source, &ok);
      if (!ok || already_present(candidate) ||
          std::any_of(extra.begin(), extra.end(),
                      [&candidate](const Candidate& entry) { return same_path(entry, candidate); })) {
        continue;
      }
      extra.push_back(std::move(candidate));
    }
    if (node_disjoint) {
      for (const NodeIndex node : blocked_nodes) {
        const SearchOutcome found = search_blocked({}, {node});
        if (!found.found) {
          continue;
        }
        bool ok = true;
        Candidate candidate = make_candidate(state, found.edges, source, &ok);
        if (!ok || already_present(candidate) ||
            std::any_of(extra.begin(), extra.end(),
                        [&candidate](const Candidate& entry) { return same_path(entry, candidate); })) {
          continue;
        }
        extra.push_back(std::move(candidate));
      }
    }
    std::sort(extra.begin(), extra.end(), candidate_less);
    for (Candidate& candidate : extra) {
      if (pool.size() >= cap) {
        break;
      }
      pool.push_back(std::move(candidate));
    }
  }
  // Fill any remaining room with the cheapest enumerated routes, which are the
  // best candidates for minimising cost once diversity is already represented.
  for (const Candidate& candidate : candidates) {
    if (pool.size() >= cap) {
      break;
    }
    if (!already_present(candidate)) {
      pool.push_back(candidate);
    }
  }
  std::sort(pool.begin(), pool.end(), candidate_less);
  state.blocks.reset();
}

// Analyses one logical edge completely and records either the alternatives the
// global solver may choose from or a categorised denial.
void analyse_logical_edge(PlannerState& state, const PlanningRequest& request, std::size_t index,
                          const PlannerLimits& limits, EdgePlan& plan) {
  const Collective& collective = request.collective;
  const FabricGraph& fabric = request.fabric;
  const LogicalEdge& logical = collective.logical_edges[index];
  plan.id = collective.logical_edge_id(index);
  plan.src = logical.src;
  plan.dst = logical.dst;
  plan.stage = collective.logical_edge_stage[index];

  const EndpointBinding* source_binding = request.find_binding(logical.src);
  const EndpointBinding* target_binding = request.find_binding(logical.dst);
  if (source_binding == nullptr || target_binding == nullptr) {
    const ParticipantId& missing = source_binding == nullptr ? logical.src : logical.dst;
    plan.failed = true;
    plan.kind = DenialKind::kInvalidRequest;
    plan.code = ErrorCode::kUnboundParticipant;
    plan.conflict = ConflictKind::kMissingBinding;
    plan.message = "participant " + missing.value() + " has no endpoint binding";
    return;
  }

  const NodeIndex source = fabric.node_index(source_binding->node);
  const NodeIndex target = fabric.node_index(target_binding->node);
  if (source == kInvalidIndex || target == kInvalidIndex) {
    plan.failed = true;
    plan.kind = DenialKind::kInvalidRequest;
    plan.code = ErrorCode::kUnknownEndpointNode;
    plan.conflict = ConflictKind::kUnknownNode;
    plan.message = "endpoint binding references a node that is not in the fabric";
    return;
  }
  if (state.node_allowed[source] == 0 || state.node_allowed[target] == 0) {
    plan.failed = true;
    plan.kind = DenialKind::kProvenInfeasible;
    plan.code = ErrorCode::kEndpointIneligible;
    plan.conflict = ConflictKind::kIneligibleEndpoint;
    plan.witness_nodes.push_back(state.node_allowed[source] == 0 ? fabric.nodes()[source].id
                                                                 : fabric.nodes()[target].id);
    plan.message = "the bound endpoint node is ineligible under the policy";
    return;
  }

  const std::size_t wanted = std::max<std::size_t>(1u, state.policy->paths_per_logical_edge);
  const Reachability reach = bfs_min_hops(state, source, target, *source_binding, *target_binding);
  if (!reach.reached) {
    plan.failed = true;
    plan.kind = DenialKind::kProvenInfeasible;
    diagnose_unreachable(state, source, target, *source_binding, *target_binding, plan);
    return;
  }
  if (reach.hops > state.policy->max_hops) {
    plan.failed = true;
    plan.kind = DenialKind::kProvenInfeasible;
    plan.code = ErrorCode::kHopLimitExceeded;
    plan.conflict = ConflictKind::kHopLimit;
    plan.message = "the shortest policy-compliant route needs " + std::to_string(reach.hops) +
                   " hops and the policy allows " + std::to_string(state.policy->max_hops);
    return;
  }

  // Enumerate the simple paths this logical edge may use.
  Enumeration enumeration;
  enumeration.steps = 0;
  {
    state.blocks.reset();
    enumerate_paths(state, source, target, *source_binding, *target_binding, enumeration,
                    limits.max_exhaustive_paths, limits.max_enumeration_steps);
    enumeration.exhaustive =
        enumeration.exhaustive && !enumeration.step_limited &&
        enumeration.paths.size() <= limits.max_exhaustive_paths;
    if (enumeration.paths.size() > limits.max_exhaustive_paths) {
      enumeration.paths.resize(limits.max_exhaustive_paths);
    }
  }
  plan.enumeration_exhaustive = enumeration.exhaustive;
  plan.paths_enumerated = enumeration.paths.size();

  std::vector<Candidate> pool;
  std::size_t enumerated_candidates = 0;
  build_pool(state, source, target, *source_binding, *target_binding, enumeration, limits, pool,
             enumerated_candidates);
  plan.candidates_considered = pool.size();

  if (pool.empty()) {
    plan.failed = true;
    if (!enumeration.exhaustive) {
      // Reachability says a compliant route exists, so an empty pool means the
      // enumeration stopped early rather than that nothing exists.
      plan.kind = DenialKind::kSearchLimitReached;
      plan.code = ErrorCode::kSearchBudgetExceeded;
      plan.conflict = ConflictKind::kSearchBudget;
      plan.message = "the path enumeration and the fallback search both stopped at their declared bounds (" +
                     std::to_string(enumeration.steps) + " enumeration steps, " +
                     std::to_string(state.expansions) + " search expansions) before finding a compliant route";
      return;
    }
    plan.kind = DenialKind::kProvenInfeasible;
    diagnose_unreachable(state, source, target, *source_binding, *target_binding, plan);
    plan.message = "no compliant route exists: " + plan.message;
    return;
  }

  // Enumerate the candidate SETS for this logical edge.
  SetEnumeration sets;
  std::vector<const Candidate*> chosen;
  enumerate_sets(state, pool, wanted, 0, chosen, sets, limits.max_set_alternatives, limits.max_combination_steps);
  sets.exhaustive = enumeration.exhaustive && !sets.step_limited;
  plan.sets_exhaustive = sets.exhaustive;
  plan.alternatives = std::move(sets.best);

  if (!plan.alternatives.empty()) {
    return;
  }

  // No admissible set. Decide whether that is proven or merely unobserved.
  plan.failed = true;
  const bool disjoint_requirement = state.policy->disjointness != Disjointness::kNone && wanted > 1u;
  if (disjoint_requirement) {
    const std::uint32_t certificate = max_disjoint_paths(
        state, source, target, state.policy->disjointness == Disjointness::kNode, *source_binding, *target_binding,
        static_cast<std::uint32_t>(wanted));
    if (certificate < wanted) {
      plan.kind = DenialKind::kProvenInfeasible;
      plan.code = ErrorCode::kInsufficientDisjointPaths;
      plan.conflict = ConflictKind::kDisjointness;
      plan.message = "the fabric admits at most " + std::to_string(certificate) + " " +
                     (state.policy->disjointness == Disjointness::kNode ? "node-disjoint" : "edge-disjoint") +
                     " compliant paths between the endpoints and " + std::to_string(wanted) + " are required";
      return;
    }
  }
  if (sets.exhaustive) {
    plan.kind = DenialKind::kProvenInfeasible;
    if (sets.rejected_capacity >= sets.rejected_disjointness && sets.rejected_capacity > 0) {
      plan.code = ErrorCode::kInsufficientCapacity;
      plan.conflict = ConflictKind::kInsufficientCapacity;
      plan.message = "every combination of " + std::to_string(wanted) +
                     " compliant paths would commit more than the verified spare capacity of some edge";
    } else if (sets.rejected_domain > 0) {
      plan.code = ErrorCode::kDomainDiversityUnsatisfiable;
      plan.conflict = ConflictKind::kDomainDiversity;
      plan.message = "no combination of " + std::to_string(wanted) +
                     " compliant paths is independent at failure-domain level " +
                     std::string(to_string(state.policy->domain_diversity_level));
    } else {
      plan.code = ErrorCode::kInsufficientDisjointPaths;
      plan.conflict = ConflictKind::kDisjointness;
      plan.message = "no combination of " + std::to_string(wanted) +
                     " compliant paths satisfies the disjointness requirement";
    }
    return;
  }
  plan.kind = DenialKind::kSearchLimitReached;
  plan.code = ErrorCode::kSearchBudgetExceeded;
  plan.conflict = ConflictKind::kSearchBudget;
  plan.message = "the combination search stopped at its declared bound after " + std::to_string(sets.considered) +
                 " steps; " + std::to_string(plan.candidates_considered) +
                 " candidate paths were considered and the path set was " +
                 (enumeration.exhaustive ? "complete" : "not complete");
}

// ---------------------------------------------------------------------------
// Global capacity-coherent assignment.
//
// Per-logical-edge alternatives are chosen together so that the request never
// commits more than the verified spare capacity of any physical edge. The
// assignment is explored best-first over the same total order used everywhere
// else, so the result is deterministic and, when nothing bounds the search,
// provably optimal.
// ---------------------------------------------------------------------------
struct GlobalCandidate {
  std::vector<std::size_t> choice{};
  std::uint64_t total_cost{0};
  std::size_t conflicts{0};
  std::uint64_t max_path_cost{0};
  std::uint64_t min_headroom{kCostCeiling};
};

bool global_better(const GlobalCandidate& lhs, const GlobalCandidate& rhs) {
  if (lhs.total_cost != rhs.total_cost) {
    return lhs.total_cost < rhs.total_cost;
  }
  if (lhs.conflicts != rhs.conflicts) {
    return lhs.conflicts < rhs.conflicts;
  }
  if (lhs.max_path_cost != rhs.max_path_cost) {
    return lhs.max_path_cost < rhs.max_path_cost;
  }
  if (lhs.min_headroom != rhs.min_headroom) {
    return lhs.min_headroom > rhs.min_headroom;
  }
  return false;  // the exploration order already breaks ties canonically
}

struct GlobalResult {
  bool found{false};
  bool exhaustive{false};
  std::size_t nodes{0};
  std::vector<std::size_t> choice{};
  std::uint64_t total_cost{0};
  std::size_t conflicts{0};
  std::uint64_t max_path_cost{0};
  std::uint64_t min_headroom{kCostCeiling};
};

GlobalResult solve_global(const std::vector<EdgePlan>& plans, std::vector<std::uint64_t>& remaining,
                          std::size_t budget) {
  GlobalResult result;
  const std::size_t levels = plans.size();
  if (levels == 0) {
    result.found = true;
    result.exhaustive = true;
    return result;
  }

  // Fast path: each logical edge takes its own best alternative against the
  // fabric's spare capacity. If the union fits, the assignment is at every
  // level's individual optimum and is therefore globally optimal.
  {
    std::vector<std::uint64_t> trial = remaining;
    bool fits = true;
    for (const EdgePlan& plan : plans) {
      if (plan.alternatives.empty()) {
        fits = false;
        break;
      }
      for (const auto& entry : plan.alternatives.front().usage) {
        if (entry.second > trial[entry.first]) {
          fits = false;
          break;
        }
        trial[entry.first] -= entry.second;
      }
      if (!fits) {
        break;
      }
    }
    if (fits) {
      result.found = true;
      result.choice.assign(levels, 0u);
      for (const EdgePlan& plan : plans) {
        const PathSet& set = plan.alternatives.front();
        result.total_cost = saturating_add(result.total_cost, set.total_cost, kCostCeiling);
        result.conflicts += set.domain_conflicts;
        result.max_path_cost = std::max(result.max_path_cost, set.max_path_cost);
      }
      // Headroom is the smallest remaining capacity over the edges this
      // assignment actually touches, not over the whole fabric.
      result.min_headroom = kCostCeiling;
      for (const EdgePlan& plan : plans) {
        for (const auto& entry : plan.alternatives.front().usage) {
          result.min_headroom = std::min(result.min_headroom, trial[entry.first]);
        }
      }
      result.exhaustive = true;
      for (const EdgePlan& plan : plans) {
        if (!plan.sets_exhaustive) {
          result.exhaustive = false;
          break;
        }
      }
      return result;
    }
  }

  // Otherwise explore the product of the alternatives, best first, pruning every
  // branch whose capacity footprint already exceeds the fabric.
  GlobalCandidate best;
  bool have_best = false;
  std::vector<std::size_t> choice(levels, 0u);
  std::vector<char> started(levels, 0);
  long long level = 0;
  bool limited = false;
  const auto apply = [&remaining](const PathSet& set) {
    for (const auto& entry : set.usage) {
      remaining[entry.first] -= entry.second;
    }
  };
  const auto unapply = [&remaining](const PathSet& set) {
    for (const auto& entry : set.usage) {
      remaining[entry.first] += entry.second;
    }
  };
  const auto fits = [&remaining](const PathSet& set) {
    for (const auto& entry : set.usage) {
      if (entry.second > remaining[entry.first]) {
        return false;
      }
    }
    return true;
  };

  while (level >= 0) {
    if (level == static_cast<long long>(levels)) {
      GlobalCandidate candidate;
      candidate.choice = choice;
      for (std::size_t index = 0; index < levels; ++index) {
        const PathSet& set = plans[index].alternatives[choice[index]];
        candidate.total_cost = saturating_add(candidate.total_cost, set.total_cost, kCostCeiling);
        candidate.conflicts += set.domain_conflicts;
        candidate.max_path_cost = std::max(candidate.max_path_cost, set.max_path_cost);
      }
      candidate.min_headroom = kCostCeiling;
      for (std::size_t index = 0; index < levels; ++index) {
        for (const auto& entry : plans[index].alternatives[choice[index]].usage) {
          candidate.min_headroom = std::min(candidate.min_headroom, remaining[entry.first]);
        }
      }
      if (!have_best || global_better(candidate, best)) {
        best = std::move(candidate);
        have_best = true;
      }
      --level;
      if (level >= 0) {
        unapply(plans[static_cast<std::size_t>(level)].alternatives[choice[static_cast<std::size_t>(level)]]);
      }
      continue;
    }
    const std::size_t level_index = static_cast<std::size_t>(level);
    const std::vector<PathSet>& alternatives = plans[level_index].alternatives;
    const std::size_t start = started[level_index] != 0 ? choice[level_index] + 1u : 0u;
    bool advanced = false;
    for (std::size_t index = start; index < alternatives.size(); ++index) {
      if (result.nodes >= budget) {
        limited = true;
        break;
      }
      ++result.nodes;
      if (!fits(alternatives[index])) {
        continue;
      }
      apply(alternatives[index]);
      choice[level_index] = index;
      started[level_index] = 1;
      advanced = true;
      break;
    }
    if (limited) {
      break;
    }
    if (advanced) {
      ++level;
      continue;
    }
    started[level_index] = 0;
    --level;
    if (level >= 0) {
      unapply(plans[static_cast<std::size_t>(level)].alternatives[choice[static_cast<std::size_t>(level)]]);
    }
  }

  result.exhaustive = !limited;
  if (have_best) {
    result.found = true;
    result.choice = std::move(best.choice);
    result.total_cost = best.total_cost;
    result.conflicts = best.conflicts;
    result.max_path_cost = best.max_path_cost;
    result.min_headroom = best.min_headroom;
    if (!result.exhaustive) {
      result.exhaustive = false;  // found, but not proven optimal
    }
    if (result.exhaustive) {
      for (const EdgePlan& plan : plans) {
        if (!plan.sets_exhaustive) {
          result.exhaustive = false;
          break;
        }
      }
    }
  }
  return result;
}

}  // namespace

const char* to_string(ConflictKind value) noexcept {
  switch (value) {
    case ConflictKind::kNone:
      return "none";
    case ConflictKind::kMissingBinding:
      return "missing-binding";
    case ConflictKind::kUnknownNode:
      return "unknown-node";
    case ConflictKind::kIneligibleEndpoint:
      return "ineligible-endpoint";
    case ConflictKind::kEndpointConstraint:
      return "endpoint-constraint";
    case ConflictKind::kForbiddenNode:
      return "forbidden-node";
    case ConflictKind::kForbiddenTier:
      return "forbidden-tier";
    case ConflictKind::kForbiddenFailureDomain:
      return "forbidden-failure-domain";
    case ConflictKind::kHopLimit:
      return "hop-limit";
    case ConflictKind::kCostLimit:
      return "cost-limit";
    case ConflictKind::kDisconnected:
      return "disconnected";
    case ConflictKind::kNoVerifiedCapacity:
      return "no-verified-capacity";
    case ConflictKind::kInsufficientCapacity:
      return "insufficient-capacity";
    case ConflictKind::kEvidenceInsufficient:
      return "evidence-insufficient";
    case ConflictKind::kDisjointness:
      return "disjointness";
    case ConflictKind::kDomainDiversity:
      return "domain-diversity";
    case ConflictKind::kSearchBudget:
      return "search-budget";
    case ConflictKind::kStructure:
      return "structure";
  }
  return "none";
}

const char* to_string(DenialKind value) noexcept {
  switch (value) {
    case DenialKind::kInvalidRequest:
      return "invalid-request";
    case DenialKind::kStaleInput:
      return "stale-input";
    case DenialKind::kProvenInfeasible:
      return "proven-infeasible";
    case DenialKind::kSearchLimitReached:
      return "search-limit-reached";
    case DenialKind::kUnsupportedConstraint:
      return "unsupported-constraint";
  }
  return "proven-infeasible";
}

ErrorCode PlanningOutcome::primary_code() const noexcept {
  if (plan.has_value()) {
    return ErrorCode::kOk;
  }
  if (!denials.empty()) {
    return denials.front().code;
  }
  return ErrorCode::kInternalError;
}

// Only meaningful when ok() is false.
DenialKind PlanningOutcome::primary_kind() const noexcept {
  if (denials.empty()) {
    return DenialKind::kProvenInfeasible;
  }
  return denials.front().kind;
}

bool PlanningOutcome::proven_infeasible() const noexcept {
  if (ok() || denials.empty()) {
    return false;
  }
  return std::all_of(denials.begin(), denials.end(), [](const DenialDetail& denial) {
    return denial.kind == DenialKind::kProvenInfeasible || denial.kind == DenialKind::kInvalidRequest ||
           denial.kind == DenialKind::kStaleInput || denial.kind == DenialKind::kUnsupportedConstraint;
  });
}

bool PlanningOutcome::indeterminate() const noexcept {
  if (ok()) {
    return false;
  }
  return std::any_of(denials.begin(), denials.end(), [](const DenialDetail& denial) {
    return denial.kind == DenialKind::kSearchLimitReached;
  });
}

bool hops_are_simple(const std::vector<Hop>& hops) {
  for (std::size_t index = 0; index < hops.size(); ++index) {
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (hops[previous].edge == hops[index].edge) {
        return false;
      }
    }
  }
  // No node may be visited twice. That includes the source, which must not
  // reappear as an intermediate or as the terminal node.
  for (std::size_t index = 0; index < hops.size(); ++index) {
    for (std::size_t other = index + 1u; other < hops.size(); ++other) {
      if (hops[index].to == hops[other].to) {
        return false;
      }
    }
  }
  if (!hops.empty()) {
    const NodeId& source = hops.front().from;
    for (const Hop& hop : hops) {
      if (hop.to == source) {
        return false;
      }
    }
  }
  return true;
}

PlanningOutcome plan_collective(const PlanningRequest& request, const PlannerLimits& limits) {
  PlanningOutcome outcome;

  if (Status status = request.validate(); !status.is_ok()) {
    DenialDetail denial;
    denial.code = status.code();
    denial.kind = category_of(status.code()) == ErrorCategory::kAuthority ? DenialKind::kStaleInput
                                                                          : DenialKind::kInvalidRequest;
    denial.conflict = ConflictKind::kStructure;
    denial.message = status.detail();
    outcome.denials.push_back(std::move(denial));
    return outcome;
  }

  const FabricGraph& fabric = request.fabric;
  const Collective& collective = request.collective;
  const Policy& policy = request.policy;

  PlannerState state;
  state.request = &request;
  state.fabric = &fabric;
  state.policy = &policy;
  state.max_expansions = std::max<std::size_t>(1u, policy.max_search_expansions);
  state.enumeration_budget = std::max<std::size_t>(1u, limits.max_total_enumeration_steps);
  state.node_allowed.assign(fabric.node_count(), 1u);
  state.edge_allowed.assign(fabric.edge_count(), 1u);
  state.edge_cost.assign(fabric.edge_count(), 0u);
  state.spare.assign(fabric.edge_count(), 0u);
  state.blocks.resize(fabric.node_count(), fabric.edge_count());
  state.hop_limit = policy.max_hops;

  for (std::size_t index = 0; index < fabric.node_count(); ++index) {
    const Node& node = fabric.nodes()[index];
    bool allowed = node.eligible;
    if (allowed && tier_forbidden(policy, node.tier)) {
      allowed = false;
    }
    if (allowed && std::binary_search(policy.forbidden_nodes.begin(), policy.forbidden_nodes.end(), node.id)) {
      allowed = false;
    }
    state.node_allowed[index] = allowed ? 1u : 0u;
  }
  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    const Edge& edge = fabric.edges()[index];
    bool allowed = edge.eligible;
    if (allowed && tier_forbidden(policy, edge.tier)) {
      allowed = false;
    }
    if (allowed && !policy.allowed_path_tiers.empty() &&
        (!edge.tier.valid() ||
         !std::binary_search(policy.allowed_path_tiers.begin(), policy.allowed_path_tiers.end(), edge.tier))) {
      allowed = false;
    }
    if (allowed && domain_forbidden(fabric, policy, edge.failure_domain)) {
      allowed = false;
    }
    if (allowed && !evidence_satisfies(edge.evidence, policy.evidence)) {
      allowed = false;
    }
    state.edge_allowed[index] = allowed ? 1u : 0u;
    state.spare[index] = edge_spare(state, static_cast<EdgeIndex>(index));
    state.edge_cost[index] = edge_cost_of(state, static_cast<EdgeIndex>(index));
  }

  const std::size_t wanted = std::max<std::size_t>(1u, policy.paths_per_logical_edge);
  const std::uint64_t per_path_demand =
      collective.demand_mbps == 0 ? 0u : (collective.demand_mbps + wanted - 1u) / wanted;
  state.per_path_demand = per_path_demand;

  const std::size_t logical_count = collective.logical_edges.size();
  std::vector<EdgePlan> plans(logical_count);
  for (std::size_t index = 0; index < logical_count; ++index) {
    analyse_logical_edge(state, request, index, limits, plans[index]);
    outcome.paths_enumerated += plans[index].paths_enumerated;
    outcome.combination_steps += plans[index].candidates_considered;
    if (!plans[index].failed) {
      continue;
    }
    const EdgePlan& plan = plans[index];
    DenialDetail denial;
    denial.code = plan.code;
    denial.kind = plan.kind;
    denial.conflict = plan.conflict;
    denial.logical_edge = plan.id;
    denial.participant = plan.src;
    denial.message = plan.message;
    denial.candidates_considered = plan.candidates_considered;
    denial.paths_enumerated = plan.paths_enumerated;
    denial.enumeration_exhaustive = plan.enumeration_exhaustive;
    denial.nodes = plan.witness_nodes;
    denial.edges = plan.witness_edges;
    outcome.denials.push_back(std::move(denial));
    if (!limits.collect_all_denials) {
      break;
    }
  }
  outcome.search_expansions = state.expansions;
  outcome.enumeration_steps = state.enumeration_steps;
  if (!outcome.denials.empty()) {
    return outcome;
  }

  std::vector<std::uint64_t> remaining(fabric.edge_count(), 0u);
  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    remaining[index] = state.spare[index];
  }
  const GlobalResult global = solve_global(plans, remaining, limits.max_global_nodes);
  outcome.global_search_nodes = global.nodes;

  if (!global.found) {
    const bool all_exhaustive = std::all_of(plans.begin(), plans.end(),
                                            [](const EdgePlan& plan) { return plan.sets_exhaustive; });
    DenialDetail denial;
    denial.conflict = ConflictKind::kInsufficientCapacity;
    denial.code = ErrorCode::kInsufficientCapacity;
    if (global.exhaustive && all_exhaustive) {
      denial.kind = DenialKind::kProvenInfeasible;
      denial.message =
          "no assignment of compliant path sets keeps the request inside the verified spare capacity of every "
          "physical edge";
    } else {
      denial.kind = DenialKind::kSearchLimitReached;
      denial.code = ErrorCode::kSearchBudgetExceeded;
      denial.conflict = ConflictKind::kSearchBudget;
      denial.message =
          "the global capacity search stopped at its declared bound after " + std::to_string(global.nodes) +
          " nodes; the request may or may not be mappable inside the proven capacity budget";
    }
    outcome.denials.push_back(std::move(denial));
    return outcome;
  }

  Plan plan;
  plan.id = CollectivePlanId{request.canonical_digest};
  plan.input_digest = request.canonical_digest;
  plan.generation = request.plan_generation;
  plan.bindings = request.current_bindings();
  plan.collective = collective.id;
  plan.kind = collective.kind;
  plan.stage_count = collective.stage_count;
  plan.stages.resize(collective.stage_count);
  for (std::uint32_t stage = 0; stage < collective.stage_count; ++stage) {
    plan.stages[stage].index = stage;
  }

  std::vector<std::uint64_t> allocation(fabric.edge_count(), 0u);
  std::uint64_t path_id = 0;
  std::uint64_t total_cost = 0;
  std::uint64_t max_path_cost = 0;
  std::uint64_t bottleneck = kCostCeiling;
  std::size_t hop_count = 0;
  EvidenceClass weakest = EvidenceClass::kMeasured;

  for (std::size_t index = 0; index < logical_count; ++index) {
    const PathSet& set = plans[index].alternatives[global.choice[index]];
    const std::uint32_t stage = plans[index].stage;
    plan.stages[stage].logical_edges.push_back(plans[index].id);
    for (const Candidate& candidate : set.paths) {
      PathPlan entry;
      entry.id = PathId{++path_id};
      entry.logical_edge = plans[index].id;
      entry.src = plans[index].src;
      entry.dst = plans[index].dst;
      entry.stage = stage;
      entry.cost = candidate.cost;
      entry.bottleneck_mbps = candidate.bottleneck;
      entry.domain_signature = candidate.signature;
      entry.weakest_evidence = candidate.weakest;
      for (const EdgeIndex edge_index : candidate.edges) {
        const Edge& edge = fabric.edges()[edge_index];
        entry.hops.push_back(Hop{edge.id, edge.from, edge.to});
        // The allocation table records what this plan would place on every edge
        // it uses. Whether that fits is checked against VERIFIED capacity only:
        // an edge with no verified capacity cannot be shown to have room, and
        // the plan says so through its weakest-evidence label.
        allocation[edge_index] = saturating_add(allocation[edge_index], per_path_demand, kCostCeiling);
      }
      hop_count += entry.hops.size();
      total_cost = saturating_add(total_cost, entry.cost, kCostCeiling);
      max_path_cost = std::max(max_path_cost, entry.cost);
      if (!entry.hops.empty()) {
        bottleneck = std::min(bottleneck, entry.bottleneck_mbps);
      }
      if (static_cast<std::uint8_t>(entry.weakest_evidence) < static_cast<std::uint8_t>(weakest)) {
        weakest = entry.weakest_evidence;
      }
      plan.stages[stage].paths.push_back(entry.id);
      plan.paths.push_back(std::move(entry));
    }
  }

  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    if (allocation[index] == 0) {
      continue;
    }
    plan.allocations.push_back(Allocation{fabric.edges()[index].id, allocation[index]});
  }

  plan.stats.path_count = plan.paths.size();
  plan.stats.hop_count = hop_count;
  plan.stats.logical_edge_count = plans.size();
  plan.stats.total_cost = total_cost;
  plan.stats.max_path_cost = max_path_cost;
  plan.stats.bottleneck_mbps =
      plan.paths.empty() || bottleneck == kCostCeiling ? 0u : bottleneck;
  plan.weakest_evidence = weakest;

  if (Status status = validate_plan(plan, request); !status.is_ok()) {
    DenialDetail denial;
    denial.kind = DenialKind::kUnsupportedConstraint;
    denial.code = ErrorCode::kInternalError;
    denial.conflict = ConflictKind::kStructure;
    denial.message = std::string("the produced plan failed the planner's own independent validation: ") +
                     status.to_string();
    outcome.denials.push_back(std::move(denial));
    return outcome;
  }

  outcome.optimal = global.exhaustive;
  outcome.plan = std::move(plan);
  return outcome;
}

Result<std::vector<Hop>> find_path(const PlanningRequest& request, const NodeId& source,
                                   const NodeId& destination, const PlannerLimits& limits) {
  if (Status status = request.validate(); !status.is_ok()) {
    return Result<std::vector<Hop>>::failure(status);
  }
  const FabricGraph& fabric = request.fabric;
  const NodeIndex source_index = fabric.node_index(source);
  const NodeIndex target_index = fabric.node_index(destination);
  if (source_index == kInvalidIndex) {
    return Result<std::vector<Hop>>::failure(ErrorCode::kUnknownEndpointNode, "source node is not in the fabric");
  }
  if (target_index == kInvalidIndex) {
    return Result<std::vector<Hop>>::failure(ErrorCode::kUnknownEndpointNode,
                                             "destination node is not in the fabric");
  }

  PlannerState state;
  state.request = &request;
  state.fabric = &fabric;
  state.policy = &request.policy;
  state.max_expansions = std::max<std::size_t>(1u, request.policy.max_search_expansions);
  state.node_allowed.assign(fabric.node_count(), 1u);
  state.edge_allowed.assign(fabric.edge_count(), 1u);
  state.edge_cost.assign(fabric.edge_count(), 0u);
  state.spare.assign(fabric.edge_count(), 0u);
  state.blocks.resize(fabric.node_count(), fabric.edge_count());
  state.per_path_demand = 0;
  state.hop_limit = request.policy.max_hops;
  (void)limits;

  for (std::size_t index = 0; index < fabric.node_count(); ++index) {
    const Node& node = fabric.nodes()[index];
    const bool allowed = node.eligible && !tier_forbidden(request.policy, node.tier) &&
                         !std::binary_search(request.policy.forbidden_nodes.begin(),
                                             request.policy.forbidden_nodes.end(), node.id);
    state.node_allowed[index] = allowed ? 1u : 0u;
  }
  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    const Edge& edge = fabric.edges()[index];
    bool allowed = edge.eligible && !tier_forbidden(request.policy, edge.tier) &&
                   !domain_forbidden(fabric, request.policy, edge.failure_domain) &&
                   evidence_satisfies(edge.evidence, request.policy.evidence);
    if (allowed && !request.policy.allowed_path_tiers.empty() &&
        (!edge.tier.valid() ||
         !std::binary_search(request.policy.allowed_path_tiers.begin(),
                             request.policy.allowed_path_tiers.end(), edge.tier))) {
      allowed = false;
    }
    state.edge_allowed[index] = allowed ? 1u : 0u;
    state.spare[index] = edge_spare(state, static_cast<EdgeIndex>(index));
    state.edge_cost[index] = edge_cost_of(state, static_cast<EdgeIndex>(index));
  }

  const EndpointBinding unconstrained;
  const SearchOutcome found = shortest_path(state, source_index, target_index, unconstrained, unconstrained);
  if (!found.found) {
    if (state.budget_exceeded) {
      return Result<std::vector<Hop>>::failure(ErrorCode::kSearchBudgetExceeded,
                                               "the search stopped at its declared bound before deciding");
    }
    return Result<std::vector<Hop>>::failure(ErrorCode::kNoPath, "no compliant path exists");
  }
  std::vector<Hop> hops;
  hops.reserve(found.edges.size());
  for (const EdgeIndex index : found.edges) {
    const Edge& edge = fabric.edges()[index];
    hops.push_back(Hop{edge.id, edge.from, edge.to});
  }
  if (!hops_are_simple(hops)) {
    return Result<std::vector<Hop>>::failure(ErrorCode::kInternalError,
                                             "the search produced a path that is not simple");
  }
  return Result<std::vector<Hop>>::success(std::move(hops));
}

}  // namespace cpath
