// Collective Path Planner - algorithm attack suite.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This suite attacks the PLANNER, not the serialization around it. Every case
// builds a PlanningRequest in code, runs plan_collective(), and then re-derives
// the contract from the produced artefact: validate_plan() against the request
// it claims to have used, plus direct per-hop checks that do not trust any
// planner summary. A denial is never accepted at face value either: its code,
// its ConflictKind and its epistemic category are checked against the claim the
// request actually justifies, and "proven infeasible" is never allowed to stand
// in for "the search stopped at a bound".
//
// Family map:
//   A  cheapest-first trap                J  edge-disjoint but not node-disjoint
//   B  locally expensive / globally needed K  failure-domain traps
//   C  candidate-pool truncation          L  high branching factor
//   D  cyclic graphs and walks            M  long narrow graphs at the hop bound
//   E  symmetric equal-cost tie stability N  disconnected fabrics
//   F  parallel physical links            O  irrelevant additional edges
//   G  zero-capacity links                P  beneficial additional edges
//   H  bottleneck cuts                    Q  canonical graph-order independence
//   I  node-disjoint sharing endpoints    R  saturated cost arithmetic
//
// Every fabric in this file is SYNTHETIC: constructed by the test from declared
// capacity, latency and failure-domain labels, never measured on a device. The
// numbers carry no authority; they exist only to make the attack sharp.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/planner.hpp"
#include "fixtures.hpp"
#include "test_framework.hpp"

namespace {

using cpath::DenialKind;
using cpath::Disjointness;
using cpath::DomainDiversity;
using cpath::DomainKind;
using cpath::ErrorCode;
using cpath::EvidenceClass;
using cpath::EvidenceRequirement;
using cpath::PlanningOutcome;
using cpath::PlanningRequest;
using cpath::PlannerLimits;
using cpath::Policy;

// ---------------------------------------------------------------------------
// Fabric construction helpers.
// ---------------------------------------------------------------------------
struct Fabric {
  cpath::FabricSpec spec{};

  Fabric() {
    spec.topology_generation = cpath::TopologyGeneration{7};
    spec.failure_domain_generation = cpath::FailureDomainGeneration{3};
    spec.capacity_evidence_generation = cpath::CapacityEvidenceGeneration{11};
  }

  Fabric& domain(const char* id, DomainKind kind, const char* parent = "") {
    cpath::FailureDomain record;
    record.id = cpath::FailureDomainId{id};
    record.kind = kind;
    if (parent[0] != '\0') {
      record.parent = cpath::FailureDomainId{parent};
    }
    spec.failure_domains.push_back(std::move(record));
    return *this;
  }

  Fabric& node(const char* id, const char* tier = "LEAF", bool eligible = true) {
    cpath::Node record;
    record.id = cpath::NodeId{id};
    record.tier = cpath::TierId{tier};
    record.eligible = eligible;
    spec.nodes.push_back(std::move(record));
    return *this;
  }

  Fabric& edge(const char* id, const char* from, const char* to, std::uint64_t capacity, std::uint64_t latency,
               const char* domain = "", std::uint64_t reserved = 0,
               EvidenceClass evidence = EvidenceClass::kSynthetic, const char* tier = "LEAF",
               bool eligible = true) {
    cpath::Edge record;
    record.id = cpath::EdgeId{id};
    record.from = cpath::NodeId{from};
    record.to = cpath::NodeId{to};
    record.capacity_mbps = capacity;
    record.latency_micros = latency;
    record.reserved_mbps = reserved;
    record.tier = cpath::TierId{tier};
    if (domain[0] != '\0') {
      record.failure_domain = cpath::FailureDomainId{domain};
    }
    record.evidence = evidence;
    record.eligible = eligible;
    spec.edges.push_back(std::move(record));
    return *this;
  }
};

cpath::EndpointBinding bind(const char* participant, const char* node) {
  cpath::EndpointBinding binding;
  binding.participant = cpath::ParticipantId{participant};
  binding.node = cpath::NodeId{node};
  return binding;
}

cpath::GroupSpec tree_group(const char* id, const char* root, std::vector<const char*> members) {
  cpath::GroupSpec group;
  group.id = cpath::GroupId{id};
  group.level = 0;
  group.pattern = cpath::GroupPattern::kTree;
  group.direction = cpath::TreeDirection::kForward;
  group.root = cpath::ParticipantId{root};
  for (const char* member : members) {
    group.members.push_back(cpath::ParticipantId{member});
  }
  return group;
}

cpath::GroupSpec ring_group(const char* id, std::uint32_t level, std::vector<const char*> members) {
  cpath::GroupSpec group;
  group.id = cpath::GroupId{id};
  group.level = level;
  group.pattern = cpath::GroupPattern::kRing;
  for (const char* member : members) {
    group.members.push_back(cpath::ParticipantId{member});
  }
  return group;
}

// Exactly one logical edge src -> dst, at hierarchy level 0.
cpath::CollectiveSpec one_edge(const char* id, const char* src, const char* dst, std::uint64_t demand = 0) {
  cpath::CollectiveSpec collective;
  collective.id = cpath::CollectiveId{id};
  collective.kind = cpath::CollectiveKind::kTree;
  collective.demand_mbps = demand;
  collective.groups.push_back(tree_group("g0", src, {src, dst}));
  return collective;
}

Policy base_policy(std::size_t max_hops = 8, std::size_t paths_per_logical_edge = 1) {
  Policy policy;
  policy.generation = cpath::PolicyGeneration{5};
  policy.max_hops = max_hops;
  policy.paths_per_logical_edge = paths_per_logical_edge;
  return policy;
}

cpath::RequestSpec make_spec(cpath::CollectiveSpec collective, cpath::FabricSpec fabric, Policy policy,
                             std::vector<cpath::EndpointBinding> bindings) {
  cpath::RequestSpec spec;
  spec.collective = std::move(collective);
  spec.fabric = std::move(fabric);
  spec.policy = std::move(policy);
  spec.plan_generation = cpath::CollectivePlanGeneration{1};
  spec.bindings = std::move(bindings);
  return spec;
}

// ---------------------------------------------------------------------------
// Assertion helpers.
// ---------------------------------------------------------------------------
void fail_at(const char* context, const std::string& detail) {
  ::cpath_test::record_failure(__FILE__, __LINE__, std::string(context) + ": " + detail);
}

std::string render_path(const cpath::PathPlan& path) {
  std::string out = "logical=" + std::to_string(path.logical_edge.value()) + " cost=" + std::to_string(path.cost) +
                    " bottleneck=" + std::to_string(path.bottleneck_mbps) + " hops=[";
  for (std::size_t index = 0; index < path.hops.size(); ++index) {
    if (index != 0u) {
      out += ",";
    }
    out += path.hops[index].edge.value();
  }
  out += "]";
  return out;
}

std::string render_plan(const cpath::Plan& plan) {
  std::string out = "paths=" + std::to_string(plan.paths.size()) + " total=" + std::to_string(plan.stats.total_cost);
  for (const cpath::PathPlan& path : plan.paths) {
    out += " {";
    out += render_path(path);
    out += "}";
  }
  return out;
}

// One line of evidence per planning call: what the planner actually decided.
// Printed so that a run leaves a record of every family's result, including the
// cases where it could not prove or find a mapping.
void report(const char* family, const PlanningOutcome& outcome, const PlanningRequest& request,
            const std::string& label) {
  std::string line = std::string("[algorithm] family ") + family + " " + label + " -> ";
  if (outcome.ok()) {
    const cpath::Status validation = cpath::validate_plan(*outcome.plan, request);
    line += "plan paths=" + std::to_string(outcome.plan->paths.size()) +
            " total_cost=" + std::to_string(outcome.plan->stats.total_cost) +
            " max_path_cost=" + std::to_string(outcome.plan->stats.max_path_cost) +
            " bottleneck_mbps=" + std::to_string(outcome.plan->stats.bottleneck_mbps) +
            " optimal=" + (outcome.optimal ? "1" : "0") +
            " validate_plan=" + (validation.is_ok() ? "ok" : "REJECTED");
  } else if (outcome.denials.empty()) {
    line += "denied with no denial detail (INTERNAL)";
  } else {
    for (const cpath::DenialDetail& denial : outcome.denials) {
      line += std::string("denied code=") + cpath::code_symbol(denial.code) +
              " kind=" + cpath::to_string(denial.kind) + " conflict=" + cpath::to_string(denial.conflict) +
              " proven=" + (outcome.proven_infeasible() ? "1" : "0") +
              " indeterminate=" + (outcome.indeterminate() ? "1" : "0") + " message=\"" + denial.message + "\"";
    }
  }
  std::cout << line << "\n";
}

bool path_uses_edge(const cpath::PathPlan& path, const char* edge_id) {
  for (const cpath::Hop& hop : path.hops) {
    if (hop.edge.value() == edge_id) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> hop_edge_ids(const cpath::PathPlan& path) {
  std::vector<std::string> out;
  out.reserve(path.hops.size());
  for (const cpath::Hop& hop : path.hops) {
    out.push_back(hop.edge.value());
  }
  return out;
}

// The node sequence of a path: the source of the first hop, then the head of
// every hop. A repeated entry is a revisited physical node.
std::vector<std::string> hop_node_sequence(const cpath::PathPlan& path) {
  std::vector<std::string> out;
  if (!path.hops.empty()) {
    out.push_back(path.hops.front().from.value());
  }
  for (const cpath::Hop& hop : path.hops) {
    out.push_back(hop.to.value());
  }
  return out;
}

bool has_duplicate(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

// Direct re-derivation of the simplicity promise: no physical edge twice, no
// physical node twice, hops contiguous, and the path anchored on the request's
// bound endpoints. Nothing here consults the planner.
void check_path_contract(const cpath::PathPlan& path, const cpath::PlanningRequest& request, const char* context) {
  CPATH_CHECK(cpath::hops_are_simple(path.hops));
  if (has_duplicate(hop_edge_ids(path))) {
    fail_at(context, "path repeats a physical edge: " + render_path(path));
  }
  if (has_duplicate(hop_node_sequence(path))) {
    fail_at(context, "path revisits a physical node: " + render_path(path));
  }
  for (std::size_t index = 0; index + 1u < path.hops.size(); ++index) {
    if (!(path.hops[index].to == path.hops[index + 1u].from)) {
      fail_at(context, "path hops are not contiguous: " + render_path(path));
    }
  }
  const cpath::LogicalEdge& logical = request.collective.logical_edges[path.logical_edge.value() - 1u];
  const cpath::EndpointBinding* source = request.find_binding(logical.src);
  const cpath::EndpointBinding* target = request.find_binding(logical.dst);
  CPATH_REQUIRE(source != nullptr);
  CPATH_REQUIRE(target != nullptr);
  if (!path.hops.empty()) {
    if (!(path.hops.front().from == source->node)) {
      fail_at(context, "path does not start at the bound source node: " + render_path(path));
    }
    if (!(path.hops.back().to == target->node)) {
      fail_at(context, "path does not end at the bound destination node: " + render_path(path));
    }
  }
  // Every hop must reference a real physical edge with matching endpoints.
  for (const cpath::Hop& hop : path.hops) {
    const cpath::Edge* edge = request.fabric.find_edge(hop.edge);
    if (edge == nullptr) {
      fail_at(context, "hop references an edge that is not in the fabric: " + hop.edge.value());
      continue;
    }
    if (!(edge->from == hop.from) || !(edge->to == hop.to)) {
      fail_at(context, "hop endpoints disagree with the physical edge " + hop.edge.value());
    }
  }
}

// Denial shape: no plan, a non-empty explanation, a precise code, a declared
// DenialKind, and epistemics that match the kinds actually reported.
void check_denial_shape(const PlanningOutcome& outcome, const char* context) {
  CPATH_CHECK(!outcome.plan.has_value());
  CPATH_CHECK(!outcome.ok());
  if (outcome.denials.empty()) {
    fail_at(context, "a refusal must carry at least one DenialDetail");
    return;
  }
  bool any_limit = false;
  bool all_proven = true;
  for (const cpath::DenialDetail& denial : outcome.denials) {
    if (denial.message.empty()) {
      fail_at(context, "denial message is empty");
    }
    if (denial.code == ErrorCode::kOk || denial.code == ErrorCode::kInternalError) {
      fail_at(context, std::string("denial carries an imprecise code: ") + cpath::code_symbol(denial.code));
    }
    if (denial.kind == DenialKind::kSearchLimitReached) {
      any_limit = true;
      all_proven = false;
    }
  }
  if (outcome.indeterminate() != any_limit) {
    fail_at(context, "indeterminate() disagrees with the reported denial kinds");
  }
  if (outcome.proven_infeasible() != all_proven) {
    fail_at(context, "proven_infeasible() disagrees with the reported denial kinds");
  }
  if (outcome.primary_kind() != outcome.denials.front().kind) {
    fail_at(context, "primary_kind() does not report the first denial");
  }
  if (outcome.primary_code() != outcome.denials.front().code) {
    fail_at(context, "primary_code() does not report the first denial");
  }
}

void require_denial(const PlanningOutcome& outcome, ErrorCode code, DenialKind kind, const char* context) {
  check_denial_shape(outcome, context);
  if (outcome.denials.empty()) {
    return;
  }
  const cpath::DenialDetail& denial = outcome.denials.front();
  if (denial.code != code) {
    fail_at(context, std::string("expected denial code ") + cpath::code_symbol(code) + " but got " +
                        cpath::code_symbol(denial.code) + " (" + denial.message + ")");
  }
  if (denial.kind != kind) {
    fail_at(context, std::string("expected DenialKind ") + cpath::to_string(kind) + " but got " +
                        cpath::to_string(denial.kind));
  }
  if (kind == DenialKind::kProvenInfeasible) {
    CPATH_CHECK(outcome.proven_infeasible());
    CPATH_CHECK(!outcome.indeterminate());
  }
  if (kind == DenialKind::kSearchLimitReached) {
    CPATH_CHECK(outcome.indeterminate());
    CPATH_CHECK(!outcome.proven_infeasible());
  }
}

void require_plan(const PlanningOutcome& outcome, const PlanningRequest& request, const char* context) {
  if (!outcome.ok()) {
    fail_at(context, "expected a mapping but the planner refused: " + cpath_test::describe_outcome(outcome));
    return;
  }
  if (!outcome.denials.empty()) {
    fail_at(context, "a plan must never be returned alongside a denial");
  }
  CPATH_CHECK(!outcome.indeterminate());
  CPATH_CHECK(!outcome.proven_infeasible());
  const cpath::Status status = cpath::validate_plan(*outcome.plan, request);
  if (!status.is_ok()) {
    fail_at(context, std::string("validate_plan rejected the produced plan: ") + status.to_string() + " in " +
                        render_plan(*outcome.plan));
  }
  for (const cpath::PathPlan& path : outcome.plan->paths) {
    check_path_contract(path, request, context);
  }
}

const cpath::PathPlan* path_for_logical(const cpath::Plan& plan, std::uint64_t logical_edge) {
  for (const cpath::PathPlan& path : plan.paths) {
    if (path.logical_edge.value() == logical_edge) {
      return &path;
    }
  }
  return nullptr;
}

std::uint64_t allocation_of(const cpath::Plan& plan, const char* edge_id) {
  for (const cpath::Allocation& allocation : plan.allocations) {
    if (allocation.edge.value() == edge_id) {
      return allocation.planned_mbps;
    }
  }
  return 0;
}

bool siblings_are_edge_disjoint(const cpath::Plan& plan) {
  for (std::size_t i = 0; i < plan.paths.size(); ++i) {
    for (std::size_t j = i + 1u; j < plan.paths.size(); ++j) {
      if (plan.paths[i].logical_edge != plan.paths[j].logical_edge) {
        continue;
      }
      for (const cpath::Hop& left : plan.paths[i].hops) {
        for (const cpath::Hop& right : plan.paths[j].hops) {
          if (left.edge == right.edge) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

std::vector<std::string> interior_nodes(const cpath::PathPlan& path) {
  std::vector<std::string> out;
  for (std::size_t index = 0; index + 1u < path.hops.size(); ++index) {
    out.push_back(path.hops[index].to.value());
  }
  return out;
}

bool siblings_share_interior_node(const cpath::Plan& plan) {
  for (std::size_t i = 0; i < plan.paths.size(); ++i) {
    for (std::size_t j = i + 1u; j < plan.paths.size(); ++j) {
      if (plan.paths[i].logical_edge != plan.paths[j].logical_edge) {
        continue;
      }
      const std::vector<std::string> left = interior_nodes(plan.paths[i]);
      const std::vector<std::string> right = interior_nodes(plan.paths[j]);
      for (const std::string& node : left) {
        if (std::find(right.begin(), right.end(), node) != right.end()) {
          return true;
        }
      }
    }
  }
  return false;
}

// The decision part of a plan: everything Plan::encode() writes except the
// canonical request digest. Two requests over different fabrics necessarily
// have different digests, and that is correct; what must not move is the
// mapping decision.
std::vector<std::uint8_t> decision_bytes(const cpath::Plan& plan) {
  cpath::ByteWriter writer;
  writer.tagged("cpath.test.decision.v1");
  writer.u64(plan.generation.value());
  writer.string(plan.collective.value());
  writer.u8(static_cast<std::uint8_t>(plan.kind));
  writer.u32(plan.stage_count);
  writer.u32(static_cast<std::uint32_t>(plan.stages.size()));
  for (const cpath::Stage& stage : plan.stages) {
    writer.u32(stage.index);
    writer.u32(static_cast<std::uint32_t>(stage.logical_edges.size()));
    for (const cpath::LogicalEdgeId& id : stage.logical_edges) {
      writer.u64(id.value());
    }
    writer.u32(static_cast<std::uint32_t>(stage.paths.size()));
    for (const cpath::PathId& id : stage.paths) {
      writer.u64(id.value());
    }
  }
  writer.u32(static_cast<std::uint32_t>(plan.paths.size()));
  for (const cpath::PathPlan& path : plan.paths) {
    writer.u64(path.id.value());
    writer.u64(path.logical_edge.value());
    writer.string(path.src.value());
    writer.string(path.dst.value());
    writer.u32(path.stage);
    writer.u32(static_cast<std::uint32_t>(path.hops.size()));
    for (const cpath::Hop& hop : path.hops) {
      writer.string(hop.edge.value());
      writer.string(hop.from.value());
      writer.string(hop.to.value());
    }
    writer.u64(path.cost);
    writer.u64(path.bottleneck_mbps);
    writer.u32(static_cast<std::uint32_t>(path.domain_signature.size()));
    for (const cpath::FailureDomainId& domain : path.domain_signature) {
      writer.string(domain.value());
    }
    writer.u8(static_cast<std::uint8_t>(path.weakest_evidence));
  }
  writer.u32(static_cast<std::uint32_t>(plan.allocations.size()));
  for (const cpath::Allocation& allocation : plan.allocations) {
    writer.string(allocation.edge.value());
    writer.u64(allocation.planned_mbps);
  }
  writer.u64(plan.stats.total_cost);
  writer.u64(plan.stats.max_path_cost);
  writer.u64(static_cast<std::uint64_t>(plan.stats.path_count));
  writer.u64(static_cast<std::uint64_t>(plan.stats.hop_count));
  writer.u64(static_cast<std::uint64_t>(plan.stats.logical_edge_count));
  writer.u64(plan.stats.bottleneck_mbps);
  writer.u8(static_cast<std::uint8_t>(plan.weakest_evidence));
  return writer.take();
}

// The cost model of policy.hpp, re-derived: one hop costs
// (latency * latency_weight + congestion_penalty * congestion_weight) / 1000
// with saturating arithmetic and a saturating sum over the hops.
std::uint64_t model_edge_cost(const cpath::Edge& edge, const Policy& policy) {
  const std::uint64_t latency_term =
      cpath::saturating_mul(edge.latency_micros, policy.latency_weight_milli, cpath::kCostCeiling) / 1000u;
  if (policy.congestion_weight_milli == 0 || edge.capacity_mbps == 0) {
    return latency_term;
  }
  if (edge.reserved_mbps >= edge.capacity_mbps) {
    return cpath::kCostCeiling;
  }
  const std::uint64_t utilisation_milli =
      cpath::saturating_mul(edge.reserved_mbps, 1000u, cpath::kCostCeiling) / edge.capacity_mbps;
  const std::uint64_t spare_milli = 1000u - utilisation_milli;
  const std::uint64_t penalty =
      cpath::saturating_mul(policy.congestion_weight_milli, utilisation_milli, cpath::kCostCeiling) /
      (spare_milli == 0u ? 1u : spare_milli);
  return cpath::saturating_add(latency_term, penalty, cpath::kCostCeiling);
}

std::uint64_t model_path_cost(const cpath::PathPlan& path, const cpath::FabricGraph& fabric, const Policy& policy) {
  std::uint64_t total = 0;
  for (const cpath::Hop& hop : path.hops) {
    const cpath::Edge* edge = fabric.find_edge(hop.edge);
    CPATH_REQUIRE(edge != nullptr);
    total = cpath::saturating_add(total, model_edge_cost(*edge, policy), cpath::kCostCeiling);
  }
  return total;
}

// ===========================================================================
// A. cheapest-first trap.
//
// Logical edge p0->p1 has a cheap route that crosses the single scarce edge
// "bottleneck" (spare 150 for a per-path demand of 100) and an expensive route
// that avoids it. Logical edge p2->p3 has exactly one route, and that route
// needs the same scarce edge. Committing p0->p1's cheapest route starves
// p2->p3, so a planner that greedily locks in the cheapest candidate per
// logical edge must fail while a complete mapping exists.
// ===========================================================================
CPATH_TEST(algorithm, a_cheapest_first_trap) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  for (const char* node : {"n0", "n1", "n2", "n3", "m", "h", "q", "r"}) {
    fabric.node(node);
  }
  fabric.edge("a1", "n0", "m", 1000, 10, "R0");
  fabric.edge("bottleneck", "m", "h", 150, 10, "R0");
  fabric.edge("a3", "h", "n1", 1000, 10, "R0");
  fabric.edge("c1", "n0", "q", 1000, 50, "R0");
  fabric.edge("c2", "q", "r", 1000, 50, "R0");
  fabric.edge("c3", "r", "n1", 1000, 50, "R0");
  fabric.edge("d1", "n2", "m", 1000, 10, "R0");
  fabric.edge("d3", "h", "n3", 1000, 10, "R0");

  cpath::CollectiveSpec collective;
  collective.id = cpath::CollectiveId{"trap0"};
  collective.kind = cpath::CollectiveKind::kTree;
  collective.demand_mbps = 100;  // per-path share = 100 with one path per logical edge
  collective.groups.push_back(tree_group("g0", "p0", {"p0", "p1"}));
  collective.groups.push_back(tree_group("g1", "p2", {"p2", "p3"}));

  const PlanningRequest request = cpath_test::build_or_fail(
      make_spec(collective, fabric.spec, base_policy(6, 1),
                {bind("p0", "n0"), bind("p1", "n1"), bind("p2", "n2"), bind("p3", "n3")}),
      "family A");

  const PlanningOutcome outcome = cpath::plan_collective(request);
  require_plan(outcome, request, "family A: the complete mapping exists");
  report("A", outcome, request, "cheapest-first-trap");
  if (!outcome.ok()) {
    return;
  }
  const cpath::Plan& plan = *outcome.plan;
  CPATH_REQUIRE_EQ(plan.paths.size(), static_cast<std::size_t>(2));
  // The cheapest global assignment is the expensive p0->p1 route (150) plus the
  // single p2->p3 route (30): 180, with the scarce edge committed exactly once.
  CPATH_CHECK_EQ(plan.stats.total_cost, static_cast<std::uint64_t>(180));
  CPATH_CHECK(outcome.optimal);
  const cpath::PathPlan* first = path_for_logical(plan, 1);
  const cpath::PathPlan* second = path_for_logical(plan, 2);
  CPATH_REQUIRE(first != nullptr);
  CPATH_REQUIRE(second != nullptr);
  if (path_uses_edge(*first, "bottleneck")) {
    fail_at("family A", "the planner committed the greedy cheapest route and starved the sibling logical edge");
  }
  CPATH_CHECK_EQ(first->cost, static_cast<std::uint64_t>(150));
  CPATH_CHECK(path_uses_edge(*second, "bottleneck"));
  CPATH_CHECK_EQ(allocation_of(plan, "bottleneck"), static_cast<std::uint64_t>(100));
  CPATH_CHECK_EQ(allocation_of(plan, "a1"), static_cast<std::uint64_t>(0));
}

// ===========================================================================
// B. locally expensive / globally necessary.
//
// Two edge-disjoint siblings are required, and only two routes exist: the
// cheapest route for the logical edge (cost 20) and a much worse one (cost
// 1000). Every complete solution therefore contains a path that is not the
// cheapest for its logical edge.
// ===========================================================================
CPATH_TEST(algorithm, b_locally_expensive_globally_necessary) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  for (const char* node : {"n0", "x", "y", "n1"}) {
    fabric.node(node);
  }
  fabric.edge("e1", "n0", "x", 10000, 10, "R0");
  fabric.edge("e2", "x", "n1", 10000, 10, "R0");
  fabric.edge("e3", "n0", "y", 10000, 500, "R0");
  fabric.edge("e4", "y", "n1", 10000, 500, "R0");

  Policy policy = base_policy(4, 2);
  policy.disjointness = Disjointness::kEdge;
  const PlanningRequest request =
      cpath_test::build_or_fail(make_spec(one_edge("b0", "p0", "p1"), fabric.spec, policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family B");

  const PlanningOutcome outcome = cpath::plan_collective(request);
  require_plan(outcome, request, "family B");
  report("B", outcome, request, "locally-expensive-globally-necessary");
  if (!outcome.ok()) {
    return;
  }
  const cpath::Plan& plan = *outcome.plan;
  CPATH_REQUIRE_EQ(plan.paths.size(), static_cast<std::size_t>(2));
  std::vector<std::uint64_t> costs;
  for (const cpath::PathPlan& path : plan.paths) {
    costs.push_back(path.cost);
  }
  std::sort(costs.begin(), costs.end());
  CPATH_REQUIRE_EQ(costs.size(), static_cast<std::size_t>(2));
  CPATH_CHECK_EQ(costs[0], static_cast<std::uint64_t>(20));
  CPATH_CHECK_EQ(costs[1], static_cast<std::uint64_t>(1000));
  CPATH_CHECK_EQ(plan.stats.total_cost, static_cast<std::uint64_t>(1020));
  CPATH_CHECK_EQ(plan.stats.max_path_cost, static_cast<std::uint64_t>(1000));
  CPATH_CHECK(siblings_are_edge_disjoint(plan));
  CPATH_CHECK(outcome.optimal);
}

// ===========================================================================
// C. candidate-pool truncation.
//
// Twelve near-identical cheap routes all sit in rack R0, and exactly one
// expensive route sits in rack R1. With required domain diversity the only
// admissible pair mixes the two racks. When the candidate pool and the
// enumeration are starved, the planner may miss that pair - and then it must
// say "search limit", never "proven infeasible".
// ===========================================================================
CPATH_TEST(algorithm, c_candidate_pool_truncation) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite)
      .domain("R0", DomainKind::kRack, "S0")
      .domain("R1", DomainKind::kRack, "S0");
  fabric.node("n0").node("n1").node("h").node("b").node("c");
  for (int index = 0; index < 12; ++index) {
    const std::string suffix = std::to_string(index);
    const std::string leaf = "a" + suffix;
    fabric.node(leaf.c_str());
    fabric.edge(("ca" + suffix).c_str(), "n0", leaf.c_str(), 10000, 10, "R0");
    fabric.edge(("cb" + suffix).c_str(), leaf.c_str(), "h", 10000, 10, "R0");
  }
  fabric.edge("ch", "h", "n1", 10000, 10, "R0");
  fabric.edge("x1", "n0", "b", 10000, 900, "R1");
  fabric.edge("x2", "b", "c", 10000, 900, "R1");
  fabric.edge("x3", "c", "n1", 10000, 900, "R1");

  Policy policy = base_policy(4, 2);
  policy.domain_diversity = DomainDiversity::kRequired;
  policy.domain_diversity_level = DomainKind::kRack;
  const cpath::RequestSpec spec = make_spec(one_edge("c0", "p0", "p1"), fabric.spec, policy,
                                           {bind("p0", "n0"), bind("p1", "n1")});
  const PlanningRequest request = cpath_test::build_or_fail(spec, "family C");

  const PlanningOutcome full = cpath::plan_collective(request);
  require_plan(full, request, "family C: exhaustive enumeration finds the mixed pair");
  report("C", full, request, "default-limits");
  if (full.ok()) {
    CPATH_REQUIRE_EQ(full.plan->paths.size(), static_cast<std::size_t>(2));
    CPATH_CHECK(full.plan->paths[0].domain_signature != full.plan->paths[1].domain_signature);
    CPATH_CHECK(full.optimal);
  }

  // A starved but not empty pool: the greedy diversity chain still reaches the
  // R1 route, so the planner may plan - and if it cannot, it may not claim proof.
  PlannerLimits starved;
  starved.max_candidate_paths = 2;
  starved.max_exhaustive_paths = 2;
  starved.max_enumeration_steps = 64;
  starved.max_combination_steps = 64;
  starved.max_set_alternatives = 1;
  const PlanningOutcome capped = cpath::plan_collective(request, starved);
  if (capped.ok()) {
    require_plan(capped, request, "family C: starved pool still plans");
  } else {
    check_denial_shape(capped, "family C: starved pool refuses");
    CPATH_CHECK(!capped.proven_infeasible());
    CPATH_CHECK(capped.indeterminate());
  }

  if (capped.ok()) {
    // A search that was cut off must not claim optimality: "valid and
    // deterministic" and "provably optimal" are different results.
    CPATH_CHECK(!capped.optimal);
  }
  report("C", capped, request, "starved-pool");

  // A pool of one can never yield a diverse pair. The enumeration is not
  // exhaustive, so the only honest answer is INDETERMINATE.
  PlannerLimits single;
  single.max_candidate_paths = 1;
  single.max_exhaustive_paths = 2;
  single.max_enumeration_steps = 64;
  const PlanningOutcome truncated = cpath::plan_collective(request, single);
  require_denial(truncated, ErrorCode::kSearchBudgetExceeded, DenialKind::kSearchLimitReached,
                 "family C: a one-candidate pool cannot prove infeasibility");
  report("C", truncated, request, "one-candidate-pool");
  if (!truncated.denials.empty()) {
    CPATH_CHECK(truncated.denials.front().conflict == cpath::ConflictKind::kSearchBudget);
  }
}

// ===========================================================================
// D. cyclic graphs.
//
// Two-cycles, self-referential back edges and a dense strongly connected
// component, plus a fabric in which the cheapest WALK to the destination
// leaves it and comes back. Every emitted path must be simple: no physical
// node and no physical edge twice, re-derived from the hops.
// ===========================================================================
CPATH_TEST(algorithm, d_cyclic_graphs_never_emit_walks) {
  {
    Fabric fabric;  // SYNTHETIC: a two-cycle in both directions.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1").node("x");
    fabric.edge("t1", "n0", "n1", 10000, 10, "R0");
    fabric.edge("t2", "n1", "n0", 10000, 10, "R0");
    fabric.edge("t3", "n1", "x", 10000, 10, "R0");
    fabric.edge("t4", "x", "n1", 10000, 10, "R0");
    fabric.edge("t5", "n0", "x", 10000, 10, "R0");
    fabric.edge("t6", "x", "n0", 10000, 10, "R0");
    const PlanningRequest request =
        cpath_test::build_or_fail(make_spec(one_edge("d0", "p0", "p1"), fabric.spec, base_policy(6, 1),
                                            {bind("p0", "n0"), bind("p1", "n1")}),
                                  "family D two-cycle");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family D: two-cycle fabric");
    report("D", outcome, request, "two-cycle");
    if (outcome.ok()) {
      CPATH_REQUIRE_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(1));
      const cpath::PathPlan& path = outcome.plan->paths.front();
      CPATH_CHECK_EQ(path.hops.size(), static_cast<std::size_t>(1));
      CPATH_CHECK_EQ(path.cost, static_cast<std::uint64_t>(10));
      if (!path.hops.empty()) {
        CPATH_CHECK_EQ(path.hops.front().edge.value(), std::string("t1"));
      }
    }
  }

  {
    Fabric fabric;  // SYNTHETIC: a dense strongly connected component.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    std::vector<std::string> ids;
    for (int index = 0; index < 6; ++index) {
      ids.push_back("f" + std::to_string(index));
    }
    for (const std::string& id : ids) {
      fabric.node(id.c_str());
    }
    for (const std::string& from : ids) {
      for (const std::string& to : ids) {
        if (from == to) {
          continue;
        }
        fabric.edge(("q_" + from + "_" + to).c_str(), from.c_str(), to.c_str(), 10000, 10, "R0");
      }
    }
    Policy policy = base_policy(5, 3);
    policy.disjointness = Disjointness::kEdge;
    const PlanningRequest request =
        cpath_test::build_or_fail(make_spec(one_edge("d1", "p0", "p1"), fabric.spec, policy,
                                            {bind("p0", "f0"), bind("p1", "f5")}),
                                  "family D dense");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family D: dense strongly connected component");
    report("D", outcome, request, "dense-strongly-connected");
    if (outcome.ok()) {
      CPATH_CHECK_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(3));
      CPATH_CHECK(siblings_are_edge_disjoint(*outcome.plan));
    }
  }

  {
    // The cheapest walk to n1 is n0 -> n1 -> x -> n1 (cost 3, exactly the hop
    // limit). A walk-based search would happily emit it. The cheapest SIMPLE
    // path is its own prefix, n0 -> n1 (cost 1), and that is what must come out.
    Fabric fabric;  // SYNTHETIC: a round trip through the destination.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1").node("x");
    fabric.edge("p0e", "n0", "n1", 10000, 1, "R0");
    fabric.edge("p1e", "n1", "x", 10000, 1, "R0");
    fabric.edge("p2e", "x", "n1", 10000, 1, "R0");
    const PlanningRequest request =
        cpath_test::build_or_fail(make_spec(one_edge("d2", "p0", "p1"), fabric.spec, base_policy(3, 1),
                                            {bind("p0", "n0"), bind("p1", "n1")}),
                                  "family D round trip");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family D: round trip through the destination");
    report("D", outcome, request, "round-trip-through-destination");
    if (outcome.ok()) {
      CPATH_REQUIRE_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(1));
      const cpath::PathPlan& path = outcome.plan->paths.front();
      CPATH_CHECK_EQ(path.hops.size(), static_cast<std::size_t>(1));
      CPATH_CHECK_EQ(path.cost, static_cast<std::uint64_t>(1));
      const std::vector<std::string> edges = hop_edge_ids(path);
      CPATH_CHECK(edges.size() == 1u && edges.front() == "p0e");
      if (path.hops.size() > 1u) {
        fail_at("family D", "the planner emitted a walk that re-enters the destination: " + render_path(path));
      }
    }
  }
}

// ===========================================================================
// E. symmetric graphs with many equal-cost alternatives.
//
// Four equally cheap routes, two of which are edge-disjoint. The plan must be
// byte-identical across repeated runs and across two independently constructed
// but canonically identical requests.
// ===========================================================================
CPATH_TEST(algorithm, e_symmetric_ties_byte_identical) {
  Fabric fabric;  // SYNTHETIC: fully symmetric two-spine fabric.
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  fabric.node("n0").node("n1").node("h0").node("h1");
  for (int index = 0; index < 4; ++index) {
    const std::string leaf = "a" + std::to_string(index);
    const std::string spine = (index % 2 == 0) ? "h0" : "h1";
    fabric.node(leaf.c_str());
    fabric.edge(("s" + std::to_string(index)).c_str(), "n0", leaf.c_str(), 10000, 20, "R0");
    fabric.edge(("r" + std::to_string(index)).c_str(), leaf.c_str(), spine.c_str(), 10000, 20, "R0");
  }
  fabric.edge("hn0", "h0", "n1", 10000, 20, "R0");
  fabric.edge("hn1", "h1", "n1", 10000, 20, "R0");

  Policy policy = base_policy(5, 2);
  policy.disjointness = Disjointness::kEdge;
  const auto make = [&policy]() {
    Fabric copy;  // SYNTHETIC, rebuilt from scratch for the independence check.
    copy.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    copy.node("n0").node("n1").node("h0").node("h1");
    for (int index = 0; index < 4; ++index) {
      const std::string leaf = "a" + std::to_string(index);
      const std::string spine = (index % 2 == 0) ? "h0" : "h1";
      copy.node(leaf.c_str());
      copy.edge(("s" + std::to_string(index)).c_str(), "n0", leaf.c_str(), 10000, 20, "R0");
      copy.edge(("r" + std::to_string(index)).c_str(), leaf.c_str(), spine.c_str(), 10000, 20, "R0");
    }
    copy.edge("hn0", "h0", "n1", 10000, 20, "R0");
    copy.edge("hn1", "h1", "n1", 10000, 20, "R0");
    return cpath_test::build_or_fail(make_spec(one_edge("e0", "p0", "p1"), copy.spec, policy,
                                               {bind("p0", "n0"), bind("p1", "n1")}),
                                     "family E");
  };

  const PlanningRequest request = cpath_test::build_or_fail(
      make_spec(one_edge("e0", "p0", "p1"), fabric.spec, policy, {bind("p0", "n0"), bind("p1", "n1")}),
      "family E");

  const PlanningOutcome first = cpath::plan_collective(request);
  require_plan(first, request, "family E");
  report("E", first, request, "byte-identical-reference");
  if (!first.ok()) {
    return;
  }
  CPATH_CHECK_EQ(first.plan->paths.size(), static_cast<std::size_t>(2));
  CPATH_CHECK(siblings_are_edge_disjoint(*first.plan));
  const std::vector<std::uint8_t> reference = first.plan->encode();
  for (int run = 0; run < 8; ++run) {
    const PlanningOutcome repeat = cpath::plan_collective(request);
    CPATH_REQUIRE(repeat.ok());
    if (repeat.plan->encode() != reference) {
      fail_at("family E", "a repeated run produced different plan bytes: " + render_plan(*repeat.plan));
      break;
    }
  }

  const PlanningRequest rebuilt = make();
  if (!(rebuilt.canonical_digest == request.canonical_digest)) {
    fail_at("family E", "two identically declared requests produced different canonical digests");
  }
  const PlanningOutcome second = cpath::plan_collective(rebuilt);
  require_plan(second, rebuilt, "family E: independently built request");
  report("E", second, rebuilt, "independently-built-request");
  if (second.ok() && second.plan->encode() != reference) {
    fail_at("family E", "an independently built but canonically identical request produced different plan bytes");
  }
}

// ===========================================================================
// F. parallel links.
//
// Four distinct physical edges between the same pair of nodes. Distinct
// identities must be usable as edge-disjoint siblings.
// ===========================================================================
CPATH_TEST(algorithm, f_parallel_links) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  fabric.node("n0").node("n1");
  std::size_t parallel = 0;
  for (int index = 0; index < 4; ++index) {
    fabric.edge(("pl" + std::to_string(index)).c_str(), "n0", "n1", 10000, 100, "R0");
    ++parallel;
  }
  CPATH_REQUIRE_EQ(parallel, static_cast<std::size_t>(4));

  Policy policy = base_policy(3, 3);
  policy.disjointness = Disjointness::kEdge;
  const PlanningRequest request =
      cpath_test::build_or_fail(make_spec(one_edge("f0", "p0", "p1"), fabric.spec, policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family F");
  const PlanningOutcome outcome = cpath::plan_collective(request);
  require_plan(outcome, request, "family F");
  report("F", outcome, request, "parallel-links");
  if (!outcome.ok()) {
    return;
  }
  const cpath::Plan& plan = *outcome.plan;
  CPATH_REQUIRE_EQ(plan.paths.size(), static_cast<std::size_t>(3));
  std::vector<std::string> used;
  for (const cpath::PathPlan& path : plan.paths) {
    CPATH_CHECK_EQ(path.hops.size(), static_cast<std::size_t>(1));
    for (const cpath::Hop& hop : path.hops) {
      used.push_back(hop.edge.value());
    }
  }
  CPATH_CHECK_EQ(used.size(), static_cast<std::size_t>(3));
  if (has_duplicate(used)) {
    fail_at("family F", "parallel physical edges were not distinguished: the plan repeats an edge identity");
  }
  CPATH_CHECK(siblings_are_edge_disjoint(plan));
  CPATH_CHECK(outcome.optimal);
}

// ===========================================================================
// G. zero-capacity links.
//
// A zero-capacity edge is not "free capacity": by default it is unusable and
// the denial must name the capacity gap. With allow_unverified_capacity the
// edge may be used, and then the plan must report the gap it relied on.
// ===========================================================================
CPATH_TEST(algorithm, g_zero_capacity_links) {
  {
    Fabric fabric;  // SYNTHETIC: one edge with no verified capacity at all.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1");
    fabric.edge("zero", "n0", "n1", 0, 1, "R0", 0, EvidenceClass::kUnknown);
    const cpath::RequestSpec spec = make_spec(one_edge("g0", "p0", "p1"), fabric.spec, base_policy(3, 1),
                                             {bind("p0", "n0"), bind("p1", "n1")});
    const PlanningRequest request = cpath_test::build_or_fail(spec, "family G default");

    const PlanningOutcome denied = cpath::plan_collective(request);
    require_denial(denied, ErrorCode::kEdgeHasNoVerifiedCapacity, DenialKind::kProvenInfeasible,
                   "family G: a zero-capacity edge is unusable by default");
    report("G", denied, request, "zero-capacity-default");
    if (!denied.denials.empty()) {
      CPATH_CHECK(denied.denials.front().conflict == cpath::ConflictKind::kNoVerifiedCapacity);
      CPATH_CHECK(denied.denials.front().message.find("capacity") != std::string::npos);
    }

    cpath::RequestSpec allowed_spec = spec;
    allowed_spec.policy.allow_unverified_capacity = true;
    const PlanningRequest allowed = cpath_test::build_or_fail(allowed_spec, "family G unverified");
    const PlanningOutcome outcome = cpath::plan_collective(allowed);
    require_plan(outcome, allowed, "family G: unverified capacity is usable when the policy allows it");
    report("G", outcome, allowed, "zero-capacity-allowed");
    if (outcome.ok()) {
      CPATH_REQUIRE_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(1));
      const cpath::PathPlan& path = outcome.plan->paths.front();
      CPATH_CHECK(path_uses_edge(path, "zero"));
      CPATH_CHECK_EQ(path.bottleneck_mbps, static_cast<std::uint64_t>(0));
      CPATH_CHECK(path.weakest_evidence == EvidenceClass::kUnknown);
      CPATH_CHECK_EQ(outcome.plan->stats.bottleneck_mbps, static_cast<std::uint64_t>(0));
      CPATH_CHECK(outcome.plan->weakest_evidence == EvidenceClass::kUnknown);
    }
  }

  {
    Fabric fabric;  // SYNTHETIC: a cheap unverified edge beside a verified one.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1");
    fabric.edge("cheap_zero", "n0", "n1", 0, 1, "R0", 0, EvidenceClass::kUnknown);
    fabric.edge("verified", "n0", "n1", 1000, 900, "R0", 0, EvidenceClass::kSynthetic);
    const cpath::RequestSpec spec = make_spec(one_edge("g1", "p0", "p1"), fabric.spec, base_policy(3, 1),
                                             {bind("p0", "n0"), bind("p1", "n1")});

    const PlanningOutcome strict = cpath::plan_collective(cpath_test::build_or_fail(spec, "family G mixed"));
    const PlanningRequest strict_request = cpath_test::build_or_fail(spec, "family G mixed");
    require_plan(strict, strict_request, "family G: only the verified edge is usable by default");
    report("G", strict, strict_request, "mixed-default");
    if (strict.ok()) {
      CPATH_CHECK(!path_uses_edge(strict.plan->paths.front(), "cheap_zero"));
      CPATH_CHECK(path_uses_edge(strict.plan->paths.front(), "verified"));
      CPATH_CHECK(strict.plan->weakest_evidence == EvidenceClass::kSynthetic);
      CPATH_CHECK_EQ(strict.plan->stats.bottleneck_mbps, static_cast<std::uint64_t>(1000));
    }

    cpath::RequestSpec allowed_spec = spec;
    allowed_spec.policy.allow_unverified_capacity = true;
    const PlanningRequest allowed = cpath_test::build_or_fail(allowed_spec, "family G mixed unverified");
    const PlanningOutcome outcome = cpath::plan_collective(allowed);
    require_plan(outcome, allowed, "family G: the unverified edge is chosen once the gap is allowed");
    report("G", outcome, allowed, "mixed-allowed");
    if (outcome.ok()) {
      const cpath::PathPlan& path = outcome.plan->paths.front();
      CPATH_CHECK(path_uses_edge(path, "cheap_zero"));
      CPATH_CHECK_EQ(path.bottleneck_mbps, static_cast<std::uint64_t>(0));
      CPATH_CHECK(path.weakest_evidence == EvidenceClass::kUnknown);
      CPATH_CHECK(outcome.plan->weakest_evidence == EvidenceClass::kUnknown);
    }
  }
}

// ===========================================================================
// H. bottleneck cuts.
//
// One edge every route must cross. With enough spare it must appear as the
// plan's bottleneck with exactly its spare capacity; with too little spare the
// refusal must be proven and must name the insufficiency.
// ===========================================================================
CPATH_TEST(algorithm, h_bottleneck_cuts) {
  {
    Fabric fabric;  // SYNTHETIC: every route crosses "narrow".
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("b").node("n1");
    fabric.edge("h1", "n0", "b", 1000, 10, "R0");
    fabric.edge("narrow", "b", "n1", 100, 10, "R0");
    const PlanningRequest request = cpath_test::build_or_fail(
        make_spec(one_edge("h0", "p0", "p1", 100), fabric.spec, base_policy(4, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family H bottleneck");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family H: a plan exists and the cut is reported");
    report("H", outcome, request, "cut-with-spare");
    if (outcome.ok()) {
      CPATH_CHECK_EQ(outcome.plan->stats.bottleneck_mbps, static_cast<std::uint64_t>(100));
      CPATH_CHECK_EQ(allocation_of(*outcome.plan, "narrow"), static_cast<std::uint64_t>(100));
    }
  }

  {
    // Two logical edges share the cut, and together they need more than the cut
    // can carry. That is a request-level capacity insufficiency, proven.
    Fabric fabric;  // SYNTHETIC
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1").node("n2").node("n3").node("m").node("h");
    fabric.edge("c1", "n0", "m", 1000, 10, "R0");
    fabric.edge("c2", "n2", "m", 1000, 10, "R0");
    fabric.edge("cut", "m", "h", 150, 10, "R0");
    fabric.edge("c3", "h", "n1", 1000, 10, "R0");
    fabric.edge("c4", "h", "n3", 1000, 10, "R0");
    cpath::CollectiveSpec collective;
    collective.id = cpath::CollectiveId{"cut0"};
    collective.kind = cpath::CollectiveKind::kTree;
    collective.demand_mbps = 100;
    collective.groups.push_back(tree_group("g0", "p0", {"p0", "p1"}));
    collective.groups.push_back(tree_group("g1", "p2", {"p2", "p3"}));
    const PlanningRequest request = cpath_test::build_or_fail(
        make_spec(collective, fabric.spec, base_policy(5, 1),
                  {bind("p0", "n0"), bind("p1", "n1"), bind("p2", "n2"), bind("p3", "n3")}),
        "family H cut");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_denial(outcome, ErrorCode::kInsufficientCapacity, DenialKind::kProvenInfeasible,
                   "family H: the cut cannot carry both logical edges");
    report("H", outcome, request, "shared-cut-overcommitted");
    if (!outcome.denials.empty()) {
      CPATH_CHECK(outcome.denials.front().conflict == cpath::ConflictKind::kInsufficientCapacity);
      CPATH_CHECK(!outcome.denials.front().logical_edge.valid());
      CPATH_CHECK(outcome.denials.front().message.find("capacity") != std::string::npos);
    }
  }

  {
    // The cut cannot carry one path's share of the demand, and it is the edge
    // leaving the bound endpoint: the refusal must name the missing verified
    // capacity rather than report a generic disconnection.
    Fabric fabric;  // SYNTHETIC
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("n1");
    fabric.edge("narrow", "n0", "n1", 40, 10, "R0");
    const PlanningRequest request = cpath_test::build_or_fail(
        make_spec(one_edge("h2", "p0", "p1", 100), fabric.spec, base_policy(4, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family H starved cut");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_denial(outcome, ErrorCode::kEdgeHasNoVerifiedCapacity, DenialKind::kProvenInfeasible,
                   "family H: the cut is below the per-path demand");
    report("H", outcome, request, "cut-below-per-path-demand");
    if (!outcome.denials.empty()) {
      CPATH_CHECK(outcome.denials.front().conflict == cpath::ConflictKind::kNoVerifiedCapacity);
      CPATH_CHECK(outcome.denials.front().message.find("capacity") != std::string::npos);
    }
  }

  {
    // The same starvation one hop deeper. Every edge leaving the source is
    // fine, so the planner cannot attribute the failure to a filter it has not
    // isolated; it reports the reachability it actually proved and names both
    // endpoints. The claim is still a PROOF - no compliant route exists - and
    // the report never pretends to know which deeper hop was responsible.
    Fabric fabric;  // SYNTHETIC
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    fabric.node("n0").node("b").node("n1");
    fabric.edge("h1", "n0", "b", 1000, 10, "R0");
    fabric.edge("narrow", "b", "n1", 40, 10, "R0");
    const PlanningRequest request = cpath_test::build_or_fail(
        make_spec(one_edge("h3", "p0", "p1", 100), fabric.spec, base_policy(4, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family H deep starved cut");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_denial(outcome, ErrorCode::kNoPath, DenialKind::kProvenInfeasible,
                   "family H: a demand-starved hop deeper in the fabric is a proven unreachability");
    report("H", outcome, request, "deep-starved-hop");
    if (!outcome.denials.empty()) {
      CPATH_CHECK(outcome.denials.front().conflict == cpath::ConflictKind::kDisconnected);
      CPATH_CHECK(outcome.denials.front().message.find("n0") != std::string::npos);
      CPATH_CHECK(outcome.denials.front().message.find("n1") != std::string::npos);
    }
  }
}

// ===========================================================================
// I. node-disjoint siblings that share endpoint attachment domains.
//
// Both endpoints sit in rack R0, so every sibling necessarily traverses R0 at
// its first and last hop. Those attachment domains carry no discriminating
// information and must not make two interior-disjoint paths look dependent.
// ===========================================================================
CPATH_TEST(algorithm, i_node_disjoint_shared_endpoint_domains) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite)
      .domain("R0", DomainKind::kRack, "S0")
      .domain("R1", DomainKind::kRack, "S0")
      .domain("R2", DomainKind::kRack, "S0");
  for (const char* node : {"n0", "n1", "a1", "b1", "a2", "b2"}) {
    fabric.node(node);
  }
  fabric.edge("i1", "n0", "a1", 10000, 10, "R0");
  fabric.edge("i2", "a1", "b1", 10000, 10, "R1");
  fabric.edge("i3", "b1", "n1", 10000, 10, "R0");
  fabric.edge("i4", "n0", "a2", 10000, 10, "R0");
  fabric.edge("i5", "a2", "b2", 10000, 10, "R2");
  fabric.edge("i6", "b2", "n1", 10000, 10, "R0");

  Policy policy = base_policy(5, 2);
  policy.disjointness = Disjointness::kNode;
  policy.domain_diversity = DomainDiversity::kRequired;
  policy.domain_diversity_level = DomainKind::kRack;
  const PlanningRequest request =
      cpath_test::build_or_fail(make_spec(one_edge("i0", "p0", "p1"), fabric.spec, policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family I");
  const PlanningOutcome outcome = cpath::plan_collective(request);
  require_plan(outcome, request, "family I: node-disjoint siblings over shared attachment domains");
  report("I", outcome, request, "node-disjoint-shared-attachment-domains");
  if (!outcome.ok()) {
    return;
  }
  const cpath::Plan& plan = *outcome.plan;
  CPATH_REQUIRE_EQ(plan.paths.size(), static_cast<std::size_t>(2));
  CPATH_CHECK(siblings_are_edge_disjoint(plan));
  CPATH_CHECK(!siblings_share_interior_node(plan));
  for (const cpath::PathPlan& path : plan.paths) {
    CPATH_REQUIRE(path.hops.size() >= 2u);
    const cpath::Edge* first = request.fabric.find_edge(path.hops.front().edge);
    const cpath::Edge* last = request.fabric.find_edge(path.hops.back().edge);
    CPATH_REQUIRE(first != nullptr);
    CPATH_REQUIRE(last != nullptr);
    // Both paths attach through rack R0 - a domain they necessarily share.
    CPATH_CHECK(first->failure_domain == cpath::FailureDomainId{"R0"});
    CPATH_CHECK(last->failure_domain == cpath::FailureDomainId{"R0"});
    // And the shared attachment domain is excluded from the discriminating set.
    CPATH_CHECK_EQ(path.domain_signature.size(), static_cast<std::size_t>(1));
  }
  CPATH_CHECK(plan.paths[0].domain_signature != plan.paths[1].domain_signature);
}

// ===========================================================================
// J. edge-disjoint but not node-disjoint.
//
// Two routes through the same interior node m. They are edge-disjoint, so kEdge
// accepts them; they are not node-disjoint, so kNode must refuse - and refuse
// with a proof, not with a shrug.
// ===========================================================================
CPATH_TEST(algorithm, j_edge_disjoint_not_node_disjoint) {
  Fabric fabric;  // SYNTHETIC
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  fabric.node("n0").node("m").node("n1");
  fabric.edge("e1", "n0", "m", 10000, 10, "R0");
  fabric.edge("e2", "m", "n1", 10000, 10, "R0");
  fabric.edge("e3", "n0", "m", 10000, 10, "R0");
  fabric.edge("e4", "m", "n1", 10000, 10, "R0");

  Policy edge_policy = base_policy(4, 2);
  edge_policy.disjointness = Disjointness::kEdge;
  const PlanningRequest edge_request =
      cpath_test::build_or_fail(make_spec(one_edge("j0", "p0", "p1"), fabric.spec, edge_policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family J edge");
  const PlanningOutcome edge_outcome = cpath::plan_collective(edge_request);
  require_plan(edge_outcome, edge_request, "family J: edge-disjoint siblings sharing an interior node");
  report("J", edge_outcome, edge_request, "kEdge-shared-interior-node");
  if (edge_outcome.ok()) {
    CPATH_CHECK_EQ(edge_outcome.plan->paths.size(), static_cast<std::size_t>(2));
    CPATH_CHECK(siblings_are_edge_disjoint(*edge_outcome.plan));
    CPATH_CHECK(siblings_share_interior_node(*edge_outcome.plan));
  }

  Policy node_policy = base_policy(4, 2);
  node_policy.disjointness = Disjointness::kNode;
  const PlanningRequest node_request =
      cpath_test::build_or_fail(make_spec(one_edge("j1", "p0", "p1"), fabric.spec, node_policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family J node");
  const PlanningOutcome node_outcome = cpath::plan_collective(node_request);
  require_denial(node_outcome, ErrorCode::kInsufficientDisjointPaths, DenialKind::kProvenInfeasible,
                 "family J: kNode refuses the same pair with a proof");
  report("J", node_outcome, node_request, "kNode-refused");
  if (!node_outcome.denials.empty()) {
    CPATH_CHECK(node_outcome.denials.front().conflict == cpath::ConflictKind::kDisjointness);
    CPATH_CHECK(node_outcome.denials.front().enumeration_exhaustive);
  }
}

// ===========================================================================
// K. failure-domain traps.
//
// Two routes in racks R1 and R2 look independent by name, but both resolve to
// the same site S0 at the policy's diversity level, so required diversity must
// refuse the pair - and prove it, because the path set is exhaustive. The
// control fabric moves one route to site S1 and then plans.
// ===========================================================================
CPATH_TEST(algorithm, k_failure_domain_traps) {
  const auto build = [](const char* collective_id, bool move_second_route) {
    Fabric fabric;  // SYNTHETIC
    fabric.domain("S0", DomainKind::kSite)
        .domain("R1", DomainKind::kRack, "S0")
        .domain("R2", DomainKind::kRack, "S0")
        .domain("S1", DomainKind::kSite)
        .domain("R3", DomainKind::kRack, "S1");
    for (const char* node : {"n0", "n1", "a1", "c1", "a2", "c2"}) {
      fabric.node(node);
    }
    const char* second = move_second_route ? "R3" : "R2";
    fabric.edge("k1", "n0", "a1", 10000, 10, "R1");
    fabric.edge("k2", "a1", "c1", 10000, 10, "R1");
    fabric.edge("k3", "c1", "n1", 10000, 10, "R1");
    fabric.edge("k4", "n0", "a2", 10000, 10, second);
    fabric.edge("k5", "a2", "c2", 10000, 10, second);
    fabric.edge("k6", "c2", "n1", 10000, 10, second);
    Policy policy = base_policy(5, 2);
    policy.domain_diversity = DomainDiversity::kRequired;
    policy.domain_diversity_level = DomainKind::kSite;
    return cpath_test::build_or_fail(make_spec(one_edge(collective_id, "p0", "p1"), fabric.spec, policy,
                                               {bind("p0", "n0"), bind("p1", "n1")}),
                                     "family K");
  };

  const PlanningRequest trap = build("k0", false);
  // Independent of the planner: R1 and R2 really do share the ancestor S0.
  CPATH_CHECK(trap.fabric.domain_contains(cpath::FailureDomainId{"S0"}, cpath::FailureDomainId{"R1"}));
  CPATH_CHECK(trap.fabric.domain_contains(cpath::FailureDomainId{"S0"}, cpath::FailureDomainId{"R2"}));
  const PlanningOutcome refused = cpath::plan_collective(trap);
  require_denial(refused, ErrorCode::kDomainDiversityUnsatisfiable, DenialKind::kProvenInfeasible,
                 "family K: a shared ancestor domain defeats required diversity");
  report("K", refused, trap, "shared-ancestor-domain");
  if (!refused.denials.empty()) {
    CPATH_CHECK(refused.denials.front().conflict == cpath::ConflictKind::kDomainDiversity);
    CPATH_CHECK(refused.denials.front().enumeration_exhaustive);
    CPATH_CHECK(refused.denials.front().message.find("failure-domain") != std::string::npos);
  }

  const PlanningRequest control = build("k1", true);
  const PlanningOutcome planned = cpath::plan_collective(control);
  require_plan(planned, control, "family K: genuinely different sites are still independent");
  report("K", planned, control, "distinct-sites");
  if (planned.ok()) {
    CPATH_CHECK_EQ(planned.plan->paths.size(), static_cast<std::size_t>(2));
  }
}

// ===========================================================================
// L. high branching factor.
//
// 200 parallel candidate routes between the same pair of endpoints. A plan must
// exist, and with four edge-disjoint siblings required the planner must find
// four distinct routes rather than collapsing onto the cheapest one.
// ===========================================================================
CPATH_TEST(algorithm, l_high_branching_factor) {
  const int kRoutes = 200;
  Fabric fabric;  // SYNTHETIC: 200 parallel two-hop routes.
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  fabric.node("n0").node("n1");
  for (int index = 0; index < kRoutes; ++index) {
    const std::string suffix = std::to_string(index);
    const std::string leaf = "a" + suffix;
    fabric.node(leaf.c_str());
    fabric.edge(("la" + suffix).c_str(), "n0", leaf.c_str(), 10000, 10, "R0");
    fabric.edge(("lb" + suffix).c_str(), leaf.c_str(), "n1", 10000, 10, "R0");
  }

  const PlanningRequest single =
      cpath_test::build_or_fail(make_spec(one_edge("l0", "p0", "p1"), fabric.spec, base_policy(4, 1),
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family L single");
  const PlanningOutcome simple = cpath::plan_collective(single);
  require_plan(simple, single, "family L: one path out of 200 candidates");
  report("L", simple, single, "200-routes-one-path");
  if (simple.ok()) {
    CPATH_CHECK_EQ(simple.plan->paths.size(), static_cast<std::size_t>(1));
    CPATH_CHECK_EQ(simple.plan->paths.front().hops.size(), static_cast<std::size_t>(2));
  }

  Policy policy = base_policy(4, 4);
  policy.disjointness = Disjointness::kEdge;
  const PlanningRequest spread =
      cpath_test::build_or_fail(make_spec(one_edge("l1", "p0", "p1"), fabric.spec, policy,
                                          {bind("p0", "n0"), bind("p1", "n1")}),
                                "family L disjoint");
  const PlanningOutcome outcome = cpath::plan_collective(spread);
  require_plan(outcome, spread, "family L: four edge-disjoint siblings out of 200 candidates");
  report("L", outcome, spread, "200-routes-four-edge-disjoint");
  if (outcome.ok()) {
    CPATH_CHECK_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(4));
    CPATH_CHECK(siblings_are_edge_disjoint(*outcome.plan));
    std::vector<std::string> interiors;
    for (const cpath::PathPlan& path : outcome.plan->paths) {
      const std::vector<std::string> nodes = interior_nodes(path);
      CPATH_REQUIRE_EQ(nodes.size(), static_cast<std::size_t>(1));
      interiors.push_back(nodes.front());
    }
    if (has_duplicate(interiors)) {
      fail_at("family L", "four siblings were required but the plan reuses an interior node");
    }
  }
}

// ===========================================================================
// M. long narrow graphs near the hop bound.
//
// A chain exactly max_hops long must plan; one hop longer must be refused with
// the hop-limit code, proven, and never as a search-limit result.
// ===========================================================================
CPATH_TEST(algorithm, m_long_narrow_graphs_hop_bound) {
  const std::size_t kBound = 12;
  const auto chain = [](std::size_t hops) {
    Fabric fabric;  // SYNTHETIC: a single chain of "hops" physical edges.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (std::size_t index = 0; index <= hops; ++index) {
      fabric.node(("c" + std::to_string(index)).c_str());
    }
    for (std::size_t index = 0; index < hops; ++index) {
      fabric.edge(("m" + std::to_string(index)).c_str(), ("c" + std::to_string(index)).c_str(),
                  ("c" + std::to_string(index + 1u)).c_str(), 10000, 10, "R0");
    }
    const std::string last = "c" + std::to_string(hops);
    return make_spec(one_edge("m0", "p0", "p1"), fabric.spec, base_policy(kBound, 1),
                     {bind("p0", "c0"), bind("p1", last.c_str())});
  };

  const PlanningRequest exact = cpath_test::build_or_fail(chain(kBound), "family M exact");
  const PlanningOutcome planned = cpath::plan_collective(exact);
  require_plan(planned, exact, "family M: a chain exactly at the hop bound plans");
  report("M", planned, exact, "chain-at-hop-bound");
  if (planned.ok()) {
    CPATH_REQUIRE_EQ(planned.plan->paths.size(), static_cast<std::size_t>(1));
    CPATH_CHECK_EQ(planned.plan->paths.front().hops.size(), kBound);
    CPATH_CHECK_EQ(planned.plan->paths.front().cost, static_cast<std::uint64_t>(kBound * 10u));
  }

  const PlanningRequest too_long = cpath_test::build_or_fail(chain(kBound + 1u), "family M over bound");
  const PlanningOutcome refused = cpath::plan_collective(too_long);
  require_denial(refused, ErrorCode::kHopLimitExceeded, DenialKind::kProvenInfeasible,
                 "family M: one hop over the bound is refused with the hop-limit code");
  report("M", refused, too_long, "chain-over-hop-bound");
  if (!refused.denials.empty()) {
    CPATH_CHECK(refused.denials.front().conflict == cpath::ConflictKind::kHopLimit);
    CPATH_CHECK(refused.denials.front().message.find(std::to_string(kBound + 1u)) != std::string::npos);
    CPATH_CHECK(refused.denials.front().message.find(std::to_string(kBound)) != std::string::npos);
  }
}

// ===========================================================================
// N. disconnected fabrics.
//
// A destination in another component is a proven disconnection; an irrelevant
// island elsewhere in the fabric must not disturb a feasible request.
// ===========================================================================
CPATH_TEST(algorithm, n_disconnected_fabrics) {
  {
    Fabric fabric;  // SYNTHETIC: two components, no edge leaves n0.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const char* node : {"n0", "n1", "y", "z"}) {
      fabric.node(node);
    }
    fabric.edge("island1", "y", "z", 10000, 10, "R0");
    fabric.edge("island2", "z", "y", 10000, 10, "R0");
    const PlanningRequest request =
        cpath_test::build_or_fail(make_spec(one_edge("n0", "p0", "p1"), fabric.spec, base_policy(4, 1),
                                            {bind("p0", "n0"), bind("p1", "n1")}),
                                  "family N disconnected");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_denial(outcome, ErrorCode::kNoPath, DenialKind::kProvenInfeasible,
                   "family N: a destination in another component is provably unreachable");
    report("N", outcome, request, "disconnected");
    if (!outcome.denials.empty()) {
      CPATH_CHECK(outcome.denials.front().conflict == cpath::ConflictKind::kDisconnected);
      CPATH_CHECK(outcome.denials.front().logical_edge.valid());
      CPATH_CHECK(outcome.denials.front().message.find("n0") != std::string::npos);
    }
  }

  {
    Fabric fabric;  // SYNTHETIC: a feasible route plus an irrelevant island.
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const char* node : {"n0", "n1", "x", "y", "z"}) {
      fabric.node(node);
    }
    fabric.edge("main1", "n0", "x", 10000, 10, "R0");
    fabric.edge("main2", "x", "n1", 10000, 10, "R0");
    fabric.edge("island1", "y", "z", 10000, 10, "R0");
    fabric.edge("island2", "z", "y", 10000, 10, "R0");
    const PlanningRequest request =
        cpath_test::build_or_fail(make_spec(one_edge("n1", "p0", "p1"), fabric.spec, base_policy(4, 1),
                                            {bind("p0", "n0"), bind("p1", "n1")}),
                                  "family N island");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family N: a disconnected island does not disturb the mapping");
    report("N", outcome, request, "irrelevant-island");
    if (outcome.ok()) {
      for (const cpath::PathPlan& path : outcome.plan->paths) {
        for (const cpath::Hop& hop : path.hops) {
          CPATH_CHECK(hop.edge.value() == "main1" || hop.edge.value() == "main2");
        }
      }
    }
  }
}

// ===========================================================================
// O. additional IRRELEVANT edges.
//
// Adding edges that lie on no usable route must not move the mapping decision.
// The canonical request digest does change - it covers the fabric - so the
// comparison is made on the plan's decision bytes, which is what a caller
// consumes.
// ===========================================================================
CPATH_TEST(algorithm, o_irrelevant_edges_do_not_change_the_plan) {
  Fabric base;  // SYNTHETIC
  base.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  for (const char* node : {"n0", "n1", "x", "y"}) {
    base.node(node);
  }
  base.edge("o1", "n0", "x", 10000, 10, "R0");
  base.edge("o2", "x", "n1", 10000, 10, "R0");
  base.edge("o3", "n0", "y", 10000, 900, "R0");
  base.edge("o4", "y", "n1", 10000, 900, "R0");
  const cpath::RequestSpec base_spec = make_spec(one_edge("o0", "p0", "p1"), base.spec, base_policy(5, 1),
                                                {bind("p0", "n0"), bind("p1", "n1")});

  Fabric richer = base;  // SYNTHETIC: the same fabric plus unreachable clutter.
  richer.node("y2").node("z2").node("dead");
  richer.edge("island1", "y2", "z2", 10000, 10, "R0");
  richer.edge("island2", "z2", "y2", 10000, 10, "R0");
  richer.edge("dead_in", "n0", "dead", 10000, 10, "R0");
  richer.edge("dead_out", "dead", "n0", 10000, 10, "R0");
  const cpath::RequestSpec richer_spec = make_spec(one_edge("o0", "p0", "p1"), richer.spec, base_policy(5, 1),
                                                  {bind("p0", "n0"), bind("p1", "n1")});

  const PlanningRequest base_request = cpath_test::build_or_fail(base_spec, "family O base");
  const PlanningRequest richer_request = cpath_test::build_or_fail(richer_spec, "family O richer");
  const PlanningOutcome base_outcome = cpath::plan_collective(base_request);
  const PlanningOutcome richer_outcome = cpath::plan_collective(richer_request);
  require_plan(base_outcome, base_request, "family O: base plans");
  report("O", base_outcome, base_request, "base");
  require_plan(richer_outcome, richer_request, "family O: clutter still plans");
  report("O", richer_outcome, richer_request, "plus-irrelevant-edges");
  if (!base_outcome.ok() || !richer_outcome.ok()) {
    return;
  }
  if (base_outcome.plan->input_digest == richer_request.canonical_digest) {
    fail_at("family O", "the richer fabric was expected to have a different canonical digest");
  }
  const std::vector<std::uint8_t> base_bytes = decision_bytes(*base_outcome.plan);
  const std::vector<std::uint8_t> richer_bytes = decision_bytes(*richer_outcome.plan);
  if (base_bytes != richer_bytes) {
    fail_at("family O", "irrelevant edges changed the plan decision: " + render_plan(*base_outcome.plan) + " vs " +
                            render_plan(*richer_outcome.plan));
  }
  CPATH_CHECK_EQ(base_outcome.plan->stats.total_cost, richer_outcome.plan->stats.total_cost);
  CPATH_CHECK_EQ(base_outcome.plan->paths.size(), richer_outcome.plan->paths.size());
}

// ===========================================================================
// P. additional BENEFICIAL edges.
//
// Three structurally different pairs: adding usable edges may change the plan,
// but it may never withdraw it. The third pair carries a declared demand, so
// the request-level capacity budget is active. A fourth pair starts from a
// proven denial and shows the fabric becoming mappable once a route exists.
// ===========================================================================
CPATH_TEST(algorithm, p_beneficial_edges_never_withdraw_a_plan) {
  {
    // P1: a single route, then a second cheaper route appears.
    Fabric base;  // SYNTHETIC
    base.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const char* node : {"n0", "x", "n1"}) {
      base.node(node);
    }
    base.edge("p1a", "n0", "x", 10000, 50, "R0");
    base.edge("p1b", "x", "n1", 10000, 50, "R0");
    Fabric richer = base;
    richer.node("y");
    richer.edge("p1c", "n0", "y", 10000, 10, "R0");
    richer.edge("p1d", "y", "n1", 10000, 10, "R0");
    const PlanningRequest base_request = cpath_test::build_or_fail(
        make_spec(one_edge("p1", "p0", "p1"), base.spec, base_policy(5, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family P1 base");
    const PlanningRequest richer_request = cpath_test::build_or_fail(
        make_spec(one_edge("p1", "p0", "p1"), richer.spec, base_policy(5, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family P1 richer");
    const PlanningOutcome base_outcome = cpath::plan_collective(base_request);
    const PlanningOutcome richer_outcome = cpath::plan_collective(richer_request);
    require_plan(base_outcome, base_request, "family P1 base");
    report("P", base_outcome, base_request, "P1-base");
    require_plan(richer_outcome, richer_request, "family P1 richer");
    report("P", richer_outcome, richer_request, "P1-plus-beneficial-edge");
    if (base_outcome.ok() && richer_outcome.ok()) {
      CPATH_CHECK(richer_outcome.plan->stats.total_cost <= base_outcome.plan->stats.total_cost);
    }
  }

  {
    // P2: two edge-disjoint siblings, then a third route is added.
    Fabric base;  // SYNTHETIC
    base.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const char* node : {"n0", "x", "y", "n1"}) {
      base.node(node);
    }
    base.edge("p2a", "n0", "x", 10000, 10, "R0");
    base.edge("p2b", "x", "n1", 10000, 10, "R0");
    base.edge("p2c", "n0", "y", 10000, 20, "R0");
    base.edge("p2d", "y", "n1", 10000, 20, "R0");
    Fabric richer = base;
    richer.node("z");
    richer.edge("p2e", "n0", "z", 10000, 30, "R0");
    richer.edge("p2f", "z", "n1", 10000, 30, "R0");
    Policy policy = base_policy(5, 2);
    policy.disjointness = Disjointness::kEdge;
    const PlanningRequest base_request = cpath_test::build_or_fail(
        make_spec(one_edge("p2", "p0", "p1"), base.spec, policy, {bind("p0", "n0"), bind("p1", "n1")}),
        "family P2 base");
    const PlanningRequest richer_request = cpath_test::build_or_fail(
        make_spec(one_edge("p2", "p0", "p1"), richer.spec, policy, {bind("p0", "n0"), bind("p1", "n1")}),
        "family P2 richer");
    const PlanningOutcome base_outcome = cpath::plan_collective(base_request);
    const PlanningOutcome richer_outcome = cpath::plan_collective(richer_request);
    require_plan(base_outcome, base_request, "family P2 base");
    report("P", base_outcome, base_request, "P2-base");
    require_plan(richer_outcome, richer_request, "family P2 richer");
    report("P", richer_outcome, richer_request, "P2-plus-beneficial-edge");
    if (richer_outcome.ok()) {
      CPATH_CHECK_EQ(richer_outcome.plan->paths.size(), static_cast<std::size_t>(2));
      CPATH_CHECK(siblings_are_edge_disjoint(*richer_outcome.plan));
    }
  }

  {
    // P3: a declared demand makes the capacity budget active, then a parallel
    // link with real spare capacity is added.
    Fabric base;  // SYNTHETIC
    base.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const char* node : {"n0", "x", "n1"}) {
      base.node(node);
    }
    base.edge("p3a", "n0", "x", 1000, 20, "R0");
    base.edge("p3b", "x", "n1", 1000, 20, "R0");
    Fabric richer = base;
    richer.edge("p3c", "n0", "x", 1000, 20, "R0");
    richer.edge("p3d", "x", "n1", 1000, 20, "R0");
    const PlanningRequest base_request = cpath_test::build_or_fail(
        make_spec(one_edge("p3", "p0", "p1", 400), base.spec, base_policy(5, 1), {bind("p0", "n0"), bind("p1", "n1")}),
        "family P3 base");
    const PlanningRequest richer_request = cpath_test::build_or_fail(
        make_spec(one_edge("p3", "p0", "p1", 400), richer.spec, base_policy(5, 1),
                  {bind("p0", "n0"), bind("p1", "n1")}),
        "family P3 richer");
    const PlanningOutcome base_outcome = cpath::plan_collective(base_request);
    const PlanningOutcome richer_outcome = cpath::plan_collective(richer_request);
    require_plan(base_outcome, base_request, "family P3 base with declared demand");
    report("P", base_outcome, base_request, "P3-base-with-demand");
    require_plan(richer_outcome, richer_request, "family P3 richer with declared demand");
    report("P", richer_outcome, richer_request, "P3-plus-capacity");
    if (richer_outcome.ok()) {
      CPATH_CHECK(richer_outcome.plan->stats.bottleneck_mbps > 0u);
    }
    // The declared demand is really on the books: each hop of the mapping is
    // charged its per-path share against the fabric's verified spare capacity.
    if (base_outcome.ok()) {
      CPATH_CHECK_EQ(allocation_of(*base_outcome.plan, "p3a"), static_cast<std::uint64_t>(400));
      CPATH_CHECK_EQ(allocation_of(*base_outcome.plan, "p3b"), static_cast<std::uint64_t>(400));
      CPATH_CHECK_EQ(base_outcome.plan->allocations.size(), static_cast<std::size_t>(2));
    }
  }

  {
    // P4: the base fabric starves the second logical edge, and one added route
    // makes the whole request mappable. Feasibility is monotone in edges.
    Fabric base;  // SYNTHETIC
    base.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    base.node("n0").node("n1").node("n2").node("n3").node("m").node("h");
    base.edge("c1", "n0", "m", 1000, 10, "R0");
    base.edge("c2", "n2", "m", 1000, 10, "R0");
    base.edge("cut", "m", "h", 150, 10, "R0");
    base.edge("c3", "h", "n1", 1000, 10, "R0");
    base.edge("c4", "h", "n3", 1000, 10, "R0");
    Fabric richer = base;
    richer.node("q").node("r");
    richer.edge("alt1", "n0", "q", 1000, 20, "R0");
    richer.edge("alt2", "q", "r", 1000, 20, "R0");
    richer.edge("alt3", "r", "n1", 1000, 20, "R0");
    cpath::CollectiveSpec collective;
    collective.id = cpath::CollectiveId{"mono0"};
    collective.kind = cpath::CollectiveKind::kTree;
    collective.demand_mbps = 100;
    collective.groups.push_back(tree_group("g0", "p0", {"p0", "p1"}));
    collective.groups.push_back(tree_group("g1", "p2", {"p2", "p3"}));
    const std::vector<cpath::EndpointBinding> bindings = {bind("p0", "n0"), bind("p1", "n1"), bind("p2", "n2"),
                                                         bind("p3", "n3")};
    const PlanningRequest base_request =
        cpath_test::build_or_fail(make_spec(collective, base.spec, base_policy(5, 1), bindings), "family P4 base");
    const PlanningRequest richer_request =
        cpath_test::build_or_fail(make_spec(collective, richer.spec, base_policy(5, 1), bindings), "family P4 richer");
    const PlanningOutcome base_outcome = cpath::plan_collective(base_request);
    const PlanningOutcome richer_outcome = cpath::plan_collective(richer_request);
    require_denial(base_outcome, ErrorCode::kInsufficientCapacity, DenialKind::kProvenInfeasible,
                   "family P4 baseline is provably starved");
    report("P", base_outcome, base_request, "P4-base-starved");
    require_plan(richer_outcome, richer_request, "family P4: the added route makes the request mappable");
    report("P", richer_outcome, richer_request, "P4-plus-route");
  }
}

// ===========================================================================
// Q. canonical graph-order independence.
//
// The same logical fabric declared four ways: nodes and edges in different
// orders, adjacency built in reverse, and the collective's member list rotated
// (which the canonical rotation must absorb). All four must produce the same
// canonical request digest and byte-identical plan bytes.
// ===========================================================================
CPATH_TEST(algorithm, q_canonical_order_independence) {
  std::vector<cpath::Node> nodes;
  const auto add_node = [&nodes](const char* id) {
    cpath::Node node;
    node.id = cpath::NodeId{id};
    node.tier = cpath::TierId{"LEAF"};
    nodes.push_back(std::move(node));
  };
  add_node("n0");
  add_node("n1");
  add_node("n2");
  add_node("h0");
  add_node("h1");

  std::vector<cpath::Edge> edges;
  const auto add_edge = [&edges](const char* id, const char* from, const char* to, std::uint64_t latency) {
    cpath::Edge edge;
    edge.id = cpath::EdgeId{id};
    edge.from = cpath::NodeId{from};
    edge.to = cpath::NodeId{to};
    edge.capacity_mbps = 10000;
    edge.latency_micros = latency;
    edge.tier = cpath::TierId{"LEAF"};
    edge.failure_domain = cpath::FailureDomainId{"R0"};
    edge.evidence = cpath::EvidenceClass::kSynthetic;
    edges.push_back(std::move(edge));
  };
  for (int leaf = 0; leaf < 3; ++leaf) {
    for (int spine = 0; spine < 2; ++spine) {
      const std::string suffix = std::to_string(leaf) + "_" + std::to_string(spine);
      const std::string source = "n" + std::to_string(leaf);
      const std::string target = "n" + std::to_string((leaf + 1) % 3);
      const std::string hub = "h" + std::to_string(spine);
      add_edge(("up" + suffix).c_str(), source.c_str(), hub.c_str(), 100);
      add_edge(("down" + suffix).c_str(), hub.c_str(), target.c_str(), 100);
    }
  }

  const auto fabric_for = [&nodes, &edges](const std::vector<std::size_t>& node_order,
                                           const std::vector<std::size_t>& edge_order) {
    Fabric fabric;
    fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
    for (const std::size_t index : node_order) {
      fabric.spec.nodes.push_back(nodes[index]);
    }
    for (const std::size_t index : edge_order) {
      fabric.spec.edges.push_back(edges[index]);
    }
    return fabric;
  };

  const std::vector<std::size_t> forward = {0, 1, 2, 3, 4};
  const std::vector<std::size_t> backward = {4, 3, 2, 1, 0};
  const std::vector<std::size_t> interleaved = {3, 0, 4, 1, 2};
  std::vector<std::size_t> edge_forward;
  for (std::size_t index = 0; index < edges.size(); ++index) {
    edge_forward.push_back(index);
  }
  std::vector<std::size_t> edge_backward = edge_forward;
  std::reverse(edge_backward.begin(), edge_backward.end());
  std::vector<std::size_t> edge_shuffled = {5, 0, 11, 2, 9, 1, 7, 3, 10, 4, 8, 6};

  Policy policy = base_policy(4, 2);
  policy.disjointness = Disjointness::kEdge;
  const std::vector<std::vector<const char*>> member_orders = {{"p0", "p1", "p2"},
                                                              {"p1", "p2", "p0"},
                                                              {"p2", "p0", "p1"},
                                                              {"p0", "p1", "p2"}};
  const std::vector<std::pair<std::vector<std::size_t>, std::vector<std::size_t>>> orders = {
      {forward, edge_forward}, {backward, edge_backward}, {interleaved, edge_shuffled}, {forward, edge_backward}};

  std::vector<std::uint8_t> reference;
  cpath::Digest reference_digest;
  bool first_variant = true;
  for (std::size_t variant = 0; variant < orders.size(); ++variant) {
    Fabric fabric = fabric_for(orders[variant].first, orders[variant].second);
    cpath::CollectiveSpec collective;
    collective.id = cpath::CollectiveId{"q0"};
    collective.kind = cpath::CollectiveKind::kRing;
    collective.groups.push_back(ring_group("g0", 0, member_orders[variant]));
    const PlanningRequest request = cpath_test::build_or_fail(
        make_spec(collective, fabric.spec, policy, {bind("p0", "n0"), bind("p1", "n1"), bind("p2", "n2")}),
        "family Q");
    const PlanningOutcome outcome = cpath::plan_collective(request);
    require_plan(outcome, request, "family Q variant");
    report("Q", outcome, request, "declaration-order-variant-" + std::to_string(variant));
    if (!outcome.ok()) {
      continue;
    }
    CPATH_CHECK_EQ(outcome.plan->paths.size(), static_cast<std::size_t>(6));
    const std::vector<std::uint8_t> bytes = outcome.plan->encode();
    if (first_variant) {
      reference = bytes;
      reference_digest = request.canonical_digest;
      first_variant = false;
      continue;
    }
    if (!(request.canonical_digest == reference_digest)) {
      fail_at("family Q", "variant " + std::to_string(variant) + " produced a different canonical request digest");
    }
    if (bytes != reference) {
      fail_at("family Q", "variant " + std::to_string(variant) + " produced different plan bytes");
    }
  }
  CPATH_CHECK(!first_variant);
}

// ===========================================================================
// R. saturated cost arithmetic.
//
// Latency and both scoring weights at the model ceiling, on a long chain. Every
// per-hop cost, every path cost and the plan total must stay at or below
// kCostCeiling: nothing wraps, nothing is silently truncated, and the cost the
// planner reports is the cost the policy defines.
// ===========================================================================
CPATH_TEST(algorithm, r_saturated_cost_arithmetic) {
  const std::size_t kHops = 32;
  Fabric fabric;  // SYNTHETIC: latency and utilisation both at the model ceiling.
  fabric.domain("S0", DomainKind::kSite).domain("R0", DomainKind::kRack, "S0");
  for (std::size_t index = 0; index <= kHops; ++index) {
    fabric.node(("s" + std::to_string(index)).c_str());
  }
  for (std::size_t index = 0; index < kHops; ++index) {
    fabric.edge(("ce" + std::to_string(index)).c_str(), ("s" + std::to_string(index)).c_str(),
                ("s" + std::to_string(index + 1u)).c_str(), cpath::kMaxCapacityMbps,
                cpath::kMaxLatencyMicros, "R0", cpath::kMaxCapacityMbps - 1u);
  }
  Policy policy = base_policy(kHops, 1);
  policy.latency_weight_milli = cpath::kMaxScoringWeightMilli;
  policy.congestion_weight_milli = cpath::kMaxScoringWeightMilli;
  const std::string last = "s" + std::to_string(kHops);
  const PlanningRequest request =
      cpath_test::build_or_fail(make_spec(one_edge("r0", "p0", "p1"), fabric.spec, policy,
                                          {bind("p0", "s0"), bind("p1", last.c_str())}),
                                "family R");
  const PlanningOutcome outcome = cpath::plan_collective(request);
  require_plan(outcome, request, "family R: ceiling-valued arithmetic still plans");
  report("R", outcome, request, "ceiling-cost-arithmetic");
  if (!outcome.ok()) {
    return;
  }
  const cpath::Plan& plan = *outcome.plan;
  std::uint64_t recomputed_total = 0;
  for (const cpath::PathPlan& path : plan.paths) {
    const std::uint64_t expected = model_path_cost(path, request.fabric, policy);
    if (path.cost != expected) {
      fail_at("family R", "path cost " + std::to_string(path.cost) + " does not match the policy model value " +
                              std::to_string(expected));
    }
    CPATH_CHECK(path.cost <= cpath::kCostCeiling);
    recomputed_total = cpath::saturating_add(recomputed_total, path.cost, cpath::kCostCeiling);
  }
  CPATH_CHECK_EQ(plan.stats.total_cost, recomputed_total);
  CPATH_CHECK(plan.stats.total_cost <= cpath::kCostCeiling);
  CPATH_CHECK(plan.stats.max_path_cost <= cpath::kCostCeiling);
  CPATH_CHECK(plan.stats.total_cost > 0u);
}

}  // namespace

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }
