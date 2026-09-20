// Collective Path Planner - planning request model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_REQUEST_HPP
#define CPATH_REQUEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "cpath/collective.hpp"
#include "cpath/digest.hpp"
#include "cpath/fabric.hpp"
#include "cpath/ids.hpp"
#include "cpath/limits.hpp"
#include "cpath/policy.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Binding of one logical participant to one physical endpoint node, with the
// per-participant constraints that override the global policy.
struct EndpointBinding {
  ParticipantId participant{};
  NodeId node{};
  // When non-empty, every hop of every path carrying this participant must stay
  // inside one of these failure domains (the hop domain must be the allowed
  // domain or be contained by it).
  std::vector<FailureDomainId> allowed_failure_domains{};
  // When non-empty, every hop of every path carrying this participant must use
  // one of these tiers.
  std::vector<TierId> allowed_tiers{};

  friend bool operator==(const EndpointBinding&, const EndpointBinding&) = default;
};

// Generation fence bound into a plan. Every field is a hard identity: a
// mismatch invalidates the authority of anything derived from it.
struct GenerationBindings {
  TopologyGeneration topology{};
  FailureDomainGeneration failure_domains{};
  CapacityEvidenceGeneration capacity_evidence{};
  PolicyGeneration policy{};
  CollectivePlanGeneration plan{};

  friend bool operator==(const GenerationBindings&, const GenerationBindings&) = default;
};

// Unvalidated planning request.
struct RequestSpec {
  CollectiveSpec collective{};
  FabricSpec fabric{};
  Policy policy{};
  CollectivePlanGeneration plan_generation{};
  std::vector<EndpointBinding> bindings{};
};

// Validated, canonicalised planning request. The canonical digest covers every
// input that can change the mapping: collective structure, fabric topology,
// capacity evidence, policy and endpoint bindings. It deliberately excludes the
// plan generation, which is a caller-supplied sequencing label rather than an
// input to the mapping; the plan binds it separately.
struct PlanningRequest {
  Collective collective{};
  FabricGraph fabric{};
  Policy policy{};
  CollectivePlanGeneration plan_generation{};
  std::vector<EndpointBinding> bindings{};
  Digest canonical_digest{};
  std::size_t canonical_bytes{0};

  // Current generation bindings, as supplied by this request.
  struct GenerationBindings current_bindings() const noexcept;

  const EndpointBinding* find_binding(const ParticipantId& participant) const noexcept;

  Status validate() const;
  void encode_canonical(ByteWriter& writer) const;
};

Result<PlanningRequest> build_request(const RequestSpec& spec);
Result<PlanningRequest> build_request(RequestSpec&& spec);

// Canonical text rendering of a request, suitable for fingerprints in audit
// output. Round-trips through the DSL reader.
std::string encode_request_text(const PlanningRequest& request);

}  // namespace cpath

#endif  // CPATH_REQUEST_HPP
