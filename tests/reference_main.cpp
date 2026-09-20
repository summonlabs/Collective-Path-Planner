// Collective Path Planner - differential testing against an exact reference.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every fabric in this file is SYNTHETIC: generated here, never measured on a
// device.
//
// The property under test is the one that matters most for a planner: the
// production planner must never confuse "I did not find a mapping" with "there
// is no mapping". For every bounded randomized instance this suite solves the
// problem exhaustively with tests/support/exact_solver.cpp - a slow, independent
// reference that shares no code with src/planner.cpp - and then asserts:
//
//   1. SAFETY: every plan the production planner returns is valid, re-derived
//      here from the request rather than taken on trust;
//   2. SOUNDNESS OF PROOF: when production claims proven infeasibility, the
//      reference must agree that no assignment exists;
//   3. NO MISSED SOLUTIONS: when the reference finds an assignment, production
//      must return a plan or report an INDETERMINATE result - never proof that
//      no mapping exists;
//   4. OPTIMALITY WHERE CLAIMED: when production reports optimal == true, its
//      objective must equal the reference's, level by level;
//   5. DETERMINISM: planning the same request twice yields identical bytes.
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpath/dsl.hpp"
#include "cpath/explain.hpp"
#include "cpath/planner.hpp"
#include "cpath/request.hpp"
#include "exact_solver.hpp"
#include "test_framework.hpp"

namespace cp = cpath;

