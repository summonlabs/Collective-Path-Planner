// Collective Path Planner - deterministic constrained path planner.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
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

// A chosen physical path before it becomes part of a plan.
struct Candidate {
  std::vector<EdgeIndex> edges{};
  std::uint64_t cost{0};
  std::uint64_t bottleneck{0};
  EvidenceClass weakest{EvidenceClass::kMeasured};
  std::vector<FailureDomainId> signature{};
  bool domain_known{true};
};

// Total order used everywhere candidates are compared, so that ties can never
// depend on container iteration order.
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

// Generation-stamped block sets: resetting is O(1) regardless of graph size, so
// a search never pays a per-edge clear cost.
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
  std::uint64_t cost{0};
  std::vector<EdgeIndex> edges{};
};

struct PlannerState {
  const PlanningRequest* request{nullptr};
  const FabricGraph* fabric{nullptr};
  const Policy* policy{nullptr};
  std::vector<std::uint8_t> node_allowed{};
  std::vector<std::uint8_t> edge_allowed{};
  std::vector<std::uint64_t> allocated{};
  std::uint64_t per_path_demand{0};
  std::size_t hop_limit{0};
  std::size_t expansions{0};
  std::size_t max_expansions{0};
  bool budget_exceeded{false};
  BlockSet blocks{};
  std::unordered_map<std::uint64_t, std::uint64_t> dist{};
  std::unordered_map<std::uint64_t, std::pair<std::uint64_t, EdgeIndex>> prev{};
  std::priority_queue<Label, std::vector<Label>, LabelGreater> frontier{};
};

const Node& node_at(const PlannerState& state, NodeIndex index) { return state.fabric->nodes()[index]; }

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
    // Zero verified capacity is not capacity. It is only usable when the policy
    // explicitly accepts unverified capacity, and never when some authority has
    // already claimed a reservation on it.
    return state.policy->allow_unverified_capacity && edge.reserved_mbps == 0;
  }
  return edge.reserved_mbps < edge.capacity_mbps;
}

// Capacity remaining after already-planned allocations, in this request's
// deterministic planning order. Saturates instead of wrapping.
std::uint64_t edge_available(const PlannerState& state, EdgeIndex index) {
  const Edge& edge = state.fabric->edges()[index];
  if (edge.capacity_mbps == 0) {
    return 0;
  }
  const std::uint64_t committed = state.allocated[index];
  if (committed >= edge.capacity_mbps || edge.reserved_mbps >= edge.capacity_mbps) {
    return 0;
  }
  const std::uint64_t spare = edge.capacity_mbps - edge.reserved_mbps;
  return committed >= spare ? 0 : spare - committed;
}

bool edge_dynamically_usable(const PlannerState& state, EdgeIndex index) {
  const Edge& edge = state.fabric->edges()[index];
  if (edge.capacity_mbps == 0) {
    return true;  // static check already applied the unverified-capacity policy
  }
  const std::uint64_t available = edge_available(state, index);
  if (available == 0) {
    return false;
  }
  if (state.per_path_demand > 0 && available < state.per_path_demand) {
    return false;
  }
  return true;
}

std::uint64_t edge_cost(const PlannerState& state, EdgeIndex index) {
  const Edge& edge = state.fabric->edges()[index];
  const Policy& policy = *state.policy;
  const std::uint64_t latency_term =
      saturating_mul(edge.latency_micros, policy.latency_weight_milli, kCostCeiling) / 1000u;
  if (policy.congestion_weight_milli == 0 || edge.capacity_mbps == 0) {
    return latency_term;
  }
  const std::uint64_t committed = state.allocated[index];
  const std::uint64_t used = saturating_add(edge.reserved_mbps, committed, kCostCeiling);
  if (used >= edge.capacity_mbps) {
    return kCostCeiling;
  }
  const std::uint64_t utilisations_milli = saturating_mul(used, 1000u, kCostCeiling) / edge.capacity_mbps;
  const std::uint64_t spare = 1000u - utilisations_milli;
  const std::uint64_t penalty =
      saturating_mul(policy.congestion_weight_milli, utilisations_milli, kCostCeiling) / (spare == 0 ? 1u : spare);
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
    if (!edge.tier.valid() || !std::binary_search(binding.allowed_tiers.begin(), binding.allowed_tiers.end(),
                                                  edge.tier)) {
      return false;
    }
  }
  return true;
}

