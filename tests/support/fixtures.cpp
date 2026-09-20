// Collective Path Planner - shared synthetic fixtures for the test suites.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "fixtures.hpp"

#include <string>

namespace cpath_test {
namespace {

std::string node_name(std::size_t index) { return "n" + std::to_string(index); }
std::string participant_name(std::size_t index) { return "p" + std::to_string(index); }
std::string spine_name(std::size_t index) { return "s" + std::to_string(index); }
std::string rack_name(std::size_t index) { return "R" + std::to_string(index); }
std::string host_name(std::size_t index) { return "H" + std::to_string(index); }

cpath::Edge make_edge(const std::string& id, const std::string& from, const std::string& to,
                      const SyntheticParams& params, cpath_test::Random& rng, std::uint64_t capacity,
                      std::uint64_t latency, const std::string& tier, const std::string& domain,
                      cpath::EvidenceClass evidence) {
  cpath::Edge edge;
  edge.id = cpath::EdgeId{id};
  edge.from = cpath::NodeId{from};
  edge.to = cpath::NodeId{to};
  edge.capacity_mbps = capacity;
  edge.latency_micros = latency;
  edge.tier = cpath::TierId{tier};
  edge.failure_domain = cpath::FailureDomainId{domain};
  edge.evidence = evidence;
  if (params.zero_capacity_percent > 0 && rng.chance(params.zero_capacity_percent, 100u)) {
    edge.capacity_mbps = 0;
  }
  if (params.unknown_evidence_percent > 0 && rng.chance(params.unknown_evidence_percent, 100u)) {
    edge.evidence = cpath::EvidenceClass::kUnknown;
  }
  if (params.huge_latency_percent > 0 && rng.chance(params.huge_latency_percent, 100u)) {
    edge.latency_micros = cpath::kMaxLatencyMicros;
  }
  if (params.ineligible_edge_percent > 0 && rng.chance(params.ineligible_edge_percent, 100u)) {
    edge.eligible = false;
  }
  return edge;
}

// Builds the shared fabric used by every synthetic collective in this file.
void append_fabric(cpath::RequestSpec& spec, const SyntheticParams& params, cpath_test::Random& rng) {
  spec.fabric.topology_generation = cpath::TopologyGeneration{params.topology_generation};
  spec.fabric.failure_domain_generation = cpath::FailureDomainGeneration{params.failure_domain_generation};
  spec.fabric.capacity_evidence_generation = cpath::CapacityEvidenceGeneration{params.capacity_evidence_generation};
  spec.policy.generation = cpath::PolicyGeneration{params.policy_generation};
  spec.plan_generation = cpath::CollectivePlanGeneration{params.plan_generation};

  cpath::FailureDomain site;
  site.id = cpath::FailureDomainId{"S0"};
  site.kind = cpath::DomainKind::kSite;
  spec.fabric.failure_domains.push_back(site);

  for (std::size_t rack = 0; rack < params.rack_count; ++rack) {
    cpath::FailureDomain record;
    record.id = cpath::FailureDomainId{rack_name(rack)};
    record.parent = cpath::FailureDomainId{"S0"};
    record.kind = cpath::DomainKind::kRack;
    spec.fabric.failure_domains.push_back(record);
  }
  cpath::FailureDomain spine_domain;
  spine_domain.id = cpath::FailureDomainId{"CORE"};
  spine_domain.parent = cpath::FailureDomainId{"S0"};
  spine_domain.kind = cpath::DomainKind::kPod;
  spec.fabric.failure_domains.push_back(spine_domain);

  for (std::size_t index = 0; index < params.participants; ++index) {
    cpath::FailureDomain host;
    host.id = cpath::FailureDomainId{host_name(index)};
    host.parent = cpath::FailureDomainId{rack_name(index % params.rack_count)};
    host.kind = cpath::DomainKind::kHost;
    spec.fabric.failure_domains.push_back(host);

    cpath::Node node;
    node.id = cpath::NodeId{node_name(index)};
    node.placement.supplied = true;
    node.placement.site = "S0";
    node.placement.pod = "P0";
    node.placement.rack = rack_name(index % params.rack_count);
    node.placement.host = host_name(index);
    node.placement.device_index = 0;
    node.tier = cpath::TierId{"LEAF"};
    spec.fabric.nodes.push_back(node);
  }
  for (std::size_t index = 0; index < params.spine_count; ++index) {
    cpath::Node node;
    node.id = cpath::NodeId{spine_name(index)};
    node.placement.supplied = true;
    node.placement.site = "S0";
    node.placement.pod = "CORE";
    node.placement.rack = "CORE";
    node.placement.host = spine_name(index);
    node.placement.device_index = 0;
    node.tier = cpath::TierId{"SPINE"};
    spec.fabric.nodes.push_back(node);
  }

  std::size_t counter = 0;
  const auto next_id = [&counter]() { return "e" + std::to_string(counter++); };

  for (std::size_t leaf = 0; leaf < params.participants; ++leaf) {
    for (std::size_t spine = 0; spine < params.spine_count; ++spine) {
      const std::string id = next_id();
      spec.fabric.edges.push_back(make_edge(id, node_name(leaf), spine_name(spine), params, rng,
                                            params.capacity_mbps, params.latency_micros, "LEAF",
                                            host_name(leaf), cpath::EvidenceClass::kSynthetic));
      if (params.asymmetric_percent == 0 || !rng.chance(params.asymmetric_percent, 100u)) {
        spec.fabric.edges.push_back(make_edge(id + "r", spine_name(spine), node_name(leaf), params, rng,
                                              params.capacity_mbps, params.latency_micros, "LEAF",
                                              host_name(leaf), cpath::EvidenceClass::kSynthetic));
      }
      if (params.duplicate_edge_percent > 0 && rng.chance(params.duplicate_edge_percent, 100u)) {
        // A second physical edge with the same endpoints but a different id.
        spec.fabric.edges.push_back(make_edge(id + "d", node_name(leaf), spine_name(spine), params, rng,
                                              params.capacity_mbps, params.latency_micros, "LEAF",
                                              host_name(leaf), cpath::EvidenceClass::kSynthetic));
      }
    }
  }
  for (std::size_t from = 0; from < params.spine_count; ++from) {
    for (std::size_t to = 0; to < params.spine_count; ++to) {
      if (from == to) {
        continue;
      }
      spec.fabric.edges.push_back(make_edge(next_id(), spine_name(from), spine_name(to), params, rng,
                                            params.capacity_mbps * 4u, params.latency_micros * 2u, "SPINE",
                                            "CORE", cpath::EvidenceClass::kSynthetic));
    }
  }
}

void append_bindings(cpath::RequestSpec& spec, const SyntheticParams& params) {
  for (std::size_t index = 0; index < params.participants; ++index) {
    cpath::EndpointBinding binding;
    binding.participant = cpath::ParticipantId{participant_name(index)};
    binding.node = cpath::NodeId{node_name(index)};
    spec.bindings.push_back(std::move(binding));
  }
}

}  // namespace

cpath::RequestSpec make_ring_spec(const SyntheticParams& params) {
  cpath::RequestSpec spec;
  cpath_test::Random rng(params.seed);
  append_fabric(spec, params, rng);
  append_bindings(spec, params);

  spec.collective.id = cpath::CollectiveId{"ring0"};
  spec.collective.kind = cpath::CollectiveKind::kRing;
  spec.collective.demand_mbps = params.demand_mbps;
  cpath::GroupSpec group;
  group.id = cpath::GroupId{"g0"};
  group.level = 0;
  group.pattern = cpath::GroupPattern::kRing;
  for (std::size_t index = 0; index < params.participants; ++index) {
    group.members.push_back(cpath::ParticipantId{participant_name(index)});
  }
  spec.collective.groups.push_back(std::move(group));
  return spec;
}

cpath::RequestSpec make_alltoall_spec(const SyntheticParams& params) {
  cpath::RequestSpec spec;
  cpath_test::Random rng(params.seed + 1u);
  append_fabric(spec, params, rng);
  append_bindings(spec, params);

  spec.collective.id = cpath::CollectiveId{"a2a0"};
  spec.collective.kind = cpath::CollectiveKind::kAllToAll;
  spec.collective.demand_mbps = params.demand_mbps;
  cpath::GroupSpec group;
  group.id = cpath::GroupId{"g0"};
  group.level = 0;
  group.pattern = cpath::GroupPattern::kAllToAll;
  for (std::size_t index = 0; index < params.participants; ++index) {
    group.members.push_back(cpath::ParticipantId{participant_name(index)});
  }
  spec.collective.groups.push_back(std::move(group));
  return spec;
}

cpath::RequestSpec make_pairwise_spec(const SyntheticParams& params) {
  cpath::RequestSpec spec;
  cpath_test::Random rng(params.seed + 2u);
  append_fabric(spec, params, rng);
  append_bindings(spec, params);

  spec.collective.id = cpath::CollectiveId{"pair0"};
  spec.collective.kind = cpath::CollectiveKind::kPairwise;
  spec.collective.demand_mbps = params.demand_mbps;
  cpath::GroupSpec group;
  group.id = cpath::GroupId{"g0"};
  group.level = 0;
  group.pattern = cpath::GroupPattern::kPairwise;
  const std::size_t usable = params.participants - (params.participants % 2u);
  for (std::size_t index = 0; index < usable; ++index) {
    group.members.push_back(cpath::ParticipantId{participant_name(index)});
  }
  spec.collective.groups.push_back(std::move(group));
  return spec;
}

cpath::RequestSpec make_hierarchical_spec(const SyntheticParams& params) {
  cpath::RequestSpec spec;
  cpath_test::Random rng(params.seed + 3u);
  append_fabric(spec, params, rng);
  append_bindings(spec, params);

  spec.collective.id = cpath::CollectiveId{"hier0"};
  spec.collective.kind = cpath::CollectiveKind::kHierarchical;
  spec.collective.demand_mbps = params.demand_mbps;

  // Level 0: a ring inside each rack. Level 1: a ring across the rack leaders.
  for (std::size_t rack = 0; rack < params.rack_count; ++rack) {
    cpath::GroupSpec group;
    group.id = cpath::GroupId{"r" + std::to_string(rack)};
    group.level = 0;
    group.pattern = cpath::GroupPattern::kRing;
    for (std::size_t index = rack; index < params.participants; index += params.rack_count) {
      group.members.push_back(cpath::ParticipantId{participant_name(index)});
    }
    if (group.members.size() >= 2) {
      spec.collective.groups.push_back(std::move(group));
    }
  }
  cpath::GroupSpec top;
  top.id = cpath::GroupId{"top"};
  top.level = 1;
  top.pattern = cpath::GroupPattern::kRing;
  for (std::size_t rack = 0; rack < params.rack_count; ++rack) {
    if (rack < params.participants) {
      top.members.push_back(cpath::ParticipantId{participant_name(rack)});
    }
  }
  if (top.members.size() >= 2) {
    spec.collective.groups.push_back(std::move(top));
  }
  return spec;
}

cpath::PlanningRequest build_or_fail(const cpath::RequestSpec& spec, const char* context) {
  auto request = cpath::build_request(spec);
  if (!request.has_value()) {
    CPATH_FAIL(std::string(context) + ": build_request failed: " + request.status().to_string());
  }
  return std::move(request.value());
}

cpath::PlanningRequest build_or_fail(cpath::RequestSpec&& spec, const char* context) {
  auto request = cpath::build_request(std::move(spec));
  if (!request.has_value()) {
    CPATH_FAIL(std::string(context) + ": build_request failed: " + request.status().to_string());
  }
  return std::move(request.value());
}

std::string minimal_ring_text() {
  return std::string(
      "cpath-request 1\n"
      "plan-generation 1\n"
      "topology-generation 7\n"
      "failure-domain-generation 3\n"
      "capacity-evidence-generation 11\n"
      "policy-generation 5\n"
      "\n"
      "[collective]\n"
      "id=c0 kind=ring\n"
      "\n"
      "[groups]\n"
      "g0 level=0 pattern=ring members=p0,p1\n"
      "\n"
      "[domains]\n"
      "S0 kind=site\n"
      "R0 kind=rack parent=S0\n"
      "\n"
      "[nodes]\n"
      "n0 site=S0 rack=R0 host=H0 device=0 tier=LEAF\n"
      "n1 site=S0 rack=R0 host=H1 device=0 tier=LEAF\n"
      "\n"
      "[edges]\n"
      "e0 from=n0 to=n1 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "e1 from=n1 to=n0 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "\n"
      "[bindings]\n"
      "p0 node=n0\n"
      "p1 node=n1\n"
      "\n"
      "[policy]\n"
      "max-hops=4\n"
      "paths-per-logical-edge=1\n"
      "disjointness=none\n");
}

std::string four_node_ring_text() {
  return std::string(
      "cpath-request 1\n"
      "plan-generation 2\n"
      "topology-generation 7\n"
      "failure-domain-generation 3\n"
      "capacity-evidence-generation 11\n"
      "policy-generation 5\n"
      "\n"
      "[collective]\n"
      "id=c0 kind=ring demand=1000\n"
      "\n"
      "[groups]\n"
      "g0 level=0 pattern=ring members=p0,p1,p2,p3\n"
      "\n"
      "[domains]\n"
      "S0 kind=site\n"
      "R0 kind=rack parent=S0\n"
      "R1 kind=rack parent=S0\n"
      "\n"
      "[nodes]\n"
      "n0 site=S0 rack=R0 host=H0 device=0 tier=LEAF\n"
      "n1 site=S0 rack=R1 host=H1 device=0 tier=LEAF\n"
      "n2 site=S0 rack=R0 host=H2 device=0 tier=LEAF\n"
      "n3 site=S0 rack=R1 host=H3 device=0 tier=LEAF\n"
      "s0 site=S0 pod=P0 rack=CORE host=s0 device=0 tier=SPINE\n"
      "s1 site=S0 pod=P1 rack=CORE host=s1 device=0 tier=SPINE\n"
      "\n"
      "[edges]\n"
      "l0 from=n0 to=s0 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "l1 from=s0 to=n0 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "l2 from=n1 to=s0 capacity=25000 latency=100 tier=LEAF domain=R1 evidence=synthetic\n"
      "l3 from=s0 to=n1 capacity=25000 latency=100 tier=LEAF domain=R1 evidence=synthetic\n"
      "l4 from=n2 to=s0 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "l5 from=s0 to=n2 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic\n"
      "l6 from=n3 to=s0 capacity=25000 latency=100 tier=LEAF domain=R1 evidence=synthetic\n"
      "l7 from=s0 to=n3 capacity=25000 latency=100 tier=LEAF domain=R1 evidence=synthetic\n"
      "l8 from=n0 to=s1 capacity=25000 latency=180 tier=LEAF domain=R0 evidence=synthetic\n"
      "l9 from=s1 to=n0 capacity=25000 latency=180 tier=LEAF domain=R0 evidence=synthetic\n"
      "l10 from=n1 to=s1 capacity=25000 latency=180 tier=LEAF domain=R1 evidence=synthetic\n"
      "l11 from=s1 to=n1 capacity=25000 latency=180 tier=LEAF domain=R1 evidence=synthetic\n"
      "l12 from=n2 to=s1 capacity=25000 latency=180 tier=LEAF domain=R0 evidence=synthetic\n"
      "l13 from=s1 to=n2 capacity=25000 latency=180 tier=LEAF domain=R0 evidence=synthetic\n"
      "l14 from=n3 to=s1 capacity=25000 latency=180 tier=LEAF domain=R1 evidence=synthetic\n"
      "l15 from=s1 to=n3 capacity=25000 latency=180 tier=LEAF domain=R1 evidence=synthetic\n"
      "\n"
      "[bindings]\n"
      "p0 node=n0\n"
      "p1 node=n1\n"
      "p2 node=n2\n"
      "p3 node=n3\n"
      "\n"
      "[policy]\n"
      "max-hops=6\n"
      "paths-per-logical-edge=2\n"
      "disjointness=edge\n"
      "domain-diversity=required\n"
      "domain-diversity-level=rack\n");
}

std::string describe_outcome(const cpath::PlanningOutcome& outcome) {
  std::string out = outcome.ok() ? "ok" : "denied";
  for (const cpath::DenialDetail& denial : outcome.denials) {
    out += " [";
    out += cpath::code_symbol(denial.code);
    out += "/";
    out += cpath::to_string(denial.conflict);
    out += ": ";
    out += denial.message;
    out += "]";
  }
  return out;
}

bool plan_uses_ineligible_edge(const cpath::Plan& plan, const cpath::FabricGraph& fabric) {
  for (const cpath::PathPlan& path : plan.paths) {
    for (const cpath::Hop& hop : path.hops) {
      const cpath::Edge* edge = fabric.find_edge(hop.edge);
      if (edge == nullptr || !edge->eligible) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace cpath_test
