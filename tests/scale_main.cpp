// Collective Path Planner - bounded-resource and complexity proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Everything in this suite is SYNTHETIC: every fabric is generated here by a
// deterministic model, nothing is measured on a device, and no number printed
// here is a hardware claim. Every case prints its own wall-clock time; those
// numbers are REPORTING, not assertions - nothing in this file asserts on time,
// because a slow machine is not a defect and a timeout would hide a hang.
//
// Sizing note (what was tuned, and why): the large fabric is a 64x64 torus grid
// (4096 nodes, 16384 edges) and the doubling case compares it against 64x128
// (8192 nodes, 32768 edges). Those two sizes keep the whole suite well inside
// the 60 second budget while still being large enough that any accidental
// O(nodes) or O(nodes * hops) per-search behaviour shows up as a ratio far above
// the documented bound. The saturation chain is 1001 nodes so that a single path
// can use the maximum admissible 256 hops (kMaxHops) and 17 such paths overflow
// kCostCeiling exactly.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "cpath/limits.hpp"
#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/request.hpp"
#include "fixtures.hpp"
#include "test_framework.hpp"

namespace {

namespace cp = cpath;

using Clock = std::chrono::steady_clock;

double millis_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string numbered(const char* prefix, std::size_t index) {
  return std::string(prefix) + std::to_string(index);
}

void add_domains(cp::RequestSpec& spec) {
  cp::FailureDomain site;
  site.id = cp::FailureDomainId{"S0"};
  site.kind = cp::DomainKind::kSite;
  spec.fabric.failure_domains.push_back(std::move(site));
  cp::FailureDomain rack;
  rack.id = cp::FailureDomainId{"R0"};
  rack.parent = cp::FailureDomainId{"S0"};
  rack.kind = cp::DomainKind::kRack;
  spec.fabric.failure_domains.push_back(std::move(rack));
}

void add_node(cp::RequestSpec& spec, const std::string& id, const std::string& tier) {
  cp::Node node;
  node.id = cp::NodeId{id};
  node.tier = cp::TierId{tier};
  node.placement.supplied = true;
  node.placement.site = "S0";
  node.placement.rack = "R0";
  node.placement.device_index = 0;
  spec.fabric.nodes.push_back(std::move(node));
}

void add_edge(cp::RequestSpec& spec, const std::string& id, const std::string& from, const std::string& to,
              std::uint64_t capacity, std::uint64_t latency, const std::string& tier) {
  cp::Edge edge;
  edge.id = cp::EdgeId{id};
  edge.from = cp::NodeId{from};
  edge.to = cp::NodeId{to};
  edge.capacity_mbps = capacity;
  edge.latency_micros = latency;
  edge.tier = cp::TierId{tier};
  edge.failure_domain = cp::FailureDomainId{"R0"};
  edge.evidence = cp::EvidenceClass::kSynthetic;
  spec.fabric.edges.push_back(std::move(edge));
}

void add_binding(cp::RequestSpec& spec, const std::string& participant, const std::string& node) {
  cp::EndpointBinding binding;
  binding.participant = cp::ParticipantId{participant};
  binding.node = cp::NodeId{node};
  spec.bindings.push_back(std::move(binding));
}

void add_ring(cp::RequestSpec& spec, const std::string& collective_id, std::size_t participants) {
  cp::GroupSpec group;
  group.id = cp::GroupId{"g0"};
  group.pattern = cp::GroupPattern::kRing;
  group.level = 0;
  for (std::size_t index = 0; index < participants; ++index) {
    group.members.push_back(cp::ParticipantId{numbered("p", index)});
  }
  spec.collective.id = cp::CollectiveId{collective_id};
  spec.collective.kind = cp::CollectiveKind::kRing;
  spec.collective.groups.push_back(std::move(group));
}

void set_generations(cp::RequestSpec& spec) {
  spec.fabric.topology_generation = cp::TopologyGeneration{11};
  spec.fabric.failure_domain_generation = cp::FailureDomainGeneration{12};
  spec.fabric.capacity_evidence_generation = cp::CapacityEvidenceGeneration{13};
  spec.policy.generation = cp::PolicyGeneration{14};
  spec.plan_generation = cp::CollectivePlanGeneration{15};
}

// The structural part of the plan contract, re-derived from the plan and the
// request. The randomized property suite owns the exhaustive version; this is
// the subset the scale proofs need so that a "planned" result is never taken on
// faith.
void check_plan_basics(const cp::Plan& plan, const cp::PlanningRequest& request, const std::string& where) {
  CPATH_CHECK(cp::validate_plan(plan, request).is_ok());
  CPATH_CHECK_EQ(plan.stats.path_count, plan.paths.size());
  CPATH_CHECK_EQ(plan.stats.logical_edge_count, request.collective.logical_edges.size());
  std::size_t hops = 0;
  for (const cp::PathPlan& path : plan.paths) {
    CPATH_CHECK(path.hops.size() <= request.policy.max_hops);
    CPATH_CHECK(!path.hops.empty());
    if (!path.hops.empty()) {
      const cp::LogicalEdge& logical = request.collective.logical_edges[path.logical_edge.value() - 1u];
      const cp::EndpointBinding* source = request.find_binding(logical.src);
      const cp::EndpointBinding* target = request.find_binding(logical.dst);
      CPATH_CHECK(source != nullptr && target != nullptr);
      if (source != nullptr && target != nullptr) {
        CPATH_CHECK(path.hops.front().from == source->node);
        CPATH_CHECK(path.hops.back().to == target->node);
      }
    }
    for (std::size_t index = 0; index < path.hops.size(); ++index) {
      const cp::Edge* edge = request.fabric.find_edge(path.hops[index].edge);
      CPATH_CHECK(edge != nullptr);
      if (edge == nullptr) {
        continue;
      }
      CPATH_CHECK(edge->from == path.hops[index].from && edge->to == path.hops[index].to);
      if (index + 1u < path.hops.size()) {
        CPATH_CHECK(path.hops[index + 1u].from == path.hops[index].to);
      }
    }
    hops += path.hops.size();
  }
  CPATH_CHECK_EQ(plan.stats.hop_count, hops);
  if (!plan.paths.empty()) {
    (void)where;
  }
}

// SYNTHETIC torus grid: width * height nodes, four directed edges each
// (right, left, up, down, wrapping). Generated by a loop, never randomly.
cp::RequestSpec make_grid(std::size_t width, std::size_t height,
                          const std::vector<std::pair<std::size_t, std::size_t>>& placements) {
  cp::RequestSpec spec;
  set_generations(spec);
  add_domains(spec);
  const std::size_t count = width * height;
  for (std::size_t index = 0; index < count; ++index) {
    add_node(spec, numbered("g", index), "FABRIC");
  }
  const std::uint64_t capacity = 25000;
  const std::uint64_t latency = 100;
  const auto index_of = [width](std::size_t x, std::size_t y) { return y * width + x; };
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const std::size_t here = index_of(x, y);
      const std::size_t neighbours[4] = {index_of((x + 1u) % width, y), index_of((x + width - 1u) % width, y),
                                         index_of(x, (y + 1u) % height), index_of(x, (y + height - 1u) % height)};
      for (std::size_t direction = 0; direction < 4u; ++direction) {
        const std::size_t there = neighbours[direction];
        if (there == here) {
          continue;
        }
        add_edge(spec, numbered("g", here) + "d" + std::to_string(direction), numbered("g", here),
                 numbered("g", there), capacity, latency, "FABRIC");
      }
    }
  }
  add_ring(spec, "grid", placements.size());
  for (std::size_t index = 0; index < placements.size(); ++index) {
    add_binding(spec, numbered("p", index),
                numbered("g", index_of(placements[index].first, placements[index].second)));
  }
  spec.policy.max_hops = 8;
  spec.policy.paths_per_logical_edge = 1;
  return spec;
}

