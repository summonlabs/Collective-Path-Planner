// Collective Path Planner - exhaustive reference solver for small instances.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// See exact_solver.hpp for the contract. This file deliberately re-implements
// every constraint from the public model instead of calling into the library's
// planner: sharing code would make a differential test vacuous.
#include "exact_solver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cpath/collective.hpp"
#include "cpath/digest.hpp"
#include "cpath/fabric.hpp"
#include "cpath/limits.hpp"
#include "cpath/policy.hpp"
#include "cpath/status.hpp"

namespace cpath_test {
namespace {

using cpath::Collective;
using cpath::Disjointness;
using cpath::DomainDiversity;
using cpath::DomainKind;
using cpath::Edge;
using cpath::EdgeId;
using cpath::EndpointBinding;
using cpath::EvidenceClass;
using cpath::EvidenceRequirement;
using cpath::FailureDomain;
using cpath::FailureDomainId;
using cpath::FabricGraph;
using cpath::LogicalEdge;
using cpath::Policy;
using cpath::TierId;

constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;
constexpr std::uint32_t kNoEdge = 0xFFFFFFFFu;

// Saturating arithmetic, identical in meaning to the library's cost model.
std::uint64_t sat_add(std::uint64_t lhs, std::uint64_t rhs) {
  return cpath::saturating_add(lhs, rhs, cpath::kCostCeiling);
}

bool tier_forbidden(const Policy& policy, const TierId& tier) {
  return tier.valid() && std::binary_search(policy.forbidden_tiers.begin(), policy.forbidden_tiers.end(), tier);
}

bool tier_outside_allowed(const std::vector<TierId>& allowed, const TierId& tier) {
  return !tier.valid() || !std::binary_search(allowed.begin(), allowed.end(), tier);
}

bool domain_forbidden(const FabricGraph& fabric, const Policy& policy, const FailureDomainId& domain) {
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

bool evidence_satisfies(EvidenceClass observed, EvidenceRequirement required) {
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

bool binding_allows_edge(const FabricGraph& fabric, const EndpointBinding& binding, const Edge& edge) {
  if (!binding.allowed_failure_domains.empty()) {
    if (!edge.failure_domain.valid()) {
      return false;
    }
    bool contained = false;
    for (const FailureDomainId& allowed : binding.allowed_failure_domains) {
      if (fabric.domain_contains(allowed, edge.failure_domain)) {
        contained = true;
        break;
      }
    }
    if (!contained) {
      return false;
    }
  }
  if (!binding.allowed_tiers.empty() && tier_outside_allowed(binding.allowed_tiers, edge.tier)) {
    return false;
  }
  return true;
}

// The failure domain a hop contributes at the policy's diversity level: the
// first ancestor of the hop's domain with that kind, or the domain itself.
FailureDomainId domain_at_level(const FabricGraph& fabric, const FailureDomainId& domain, DomainKind level) {
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

std::uint64_t edge_cost_of(const Edge& edge, const Policy& policy) {
  const std::uint64_t latency_term =
      cpath::saturating_mul(edge.latency_micros, policy.latency_weight_milli, cpath::kCostCeiling) / 1000u;
  if (policy.congestion_weight_milli == 0 || edge.capacity_mbps == 0) {
    return latency_term;
  }
  const std::uint64_t used = edge.reserved_mbps;
  if (used >= edge.capacity_mbps) {
    return cpath::kCostCeiling;
  }
  const std::uint64_t utilisation_milli =
      cpath::saturating_mul(used, 1000u, cpath::kCostCeiling) / edge.capacity_mbps;
  const std::uint64_t spare_milli = 1000u - utilisation_milli;
  const std::uint64_t penalty = cpath::saturating_mul(policy.congestion_weight_milli, utilisation_milli,
                                                      cpath::kCostCeiling) /
                                (spare_milli == 0 ? 1u : spare_milli);
  return cpath::saturating_add(latency_term, penalty, cpath::kCostCeiling);
}

}  // namespace

// ---------------------------------------------------------------------------
// Solver.
// ---------------------------------------------------------------------------
namespace {

struct RefPath {
  std::vector<std::uint32_t> edges{};  // fabric edge indices, in traversal order
  std::uint64_t cost{0};
  bool domain_known{true};
  std::vector<FailureDomainId> signature{};  // sorted, unique, attachment domains removed
};

struct RefSet {
  std::vector<std::uint32_t> paths{};  // indices into RefEdgePlan::paths, ascending
  std::uint64_t total_cost{0};
  std::size_t conflicts{0};
  std::uint64_t max_path_cost{0};
  std::uint64_t headroom{cpath::kCostCeiling};
  std::vector<std::pair<std::uint32_t, std::uint64_t>> usage{};  // (edge index, committed demand)
};

struct RefEdgePlan {
  std::size_t index{0};
  cpath::LogicalEdgeId id{};
  std::string label{};
  std::vector<RefPath> paths{};
  std::vector<RefSet> sets{};
  std::uint64_t min_cost{0};
  std::size_t min_conflicts{0};
  std::uint64_t min_max_cost{0};
};

bool set_cheaper(const RefSet& lhs, const RefSet& rhs) {
  if (lhs.total_cost != rhs.total_cost) {
    return lhs.total_cost < rhs.total_cost;
  }
  if (lhs.conflicts != rhs.conflicts) {
    return lhs.conflicts < rhs.conflicts;
  }
  if (lhs.max_path_cost != rhs.max_path_cost) {
    return lhs.max_path_cost < rhs.max_path_cost;
  }
  if (lhs.headroom != rhs.headroom) {
    return lhs.headroom > rhs.headroom;
  }
  return lhs.paths < rhs.paths;
}

class ReferenceSolver {
 public:
  ReferenceSolver(const cpath::PlanningRequest& request, const ExactLimits& limits)
      : request_(request),
        limits_(limits),
        fabric_(request.fabric),
        policy_(request.policy),
        collective_(request.collective) {}

  ExactResult run();

 private:
  struct Best {
    bool have{false};
    std::vector<std::size_t> choice{};
    std::uint64_t cost{0};
    std::size_t conflicts{0};
    std::uint64_t max_cost{0};
    std::uint64_t headroom{cpath::kCostCeiling};
    std::vector<std::uint32_t> encoding{};
    bool encoding_valid{false};
  };

  void build_views();
  bool analyse_edge(std::size_t index, ExactResult& result);
  void enumerate_paths(std::uint32_t node, std::uint32_t target, std::size_t hops_left, std::uint64_t cost,
                       const EndpointBinding& source_binding, const EndpointBinding& target_binding);
  bool hop_usable(std::uint32_t edge_index, const EndpointBinding& source_binding,
                  const EndpointBinding& target_binding, std::uint32_t next) const;
  RefPath materialise(const std::vector<std::uint32_t>& edges) const;
  void extend_subsets(std::size_t start);
  void consider_subset();
  bool paths_edge_disjoint(const RefPath& lhs, const RefPath& rhs) const;
  bool paths_node_disjoint(const RefPath& lhs, const RefPath& rhs) const;
  bool paths_independent(const RefPath& lhs, const RefPath& rhs) const;

  void prepare_suffix_bounds();
  void search(std::size_t level, std::uint64_t cost, std::size_t conflicts, std::uint64_t max_cost);
  void evaluate_completion();
  std::vector<std::uint32_t> encode_choice(const std::vector<std::size_t>& choice) const;
  bool bound_is_worse(std::uint64_t cost, std::size_t conflicts, std::uint64_t max_cost) const;

  const cpath::PlanningRequest& request_;
  ExactLimits limits_;
  const FabricGraph& fabric_;
  const Policy& policy_;
  const Collective& collective_;

  std::vector<std::uint8_t> node_allowed_{};
  std::vector<std::uint8_t> edge_usable_{};
  std::vector<std::uint64_t> edge_cost_{};
  std::vector<std::uint64_t> spare_{};
  std::uint64_t per_path_demand_{0};
  std::size_t wanted_{1};

  std::vector<RefEdgePlan> plans_{};

  // Simple-path enumeration scratch.
  std::vector<std::uint8_t> visited_{};
  std::vector<std::uint32_t> current_{};
  std::vector<std::vector<std::uint32_t>> raw_{};
  bool path_overflow_{false};

  // Subset enumeration scratch.
  std::size_t active_edge_{0};
  std::vector<std::uint32_t> chosen_paths_{};
  std::size_t combinations_here_{0};
  bool combination_limited_{false};

  // Global search state.
  std::vector<std::uint64_t> remaining_{};
  std::vector<std::size_t> choice_{};
  std::vector<std::uint64_t> suffix_cost_{};
  std::vector<std::size_t> suffix_conflicts_{};
  std::vector<std::uint64_t> suffix_max_cost_{};
  std::size_t searched_nodes_{0};
  bool search_limited_{false};
  Best best_{};
};

void ReferenceSolver::build_views() {
  const std::size_t node_count = fabric_.node_count();
  const std::size_t edge_count = fabric_.edge_count();
  node_allowed_.assign(node_count, 1u);
  edge_usable_.assign(edge_count, 0u);
  edge_cost_.assign(edge_count, 0u);
  spare_.assign(edge_count, 0u);

  wanted_ = std::max<std::size_t>(1u, policy_.paths_per_logical_edge);
  per_path_demand_ =
      collective_.demand_mbps == 0 ? 0u : (collective_.demand_mbps + wanted_ - 1u) / wanted_;

  for (std::size_t index = 0; index < node_count; ++index) {
    const cpath::Node& node = fabric_.nodes()[index];
    bool allowed = node.eligible;
    if (allowed && tier_forbidden(policy_, node.tier)) {
      allowed = false;
    }
    if (allowed &&
        std::binary_search(policy_.forbidden_nodes.begin(), policy_.forbidden_nodes.end(), node.id)) {
      allowed = false;
    }
    node_allowed_[index] = allowed ? 1u : 0u;
  }

  for (std::size_t index = 0; index < edge_count; ++index) {
    const Edge& edge = fabric_.edges()[index];
    const std::uint64_t spare =
        edge.capacity_mbps == 0 || edge.reserved_mbps >= edge.capacity_mbps ? 0u
                                                                           : edge.capacity_mbps - edge.reserved_mbps;
    spare_[index] = spare;
    edge_cost_[index] = edge_cost_of(edge, policy_);

    bool usable = edge.eligible;
    if (usable && tier_forbidden(policy_, edge.tier)) {
      usable = false;
    }
    if (usable && !policy_.allowed_path_tiers.empty() &&
        tier_outside_allowed(policy_.allowed_path_tiers, edge.tier)) {
      usable = false;
    }
    if (usable && domain_forbidden(fabric_, policy_, edge.failure_domain)) {
      usable = false;
    }
    if (usable && !evidence_satisfies(edge.evidence, policy_.evidence)) {
      usable = false;
    }
    if (usable) {
      usable = edge.capacity_mbps == 0 ? (policy_.allow_unverified_capacity && edge.reserved_mbps == 0)
                                       : edge.reserved_mbps < edge.capacity_mbps;
    }
    // The per-path share of the declared demand must fit in this edge's spare
    // capacity. An edge with no verified capacity carries no such test: the
    // policy already accepted that gap.
    if (usable && per_path_demand_ > 0 && edge.capacity_mbps > 0 && spare < per_path_demand_) {
      usable = false;
    }
    edge_usable_[index] = usable ? 1u : 0u;
  }
}

bool ReferenceSolver::hop_usable(std::uint32_t edge_index, const EndpointBinding& source_binding,
                                 const EndpointBinding& target_binding, std::uint32_t next) const {
  if (edge_usable_[edge_index] == 0u || node_allowed_[next] == 0u) {
    return false;
  }
  const Edge& edge = fabric_.edges()[edge_index];
  return binding_allows_edge(fabric_, source_binding, edge) &&
         binding_allows_edge(fabric_, target_binding, edge);
}

void ReferenceSolver::enumerate_paths(std::uint32_t node, std::uint32_t target, std::size_t hops_left,
                                      std::uint64_t cost, const EndpointBinding& source_binding,
                                      const EndpointBinding& target_binding) {
  if (path_overflow_) {
    return;
  }
  for (const std::uint32_t edge_index : fabric_.out_edges(node)) {
    const Edge& edge = fabric_.edges()[edge_index];
    const std::uint32_t next = fabric_.node_index(edge.to);
    if (next == kNoNode || next == node) {
      continue;
    }
    if (visited_[next] != 0u || !hop_usable(edge_index, source_binding, target_binding, next)) {
      continue;
    }
    const std::uint64_t next_cost = sat_add(cost, edge_cost_[edge_index]);
    if (next_cost > policy_.max_path_cost) {
      continue;
    }
    if (next == target) {
      // The destination is never traversed as an intermediate node.
      current_.push_back(edge_index);
      raw_.push_back(current_);
      current_.pop_back();
      if (raw_.size() > limits_.max_paths_per_edge) {
        path_overflow_ = true;
        return;
      }
      continue;
    }
    if (hops_left <= 1u) {
      continue;
    }
    current_.push_back(edge_index);
    visited_[next] = 1u;
    enumerate_paths(next, target, hops_left - 1u, next_cost, source_binding, target_binding);
    visited_[next] = 0u;
    current_.pop_back();
    if (path_overflow_) {
      return;
    }
  }
}

RefPath ReferenceSolver::materialise(const std::vector<std::uint32_t>& edges) const {
  RefPath path;
  path.edges = edges;
  std::vector<FailureDomainId> hop_domains;
  hop_domains.reserve(edges.size());
  for (const std::uint32_t edge_index : edges) {
    const Edge& edge = fabric_.edges()[edge_index];
    path.cost = sat_add(path.cost, edge_cost_[edge_index]);
    if (policy_.domain_diversity == DomainDiversity::kNone) {
      continue;
    }
    if (!edge.failure_domain.valid()) {
      path.domain_known = false;
      continue;
    }
    const FailureDomainId level_domain =
        domain_at_level(fabric_, edge.failure_domain, policy_.domain_diversity_level);
    if (!level_domain.valid()) {
      path.domain_known = false;
      continue;
    }
    hop_domains.push_back(level_domain);
  }
  // Diversity is judged on what a path adds beyond the domains its endpoints
  // unavoidably sit in: the first and last hop attachment domains are shared by
  // every sibling and carry no discriminating information.
  if (policy_.domain_diversity != DomainDiversity::kNone && !hop_domains.empty()) {
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
      path.signature.push_back(domain);
    }
  }
  std::sort(path.signature.begin(), path.signature.end());
  path.signature.erase(std::unique(path.signature.begin(), path.signature.end()), path.signature.end());
  return path;
}

bool ReferenceSolver::paths_edge_disjoint(const RefPath& lhs, const RefPath& rhs) const {
  for (const std::uint32_t left : lhs.edges) {
    if (std::find(rhs.edges.begin(), rhs.edges.end(), left) != rhs.edges.end()) {
      return false;
    }
  }
  return true;
}

bool ReferenceSolver::paths_node_disjoint(const RefPath& lhs, const RefPath& rhs) const {
  for (std::size_t left = 0; left + 1u < lhs.edges.size(); ++left) {
    const cpath::NodeId& left_node = fabric_.edges()[lhs.edges[left]].to;
    for (std::size_t right = 0; right + 1u < rhs.edges.size(); ++right) {
      if (left_node == fabric_.edges()[rhs.edges[right]].to) {
        return false;
      }
    }
  }
  return true;
}

bool ReferenceSolver::paths_independent(const RefPath& lhs, const RefPath& rhs) const {
  if (!lhs.domain_known || !rhs.domain_known) {
    // An edge with no declared failure domain makes the path unprovable, so the
    // pair is not independent.
    return false;
  }
  for (const FailureDomainId& domain : lhs.signature) {
    if (std::binary_search(rhs.signature.begin(), rhs.signature.end(), domain)) {
      return false;
    }
  }
  return true;
}

void ReferenceSolver::extend_subsets(std::size_t start) {
  if (combination_limited_) {
    return;
  }
  const RefEdgePlan& plan = plans_[active_edge_];
  if (chosen_paths_.size() == wanted_) {
    consider_subset();
    return;
  }
  const std::size_t needed = wanted_ - chosen_paths_.size();
  for (std::size_t candidate = start; candidate + needed <= plan.paths.size(); ++candidate) {
    bool compatible = true;
    for (const std::uint32_t existing : chosen_paths_) {
      const RefPath& lhs = plan.paths[existing];
      const RefPath& rhs = plan.paths[candidate];
      if (policy_.disjointness != Disjointness::kNone && !paths_edge_disjoint(lhs, rhs)) {
        compatible = false;
        break;
      }
      if (policy_.disjointness == Disjointness::kNode && !paths_node_disjoint(lhs, rhs)) {
        compatible = false;
        break;
      }
      if (policy_.domain_diversity == DomainDiversity::kRequired && !paths_independent(lhs, rhs)) {
        compatible = false;
        break;
      }
    }
    if (!compatible) {
      continue;
    }
    chosen_paths_.push_back(static_cast<std::uint32_t>(candidate));
    extend_subsets(candidate + 1u);
    chosen_paths_.pop_back();
    if (combination_limited_) {
      return;
    }
  }
}

void ReferenceSolver::consider_subset() {
  ++combinations_here_;
  if (combinations_here_ > limits_.max_combinations_per_edge) {
    combination_limited_ = true;
    return;
  }
  const RefEdgePlan& plan = plans_[active_edge_];
  RefSet set;
  set.paths = chosen_paths_;
  std::sort(set.paths.begin(), set.paths.end());
  for (const std::uint32_t path_index : set.paths) {
    const RefPath& path = plan.paths[path_index];
    set.total_cost = sat_add(set.total_cost, path.cost);
    set.max_path_cost = std::max(set.max_path_cost, path.cost);
    for (const std::uint32_t edge_index : path.edges) {
      if (fabric_.edges()[edge_index].capacity_mbps == 0) {
        continue;  // unverified capacity is not capacity
      }
      bool merged = false;
      for (auto& entry : set.usage) {
        if (entry.first == edge_index) {
          entry.second = sat_add(entry.second, per_path_demand_);
          merged = true;
          break;
        }
      }
      if (!merged) {
        set.usage.emplace_back(edge_index, per_path_demand_);
      }
    }
  }
  std::sort(set.usage.begin(), set.usage.end());
  // The set as a whole must fit in the fabric's verified spare capacity.
  for (const auto& entry : set.usage) {
    if (entry.second > spare_[entry.first]) {
      return;
    }
  }
  if (policy_.domain_diversity != DomainDiversity::kNone) {
    for (std::size_t lhs = 0; lhs < set.paths.size(); ++lhs) {
      for (std::size_t rhs = lhs + 1u; rhs < set.paths.size(); ++rhs) {
        if (!paths_independent(plan.paths[set.paths[lhs]], plan.paths[set.paths[rhs]])) {
          ++set.conflicts;
        }
      }
    }
  }
  set.headroom = cpath::kCostCeiling;
  for (const auto& entry : set.usage) {
    const std::uint64_t left =
        entry.second >= spare_[entry.first] ? 0u : spare_[entry.first] - entry.second;
    set.headroom = std::min(set.headroom, left);
  }
  plans_[active_edge_].sets.push_back(std::move(set));
}

bool ReferenceSolver::analyse_edge(std::size_t index, ExactResult& result) {
  const LogicalEdge& logical = collective_.logical_edges[index];
  RefEdgePlan& plan = plans_[index];
  plan.index = index;
  plan.id = collective_.logical_edge_id(index);
  plan.label = "logical edge " + cpath::to_string(plan.id) + " (" + logical.src.value() + " -> " +
               logical.dst.value() + ")";

  const EndpointBinding* source_binding = request_.find_binding(logical.src);
  const EndpointBinding* target_binding = request_.find_binding(logical.dst);
  if (source_binding == nullptr || target_binding == nullptr) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail = plan.label + ": an endpoint participant has no binding, so no mapping is defined";
    return false;
  }
  const std::uint32_t source = fabric_.node_index(source_binding->node);
  const std::uint32_t target = fabric_.node_index(target_binding->node);
  if (source == kNoNode || target == kNoNode) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail = plan.label + ": an endpoint binding names a node that is not in the fabric";
    return false;
  }
  if (node_allowed_[source] == 0u || node_allowed_[target] == 0u) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail = plan.label + ": a bound endpoint node is ineligible under the policy";
    return false;
  }

