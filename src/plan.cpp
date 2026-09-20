// Collective Path Planner - plan model, freshness and invariant checks.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/plan.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpath/planner.hpp"

namespace cpath {
namespace {

void encode_bindings(const GenerationBindings& bindings, ByteWriter& writer) {
  writer.u64(bindings.topology.value());
  writer.u64(bindings.failure_domains.value());
  writer.u64(bindings.capacity_evidence.value());
  writer.u64(bindings.policy.value());
  writer.u64(bindings.plan.value());
}

Status decode_bindings(ByteReader& reader, GenerationBindings& out) {
  std::uint64_t topology = 0;
  std::uint64_t failure_domains = 0;
  std::uint64_t capacity_evidence = 0;
  std::uint64_t policy = 0;
  std::uint64_t plan = 0;
  reader.u64(topology);
  reader.u64(failure_domains);
  reader.u64(capacity_evidence);
  reader.u64(policy);
  reader.u64(plan);
  if (Status status = reader.status(); !status.is_ok()) {
    return status;
  }
  out.topology = TopologyGeneration{topology};
  out.failure_domains = FailureDomainGeneration{failure_domains};
  out.capacity_evidence = CapacityEvidenceGeneration{capacity_evidence};
  out.policy = PolicyGeneration{policy};
  out.plan = CollectivePlanGeneration{plan};
  return Status::ok();
}

bool is_forbidden_tier(const Policy& policy, const TierId& tier) {
  return tier.valid() && std::binary_search(policy.forbidden_tiers.begin(), policy.forbidden_tiers.end(), tier);
}

bool is_forbidden_node(const Policy& policy, const NodeId& node) {
  return std::binary_search(policy.forbidden_nodes.begin(), policy.forbidden_nodes.end(), node);
}

bool is_forbidden_domain(const FabricGraph& fabric, const Policy& policy, const FailureDomainId& domain) {
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

bool evidence_is_sufficient(EvidenceClass observed, EvidenceRequirement required) {
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

bool binding_permits(const FabricGraph& fabric, const EndpointBinding& binding, const Edge& edge) {
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
  if (!binding.allowed_tiers.empty()) {
    if (!edge.tier.valid() ||
        !std::binary_search(binding.allowed_tiers.begin(), binding.allowed_tiers.end(), edge.tier)) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* to_string(PlanFreshness value) noexcept {
  switch (value) {
    case PlanFreshness::kCurrent:
      return "current";
    case PlanFreshness::kStaleTopology:
      return "stale-topology";
    case PlanFreshness::kStaleFailureDomains:
      return "stale-failure-domains";
    case PlanFreshness::kStaleCapacityEvidence:
      return "stale-capacity-evidence";
    case PlanFreshness::kStalePolicy:
      return "stale-policy";
    case PlanFreshness::kStalePlanGeneration:
      return "stale-plan-generation";
    case PlanFreshness::kUnverified:
      return "unverified";
  }
  return "unverified";
}

PlanFreshness assess_freshness(const Plan& plan, const GenerationBindings& current) noexcept {
  if (plan.bindings == current) {
    return PlanFreshness::kCurrent;
  }
  if (!(plan.bindings.topology == current.topology)) {
    return PlanFreshness::kStaleTopology;
  }
  if (!(plan.bindings.failure_domains == current.failure_domains)) {
    return PlanFreshness::kStaleFailureDomains;
  }
  if (!(plan.bindings.capacity_evidence == current.capacity_evidence)) {
    return PlanFreshness::kStaleCapacityEvidence;
  }
  if (!(plan.bindings.policy == current.policy)) {
    return PlanFreshness::kStalePolicy;
  }
  return PlanFreshness::kStalePlanGeneration;
}

const char* describe_freshness(PlanFreshness freshness, ErrorCode& code) noexcept {
  switch (freshness) {
    case PlanFreshness::kCurrent:
      code = ErrorCode::kOk;
      return "the plan was produced from exactly these generations";
    case PlanFreshness::kStaleTopology:
      code = ErrorCode::kStaleTopologyGeneration;
      return "the physical topology generation has changed since this plan was produced";
    case PlanFreshness::kStaleFailureDomains:
      code = ErrorCode::kStaleFailureDomainGeneration;
      return "the failure-domain generation has changed since this plan was produced";
    case PlanFreshness::kStaleCapacityEvidence:
      code = ErrorCode::kStaleCapacityEvidenceGeneration;
      return "the capacity-evidence generation has changed since this plan was produced";
    case PlanFreshness::kStalePolicy:
      code = ErrorCode::kStalePolicyGeneration;
      return "the policy generation has changed since this plan was produced";
    case PlanFreshness::kStalePlanGeneration:
      code = ErrorCode::kStalePlanGeneration;
      return "the plan generation has advanced past this plan";
    case PlanFreshness::kUnverified:
      code = ErrorCode::kRevalidationRequired;
      return "the plan was reopened from storage and has not been revalidated";
  }
  code = ErrorCode::kInternalError;
  return "unknown freshness classification";
}

std::vector<std::uint8_t> Plan::encode() const {
  ByteWriter writer;
  writer.tagged("cpath.plan.v1");
  writer.u16(kCanonicalFormatVersion);
  writer.digest(input_digest);
  writer.u64(generation.value());
  encode_bindings(bindings, writer);
  writer.string(collective.value());
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u32(stage_count);
  writer.u32(static_cast<std::uint32_t>(stages.size()));
  for (const Stage& stage : stages) {
    writer.u32(stage.index);
    writer.u32(static_cast<std::uint32_t>(stage.logical_edges.size()));
    for (const LogicalEdgeId& logical_id : stage.logical_edges) {
      writer.u64(logical_id.value());
    }
    writer.u32(static_cast<std::uint32_t>(stage.paths.size()));
    for (const PathId& path_id : stage.paths) {
      writer.u64(path_id.value());
    }
  }
  writer.u32(static_cast<std::uint32_t>(paths.size()));
  for (const PathPlan& path : paths) {
    writer.u64(path.id.value());
    writer.u64(path.logical_edge.value());
    writer.string(path.src.value());
    writer.string(path.dst.value());
    writer.u32(path.stage);
    writer.u32(static_cast<std::uint32_t>(path.hops.size()));
    for (const Hop& hop : path.hops) {
      writer.string(hop.edge.value());
      writer.string(hop.from.value());
      writer.string(hop.to.value());
    }
    writer.u64(path.cost);
    writer.u64(path.bottleneck_mbps);
    writer.u32(static_cast<std::uint32_t>(path.domain_signature.size()));
    for (const FailureDomainId& domain : path.domain_signature) {
      writer.string(domain.value());
    }
    writer.u8(static_cast<std::uint8_t>(path.weakest_evidence));
  }
  writer.u32(static_cast<std::uint32_t>(allocations.size()));
  for (const Allocation& allocation : allocations) {
    writer.string(allocation.edge.value());
    writer.u64(allocation.planned_mbps);
  }
  writer.u64(stats.total_cost);
  writer.u64(stats.max_path_cost);
  writer.u64(static_cast<std::uint64_t>(stats.path_count));
  writer.u64(static_cast<std::uint64_t>(stats.hop_count));
  writer.u64(static_cast<std::uint64_t>(stats.logical_edge_count));
  writer.u64(stats.bottleneck_mbps);
  writer.u8(static_cast<std::uint8_t>(weakest_evidence));
  return writer.take();
}

Digest Plan::body_digest() const {
  const std::vector<std::uint8_t> bytes = encode();
  return Sha256::hash(bytes.data(), bytes.size());
}

Result<Plan> Plan::decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPlanRecordBytes) {
    return Result<Plan>::failure(ErrorCode::kOversizedRecord, "plan payload exceeds the record bound");
  }
  ByteReader reader(bytes);
  std::string tag;
  reader.string(tag, 64);
  std::uint16_t version = 0;
  reader.u16(version);
  if (Status status = reader.status(); !status.is_ok()) {
    return Result<Plan>::failure(status);
  }
  if (tag != "cpath.plan.v1") {
    return Result<Plan>::failure(ErrorCode::kUnsupportedRecordVersion, "unexpected plan tag " + tag);
  }
  if (version != kCanonicalFormatVersion) {
    return Result<Plan>::failure(ErrorCode::kUnsupportedRecordVersion,
                                 "unsupported plan encoding version " + std::to_string(version));
  }

  Plan plan;
  reader.digest(plan.input_digest);
  std::uint64_t generation = 0;
  reader.u64(generation);
  plan.generation = CollectivePlanGeneration{generation};
  if (Status status = decode_bindings(reader, plan.bindings); !status.is_ok()) {
    return Result<Plan>::failure(status);
  }
  std::string collective;
  reader.string(collective, kMaxIdentifierBytes);
  plan.collective = CollectiveId{collective};
  std::uint8_t kind = 0;
  reader.u8(kind);
  if (kind > static_cast<std::uint8_t>(CollectiveKind::kHierarchical)) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan declares an unknown collective kind");
  }
  plan.kind = static_cast<CollectiveKind>(kind);
  reader.u32(plan.stage_count);
  if (plan.stage_count == 0 || plan.stage_count > kMaxStages) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan stage count is out of range");
  }

  std::uint32_t stage_count = 0;
  reader.count(stage_count, kMaxStages);
  for (std::uint32_t index = 0; index < stage_count; ++index) {
    Stage stage;
    reader.u32(stage.index);
    std::uint32_t logical_count = 0;
    reader.count(logical_count, kMaxLogicalEdges);
    for (std::uint32_t item = 0; item < logical_count; ++item) {
      std::uint64_t value = 0;
      reader.u64(value);
      stage.logical_edges.push_back(LogicalEdgeId{value});
    }
    std::uint32_t path_count = 0;
    reader.count(path_count, kMaxLogicalEdges);
    for (std::uint32_t item = 0; item < path_count; ++item) {
      std::uint64_t value = 0;
      reader.u64(value);
      stage.paths.push_back(PathId{value});
    }
    plan.stages.push_back(std::move(stage));
  }

  std::uint32_t path_total = 0;
  reader.count(path_total, kMaxLogicalEdges);
  for (std::uint32_t index = 0; index < path_total; ++index) {
    PathPlan path;
    std::uint64_t id = 0;
    std::uint64_t logical = 0;
    reader.u64(id);
    reader.u64(logical);
    path.id = PathId{id};
    path.logical_edge = LogicalEdgeId{logical};
    std::string src;
    std::string dst;
    reader.string(src, kMaxIdentifierBytes);
    reader.string(dst, kMaxIdentifierBytes);
    path.src = ParticipantId{src};
    path.dst = ParticipantId{dst};
    reader.u32(path.stage);
    std::uint32_t hop_total = 0;
    reader.count(hop_total, kMaxHops);
    for (std::uint32_t hop = 0; hop < hop_total; ++hop) {
      std::string edge;
      std::string from;
      std::string to;
      reader.string(edge, kMaxIdentifierBytes);
      reader.string(from, kMaxIdentifierBytes);
      reader.string(to, kMaxIdentifierBytes);
      path.hops.push_back(Hop{EdgeId{edge}, NodeId{from}, NodeId{to}});
    }
    reader.u64(path.cost);
    reader.u64(path.bottleneck_mbps);
    std::uint32_t signature_total = 0;
    reader.count(signature_total, kMaxFailureDomains);
    for (std::uint32_t item = 0; item < signature_total; ++item) {
      std::string domain;
      reader.string(domain, kMaxIdentifierBytes);
      path.domain_signature.push_back(FailureDomainId{domain});
    }
    std::uint8_t evidence = 0;
    reader.u8(evidence);
    if (evidence > static_cast<std::uint8_t>(EvidenceClass::kMeasured)) {
      return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan declares an unknown evidence class");
    }
    path.weakest_evidence = static_cast<EvidenceClass>(evidence);
    if (Status status = reader.status(); !status.is_ok()) {
      return Result<Plan>::failure(status);
    }
    plan.paths.push_back(std::move(path));
  }

  std::uint32_t allocation_total = 0;
  reader.count(allocation_total, kMaxEdges);
  for (std::uint32_t index = 0; index < allocation_total; ++index) {
    std::string edge;
    reader.string(edge, kMaxIdentifierBytes);
    Allocation allocation;
    allocation.edge = EdgeId{edge};
    reader.u64(allocation.planned_mbps);
    plan.allocations.push_back(std::move(allocation));
  }

  std::uint64_t path_count = 0;
  std::uint64_t hop_count = 0;
  std::uint64_t logical_count = 0;
  reader.u64(plan.stats.total_cost);
  reader.u64(plan.stats.max_path_cost);
  reader.u64(path_count);
  reader.u64(hop_count);
  reader.u64(logical_count);
  reader.u64(plan.stats.bottleneck_mbps);
  plan.stats.path_count = static_cast<std::size_t>(path_count);
  plan.stats.hop_count = static_cast<std::size_t>(hop_count);
  plan.stats.logical_edge_count = static_cast<std::size_t>(logical_count);
  std::uint8_t weakest = 0;
  reader.u8(weakest);
  if (weakest > static_cast<std::uint8_t>(EvidenceClass::kMeasured)) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan declares an unknown weakest evidence class");
  }
  plan.weakest_evidence = static_cast<EvidenceClass>(weakest);

  if (Status status = reader.require_end(); !status.is_ok()) {
    return Result<Plan>::failure(status);
  }
  if (plan.paths.size() != plan.stats.path_count) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan path count does not match its statistics");
  }
  plan.id = CollectivePlanId{plan.input_digest};
  if (!plan.id.valid()) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "plan has a zero input digest");
  }
  return Result<Plan>::success(std::move(plan));
}

Status validate_plan(const Plan& plan, const PlanningRequest& request) {
  const Policy& policy = request.policy;
  const FabricGraph& fabric = request.fabric;
  const Collective& collective = request.collective;

  if (!(plan.input_digest == request.canonical_digest)) {
    return Status::error(ErrorCode::kPlanInputMismatch, "plan digest does not match the canonical request digest");
  }
  if (!(plan.id.value() == plan.input_digest)) {
    return Status::error(ErrorCode::kDigestMismatch, "plan identity is not its input digest");
  }
  if (!(plan.generation == request.plan_generation)) {
    return Status::error(ErrorCode::kStalePlanGeneration, "plan generation does not match the request");
  }
  if (!(plan.bindings == request.current_bindings())) {
    return Status::error(ErrorCode::kGenerationRegression, "plan generation bindings do not match the request");
  }
  if (!(plan.collective == collective.id)) {
    return Status::error(ErrorCode::kPlanInputMismatch, "plan names a different collective");
  }
  if (!(plan.kind == collective.kind)) {
    return Status::error(ErrorCode::kPlanInputMismatch, "plan names a different collective kind");
  }
  if (plan.stage_count != collective.stage_count) {
    return Status::error(ErrorCode::kPlanInputMismatch, "plan stage count does not match the collective");
  }
  if (plan.stages.size() != plan.stage_count) {
    return Status::error(ErrorCode::kInconsistentMetadata, "plan does not materialise every stage");
  }

  std::vector<std::uint8_t> seen_logical(collective.logical_edges.size() + 1u, 0u);
  std::vector<std::uint8_t> seen_paths(plan.paths.size() + 1u, 0u);
  std::vector<std::vector<const PathPlan*>> per_logical(collective.logical_edges.size() + 1u);

  for (std::size_t index = 0; index < plan.paths.size(); ++index) {
    const PathPlan& path = plan.paths[index];
    if (path.id.value() == 0 || path.id.value() > plan.paths.size()) {
      return Status::error(ErrorCode::kInconsistentMetadata, "plan path identity is out of range");
    }
    if (seen_paths[path.id.value()] != 0u) {
      return Status::error(ErrorCode::kDuplicateIdentifier, "plan repeats a path identity");
    }
    seen_paths[path.id.value()] = 1u;
    const std::uint64_t logical_index = path.logical_edge.value();
    if (logical_index == 0 || logical_index > collective.logical_edges.size()) {
      return Status::error(ErrorCode::kInconsistentMetadata, "plan path names an unknown logical edge");
    }
    const LogicalEdge& logical = collective.logical_edges[logical_index - 1u];
    if (!(path.src == logical.src) || !(path.dst == logical.dst)) {
      return Status::error(ErrorCode::kPlanInputMismatch, "plan path endpoints do not match its logical edge");
    }
    if (path.stage != collective.logical_edge_stage[logical_index - 1u]) {
      return Status::error(ErrorCode::kInconsistentMetadata, "plan path stage does not match the collective");
    }
    seen_logical[logical_index] = 1u;
    per_logical[logical_index].push_back(&path);

    const EndpointBinding* source_binding = request.find_binding(logical.src);
    const EndpointBinding* target_binding = request.find_binding(logical.dst);
    if (source_binding == nullptr || target_binding == nullptr) {
      return Status::error(ErrorCode::kUnboundParticipant, "plan path has an unbound participant");
    }
    if (path.hops.size() > policy.max_hops) {
      return Status::error(ErrorCode::kHopLimitExceeded, "plan path exceeds the hop limit");
    }
    if (path.cost > policy.max_path_cost) {
      return Status::error(ErrorCode::kCostLimitExceeded, "plan path exceeds the cost limit");
    }

    // Every emitted path must be SIMPLE: no physical node and no physical edge
    // may appear twice. This is re-derived from the hops, not taken on trust.
    if (!hops_are_simple(path.hops)) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "plan path is not simple: it revisits a physical node or edge");
    }
    if (path.hops.empty()) {
      if (!(source_binding->node == target_binding->node)) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "a zero-hop plan path requires both participants on the same endpoint node");
      }
    } else {
      if (!(path.hops.front().from == source_binding->node)) {
        return Status::error(ErrorCode::kPlanInputMismatch, "plan path does not start at the bound source node");
      }
      if (!(path.hops.back().to == target_binding->node)) {
        return Status::error(ErrorCode::kPlanInputMismatch,
                             "plan path does not end at the bound destination node");
      }
    }

    std::vector<FailureDomainId> recomputed_signature;
    std::vector<FailureDomainId> hop_domains;
    hop_domains.reserve(path.hops.size());
    for (std::size_t hop_index = 0; hop_index < path.hops.size(); ++hop_index) {
      const Hop& hop = path.hops[hop_index];
      const Edge* edge = fabric.find_edge(hop.edge);
      if (edge == nullptr) {
        return Status::error(ErrorCode::kUnknownReference,
                             "plan hop references edge " + hop.edge.value() + " which is not in the fabric");
      }
      if (!(edge->from == hop.from) || !(edge->to == hop.to)) {
        return Status::error(ErrorCode::kPlanInputMismatch,
                             "plan hop endpoints do not match the referenced physical edge");
      }
      if (hop_index + 1u < path.hops.size() && !(path.hops[hop_index + 1u].from == hop.to)) {
        return Status::error(ErrorCode::kInconsistentMetadata, "plan path hops are not contiguous");
      }
      if (is_forbidden_node(policy, hop.from) || is_forbidden_node(policy, hop.to)) {
        return Status::error(ErrorCode::kNodeForbidden, "plan path traverses a forbidden node");
      }
      if (is_forbidden_tier(policy, edge->tier)) {
        return Status::error(ErrorCode::kTierForbidden, "plan path traverses a forbidden tier");
      }
      if (!policy.allowed_path_tiers.empty() &&
          (!edge->tier.valid() ||
           !std::binary_search(policy.allowed_path_tiers.begin(), policy.allowed_path_tiers.end(), edge->tier))) {
        return Status::error(ErrorCode::kTierForbidden, "plan path uses a tier outside the allowed set");
      }
      if (is_forbidden_domain(fabric, policy, edge->failure_domain)) {
        return Status::error(ErrorCode::kFailureDomainForbidden, "plan path traverses a forbidden failure domain");
      }
      if (!evidence_is_sufficient(edge->evidence, policy.evidence)) {
        return Status::error(ErrorCode::kCapacityEvidenceInsufficient, "plan path relies on insufficient evidence");
      }
      if (edge->capacity_mbps == 0 && !policy.allow_unverified_capacity) {
        return Status::error(ErrorCode::kEdgeHasNoVerifiedCapacity,
                             "plan path uses an edge with no verified capacity");
      }
      if (edge->capacity_mbps > 0 && edge->reserved_mbps >= edge->capacity_mbps) {
        return Status::error(ErrorCode::kEdgeHasNoVerifiedCapacity, "plan path uses a saturated edge");
      }
      if (!binding_permits(fabric, *source_binding, *edge) || !binding_permits(fabric, *target_binding, *edge)) {
        return Status::error(ErrorCode::kEndpointIneligible,
                             "plan path leaves the failure domains or tiers permitted by an endpoint binding");
      }
      if (policy.domain_diversity != DomainDiversity::kNone && edge->failure_domain.valid()) {
        hop_domains.push_back(domain_at_level(fabric, edge->failure_domain, policy.domain_diversity_level));
      }
    }
    // Mirrors the planner: the endpoint attachment domains are excluded because
    // every sibling path necessarily sits in them.
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
        recomputed_signature.push_back(domain);
      }
    }
    std::sort(recomputed_signature.begin(), recomputed_signature.end());
    recomputed_signature.erase(std::unique(recomputed_signature.begin(), recomputed_signature.end()),
                               recomputed_signature.end());
    if (!(recomputed_signature == path.domain_signature)) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "plan path failure-domain signature does not match its hops");
    }
    if (policy.domain_diversity == DomainDiversity::kRequired && per_logical[logical_index].size() > 1u &&
        path.hops.size() > 0 && hop_domains.size() != path.hops.size()) {
      return Status::error(ErrorCode::kDomainDiversityUnsatisfiable,
                           "plan path traverses an edge with no declared failure domain");
    }
  }

  for (std::size_t logical_index = 1; logical_index < seen_logical.size(); ++logical_index) {
    if (seen_logical[logical_index] == 0u) {
      return Status::error(ErrorCode::kPlanInputMismatch, "plan omits a logical edge of the collective");
    }
    const std::vector<const PathPlan*>& siblings = per_logical[logical_index];
    if (siblings.size() != policy.paths_per_logical_edge) {
      return Status::error(ErrorCode::kPathCountUnsupported,
                           "plan does not provide the required number of paths for a logical edge");
    }
    for (std::size_t i = 0; i < siblings.size(); ++i) {
      for (std::size_t j = i + 1u; j < siblings.size(); ++j) {
        const PathPlan& lhs = *siblings[i];
        const PathPlan& rhs = *siblings[j];
        if (policy.disjointness != Disjointness::kNone) {
          for (const Hop& left : lhs.hops) {
            for (const Hop& right : rhs.hops) {
              if (left.edge == right.edge) {
                return Status::error(ErrorCode::kInsufficientDisjointPaths,
                                     "sibling plan paths share a physical edge");
              }
            }
          }
        }
        if (policy.disjointness == Disjointness::kNode) {
          // Siblings share their source and destination by construction; only an
          // interior node in common is a diversity violation.
          for (std::size_t left_index = 0; left_index + 1u < lhs.hops.size(); ++left_index) {
            for (std::size_t right_index = 0; right_index + 1u < rhs.hops.size(); ++right_index) {
              if (lhs.hops[left_index].to == rhs.hops[right_index].to) {
                return Status::error(ErrorCode::kInsufficientDisjointPaths,
                                     "sibling plan paths share an intermediate physical node");
              }
            }
          }
        }
        if (policy.domain_diversity == DomainDiversity::kRequired) {
          if (lhs.hops.empty() || rhs.hops.empty()) {
            continue;  // a zero-hop path touches no domain
          }
          for (const FailureDomainId& domain : lhs.domain_signature) {
            if (std::binary_search(rhs.domain_signature.begin(), rhs.domain_signature.end(), domain)) {
              return Status::error(ErrorCode::kDomainDiversityUnsatisfiable,
                                   "sibling plan paths are not failure-domain independent");
            }
          }
        }
      }
    }
  }

  for (const Stage& stage : plan.stages) {
    if (stage.index >= plan.stage_count) {
      return Status::error(ErrorCode::kInconsistentMetadata, "plan stage index is out of range");
    }
    for (const PathId& id : stage.paths) {
      if (id.value() == 0 || id.value() > plan.paths.size() || !(plan.paths[id.value() - 1u].id == id)) {
        return Status::error(ErrorCode::kInconsistentMetadata, "plan stage references an unknown path");
      }
      if (plan.paths[id.value() - 1u].stage != stage.index) {
        return Status::error(ErrorCode::kInconsistentMetadata, "plan path is listed under the wrong stage");
      }
    }
    for (const LogicalEdgeId& id : stage.logical_edges) {
      const std::uint64_t logical_index = id.value();
      if (logical_index == 0 || logical_index > collective.logical_edges.size()) {
        return Status::error(ErrorCode::kInconsistentMetadata, "plan stage references an unknown logical edge");
      }
      if (collective.logical_edge_stage[logical_index - 1u] != stage.index) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "plan stage lists a logical edge that belongs to another stage");
      }
    }
  }

  // Allocation bookkeeping is recomputed from the paths themselves.
  std::unordered_map<EdgeId, std::uint64_t, StrongIdHash<EdgeIdTag, std::string>> recomputed;
  if (collective.demand_mbps > 0) {
    const std::uint64_t per_path = (collective.demand_mbps + policy.paths_per_logical_edge - 1u) /
                                   policy.paths_per_logical_edge;
    for (const PathPlan& path : plan.paths) {
      for (const Hop& hop : path.hops) {
        recomputed[hop.edge] = saturating_add(recomputed[hop.edge], per_path, kCostCeiling);
      }
    }
  }
  // Every allocation entry must name a distinct physical edge, so an alias or a
  // repeated entry cannot hide a double commitment behind two names.
  {
    std::vector<EdgeId> allocation_edges;
    allocation_edges.reserve(plan.allocations.size());
    for (const Allocation& allocation : plan.allocations) {
      allocation_edges.push_back(allocation.edge);
    }
    std::sort(allocation_edges.begin(), allocation_edges.end());
    if (std::adjacent_find(allocation_edges.begin(), allocation_edges.end()) != allocation_edges.end()) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "plan allocation table names the same physical edge twice");
    }
  }
  if (recomputed.size() != plan.allocations.size()) {
    return Status::error(ErrorCode::kInconsistentMetadata, "plan allocation table does not match its paths");
  }
  for (const Allocation& allocation : plan.allocations) {
    const auto found = recomputed.find(allocation.edge);
    if (found == recomputed.end() || found->second != allocation.planned_mbps) {
      return Status::error(ErrorCode::kInconsistentMetadata, "plan allocation does not match its paths");
    }
    // A plan may only propose what the evidence says is spare. This is checked
    // independently of the planner so that an over-committed proposal can never
    // be emitted as valid.
    const Edge* edge = fabric.find_edge(allocation.edge);
    if (edge == nullptr) {
      return Status::error(ErrorCode::kUnknownReference, "plan allocates onto an unknown physical edge");
    }
    if (edge->capacity_mbps > 0 &&
        allocation.planned_mbps > edge->capacity_mbps - edge->reserved_mbps) {
      return Status::error(ErrorCode::kInsufficientCapacity,
                           "plan allocates " + std::to_string(allocation.planned_mbps) +
                               " Mbps onto edge " + edge->id.value() +
                               " which has less verified spare capacity than that");
    }
  }

  std::uint64_t total_cost = 0;
  std::uint64_t max_cost = 0;
  std::size_t hop_count = 0;
  std::uint64_t bottleneck = kCostCeiling;
  EvidenceClass weakest = EvidenceClass::kMeasured;
  std::size_t occupied = 0;
  for (const PathPlan& path : plan.paths) {
    total_cost = saturating_add(total_cost, path.cost, kCostCeiling);
    max_cost = std::max(max_cost, path.cost);
    hop_count += path.hops.size();
    if (!path.hops.empty()) {
      bottleneck = std::min(bottleneck, path.bottleneck_mbps);
      ++occupied;
    }
    if (static_cast<std::uint8_t>(path.weakest_evidence) < static_cast<std::uint8_t>(weakest)) {
      weakest = path.weakest_evidence;
    }
  }
  if (plan.stats.total_cost != total_cost || plan.stats.max_path_cost != max_cost ||
      plan.stats.hop_count != hop_count || plan.stats.logical_edge_count != collective.logical_edges.size()) {
    return Status::error(ErrorCode::kInconsistentMetadata, "plan statistics do not match its paths");
  }
  if (occupied == 0) {
    bottleneck = 0;
  }
  if (plan.stats.bottleneck_mbps != bottleneck) {
    return Status::error(ErrorCode::kInconsistentMetadata, "plan bottleneck does not match its paths");
  }
  if (static_cast<std::uint8_t>(plan.weakest_evidence) != static_cast<std::uint8_t>(weakest)) {
    return Status::error(ErrorCode::kInconsistentMetadata, "plan weakest evidence does not match its paths");
  }
  return Status::ok();
}

Result<Plan> replan(const PlanningRequest& request) {
  const PlanningOutcome outcome = plan_collective(request);
  if (!outcome.ok()) {
    const ErrorCode code = outcome.primary_code();
    std::string detail = "replanning was refused";
    if (!outcome.denials.empty()) {
      detail = outcome.denials.front().message;
    }
    return Result<Plan>::failure(code, std::move(detail));
  }
  return Result<Plan>::success(*outcome.plan);
}

}  // namespace cpath