std::string signature_of(const cp::PathPlan& path) {
  std::string out;
  for (const cp::Hop& hop : path.hops) {
    out += hop.edge.value();
    out += ",";
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) { return cpath_test::Registry::instance().run(argc, argv); }

// ---------------------------------------------------------------------------
// Case 1 and 2: a large fabric plans well inside the budget, and doubling it
// does not double-and-then-some the work.
// ---------------------------------------------------------------------------
CPATH_TEST(Scale, large_fabric_and_doubling_stay_inside_the_search_budget) {
  // Observed bounds, documented: on this machine the 64x64 grid plans an
  // eight-participant ring in a few thousand node expansions, which is under
  // 1/16 of nodes * max_hops (32768). The assertion uses a bound four times
  // larger than the observed value so that it still fails loudly on any real
  // complexity regression while tolerating a different tie-breaking order.
  constexpr std::size_t kDocumentedExpansionBound = 8192;
  // Doubling the fabric may not multiply the work by more than this factor.
  // Anything quadratic in the node count would show up as 4x or worse.
  constexpr std::size_t kDocumentedGrowthFactor = 2;

  std::vector<std::pair<std::size_t, std::size_t>> placements;
  for (std::size_t index = 0; index < 8u; ++index) {
    placements.emplace_back(index, 0);
  }

  std::size_t expansions[2] = {0, 0};
  std::size_t nodes[2] = {0, 0};
  std::size_t edges[2] = {0, 0};
  double elapsed[2] = {0.0, 0.0};
  const std::size_t widths[2] = {64, 64};
  const std::size_t heights[2] = {64, 128};

  for (std::size_t variant = 0; variant < 2u; ++variant) {
    const Clock::time_point start = Clock::now();
    cp::RequestSpec spec = make_grid(widths[variant], heights[variant], placements);
    auto request = cp::build_request(std::move(spec));
    CPATH_REQUIRE(request.has_value());
    nodes[variant] = request.value().fabric.node_count();
    edges[variant] = request.value().fabric.edge_count();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    elapsed[variant] = millis_since(start);

    CPATH_CHECK(outcome.ok());
    if (!outcome.ok()) {
      std::printf("SYNTHETIC scale grid %zux%zu: denied: %s\n", widths[variant], heights[variant],
                  cpath_test::describe_outcome(outcome).c_str());
      continue;
    }
    check_plan_basics(*outcome.plan, request.value(), "grid");
    expansions[variant] = outcome.search_expansions;
    std::printf("SYNTHETIC scale grid %zux%zu: nodes=%zu edges=%zu expansions=%zu enumeration=%zu "
                "combinations=%zu global_nodes=%zu optimal=%d wall=%.1f ms\n",
                widths[variant], heights[variant], nodes[variant], edges[variant], expansions[variant],
                outcome.enumeration_steps, outcome.combination_steps, outcome.global_search_nodes,
                outcome.optimal ? 1 : 0, elapsed[variant]);
  }

  CPATH_CHECK(nodes[0] >= 4000u);
  CPATH_CHECK(edges[0] >= 16000u);
  CPATH_CHECK(nodes[1] >= 2u * nodes[0]);
  CPATH_CHECK(edges[1] >= 2u * edges[0]);
  const std::size_t state_space = nodes[0] * 8u;  // nodes * max_hops
  std::printf("SYNTHETIC scale bound: expansions=%zu vs nodes*max_hops=%zu (bound %zu)\n", expansions[0],
              state_space, kDocumentedExpansionBound);
  CPATH_CHECK(expansions[0] <= kDocumentedExpansionBound);
  CPATH_CHECK(expansions[0] * 4u < state_space);

  std::printf("SYNTHETIC scale doubling: expansions %zu -> %zu (ratio %.2f, allowed %zu)\n", expansions[0],
              expansions[1], expansions[0] == 0u ? 0.0
                                                 : static_cast<double>(expansions[1]) /
                                                       static_cast<double>(expansions[0]),
              kDocumentedGrowthFactor);
  CPATH_CHECK(expansions[1] <= expansions[0] * kDocumentedGrowthFactor);
}

// ---------------------------------------------------------------------------
// Case 3: an over-budget request is refused, not run unbounded.
// ---------------------------------------------------------------------------
CPATH_TEST(Scale, exhausted_search_budget_is_refused_precisely) {
  std::vector<std::pair<std::size_t, std::size_t>> placements = {{0, 0}, {0, 16}};

  // Baseline: with the shipped budget the target is reachable but too far away,
  // and the denial names the hop limit.
  {
    cp::RequestSpec spec = make_grid(64, 64, placements);
    spec.collective.id = cp::CollectiveId{"hoplimit"};
    spec.policy.max_hops = 8;
    auto request = cp::build_request(std::move(spec));
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    std::printf("SYNTHETIC scale hop-limit baseline: %s expansions=%zu wall=%.1f ms\n",
                cp::code_symbol(outcome.primary_code()), outcome.search_expansions, elapsed);
    CPATH_CHECK(!outcome.ok());
    CPATH_CHECK(!outcome.plan.has_value());
    CPATH_CHECK_EQ(std::string(cp::code_symbol(outcome.primary_code())), std::string("hop_limit_exceeded"));
    CPATH_CHECK(outcome.search_expansions <= (1u << 18));
  }

  // A deliberately tiny budget must stop the search and be reported as such.
  {
    cp::RequestSpec spec = make_grid(64, 64, placements);
    spec.collective.id = cp::CollectiveId{"budget"};
    spec.policy.max_hops = 8;
    spec.policy.max_search_expansions = 32;
    auto request = cp::build_request(std::move(spec));
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    std::printf("SYNTHETIC scale tiny budget: %s kind=%s expansions=%zu enumeration=%zu wall=%.1f ms\n",
                cp::code_symbol(outcome.primary_code()), cp::to_string(outcome.primary_kind()),
                outcome.search_expansions, outcome.enumeration_steps, elapsed);
    CPATH_CHECK(!outcome.ok());
    CPATH_CHECK(!outcome.plan.has_value());
    CPATH_CHECK(!outcome.denials.empty());
    // A boundary result must be one of exactly two things: a proof that no
    // mapping exists, or an explicit statement that the search stopped before
    // deciding. It must never be an unproven infeasibility claim.
    CPATH_CHECK(outcome.indeterminate() || outcome.proven_infeasible());
    CPATH_CHECK(outcome.indeterminate() ==
                (outcome.primary_code() == cp::ErrorCode::kSearchBudgetExceeded));
    CPATH_CHECK(outcome.search_expansions <= 32u);
  }

  // A starved ENUMERATION budget is not a failure: the heuristic pool still
  // produces a valid plan, and the outcome records that optimality is unproven.
  {
    // Four grid steps apart, so the policy's hop limit is comfortably met and
    // the request is genuinely satisfiable.
    std::vector<std::pair<std::size_t, std::size_t>> near = {{0, 0}, {0, 4}};
    cp::RequestSpec spec = make_grid(64, 64, near);
    spec.collective.id = cp::CollectiveId{"starved-enumeration"};
    spec.policy.max_hops = 8;
    auto request = cp::build_request(std::move(spec));
    CPATH_REQUIRE(request.has_value());
    cp::PlannerLimits limits;
    limits.max_enumeration_steps = 1;
    limits.max_total_enumeration_steps = 1;
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value(), limits);
    std::printf("SYNTHETIC scale starved enumeration: ok=%d optimal=%d paths=%zu enumeration=%zu\n",
                outcome.ok() ? 1 : 0, outcome.optimal ? 1 : 0,
                outcome.ok() ? outcome.plan->stats.path_count : 0u, outcome.enumeration_steps);
    CPATH_CHECK(outcome.ok());
    if (outcome.ok()) {
      CPATH_CHECK_EQ(cp::validate_plan(*outcome.plan, request.value()).is_ok(), true);
      CPATH_CHECK(!outcome.optimal);
    }
  }
}