  raw_.clear();
  current_.clear();
  visited_.assign(fabric_.node_count(), 0u);
  visited_[source] = 1u;
  path_overflow_ = false;
  enumerate_paths(source, target, policy_.max_hops, 0, *source_binding, *target_binding);
  if (path_overflow_) {
    result.status = ExactResult::Status::kLimitReached;
    result.detail = plan.label + ": the simple-path enumeration exceeded max_paths_per_edge=" +
                    std::to_string(limits_.max_paths_per_edge);
    return false;
  }
  result.paths_enumerated += raw_.size();
  plan.paths.reserve(raw_.size());
  for (const std::vector<std::uint32_t>& edges : raw_) {
    plan.paths.push_back(materialise(edges));
  }
  raw_.clear();
  std::sort(plan.paths.begin(), plan.paths.end(),
            [](const RefPath& lhs, const RefPath& rhs) { return lhs.edges < rhs.edges; });

  active_edge_ = index;
  chosen_paths_.clear();
  combinations_here_ = 0;
  combination_limited_ = false;
  if (plan.paths.size() >= wanted_) {
    extend_subsets(0);
  }
  result.combinations += combinations_here_;
  if (combination_limited_) {
    result.status = ExactResult::Status::kLimitReached;
    result.detail = plan.label + ": the k-subset enumeration exceeded max_combinations_per_edge=" +
                    std::to_string(limits_.max_combinations_per_edge) + " (" +
                    std::to_string(plan.paths.size()) + " simple paths, k=" + std::to_string(wanted_) + ")";
    return false;
  }