namespace {

using cp::CapacityEvidenceGeneration;
using cp::CollectiveId;
using cp::CollectiveKind;
using cp::CollectivePlanGeneration;
using cp::Disjointness;
using cp::DomainDiversity;
using cp::DomainKind;
using cp::Edge;
using cp::EdgeId;
using cp::EndpointBinding;
using cp::EvidenceClass;
using cp::EvidenceRequirement;
using cp::FailureDomain;
using cp::FailureDomainGeneration;
using cp::FailureDomainId;
using cp::GroupId;
using cp::GroupPattern;
using cp::GroupSpec;
using cp::Node;
using cp::NodeId;
using cp::ParticipantId;
using cp::PlanningOutcome;
using cp::PlanningRequest;
using cp::PolicyGeneration;
using cp::RequestSpec;
using cp::TierId;
using cp::TopologyGeneration;

struct CaseParams {
  std::size_t participants{3};
  std::size_t spines{2};
  std::size_t racks{2};
  std::uint64_t capacity{10000};
  std::uint64_t latency{100};
  std::uint64_t demand{0};
  std::uint64_t topology_generation{1};
  std::size_t max_hops{4};
  std::size_t paths{1};
  Disjointness disjointness{Disjointness::kNone};
  DomainDiversity diversity{DomainDiversity::kNone};
  EvidenceRequirement evidence{EvidenceRequirement::kAny};
  bool zero_capacity{false};
  bool unknown_evidence{false};
  bool duplicate_links{false};
  bool forbid_node{false};
  bool forbid_domain{false};
  bool forbid_tier{false};
  bool asymmetric{false};
};

std::string number(const char* prefix, std::size_t index) {
  return std::string(prefix) + std::to_string(index);
}

RequestSpec build_case(cpath_test::Random& rng, const CaseParams& params) {
  RequestSpec spec;
  spec.fabric.topology_generation = TopologyGeneration{params.topology_generation};
  spec.fabric.failure_domain_generation = FailureDomainGeneration{7};
  spec.fabric.capacity_evidence_generation = CapacityEvidenceGeneration{9};
  spec.policy.generation = PolicyGeneration{5};
  spec.plan_generation = CollectivePlanGeneration{3};

  const auto domain = [&spec](const std::string& id, DomainKind kind, const std::string& parent) {
    FailureDomain record;
    record.id = FailureDomainId{id};
    record.kind = kind;
    if (!parent.empty()) {
      record.parent = FailureDomainId{parent};
    }
    spec.fabric.failure_domains.push_back(record);
  };
  domain("S0", DomainKind::kSite, "");
  for (std::size_t rack = 0; rack < params.racks; ++rack) {
    domain(number("R", rack), DomainKind::kRack, "S0");
  }
  domain("CORE", DomainKind::kPod, "S0");

  for (std::size_t index = 0; index < params.participants; ++index) {
    domain(number("H", index), DomainKind::kHost, number("R", index % params.racks));
    Node node;
    node.id = NodeId{number("n", index)};
    node.placement.supplied = true;
    node.placement.rack = number("R", index % params.racks);
    node.placement.host = number("H", index);
    node.tier = TierId{"LEAF"};
    spec.fabric.nodes.push_back(node);
  }
  for (std::size_t index = 0; index < params.spines; ++index) {
    Node node;
    node.id = NodeId{number("s", index)};
    node.placement.supplied = true;
    node.placement.pod = "CORE";
    node.tier = TierId{"SPINE"};
    spec.fabric.nodes.push_back(node);
  }

  std::size_t counter = 0;
  const auto link = [&](const std::string& from, const std::string& to, const std::string& failure_domain,
                        bool reverse) {
    const auto emit = [&](const std::string& a, const std::string& b, const std::string& suffix) {
      Edge edge;
      edge.id = EdgeId{"e" + std::to_string(counter++) + suffix};
      edge.from = NodeId{a};
      edge.to = NodeId{b};
      edge.capacity_mbps = params.capacity;
      edge.latency_micros = params.latency;
      edge.tier = TierId{"LEAF"};
      edge.failure_domain = FailureDomainId{failure_domain};
      edge.evidence = EvidenceClass::kSynthetic;
      if (params.zero_capacity && rng.chance(1u, 8u)) {
        edge.capacity_mbps = 0;
      }
      if (params.unknown_evidence && rng.chance(1u, 4u)) {
        edge.evidence = EvidenceClass::kUnknown;
      }
      spec.fabric.edges.push_back(edge);
      if (params.duplicate_links && rng.chance(1u, 3u)) {
        Edge second = edge;
        second.id = EdgeId{edge.id.value() + "b"};
        second.latency_micros = edge.latency_micros + 5;
        spec.fabric.edges.push_back(second);
      }
    };
    emit(from, to, "");
    if (reverse && !params.asymmetric) {
      emit(to, from, "r");
    }
  };

  for (std::size_t leaf = 0; leaf < params.participants; ++leaf) {
    for (std::size_t spine = 0; spine < params.spines; ++spine) {
      link(number("n", leaf), number("s", spine), number("H", leaf), true);
    }
  }
  for (std::size_t from = 0; from < params.spines; ++from) {
    for (std::size_t to = 0; to < params.spines; ++to) {
      if (from != to) {
        link(number("s", from), number("s", to), "CORE", false);
      }
    }
  }

  GroupSpec group;
  group.id = GroupId{"g0"};
  group.level = 0;
  group.pattern = GroupPattern::kRing;
  for (std::size_t index = 0; index < params.participants; ++index) {
    group.members.push_back(ParticipantId{number("p", index)});
  }
  spec.collective.id = CollectiveId{"c0"};
  spec.collective.kind = CollectiveKind::kRing;
  spec.collective.demand_mbps = params.demand;
  spec.collective.groups.push_back(std::move(group));

  for (std::size_t index = 0; index < params.participants; ++index) {
    EndpointBinding binding;
    binding.participant = ParticipantId{number("p", index)};
    binding.node = NodeId{number("n", index)};
    spec.bindings.push_back(std::move(binding));
  }

  spec.policy.max_hops = params.max_hops;
  spec.policy.paths_per_logical_edge = params.paths;
  spec.policy.disjointness = params.disjointness;
  spec.policy.domain_diversity = params.diversity;
  spec.policy.domain_diversity_level = DomainKind::kRack;
  spec.policy.evidence = params.evidence;
  spec.policy.congestion_weight_milli = rng.chance(1u, 2u) ? 500u : 0u;
  spec.policy.allow_unverified_capacity = params.zero_capacity;
  if (params.forbid_node && params.participants >= 3) {
    // Never forbid an endpoint: the interesting case is a forbidden transit node.
    spec.policy.forbidden_nodes.push_back(NodeId{number("s", 0)});
  }
  if (params.forbid_domain && params.spines >= 2) {
    spec.policy.forbidden_failure_domains.push_back(FailureDomainId{"CORE"});
  }
  if (params.forbid_tier) {
    spec.policy.forbidden_tiers.push_back(TierId{"SPINE"});
  }
  return spec;
}

std::string dump(const RequestSpec& spec, const PlanningRequest& request, const CaseParams& params,
                 std::size_t iteration, std::uint64_t seed, const cpath_test::ExactResult& exact,
                 const PlanningOutcome& outcome) {
  std::string out;
  out += "iteration=" + std::to_string(iteration) + " seed=" + std::to_string(seed) + "\n";
  out += "params{participants=" + std::to_string(params.participants) + " spines=" +
         std::to_string(params.spines) + " racks=" + std::to_string(params.racks) + " capacity=" +
         std::to_string(params.capacity) + " latency=" + std::to_string(params.latency) + " demand=" +
         std::to_string(params.demand) + " topology_generation=" + std::to_string(params.topology_generation) +
         " max_hops=" + std::to_string(params.max_hops) + " paths=" + std::to_string(params.paths) +
         " disjointness=" + cp::to_string(params.disjointness) + " diversity=" + cp::to_string(params.diversity) +
         " evidence=" + cp::to_string(params.evidence) + " zero_capacity=" +
         (params.zero_capacity ? "1" : "0") + " unknown_evidence=" + (params.unknown_evidence ? "1" : "0") +
         " duplicate_links=" + (params.duplicate_links ? "1" : "0") + " forbid_node=" +
         (params.forbid_node ? "1" : "0") + " forbid_domain=" + (params.forbid_domain ? "1" : "0") +
         " forbid_tier=" + (params.forbid_tier ? "1" : "0") + " asymmetric=" +
         (params.asymmetric ? "1" : "0") + "}\n";
  out += "canonical digest " + request.canonical_digest.hex() + "\n";
  out += "nodes:\n";
  for (const Node& node : request.fabric.nodes()) {
    out += "  " + node.id.value() + " tier=" + node.tier.value() + " rack=" + node.placement.rack + "\n";
  }
  out += "edges:\n";
  for (const Edge& edge : request.fabric.edges()) {
    out += "  " + edge.id.value() + " " + edge.from.value() + "->" + edge.to.value() + " cap=" +
           std::to_string(edge.capacity_mbps) + " reserved=" + std::to_string(edge.reserved_mbps) + " lat=" +
           std::to_string(edge.latency_micros) + " tier=" + edge.tier.value() + " domain=" +
           edge.failure_domain.value() + " evidence=" + cp::to_string(edge.evidence) + " eligible=" +
           (edge.eligible ? "1" : "0") + "\n";
  }
  out += "domains:\n";
  for (const FailureDomain& domain : request.fabric.failure_domains()) {
    out += "  " + domain.id.value() + " kind=" + cp::to_string(domain.kind) + " parent=" + domain.parent.value() +
           "\n";
  }
  out += "forbidden nodes=" + std::to_string(request.policy.forbidden_nodes.size()) + " tiers=" +
         std::to_string(request.policy.forbidden_tiers.size()) + " domains=" +
         std::to_string(request.policy.forbidden_failure_domains.size()) + "\n";
  out += std::string("production: ok=") + (outcome.ok() ? "1" : "0") +
         " code=" + cp::code_symbol(outcome.primary_code()) +
         " kind=" + cp::to_string(outcome.primary_kind()) +
         " optimal=" + (outcome.optimal ? "1" : "0") + " paths=" + std::to_string(outcome.paths_enumerated) +
         "\n";
  for (const cp::DenialDetail& denial : outcome.denials) {
    out += "  denial " + std::string(cp::code_symbol(denial.code)) + " le=" +
           std::to_string(denial.logical_edge.value()) + " :: " + denial.message + "\n";
  }
  out += std::string("reference: status=") + cpath_test::to_string(exact.status) +
         " total_cost=" + std::to_string(exact.total_cost) +
         " max_path_cost=" + std::to_string(exact.max_path_cost) +
         " conflicts=" + std::to_string(exact.domain_conflicts) +
         " detail=" + exact.detail + "\n";
  (void)spec;
  return out;
}

// The plan's objective, re-derived from the emitted paths.
struct Objective {
  std::uint64_t total_cost{0};
  std::size_t conflicts{0};
  std::uint64_t max_path_cost{0};
};

// Two sibling paths are failure-domain independent when neither of them relies
// on an edge whose failure domain is undeclared, and their signatures do not
// intersect. Two empty signatures are vacuously independent: they claim nothing
// in common, which is exactly what disjointness means.
bool independent_pair(const PlanningRequest& request, const cp::PathPlan& lhs, const cp::PathPlan& rhs) {
  const auto domain_known = [&request](const cp::PathPlan& path) {
    for (const cp::Hop& hop : path.hops) {
      const Edge* edge = request.fabric.find_edge(hop.edge);
      if (edge == nullptr || !edge->failure_domain.valid()) {
        return false;
      }
    }
    return true;
  };
  if (!domain_known(lhs) || !domain_known(rhs)) {
    return false;
  }
  for (const FailureDomainId& domain : lhs.domain_signature) {
    if (std::binary_search(rhs.domain_signature.begin(), rhs.domain_signature.end(), domain)) {
      return false;
    }
  }
  return true;
}

Objective objective_of(const PlanningRequest& request, const cp::Plan& plan) {
  Objective value;
  value.total_cost = plan.stats.total_cost;
  value.max_path_cost = plan.stats.max_path_cost;
  if (request.policy.domain_diversity != DomainDiversity::kNone) {
    for (std::size_t index = 0; index < plan.paths.size(); ++index) {
      for (std::size_t other = index + 1u; other < plan.paths.size(); ++other) {
        if (!(plan.paths[index].logical_edge == plan.paths[other].logical_edge)) {
          continue;
        }
        if (!independent_pair(request, plan.paths[index], plan.paths[other])) {
          ++value.conflicts;
        }
      }
    }
  }
  return value;
}

// Independent re-derivation of every hard constraint from the request.
void assert_plan_valid(const PlanningRequest& request, const cp::Plan& plan, const std::string& context) {
  const cp::Status status = cp::validate_plan(plan, request);
  if (!status.is_ok()) {
    CPATH_FAIL(context + ": validate_plan rejected a produced plan: " + status.to_string());
  }
  const auto& policy = request.policy;
  for (const cp::PathPlan& path : plan.paths) {
    if (!cp::hops_are_simple(path.hops)) {
      CPATH_FAIL(context + ": emitted a path that is not simple");
    }
    if (path.hops.size() > policy.max_hops) {
      CPATH_FAIL(context + ": emitted a path beyond the hop limit");
    }
    if (path.cost > policy.max_path_cost) {
      CPATH_FAIL(context + ": emitted a path beyond the cost limit");
    }
    for (const cp::Hop& hop : path.hops) {
      const Edge* edge = request.fabric.find_edge(hop.edge);
      if (edge == nullptr || !(edge->from == hop.from) || !(edge->to == hop.to)) {
        CPATH_FAIL(context + ": a hop does not match its physical edge");
      }
    }
  }
  // Sibling disjointness, re-derived.
  std::unordered_map<std::uint64_t, std::vector<const cp::PathPlan*>> by_edge;
  for (const cp::PathPlan& path : plan.paths) {
    by_edge[path.logical_edge.value()].push_back(&path);
  }
  for (const auto& entry : by_edge) {
    const std::vector<const cp::PathPlan*>& siblings = entry.second;
    if (siblings.size() != policy.paths_per_logical_edge) {
      CPATH_FAIL(context + ": a logical edge does not carry the required number of paths");
    }
    for (std::size_t i = 0; i < siblings.size(); ++i) {
      for (std::size_t j = i + 1u; j < siblings.size(); ++j) {
        if (policy.disjointness != Disjointness::kNone) {
          for (const cp::Hop& left : siblings[i]->hops) {
            for (const cp::Hop& right : siblings[j]->hops) {
              if (left.edge == right.edge) {
                CPATH_FAIL(context + ": sibling paths share a physical edge");
              }
            }
          }
        }
        if (policy.disjointness == Disjointness::kNode) {
          for (std::size_t a = 0; a + 1u < siblings[i]->hops.size(); ++a) {
            for (std::size_t b = 0; b + 1u < siblings[j]->hops.size(); ++b) {
              if (siblings[i]->hops[a].to == siblings[j]->hops[b].to) {
                CPATH_FAIL(context + ": sibling paths share an interior node");
              }
            }
          }
        }
        if (policy.domain_diversity == DomainDiversity::kRequired) {
          for (const FailureDomainId& domain : siblings[i]->domain_signature) {
            if (std::binary_search(siblings[j]->domain_signature.begin(),
                                   siblings[j]->domain_signature.end(), domain)) {
              CPATH_FAIL(context + ": sibling paths are not failure-domain independent");
            }
          }
        }
      }
    }
  }
  // The whole-request capacity budget, re-derived from the allocations.
  for (const cp::Allocation& allocation : plan.allocations) {
    const Edge* edge = request.fabric.find_edge(allocation.edge);
    if (edge == nullptr) {
      CPATH_FAIL(context + ": the plan allocates an edge that is not in the fabric");
    }
    if (edge->capacity_mbps > 0 && allocation.planned_mbps > edge->capacity_mbps - edge->reserved_mbps) {
      CPATH_FAIL(context + ": the plan commits more than the verified spare capacity of " + edge->id.value());
    }
  }
}

struct Tally {
  std::size_t cases{0};
  std::size_t planned{0};
  std::size_t proven_infeasible{0};
  std::size_t indeterminate{0};
  std::size_t reference_feasible{0};
  std::size_t reference_infeasible{0};
  std::size_t reference_limited{0};
  std::size_t optimality_compared{0};
  std::size_t forbidden{0};
};

void run_case(cpath_test::Random& rng, const CaseParams& params, std::uint64_t seed, std::size_t iteration,
              Tally& tally, bool require_optimality) {
  RequestSpec spec = build_case(rng, params);
  auto request = cp::build_request(spec);
  if (!request.has_value()) {
    ++tally.forbidden;
    return;
  }
  ++tally.cases;

  const cpath_test::ExactResult exact = cpath_test::solve_exact(request.value());
  const PlanningOutcome outcome = cp::plan_collective(request.value());

  const std::string context = "seed=" + std::to_string(seed) + " iteration=" + std::to_string(iteration);
  const auto fail = [&](const std::string& what) {
    CPATH_FAIL(what + "\n" + dump(spec, request.value(), params, iteration, seed, exact, outcome));
  };

  if (outcome.ok()) {
    ++tally.planned;
    assert_plan_valid(request.value(), *outcome.plan, context);
    // 5. Determinism: identical canonical input, identical bytes.
    const PlanningOutcome again = cp::plan_collective(request.value());
    if (!again.ok() || !(again.plan->body_digest() == outcome.plan->body_digest())) {
      fail("planning the same request twice did not produce identical bytes");
    }
  } else if (outcome.proven_infeasible()) {
    ++tally.proven_infeasible;
    if (outcome.plan.has_value()) {
      fail("a proven denial carried a partial plan");
    }
  } else {
    ++tally.indeterminate;
  }

  switch (exact.status) {
    case cpath_test::ExactResult::Status::kFeasible:
      ++tally.reference_feasible;
      break;
    case cpath_test::ExactResult::Status::kInfeasible:
      ++tally.reference_infeasible;
      break;
    case cpath_test::ExactResult::Status::kLimitReached:
      ++tally.reference_limited;
      break;
  }

  // 2. Soundness of proof: a claim that no mapping exists must be true.
  if (outcome.proven_infeasible() && exact.status == cpath_test::ExactResult::Status::kFeasible) {
    fail("the planner claimed proven infeasibility but the reference found a mapping");
  }
  // 3. No missed solutions: a reference solution must yield a plan or an
  //    explicit INDETERMINATE result, never a claim that none exists.
  if (exact.status == cpath_test::ExactResult::Status::kFeasible && !outcome.ok() && !outcome.indeterminate()) {
    fail("the reference found a mapping but production reported a conclusive denial");
  }
  // 4. Optimality where claimed.
  if (require_optimality && outcome.ok() && outcome.optimal &&
      exact.status == cpath_test::ExactResult::Status::kFeasible) {
    ++tally.optimality_compared;
    const Objective produced = objective_of(request.value(), *outcome.plan);
    if (produced.total_cost != exact.total_cost) {
      fail("claimed optimality but the total cost differs: production " + std::to_string(produced.total_cost) +
           ", reference " + std::to_string(exact.total_cost));
    }
    if (produced.max_path_cost != exact.max_path_cost) {
      fail("claimed optimality but the largest path cost differs: production " +
           std::to_string(produced.max_path_cost) + ", reference " + std::to_string(exact.max_path_cost));
    }
    if (produced.conflicts != exact.domain_conflicts) {
      fail("claimed optimality but the sibling domain-conflict count differs: production " +
           std::to_string(produced.conflicts) + ", reference " + std::to_string(exact.domain_conflicts));
    }
  }
  // A conclusive reference answer may never be masked by an INDETERMINATE
  // production answer when production is in fact complete for the instance.
  if (exact.status == cpath_test::ExactResult::Status::kInfeasible && outcome.ok()) {
    fail("the reference proved no mapping exists but production returned a plan");
  }
}

void report(const char* name, const Tally& tally) {
  std::printf("SYNTHETIC reference %-28s cases=%zu planned=%zu proven_infeasible=%zu indeterminate=%zu "
              "reference{feasible=%zu infeasible=%zu limited=%zu} optimality_compared=%zu rejected_specs=%zu\n",
              name, tally.cases, tally.planned, tally.proven_infeasible, tally.indeterminate,
              tally.reference_feasible, tally.reference_infeasible, tally.reference_limited,
              tally.optimality_compared, tally.forbidden);
}

CaseParams random_params(cpath_test::Random& rng, bool with_demand) {
  CaseParams params;
  params.participants = 2u + static_cast<std::size_t>(rng.below(4u));   // 2..5
  params.spines = 1u + static_cast<std::size_t>(rng.below(3u));         // 1..3
  params.racks = 1u + static_cast<std::size_t>(rng.below(3u));          // 1..3
  params.capacity = 1000u + rng.between(0u, 9000u);
  params.latency = 10u + rng.between(0u, 400u);
  params.demand = with_demand ? (rng.chance(1u, 4u) ? 0u : 100u + rng.between(0u, 900u)) : 0u;
  params.topology_generation = 1u + rng.between(0u, 4u);
  params.max_hops = 2u + static_cast<std::size_t>(rng.below(4u));       // 2..5
  params.paths = 1u + static_cast<std::size_t>(rng.below(3u));          // 1..3
  params.disjointness = static_cast<Disjointness>(rng.below(3u));
  params.diversity = static_cast<DomainDiversity>(rng.below(3u));
  params.evidence = rng.chance(1u, 4u) ? EvidenceRequirement::kSyntheticOrBetter : EvidenceRequirement::kAny;
  params.zero_capacity = rng.chance(1u, 6u);
  params.unknown_evidence = rng.chance(1u, 5u);
  params.duplicate_links = rng.chance(1u, 4u);
  params.forbid_node = rng.chance(1u, 6u);
  params.forbid_domain = rng.chance(1u, 8u);
  params.forbid_tier = rng.chance(1u, 10u);
  params.asymmetric = rng.chance(1u, 10u);
  return params;
}

void run_family(std::uint64_t stream, std::size_t iterations, bool with_demand, bool require_optimality,
                const char* name) {
  cpath_test::Random rng = cpath_test::make_random(stream);
  Tally tally;
  const std::uint64_t seed = rng.seed();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    run_case(rng, random_params(rng, with_demand), seed, iteration, tally, require_optimality);
  }
  report(name, tally);
  CPATH_CHECK(tally.cases > 0u);
}

}  // namespace