// ---------------------------------------------------------------------------
// Case 4: huge costs saturate at kCostCeiling instead of wrapping.
// ---------------------------------------------------------------------------
CPATH_TEST(Scale, maximum_hop_costs_saturate_at_the_ceiling) {
  // SYNTHETIC forward-only chain c0 -> ... -> c1000. kMaxHops is 256, so the
  // longest admissible path is 256 hops: a 1000-hop candidate chain cannot be
  // planned at all (proved below), and the saturation proof therefore uses 17
  // maximum-length paths, which is where the saturating total actually bites.
  constexpr std::size_t kChainNodes = 1001;
  const std::size_t expected_hops = cp::kMaxHops;

  cp::RequestSpec chain;
  set_generations(chain);
  add_domains(chain);
  for (std::size_t index = 0; index < kChainNodes; ++index) {
    add_node(chain, numbered("c", index), "FABRIC");
  }
  for (std::size_t index = 0; index + 1u < kChainNodes; ++index) {
    add_edge(chain, numbered("c", index) + "n", numbered("c", index), numbered("c", index + 1u),
             cp::kMaxCapacityMbps, cp::kMaxLatencyMicros, "FABRIC");
  }

  // A 1000-hop candidate chain is inadmissible: the planner must refuse it and
  // never return a plan whose hops exceed the policy.
  {
    cp::RequestSpec far = chain;
    far.collective.id = cp::CollectiveId{"chain-far"};
    far.collective.kind = cp::CollectiveKind::kRing;
    cp::GroupSpec group;
    group.id = cp::GroupId{"g0"};
    group.pattern = cp::GroupPattern::kRing;
    group.members = {cp::ParticipantId{"p0"}, cp::ParticipantId{"p1"}};
    far.collective.groups.push_back(std::move(group));
    add_binding(far, "p0", "c0");
    add_binding(far, "p1", numbered("c", kChainNodes - 1u));
    far.policy.max_hops = cp::kMaxHops;
    far.policy.latency_weight_milli = cp::kMaxScoringWeightMilli;
    far.policy.max_path_cost = cp::kMaxPathCostLimit;
    auto request = cp::build_request(std::move(far));
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    std::printf("SYNTHETIC scale 1000-hop chain: %s expansions=%zu wall=%.1f ms\n",
                cp::code_symbol(outcome.primary_code()), outcome.search_expansions, elapsed);
    CPATH_CHECK(!outcome.ok());
    CPATH_CHECK(!outcome.plan.has_value());
    CPATH_CHECK(!outcome.denials.empty());
  }

  // Seventeen maximum-length paths. Per hop the cost is
  // (kMaxLatencyMicros * kMaxScoringWeightMilli) / 1000, which does not itself
  // saturate, so the overflow can only be caught by the accumulated total.
  // These four are facts about the model constants, not about a run, so they are
  // compile-time obligations: a build failure is the strongest possible report.
  constexpr std::uint64_t per_hop = (cp::kMaxLatencyMicros * cp::kMaxScoringWeightMilli) / 1000u;
  constexpr std::uint64_t per_path = per_hop * static_cast<std::uint64_t>(expected_hops);
  static_assert(per_hop > 0u, "a maximum-latency hop at the maximum weight must cost something");
  static_assert(per_hop <= cp::kCostCeiling, "one hop may not exceed the cost ceiling on its own");
  static_assert(per_path <= cp::kCostCeiling, "one maximum-length path must stay inside the ceiling");
  static_assert(17u * per_path > cp::kCostCeiling,
                "seventeen maximum-length paths must overflow the ceiling, or the saturation proof is vacuous");

  constexpr std::size_t kLeaves = 17;
  {
    cp::RequestSpec tree = chain;
    tree.collective.id = cp::CollectiveId{"chain-saturate"};
    tree.collective.kind = cp::CollectiveKind::kTree;
    cp::GroupSpec group;
    group.id = cp::GroupId{"g0"};
    group.pattern = cp::GroupPattern::kTree;
    group.direction = cp::TreeDirection::kForward;
    group.root = cp::ParticipantId{"p0"};
    group.members.push_back(cp::ParticipantId{"p0"});
    for (std::size_t index = 0; index < kLeaves; ++index) {
      group.members.push_back(cp::ParticipantId{numbered("q", index)});
    }
    tree.collective.groups.push_back(std::move(group));
    add_binding(tree, "p0", "c0");
    for (std::size_t index = 0; index < kLeaves; ++index) {
      add_binding(tree, numbered("q", index), numbered("c", expected_hops));
    }
    tree.policy.max_hops = expected_hops;
    tree.policy.paths_per_logical_edge = 1;
    tree.policy.latency_weight_milli = cp::kMaxScoringWeightMilli;
    tree.policy.congestion_weight_milli = 0;
    tree.policy.max_path_cost = cp::kMaxPathCostLimit;
    // This request deliberately uses maximum-length paths, and the planner's
    // alternative search re-searches the fabric once per edge of the best path,
    // so the work grows quadratically in the hop limit (measured: about 33k
    // expansions per logical edge for a 256-hop path). The budget is therefore
    // raised to the library's documented hard ceiling kMaxSearchExpansions
    // (16.7M); the default 262144 is what the large-fabric case above proves.
    tree.policy.max_search_expansions = cp::kMaxSearchExpansions;

    auto request = cp::build_request(std::move(tree));
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    if (!outcome.ok()) {
      CPATH_FAIL("the saturation request was refused: " + cpath_test::describe_outcome(outcome));
    }
    check_plan_basics(*outcome.plan, request.value(), "saturate");
    std::printf("SYNTHETIC scale saturation: paths=%zu hops/path=%zu per-path=%llu total=%llu ceiling=%llu "
                "expansions=%zu wall=%.1f ms\n",
                outcome.plan->stats.path_count, outcome.plan->paths.front().hops.size(),
                static_cast<unsigned long long>(per_path),
                static_cast<unsigned long long>(outcome.plan->stats.total_cost),
                static_cast<unsigned long long>(cp::kCostCeiling), outcome.search_expansions, elapsed);
    CPATH_CHECK_EQ(outcome.plan->stats.path_count, kLeaves);
    for (const cp::PathPlan& path : outcome.plan->paths) {
      CPATH_CHECK_EQ(path.hops.size(), expected_hops);
      CPATH_CHECK_EQ(path.cost, per_path);
    }
    CPATH_CHECK_EQ(outcome.plan->stats.max_path_cost, per_path);
    CPATH_CHECK_EQ(outcome.plan->stats.total_cost, cp::kCostCeiling);
  }
}