  std::sort(plan.sets.begin(), plan.sets.end(), set_cheaper);
  if (plan.sets.empty()) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail = plan.label + ": the enumeration completed (" + std::to_string(plan.paths.size()) +
                    " compliant simple paths) and no set of " + std::to_string(wanted_) +
                    " paths satisfies the disjointness, failure-domain diversity and capacity "
                    "constraints";
    return false;
  }
  plan.min_cost = plan.sets.front().total_cost;
  plan.min_conflicts = plan.sets.front().conflicts;
  plan.min_max_cost = plan.sets.front().max_path_cost;
  for (const RefSet& set : plan.sets) {
    plan.min_cost = std::min(plan.min_cost, set.total_cost);
    plan.min_conflicts = std::min(plan.min_conflicts, set.conflicts);
    plan.min_max_cost = std::min(plan.min_max_cost, set.max_path_cost);
  }
  return true;
}

void ReferenceSolver::prepare_suffix_bounds() {
  const std::size_t levels = plans_.size();
  suffix_cost_.assign(levels + 1u, 0u);
  suffix_conflicts_.assign(levels + 1u, 0u);
  suffix_max_cost_.assign(levels + 1u, 0u);
  for (std::size_t back = levels; back-- > 0u;) {
    suffix_cost_[back] = sat_add(suffix_cost_[back + 1u], plans_[back].min_cost);
    suffix_conflicts_[back] = suffix_conflicts_[back + 1u] + plans_[back].min_conflicts;
    suffix_max_cost_[back] = std::max(suffix_max_cost_[back + 1u], plans_[back].min_max_cost);
  }
}