// The failure domain a hop is attributed to at the configured diversity level.
// When the requested granularity is not modelled for that hop, the hop's own
// domain is used, which is the finest granularity actually available.
FailureDomainId domain_at_level(const FabricGraph& fabric, const FailureDomainId& domain, DomainKind level) noexcept {
  if (!domain.valid()) {
    return FailureDomainId{};
  }
  const std::vector<FailureDomainId>& chain = fabric.domain_chain(domain);
  for (const FailureDomainId& entry : chain) {
    const FailureDomain* record = fabric.find_failure_domain(entry);
    if (record != nullptr && record->kind == level) {
      return entry;
    }
  }
  return domain;
}

// Hop-limited shortest path. The search runs over (node, hops) states so that an
// over-long path can never mask a compliant one, and it is bounded by the
// per-request expansion budget.
SearchOutcome shortest_path(PlannerState& state, NodeIndex source, NodeIndex target,
                            const EndpointBinding& source_binding, const EndpointBinding& target_binding) {
  SearchOutcome outcome;
  if (source == target) {
    outcome.found = true;
    outcome.cost = 0;
    return outcome;
  }
  if (!state.node_allowed[source] || !state.node_allowed[target]) {
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
      continue;  // stale entry superseded by a cheaper label
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
      if (state.blocks.edge_blocked(edge_index)) {
        continue;
      }
      if (!edge_statically_usable(state, edge_index)) {
        continue;
      }
      const Edge& edge = state.fabric->edges()[edge_index];
      const NodeIndex next = state.fabric->node_index(edge.to);
      if (next == kInvalidIndex || state.node_allowed[next] == 0 || state.blocks.node_blocked(next)) {
        continue;
      }
      if (!edge_dynamically_usable(state, edge_index)) {
        continue;
      }
      if (!binding_allows_edge(state, source_binding, edge) || !binding_allows_edge(state, target_binding, edge)) {
        continue;
      }
      const std::uint64_t step_cost = edge_cost(state, edge_index);
      const std::uint64_t next_cost = saturating_add(label.cost, step_cost, kCostCeiling);
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

Candidate make_candidate(const PlannerState& state, const std::vector<EdgeIndex>& edges, bool* hop_limit_ok) {
  const FabricGraph& fabric = *state.fabric;
  const Policy& policy = *state.policy;
  Candidate candidate;
  candidate.edges = edges;
  candidate.cost = 0;
  candidate.bottleneck = kCostCeiling;
  candidate.weakest = EvidenceClass::kMeasured;
  *hop_limit_ok = edges.size() <= policy.max_hops;
  std::vector<FailureDomainId> hop_domains;
  hop_domains.reserve(edges.size());

  for (const EdgeIndex index : edges) {
    const Edge& edge = fabric.edges()[index];
    candidate.cost = saturating_add(candidate.cost, edge_cost(state, index), kCostCeiling);
    candidate.bottleneck = std::min(candidate.bottleneck, edge_available(state, index));
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
  // Diversity is judged on the domains a path adds beyond the ones its endpoints
  // unavoidably sit in: both sibling paths must touch the source and destination
  // attachment domains, so those carry no discriminating information. A failure
  // there takes the endpoint down regardless of which path was chosen.
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
  if (candidate.bottleneck == kCostCeiling) {
    candidate.bottleneck = 0;  // zero-hop path: no physical capacity is consumed
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

bool paths_node_disjoint(const PlannerState& state, const Candidate& lhs, const Candidate& rhs) {
  // Both sibling paths carry the same logical edge, so they share their source
  // and their destination by definition; those two nodes are not evidence of a
  // shared failure. Every node a path passes through in between must be unique,
  // and a path that revisits an endpoint mid-route is caught because the hop
  // that lands there is an interior hop of that path.
  for (std::size_t left_index = 0; left_index + 1u < lhs.edges.size(); ++left_index) {
    const NodeId& left_node = state.fabric->edges()[lhs.edges[left_index]].to;
    for (std::size_t right_index = 0; right_index + 1u < rhs.edges.size(); ++right_index) {
      const NodeId& right_node = state.fabric->edges()[rhs.edges[right_index]].to;
      if (left_node == right_node) {
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

struct CombinationCheck {
  bool ok{false};
  ErrorCode code{ErrorCode::kInternalError};
  ConflictKind conflict{ConflictKind::kNone};
  std::string detail{};
};

CombinationCheck check_combination(const PlannerState& state, const std::vector<Candidate*>& chosen) {
  const Policy& policy = *state.policy;
  CombinationCheck result;
  for (std::size_t i = 0; i < chosen.size(); ++i) {
    for (std::size_t j = i + 1u; j < chosen.size(); ++j) {
      if (policy.disjointness != Disjointness::kNone && !paths_edge_disjoint(*chosen[i], *chosen[j])) {
        result.code = ErrorCode::kInsufficientDisjointPaths;
        result.conflict = ConflictKind::kDisjointness;
        result.detail = "two chosen paths share a physical edge";
        return result;
      }
      if (policy.disjointness == Disjointness::kNode && !paths_node_disjoint(state, *chosen[i], *chosen[j])) {
        result.code = ErrorCode::kInsufficientDisjointPaths;
        result.conflict = ConflictKind::kDisjointness;
        result.detail = "two chosen paths share an intermediate physical node";
        return result;
      }
      if (policy.domain_diversity == DomainDiversity::kRequired && !domains_independent(*chosen[i], *chosen[j])) {
        result.code = ErrorCode::kDomainDiversityUnsatisfiable;
        result.conflict = ConflictKind::kDomainDiversity;
        result.detail = "two chosen paths are not independent at the configured failure-domain level";
        return result;
      }
    }
  }
  result.ok = true;
  return result;
}

// The whole chosen set must fit the remaining verified spare capacity of every
// edge it touches. Per-candidate availability is not enough: sibling paths for
// one logical edge are searched against the state before any of them is
// committed, so two of them can each fit an edge that only one of them can
// actually share.
CombinationCheck check_capacity(const PlannerState& state, const std::vector<Candidate*>& chosen) {
  CombinationCheck result;
  if (state.per_path_demand == 0) {
    result.ok = true;
    return result;
  }
  // Bounded aggregation: at most kMaxCandidatePaths paths of at most max_hops
  // edges each, so a linear scan over a tiny vector is exact and cheap.
  std::vector<std::pair<EdgeIndex, std::uint64_t>> demand;
  for (const Candidate* candidate : chosen) {
    for (const EdgeIndex edge : candidate->edges) {
      bool merged = false;
      for (auto& entry : demand) {
        if (entry.first == edge) {
          entry.second = saturating_add(entry.second, state.per_path_demand, kCostCeiling);
          merged = true;
          break;
        }
      }
      if (!merged) {
        demand.emplace_back(edge, state.per_path_demand);
      }
    }
  }
  for (const auto& entry : demand) {
    const Edge& edge = state.fabric->edges()[entry.first];
    if (edge.capacity_mbps == 0) {
      continue;  // unverified capacity: the policy already accepted the gap
    }
    const std::uint64_t spare = edge_available(state, entry.first);
    if (entry.second > spare) {
      result.code = ErrorCode::kInsufficientCapacity;
      result.conflict = ConflictKind::kInsufficientCapacity;
      result.detail = "the chosen paths would commit " + std::to_string(entry.second) +
                      " Mbps onto edge " + edge.id.value() + " which has only " + std::to_string(spare) +
                      " Mbps of verified spare capacity";
      return result;
    }
  }
  result.ok = true;
  return result;
}

CombinationCheck check_domain_coverage(const PlannerState& state, const std::vector<Candidate*>& chosen) {
  CombinationCheck result;
  const Policy& policy = *state.policy;
  if (policy.domain_diversity == DomainDiversity::kRequired && chosen.size() > 1u) {
    for (const Candidate* candidate : chosen) {
      if (!candidate->domain_known) {
        result.code = ErrorCode::kDomainDiversityUnsatisfiable;
        result.conflict = ConflictKind::kDomainDiversity;
        result.detail =
            "a chosen path traverses an edge with no declared failure domain, so independence cannot be shown";
        return result;
      }
    }
  }
  result.ok = true;
  return result;
}

struct Selection {
  bool found{false};
  std::vector<std::size_t> indices{};
  std::uint64_t total_cost{0};
  CombinationCheck failure{};
};

// Exhaustive search over combinations of the bounded candidate pool. The pool is
// at most kMaxCandidatePaths entries, so this is bounded by construction.
Selection select_combination(PlannerState& state, const std::vector<Candidate>& pool, std::size_t wanted) {
  Selection selection;
  if (wanted == 0 || wanted > pool.size()) {
    selection.failure.code = ErrorCode::kInsufficientDisjointPaths;
    selection.failure.conflict = ConflictKind::kDisjointness;
    selection.failure.detail = "only " + std::to_string(pool.size()) + " distinct candidate paths exist but " +
                               std::to_string(wanted) + " are required";
    return selection;
  }

  std::vector<std::size_t> current(wanted);
  std::vector<Candidate*> chosen(wanted, nullptr);
  for (std::size_t index = 0; index < wanted; ++index) {
    current[index] = index;
  }

  bool more = true;
  while (more) {
    for (std::size_t index = 0; index < wanted; ++index) {
      chosen[index] = const_cast<Candidate*>(&pool[current[index]]);
    }
    CombinationCheck check = check_domain_coverage(state, chosen);
    if (check.ok) {
      check = check_combination(state, chosen);
    }
    if (check.ok) {
      check = check_capacity(state, chosen);
    }
    if (check.ok) {
      std::uint64_t total = 0;
      for (const Candidate* candidate : chosen) {
        total = saturating_add(total, candidate->cost, kCostCeiling);
      }
      if (!selection.found || total < selection.total_cost) {
        selection.found = true;
        selection.total_cost = total;
        selection.indices = current;
      }
    } else if (!selection.found && selection.failure.detail.empty()) {
      selection.failure = check;
    }

    // Advance to the next combination in lexicographic order.
    bool advanced = false;
    std::size_t position = wanted;
    while (position > 0) {
      --position;
      if (current[position] < pool.size() - wanted + position) {
        ++current[position];
        for (std::size_t index = position + 1u; index < wanted; ++index) {
          current[index] = current[index - 1u] + 1u;
        }
        advanced = true;
        break;
      }
    }
    more = advanced;
  }
  if (!selection.found && selection.failure.detail.empty()) {
    selection.failure.code = ErrorCode::kInsufficientDisjointPaths;
    selection.failure.conflict = ConflictKind::kDisjointness;
    selection.failure.detail = "no candidate combination was evaluated";
  }
  return selection;
}

// Unit-capacity max-flow used only to certify that a disjointness requirement is
// genuinely impossible when candidate search fails. A certificate is worth more
// than a vague denial.
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
                                 std::uint32_t limit) {
  const FabricGraph& fabric = *state.fabric;
  const std::size_t node_count = fabric.node_count();
  const std::size_t multiplier = node_disjoint ? 2u : 1u;
  MaxFlow flow(node_count * multiplier);
  const auto in_index = [](NodeIndex node) { return static_cast<std::size_t>(node) * 2u; };
  const auto out_index = [](NodeIndex node) { return static_cast<std::size_t>(node) * 2u + 1u; };
  const auto plain = [](NodeIndex node) { return static_cast<std::size_t>(node); };

  if (node_disjoint) {
    for (NodeIndex node = 0; node < static_cast<NodeIndex>(node_count); ++node) {
      if (!state.node_allowed[node]) {
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
    if (!edge_statically_usable(state, edge_index) || !edge_dynamically_usable(state, edge_index)) {
      continue;
    }
    const Edge& edge = fabric.edges()[edge_index];
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

struct LogicalEdgePlan {
  LogicalEdgeId id{};
  ParticipantId src{};
  ParticipantId dst{};
  std::uint32_t stage{0};
  std::vector<std::size_t> chosen{};
};

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

ErrorCode PlanningOutcome::primary_code() const noexcept {
  if (plan.has_value()) {
    return ErrorCode::kOk;
  }
  if (!denials.empty()) {
    return denials.front().code;
  }
  return ErrorCode::kInternalError;
}

namespace {

using DomainEdgeIndex = std::unordered_map<FailureDomainId, std::vector<EdgeIndex>,
                                           StrongIdHash<FailureDomainIdTag, std::string>>;

DenialDetail make_denial(ErrorCode code, ConflictKind conflict) {
  DenialDetail denial;
  denial.code = code;
  denial.conflict = conflict;
  return denial;
}

// Precise diagnosis for "no path exists", distinguishing the constraint class
// that actually removed every alternative.
DenialDetail diagnose_unreachable(const PlannerState& state, NodeIndex source, NodeIndex target,
                                  const EndpointBinding& source_binding, const EndpointBinding& target_binding,
                                  bool budget_exceeded, bool hop_limited) {
  const FabricGraph& fabric = *state.fabric;
  if (budget_exceeded) {
    DenialDetail denial = make_denial(ErrorCode::kSearchBudgetExceeded, ConflictKind::kSearchBudget);
    denial.message = "the search budget of " + std::to_string(state.max_expansions) +
                     " node expansions was exhausted before a path was decided";
    return denial;
  }
  if (hop_limited) {
    DenialDetail denial = make_denial(ErrorCode::kHopLimitExceeded, ConflictKind::kHopLimit);
    denial.message = "a path exists but every path exceeds the hop limit of " +
                     std::to_string(state.policy->max_hops);
    return denial;
  }

  const Node& source_node = fabric.nodes()[source];
  const Node& target_node = fabric.nodes()[target];
  const std::string endpoints = " from " + source_node.id.value() + " to " + target_node.id.value();
  std::size_t total = 0;
  std::size_t by_tier = 0;
  std::size_t by_domain = 0;
  std::size_t by_evidence = 0;
  std::size_t by_capacity = 0;
  std::size_t by_binding = 0;
  std::size_t by_node = 0;
  DenialDetail denial = make_denial(ErrorCode::kNoPath, ConflictKind::kDisconnected);

  for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
    const Edge& edge = fabric.edges()[index];
    if (!(edge.from == source_node.id)) {
      continue;
    }
    ++total;
    if (denial.edges.size() < kMaxDenialWitness) {
      denial.edges.push_back(edge.id);
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
    if (!binding_allows_edge(state, source_binding, edge) || !binding_allows_edge(state, target_binding, edge)) {
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
    denial.message = "no physical edge leaves the endpoint bound to " + source_node.id.value() + endpoints;
    return denial;
  }
  if (by_node == total) {
    denial.conflict = ConflictKind::kForbiddenNode;
    denial.code = ErrorCode::kNodeForbidden;
    denial.message = "every edge leaving the source endpoint is ineligible or lands on a forbidden node";
    return denial;
  }
  if (by_tier + by_node == total) {
    denial.conflict = ConflictKind::kForbiddenTier;
    denial.code = ErrorCode::kTierForbidden;
    denial.message = "every edge leaving the source endpoint is excluded by the tier policy";
    return denial;
  }
  if (by_domain + by_tier + by_node == total) {
    denial.conflict = ConflictKind::kForbiddenFailureDomain;
    denial.code = ErrorCode::kFailureDomainForbidden;
    denial.message = "every edge leaving the source endpoint is excluded by the failure-domain policy";
    return denial;
  }
  if (by_evidence + by_domain + by_tier + by_node == total) {
    denial.conflict = ConflictKind::kEvidenceInsufficient;
    denial.code = ErrorCode::kCapacityEvidenceInsufficient;
    denial.message = "every edge leaving the source endpoint has evidence below the required class";
    return denial;
  }
  if (by_capacity + by_evidence + by_domain + by_tier + by_node == total) {
    denial.conflict = ConflictKind::kNoVerifiedCapacity;
    denial.code = ErrorCode::kEdgeHasNoVerifiedCapacity;
    denial.message = "every edge leaving the source endpoint has no verified capacity";
    return denial;
  }
  if (by_binding + by_capacity + by_evidence + by_domain + by_tier + by_node == total) {
    denial.conflict = ConflictKind::kEndpointConstraint;
    denial.code = ErrorCode::kEndpointIneligible;
    denial.message = "every edge leaving the source endpoint is excluded by the endpoint binding constraints";
    return denial;
  }
  denial.message = "no path satisfies the policy" + endpoints;
  return denial;
}

}  // namespace

PlanningOutcome plan_collective(const PlanningRequest& request, const PlannerLimits& limits) {
  PlanningOutcome outcome;

  if (Status status = request.validate(); !status.is_ok()) {
    DenialDetail denial = make_denial(status.code(), ConflictKind::kStructure);
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
  state.node_allowed.assign(fabric.node_count(), 1u);
  state.edge_allowed.assign(fabric.edge_count(), 1u);
  state.allocated.assign(fabric.edge_count(), 0u);
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

  DomainEdgeIndex edges_by_domain;
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
    if (edge.failure_domain.valid()) {
      edges_by_domain[edge.failure_domain].push_back(static_cast<EdgeIndex>(index));
    }
  }

  const std::size_t wanted = std::max<std::size_t>(1u, policy.paths_per_logical_edge);
  const std::size_t pool_capacity =
      std::max<std::size_t>(wanted, std::min<std::size_t>(limits.max_candidate_paths, kMaxCandidatePaths));
  const bool node_disjoint = policy.disjointness == Disjointness::kNode;

  const std::size_t logical_count = collective.logical_edges.size();
  std::vector<LogicalEdgePlan> planned(logical_count);
  std::vector<std::vector<Candidate>> candidates(logical_count);
  std::vector<std::uint64_t> planned_allocation(fabric.edge_count(), 0u);

  for (std::size_t li = 0; li < logical_count; ++li) {
    const LogicalEdge& logical = collective.logical_edges[li];
    planned[li].id = collective.logical_edge_id(li);
    planned[li].src = logical.src;
    planned[li].dst = logical.dst;
    planned[li].stage = collective.logical_edge_stage[li];

    const EndpointBinding* source_binding = request.find_binding(logical.src);
    const EndpointBinding* target_binding = request.find_binding(logical.dst);
    if (source_binding == nullptr || target_binding == nullptr) {
      DenialDetail denial = make_denial(ErrorCode::kUnboundParticipant, ConflictKind::kMissingBinding);
      const ParticipantId& missing = source_binding == nullptr ? logical.src : logical.dst;
      denial.logical_edge = planned[li].id;
      denial.participant = missing;
      denial.message = "participant " + missing.value() + " has no endpoint binding";
      outcome.denials.push_back(std::move(denial));
      if (!limits.collect_all_denials) {
        break;
      }
      continue;
    }

    const NodeIndex source = fabric.node_index(source_binding->node);
    const NodeIndex target = fabric.node_index(target_binding->node);
    if (source == kInvalidIndex || target == kInvalidIndex) {
      DenialDetail denial = make_denial(ErrorCode::kUnknownEndpointNode, ConflictKind::kUnknownNode);
      denial.logical_edge = planned[li].id;
      denial.participant = source == kInvalidIndex ? logical.src : logical.dst;
      denial.message = "endpoint binding references a node that is not in the fabric";
      outcome.denials.push_back(std::move(denial));
      if (!limits.collect_all_denials) {
        break;
      }
      continue;
    }
    if (state.node_allowed[source] == 0 || state.node_allowed[target] == 0) {
      DenialDetail denial = make_denial(ErrorCode::kEndpointIneligible, ConflictKind::kIneligibleEndpoint);
      denial.logical_edge = planned[li].id;
      denial.participant = state.node_allowed[source] == 0 ? logical.src : logical.dst;
      denial.nodes.push_back(state.node_allowed[source] == 0 ? fabric.nodes()[source].id
                                                             : fabric.nodes()[target].id);
      denial.message = "the bound endpoint node is ineligible under the policy";
      outcome.denials.push_back(std::move(denial));
      if (!limits.collect_all_denials) {
        break;
      }
      continue;
    }

    state.per_path_demand =
        collective.demand_mbps == 0 ? 0u : (collective.demand_mbps + wanted - 1u) / wanted;
    state.blocks.reset();

    const SearchOutcome best = shortest_path(state, source, target, *source_binding, *target_binding);
    if (!best.found) {
      bool hop_limited = false;
      if (!state.budget_exceeded) {
        state.hop_limit = kMaxHops;
        state.blocks.reset();
        const SearchOutcome relaxed = shortest_path(state, source, target, *source_binding, *target_binding);
        state.hop_limit = policy.max_hops;
        hop_limited = relaxed.found;
      }
      DenialDetail denial =
          diagnose_unreachable(state, source, target, *source_binding, *target_binding, state.budget_exceeded,
                               hop_limited);
      denial.logical_edge = planned[li].id;
      denial.participant = logical.src;
      outcome.denials.push_back(std::move(denial));
      if (!limits.collect_all_denials) {
        break;
      }
      continue;
    }

    bool hop_ok = true;
    std::vector<Candidate> pool;
    pool.push_back(make_candidate(state, best.edges, &hop_ok));

    std::vector<EdgeIndex> blocked_edges = best.edges;
    std::vector<NodeIndex> blocked_nodes;
    if (node_disjoint) {
      for (std::size_t index = 0; index + 1u < best.edges.size(); ++index) {
        blocked_nodes.push_back(fabric.node_index(fabric.edges()[best.edges[index]].to));
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
      return shortest_path(state, source, target, *source_binding, *target_binding);
    };

    // Greedy diversity pass: remove what the previous path used and look again.
    std::vector<Candidate> chain;
    for (std::size_t step = 1; step < wanted; ++step) {
      const SearchOutcome next = search_blocked(blocked_edges, blocked_nodes);
      if (!next.found) {
        break;
      }
      bool candidate_ok = true;
      Candidate candidate_entry = make_candidate(state, next.edges, &candidate_ok);
      if (!candidate_ok) {
        break;
      }
      blocked_edges.insert(blocked_edges.end(), next.edges.begin(), next.edges.end());
      if (node_disjoint) {
        for (std::size_t index = 0; index + 1u < next.edges.size(); ++index) {
          blocked_nodes.push_back(fabric.node_index(fabric.edges()[next.edges[index]].to));
        }
      }
      chain.push_back(std::move(candidate_entry));
    }
    for (Candidate& entry : chain) {
      if (pool.size() >= pool_capacity) {
        break;
      }
      const bool duplicate = std::any_of(pool.begin(), pool.end(),
                                         [&](const Candidate& other) { return same_path(other, entry); });
      if (!duplicate) {
        pool.push_back(std::move(entry));
      }
    }

    // Alternative candidates: block one resource of the best path at a time.
    std::vector<Candidate> alternatives;
    const auto consider = [&](const SearchOutcome& found) {
      if (!found.found) {
        return;
      }
      bool candidate_ok = true;
      Candidate entry = make_candidate(state, found.edges, &candidate_ok);
      if (!candidate_ok) {
        return;
      }
      for (const Candidate& existing : pool) {
        if (same_path(existing, entry)) {
          return;
        }
      }
      for (const Candidate& existing : alternatives) {
        if (same_path(existing, entry)) {
          return;
        }
      }
      alternatives.push_back(std::move(entry));
    };

    for (const EdgeIndex edge : best.edges) {
      consider(search_blocked({edge}, {}));
    }
    if (node_disjoint) {
      for (const NodeIndex node : blocked_nodes) {
        consider(search_blocked({}, {node}));
      }
    }
    if (policy.domain_diversity != DomainDiversity::kNone) {
      for (const FailureDomainId& domain : pool.front().signature) {
        const auto found = edges_by_domain.find(domain);
        if (found == edges_by_domain.end()) {
          continue;
        }
        consider(search_blocked(found->second, {}));
      }
    }
    std::sort(alternatives.begin(), alternatives.end(), candidate_less);
    for (Candidate& entry : alternatives) {
      if (pool.size() >= pool_capacity) {
        break;
      }
      pool.push_back(std::move(entry));
    }

    const Selection selection = select_combination(state, pool, wanted);
    if (!selection.found) {
      DenialDetail denial;
      denial.logical_edge = planned[li].id;
      denial.participant = logical.src;
      if (policy.disjointness != Disjointness::kNone && wanted > 1u &&
          selection.failure.conflict == ConflictKind::kDisjointness) {
        const std::uint32_t certificate =
            max_disjoint_paths(state, source, target, node_disjoint, static_cast<std::uint32_t>(wanted));
        denial.code = ErrorCode::kInsufficientDisjointPaths;
        denial.conflict = ConflictKind::kDisjointness;
        if (certificate < wanted) {
          denial.message = "the fabric admits at most " + std::to_string(certificate) + " " +
                           (node_disjoint ? "node-disjoint" : "edge-disjoint") + " paths between the endpoints (" +
                           std::to_string(wanted) + " required)";
        } else {
          denial.message = selection.failure.detail + "; the fabric admits at least " +
                           std::to_string(certificate) + " disjoint paths but they were not found within the " +
                           std::to_string(pool.size()) + " retained candidates";
        }
      } else if (selection.failure.conflict == ConflictKind::kInsufficientCapacity) {
        denial.code = ErrorCode::kInsufficientCapacity;
        denial.conflict = ConflictKind::kInsufficientCapacity;
        denial.message = selection.failure.detail;
      } else if (policy.domain_diversity == DomainDiversity::kRequired) {
        denial.code = ErrorCode::kDomainDiversityUnsatisfiable;
        denial.conflict = ConflictKind::kDomainDiversity;
        denial.message = selection.failure.conflict == ConflictKind::kDomainDiversity
                             ? selection.failure.detail
                             : "no candidate set is independent at failure-domain level " +
                                   std::string(to_string(policy.domain_diversity_level));
      } else {
        denial.code = selection.failure.code == ErrorCode::kInternalError ? ErrorCode::kNoPath
                                                                          : selection.failure.code;
        denial.conflict = selection.failure.conflict == ConflictKind::kNone ? ConflictKind::kDisjointness
                                                                            : selection.failure.conflict;
        denial.message = selection.failure.detail.empty() ? "no admissible path set was found"
                                                          : selection.failure.detail;
      }
      outcome.denials.push_back(std::move(denial));
      if (!limits.collect_all_denials) {
        break;
      }
      continue;
    }

    planned[li].chosen = selection.indices;
    for (const std::size_t index : selection.indices) {
      const Candidate& chosen = pool[index];
      for (const EdgeIndex edge : chosen.edges) {
        state.allocated[edge] = saturating_add(state.allocated[edge], state.per_path_demand, kCostCeiling);
        // The plan's allocation table is accumulated here, while this logical
        // edge's per-path demand is still in scope. Reading it back at assembly
        // time would use the demand of whichever logical edge ran last.
        planned_allocation[edge] =
            saturating_add(planned_allocation[edge], state.per_path_demand, kCostCeiling);
      }
    }
    candidates[li] = std::move(pool);
  }

  outcome.search_expansions = state.expansions;
  if (!outcome.denials.empty()) {
    // A denial is terminal: never return a partial mapping beside it.
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

  std::uint64_t path_id = 0;
  std::uint64_t total_cost = 0;
  std::uint64_t max_path_cost = 0;
  std::uint64_t bottleneck = kCostCeiling;
  std::size_t hop_count = 0;
  EvidenceClass weakest = EvidenceClass::kMeasured;

  for (std::size_t li = 0; li < logical_count; ++li) {
    std::vector<std::size_t> order = planned[li].chosen;
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
      return candidate_less(candidates[li][lhs], candidates[li][rhs]);
    });
    const std::uint32_t stage = planned[li].stage;
    plan.stages[stage].logical_edges.push_back(planned[li].id);
    for (const std::size_t index : order) {
      const Candidate& chosen = candidates[li][index];
      PathPlan entry;
      entry.id = PathId{++path_id};
      entry.logical_edge = planned[li].id;
      entry.src = planned[li].src;
      entry.dst = planned[li].dst;
      entry.stage = stage;
      entry.cost = chosen.cost;
      entry.bottleneck_mbps = chosen.bottleneck;
      entry.domain_signature = chosen.signature;
      entry.weakest_evidence = chosen.weakest;
      for (const EdgeIndex edge_index : chosen.edges) {
        const Edge& edge = fabric.edges()[edge_index];
        entry.hops.push_back(Hop{edge.id, edge.from, edge.to});
      }
      hop_count += entry.hops.size();
      total_cost = saturating_add(total_cost, entry.cost, kCostCeiling);
      max_path_cost = std::max(max_path_cost, entry.cost);
      if (!entry.hops.empty()) {
        // A zero-hop mapping consumes no physical capacity and must not be read
        // as "zero bottleneck".
        bottleneck = std::min(bottleneck, entry.bottleneck_mbps);
      }
      if (static_cast<std::uint8_t>(entry.weakest_evidence) < static_cast<std::uint8_t>(weakest)) {
        weakest = entry.weakest_evidence;
      }
      plan.stages[stage].paths.push_back(entry.id);
      plan.paths.push_back(std::move(entry));
    }
  }

  if (collective.demand_mbps > 0) {
    for (std::size_t index = 0; index < fabric.edge_count(); ++index) {
      if (planned_allocation[index] == 0) {
        continue;
      }
      plan.allocations.push_back(Allocation{fabric.edges()[index].id, planned_allocation[index]});
    }
  }

  plan.stats.path_count = plan.paths.size();
  plan.stats.hop_count = hop_count;
  plan.stats.logical_edge_count = planned.size();
  plan.stats.total_cost = total_cost;
  plan.stats.max_path_cost = max_path_cost;
  plan.stats.bottleneck_mbps = plan.paths.empty() || bottleneck == kCostCeiling ? 0u : bottleneck;
  plan.weakest_evidence = weakest;

  if (Status status = validate_plan(plan, request); !status.is_ok()) {
    DenialDetail denial = make_denial(ErrorCode::kInternalError, ConflictKind::kStructure);
    denial.message = std::string("produced plan failed self-validation: ") + status.to_string();
    outcome.denials.push_back(std::move(denial));
    return outcome;
  }

  outcome.plan = std::move(plan);
  return outcome;
}

Result<std::vector<Hop>> find_path(const PlanningRequest& request, const NodeId& source, const NodeId& destination,
                                   const PlannerLimits& limits) {
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
  state.allocated.assign(fabric.edge_count(), 0u);
  state.blocks.resize(fabric.node_count(), fabric.edge_count());
  state.per_path_demand = 0;
  state.hop_limit = request.policy.max_hops;
  (void)limits;

  for (std::size_t index = 0; index < fabric.node_count(); ++index) {
    const Node& node = fabric.nodes()[index];
    bool allowed = node.eligible && !tier_forbidden(request.policy, node.tier) &&
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
  }

  const EndpointBinding unconstrained;
  const SearchOutcome found = shortest_path(state, source_index, target_index, unconstrained, unconstrained);
  if (!found.found) {
    if (state.budget_exceeded) {
      return Result<std::vector<Hop>>::failure(ErrorCode::kSearchBudgetExceeded,
                                               "search budget exhausted before a path was decided");
    }
    return Result<std::vector<Hop>>::failure(ErrorCode::kNoPath, "no path satisfies the policy");
  }
  std::vector<Hop> hops;
  hops.reserve(found.edges.size());
  for (const EdgeIndex index : found.edges) {
    const Edge& edge = fabric.edges()[index];
    hops.push_back(Hop{edge.id, edge.from, edge.to});
  }
  return Result<std::vector<Hop>>::success(std::move(hops));
}

}  // namespace cpath