// ---------------------------------------------------------------------------
// Case 5: a fan-out fabric.
// ---------------------------------------------------------------------------
CPATH_TEST(Scale, fan_out_fabric_serves_one_and_four_disjoint_paths) {
  constexpr std::size_t kFanOut = 1000;

  cp::RequestSpec spec;
  set_generations(spec);
  add_domains(spec);
  add_node(spec, "source", "LEAF");
  add_node(spec, "spine", "SPINE");
  add_node(spec, "sink", "LEAF");
  for (std::size_t index = 0; index < kFanOut; ++index) {
    add_edge(spec, numbered("sh", index), "source", "spine", 25000, 100, "LEAF");
    add_edge(spec, numbered("ht", index), "spine", "sink", 25000, 100, "LEAF");
    add_edge(spec, numbered("th", index), "sink", "spine", 25000, 100, "LEAF");
    add_edge(spec, numbered("hs", index), "spine", "source", 25000, 100, "LEAF");
  }
  add_ring(spec, "fanout", 2);
  add_binding(spec, "p0", "source");
  add_binding(spec, "p1", "sink");
  spec.policy.max_hops = 4;
  spec.policy.paths_per_logical_edge = 1;

  {
    auto request = cp::build_request(spec);
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    CPATH_REQUIRE(outcome.ok());
    check_plan_basics(*outcome.plan, request.value(), "fanout-single");
    std::printf("SYNTHETIC scale fan-out %zu parallel edges: 1 path per logical edge, expansions=%zu "
                "wall=%.1f ms\n",
                kFanOut, outcome.search_expansions, elapsed);
    CPATH_CHECK(outcome.search_expansions <= 16u);
    CPATH_CHECK_EQ(outcome.plan->stats.path_count, 2u);
    CPATH_CHECK_EQ(outcome.plan->stats.hop_count, 4u);
    for (const cp::PathPlan& path : outcome.plan->paths) {
      CPATH_CHECK_EQ(path.hops.size(), 2u);
      CPATH_CHECK_EQ(path.cost, 200u);
    }
  }

  {
    cp::RequestSpec disjoint = spec;
    disjoint.collective.id = cp::CollectiveId{"fanout-disjoint"};
    disjoint.policy.paths_per_logical_edge = 4;
    disjoint.policy.disjointness = cp::Disjointness::kEdge;
    auto request = cp::build_request(std::move(disjoint));
    CPATH_REQUIRE(request.has_value());
    const Clock::time_point start = Clock::now();
    const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
    const double elapsed = millis_since(start);
    CPATH_REQUIRE(outcome.ok());
    check_plan_basics(*outcome.plan, request.value(), "fanout-disjoint");
    std::printf("SYNTHETIC scale fan-out %zu parallel edges: 4 edge-disjoint paths, paths=%zu expansions=%zu "
                "wall=%.1f ms\n",
                kFanOut, outcome.plan->stats.path_count, outcome.search_expansions, elapsed);
    CPATH_CHECK_EQ(outcome.plan->stats.path_count, 8u);  // 2 logical edges x 4 paths
    std::vector<std::vector<const cp::PathPlan*>> siblings(request.value().collective.logical_edges.size() + 1u);
    for (const cp::PathPlan& path : outcome.plan->paths) {
      siblings[path.logical_edge.value()].push_back(&path);
      CPATH_CHECK_EQ(path.hops.size(), 2u);
    }
    for (std::size_t index = 1; index < siblings.size(); ++index) {
      CPATH_CHECK_EQ(siblings[index].size(), 4u);
      for (std::size_t i = 0; i < siblings[index].size(); ++i) {
        for (std::size_t j = i + 1u; j < siblings[index].size(); ++j) {
          for (const cp::Hop& left : siblings[index][i]->hops) {
            for (const cp::Hop& right : siblings[index][j]->hops) {
              CPATH_CHECK(!(left.edge == right.edge));
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Case 6: all-to-all over 40 participants.
// ---------------------------------------------------------------------------
CPATH_TEST(Scale, all_to_all_over_forty_participants_reports_every_logical_edge) {
  constexpr std::size_t kLeaves = 40;
  constexpr std::size_t kSpines = 4;
  constexpr std::size_t kExpectedLogicalEdges = kLeaves * (kLeaves - 1u);

  cp::RequestSpec spec;
  set_generations(spec);
  add_domains(spec);
  for (std::size_t index = 0; index < kLeaves; ++index) {
    add_node(spec, numbered("leaf", index), "LEAF");
  }
  for (std::size_t index = 0; index < kSpines; ++index) {
    add_node(spec, numbered("spine", index), "SPINE");
  }
  for (std::size_t leaf = 0; leaf < kLeaves; ++leaf) {
    for (std::size_t spine = 0; spine < kSpines; ++spine) {
      add_edge(spec, numbered("leaf", leaf) + "z" + std::to_string(spine), numbered("leaf", leaf),
               numbered("spine", spine), 25000, 100, "LEAF");
      add_edge(spec, numbered("spine", spine) + "z" + std::to_string(leaf), numbered("spine", spine),
               numbered("leaf", leaf), 25000, 100, "LEAF");
    }
  }
  cp::GroupSpec group;
  group.id = cp::GroupId{"g0"};
  group.pattern = cp::GroupPattern::kAllToAll;
  group.level = 0;
  for (std::size_t index = 0; index < kLeaves; ++index) {
    group.members.push_back(cp::ParticipantId{numbered("p", index)});
  }
  spec.collective.id = cp::CollectiveId{"a2a40"};
  spec.collective.kind = cp::CollectiveKind::kAllToAll;
  spec.collective.groups.push_back(std::move(group));
  for (std::size_t index = 0; index < kLeaves; ++index) {
    add_binding(spec, numbered("p", index), numbered("leaf", index));
  }
  spec.policy.max_hops = 8;
  spec.policy.paths_per_logical_edge = 1;

  auto request = cp::build_request(std::move(spec));
  CPATH_REQUIRE(request.has_value());
  CPATH_REQUIRE_EQ(request.value().collective.logical_edges.size(), kExpectedLogicalEdges);
  const Clock::time_point start = Clock::now();
  const cp::PlanningOutcome outcome = cp::plan_collective(request.value());
  const double elapsed = millis_since(start);
  CPATH_REQUIRE(outcome.ok());
  check_plan_basics(*outcome.plan, request.value(), "alltoall");
  std::printf("SYNTHETIC scale all-to-all: participants=%zu nodes=%zu edges=%zu logical_edges=%zu paths=%zu "
              "hops=%zu expansions=%zu enumeration=%zu combinations=%zu global_nodes=%zu optimal=%d "
              "wall=%.1f ms\n",
              kLeaves, request.value().fabric.node_count(), request.value().fabric.edge_count(),
              outcome.plan->stats.logical_edge_count, outcome.plan->stats.path_count,
              outcome.plan->stats.hop_count, outcome.search_expansions, outcome.enumeration_steps,
              outcome.combination_steps, outcome.global_search_nodes, outcome.optimal ? 1 : 0, elapsed);

  CPATH_CHECK_EQ(outcome.plan->stats.logical_edge_count, kExpectedLogicalEdges);
  CPATH_CHECK_EQ(outcome.plan->stats.path_count, kExpectedLogicalEdges);
  CPATH_CHECK_EQ(outcome.plan->stats.hop_count, 2u * kExpectedLogicalEdges);
  // Every ordered pair of distinct participants appears exactly once, and every
  // path is the two-hop leaf/spine/leaf route.
  std::vector<std::uint8_t> seen(kLeaves * kLeaves, 0u);
  for (const cp::PathPlan& path : outcome.plan->paths) {
    CPATH_CHECK_EQ(path.hops.size(), 2u);
    CPATH_CHECK_EQ(path.cost, 200u);
    const std::size_t source = static_cast<std::size_t>(std::stoul(path.src.value().substr(1)));
    const std::size_t target = static_cast<std::size_t>(std::stoul(path.dst.value().substr(1)));
    CPATH_CHECK(source < kLeaves && target < kLeaves && source != target);
    if (source < kLeaves && target < kLeaves) {
      CPATH_CHECK_EQ(seen[source * kLeaves + target], 0u);
      seen[source * kLeaves + target] = 1u;
    }
    const cp::Edge* first = request.value().fabric.find_edge(path.hops[0].edge);
    const cp::Edge* second = request.value().fabric.find_edge(path.hops[1].edge);
    CPATH_CHECK(first != nullptr && second != nullptr);
    if (first != nullptr && second != nullptr) {
      CPATH_CHECK(first->from == request.value().find_binding(path.src)->node);
      CPATH_CHECK(second->to == request.value().find_binding(path.dst)->node);
      CPATH_CHECK(first->to == second->from);
    }
  }
  std::size_t covered = 0;
  for (const std::uint8_t flag : seen) {
    covered += flag != 0u ? 1u : 0u;
  }
  CPATH_CHECK_EQ(covered, kExpectedLogicalEdges);
}