bool ReferenceSolver::bound_is_worse(std::uint64_t cost, std::size_t conflicts,
                                     std::uint64_t max_cost) const {
  if (!best_.have) {
    return false;
  }
  if (cost != best_.cost) {
    return cost > best_.cost;
  }
  if (conflicts != best_.conflicts) {
    return conflicts > best_.conflicts;
  }
  if (max_cost != best_.max_cost) {
    return max_cost > best_.max_cost;
  }
  return false;  // equal at levels 2-4: levels 5 and 6 may still improve
}

std::vector<std::uint32_t> ReferenceSolver::encode_choice(const std::vector<std::size_t>& choice) const {
  std::vector<std::uint32_t> encoding;
  for (std::size_t level = 0; level < plans_.size(); ++level) {
    const RefSet& set = plans_[level].sets[choice[level]];
    std::vector<const std::vector<std::uint32_t>*> paths;
    paths.reserve(set.paths.size());
    for (const std::uint32_t path_index : set.paths) {
      paths.push_back(&plans_[level].paths[path_index].edges);
    }
    std::sort(paths.begin(), paths.end(),
              [](const std::vector<std::uint32_t>* lhs, const std::vector<std::uint32_t>* rhs) {
                return *lhs < *rhs;
              });
    for (const std::vector<std::uint32_t>* path : paths) {
      encoding.insert(encoding.end(), path->begin(), path->end());
    }
  }
  return encoding;
}

void ReferenceSolver::evaluate_completion() {
  std::uint64_t cost = 0;
  std::size_t conflicts = 0;
  std::uint64_t max_cost = 0;
  std::uint64_t headroom = cpath::kCostCeiling;
  for (std::size_t level = 0; level < plans_.size(); ++level) {
    const RefSet& set = plans_[level].sets[choice_[level]];
    cost = sat_add(cost, set.total_cost);
    conflicts += set.conflicts;
    max_cost = std::max(max_cost, set.max_path_cost);
    for (const auto& entry : set.usage) {
      headroom = std::min(headroom, remaining_[entry.first]);
    }
  }

  int order = 0;
  if (best_.have) {
    if (cost != best_.cost) {
      order = cost < best_.cost ? -1 : 1;
    } else if (conflicts != best_.conflicts) {
      order = conflicts < best_.conflicts ? -1 : 1;
    } else if (max_cost != best_.max_cost) {
      order = max_cost < best_.max_cost ? -1 : 1;
    } else if (headroom != best_.headroom) {
      order = headroom > best_.headroom ? -1 : 1;
    }
    if (order > 0) {
      return;
    }
    if (order == 0) {
      if (!best_.encoding_valid) {
        best_.encoding = encode_choice(best_.choice);
        best_.encoding_valid = true;
      }
      const std::vector<std::uint32_t> encoding = encode_choice(choice_);
      if (!(encoding < best_.encoding)) {
        return;
      }
    }
  }

  best_.have = true;
  best_.choice = choice_;
  best_.cost = cost;
  best_.conflicts = conflicts;
  best_.max_cost = max_cost;
  best_.headroom = headroom;
  if (order == 0) {
    best_.encoding = encode_choice(choice_);
    best_.encoding_valid = true;
  } else {
    best_.encoding.clear();
    best_.encoding_valid = false;
  }
}

void ReferenceSolver::search(std::size_t level, std::uint64_t cost, std::size_t conflicts,
                             std::uint64_t max_cost) {
  if (search_limited_) {
    return;
  }
  if (level == plans_.size()) {
    evaluate_completion();
    return;
  }
  const std::vector<RefSet>& sets = plans_[level].sets;
  for (std::size_t index = 0; index < sets.size(); ++index) {
    const RefSet& set = sets[index];
    bool fits = true;
    for (const auto& entry : set.usage) {
      if (entry.second > remaining_[entry.first]) {
        fits = false;
        break;
      }
    }
    if (!fits) {
      continue;
    }
    ++searched_nodes_;
    if (searched_nodes_ > limits_.max_global_nodes) {
      search_limited_ = true;
      return;
    }
    const std::uint64_t bound_cost = sat_add(sat_add(cost, set.total_cost), suffix_cost_[level + 1u]);
    const std::size_t bound_conflicts = conflicts + set.conflicts + suffix_conflicts_[level + 1u];
    const std::uint64_t bound_max =
        std::max(std::max(max_cost, set.max_path_cost), suffix_max_cost_[level + 1u]);
    if (bound_is_worse(bound_cost, bound_conflicts, bound_max)) {
      continue;
    }
    for (const auto& entry : set.usage) {
      remaining_[entry.first] -= entry.second;
    }
    choice_[level] = index;
    search(level + 1u, sat_add(cost, set.total_cost), conflicts + set.conflicts,
           std::max(max_cost, set.max_path_cost));
    for (const auto& entry : set.usage) {
      remaining_[entry.first] += entry.second;
    }
    if (search_limited_) {
      return;
    }
  }
}