CPATH_TEST(Reference, small_fabrics_without_demand) {
  run_family(1, 700, false, true, "no-demand");
}

CPATH_TEST(Reference, small_fabrics_with_declared_demand) {
  run_family(2, 600, true, true, "with-demand");
}

CPATH_TEST(Reference, dense_and_parallel_link_fabrics) {
  run_family(3, 500, true, true, "dense");
}

CPATH_TEST(Reference, sparse_and_asymmetric_fabrics) {
  cpath_test::Random rng = cpath_test::make_random(4);
  const std::uint64_t seed = rng.seed();
  Tally tally;
  for (std::size_t iteration = 0; iteration < 500; ++iteration) {
    CaseParams params = random_params(rng, true);
    // Attack the sparse end of the space, where the reference is exhaustive and
    // the interesting failures live.
    params.participants = 2u + static_cast<std::size_t>(rng.below(3u));
    params.spines = 1u + static_cast<std::size_t>(rng.below(2u));
    params.asymmetric = rng.chance(1u, 3u);
    params.zero_capacity = rng.chance(1u, 3u);
    run_case(rng, params, seed, iteration, tally, true);
  }
  report("sparse", tally);
  CPATH_CHECK(tally.cases > 0u);
  CPATH_CHECK(tally.planned > 0u);
}