ExactResult ReferenceSolver::run() {
  ExactResult result;
  const cpath::Status validation = request_.validate();
  if (!validation.is_ok()) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail = "the request does not validate, so no mapping is defined: " + validation.to_string();
    return result;
  }
  build_views();

  plans_.assign(collective_.logical_edges.size(), RefEdgePlan{});
  for (std::size_t index = 0; index < plans_.size(); ++index) {
    if (!analyse_edge(index, result)) {
      return result;
    }
  }
  if (plans_.empty()) {
    result.status = ExactResult::Status::kFeasible;
    result.detail = "the collective declares no logical edges: the empty assignment is optimal";
    return result;
  }

  prepare_suffix_bounds();
  remaining_ = spare_;
  choice_.assign(plans_.size(), 0u);
  search(0, 0, 0, 0);
  result.global_nodes = searched_nodes_;
  if (search_limited_) {
    result.status = ExactResult::Status::kLimitReached;
    result.detail = "the global assignment search exceeded max_global_nodes=" +
                    std::to_string(limits_.max_global_nodes) + " after " +
                    std::to_string(searched_nodes_) + " nodes" +
                    (best_.have ? "; a feasible assignment was seen, but minimality was not proven"
                                : "; no assignment had been completed yet");
    return result;
  }
  if (!best_.have) {
    result.status = ExactResult::Status::kInfeasible;
    result.detail =
        "every logical edge's simple paths and k-subsets were enumerated in full and the exhaustive "
        "assignment search found no combination that fits the whole-request capacity budget";
    return result;
  }

  result.status = ExactResult::Status::kFeasible;
  result.total_cost = best_.cost;
  result.domain_conflicts = best_.conflicts;
  result.max_path_cost = best_.max_cost;
  result.assignment.assign(plans_.size(), {});
  for (std::size_t level = 0; level < plans_.size(); ++level) {
    const RefSet& set = plans_[level].sets[best_.choice[level]];
    std::vector<const std::vector<std::uint32_t>*> paths;
    paths.reserve(set.paths.size());
    for (const std::uint32_t path_index : set.paths) {
      paths.push_back(&plans_[level].paths[path_index].edges);
    }
    std::sort(paths.begin(), paths.end(),
              [](const std::vector<std::uint32_t>* lhs, const std::vector<std::uint32_t>* rhs) {
                return *lhs < *rhs;
              });
    std::vector<std::vector<EdgeId>> choice;
    choice.reserve(paths.size());
    for (const std::vector<std::uint32_t>* path : paths) {
      std::vector<EdgeId> hops;
      hops.reserve(path->size());
      for (const std::uint32_t edge_index : *path) {
        hops.push_back(fabric_.edges()[edge_index].id);
      }
      choice.push_back(std::move(hops));
    }
    result.assignment[level] = std::move(choice);
  }
  result.detail = "exhaustive: " + std::to_string(result.paths_enumerated) + " simple paths, " +
                  std::to_string(result.combinations) + " path subsets, " +
                  std::to_string(result.global_nodes) + " assignment nodes";
  return result;
}

}  // namespace

const char* to_string(ExactResult::Status status) noexcept {
  switch (status) {
    case ExactResult::Status::kFeasible:
      return "feasible";
    case ExactResult::Status::kInfeasible:
      return "infeasible";
    case ExactResult::Status::kLimitReached:
      return "limit-reached";
  }
  return "limit-reached";
}

ExactResult solve_exact(const cpath::PlanningRequest& request, const ExactLimits& limits) {
  ReferenceSolver solver(request, limits);
  return solver.run();
}

}  // namespace cpath_test