CPATH_TEST(Reference, diversity_heavy_fabrics) {
  cpath_test::Random rng = cpath_test::make_random(5);
  const std::uint64_t seed = rng.seed();
  Tally tally;
  for (std::size_t iteration = 0; iteration < 500; ++iteration) {
    CaseParams params = random_params(rng, true);
    params.paths = 2u + static_cast<std::size_t>(rng.below(2u));
    params.disjointness = rng.chance(1u, 2u) ? Disjointness::kEdge : Disjointness::kNode;
    params.diversity = DomainDiversity::kRequired;
    params.racks = 2u + static_cast<std::size_t>(rng.below(2u));
    run_case(rng, params, seed, iteration, tally, true);
  }
  report("diversity", tally);
  CPATH_CHECK(tally.cases > 0u);
}

CPATH_TEST(Reference, deterministic_replay_across_independent_constructions) {
  cpath_test::Random rng = cpath_test::make_random(6);
  const std::uint64_t seed = rng.seed();
  Tally tally;
  for (std::size_t iteration = 0; iteration < 400; ++iteration) {
    const CaseParams params = random_params(rng, true);
    RequestSpec spec = build_case(rng, params);
    auto first = cp::build_request(spec);
    if (!first.has_value()) {
      continue;
    }
    ++tally.cases;
    // The same request parsed from its own canonical text must plan identically.
    const std::string where = "seed=" + std::to_string(seed) + " iteration=" + std::to_string(iteration);
    const std::string text = cp::encode_request_text(first.value());
    auto second = cp::parse_request_text(text);
    if (!second.has_value()) {
      CPATH_FAIL("canonical text did not round-trip at " + where);
    }
    if (!(first.value().canonical_digest == second.value().canonical_digest)) {
      CPATH_FAIL("round-trip changed the canonical digest at " + where);
    }
    const PlanningOutcome left = cp::plan_collective(first.value());
    const PlanningOutcome right = cp::plan_collective(second.value());
    if (left.ok() != right.ok()) {
      CPATH_FAIL("round-trip changed the planning result at " + where);
    }
    if (left.ok() && !(left.plan->body_digest() == right.plan->body_digest())) {
      CPATH_FAIL("round-trip changed the plan bytes at " + where);
    }
    if (left.ok()) {
      ++tally.planned;
      assert_plan_valid(first.value(), *left.plan, "roundtrip");
    }
  }
  report("round-trip determinism", tally);
}

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }
