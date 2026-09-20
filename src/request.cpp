// Collective Path Planner - planning request model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/request.hpp"

#include <algorithm>
#include <utility>

namespace cpath {

GenerationBindings PlanningRequest::current_bindings() const noexcept {
  GenerationBindings current;
  current.topology = fabric.topology_generation();
  current.failure_domains = fabric.failure_domain_generation();
  current.capacity_evidence = fabric.capacity_evidence_generation();
  current.policy = policy.generation;
  current.plan = plan_generation;
  return current;
}

const EndpointBinding* PlanningRequest::find_binding(const ParticipantId& participant) const noexcept {
  const auto found = std::lower_bound(bindings.begin(), bindings.end(), participant,
                                      [](const EndpointBinding& binding, const ParticipantId& id) {
                                        return binding.participant < id;
                                      });
  if (found == bindings.end() || !(found->participant == participant)) {
    return nullptr;
  }
  return &(*found);
}

void PlanningRequest::encode_canonical(ByteWriter& writer) const {
  writer.tagged("cpath.request.v1");
  writer.u16(kCanonicalFormatVersion);
  collective.encode_canonical(writer);
  fabric.encode_canonical(writer);
  policy.encode_canonical(writer);
  writer.u32(static_cast<std::uint32_t>(bindings.size()));
  for (const EndpointBinding& binding : bindings) {
    writer.string(binding.participant.value());
    writer.string(binding.node.value());
    writer.u32(static_cast<std::uint32_t>(binding.allowed_failure_domains.size()));
    for (const FailureDomainId& domain : binding.allowed_failure_domains) {
      writer.string(domain.value());
    }
    writer.u32(static_cast<std::uint32_t>(binding.allowed_tiers.size()));
    for (const TierId& tier : binding.allowed_tiers) {
      writer.string(tier.value());
    }
  }
}

Status PlanningRequest::validate() const {
  if (Status status = collective.validate(); !status.is_ok()) {
    return status;
  }
  if (Status status = fabric.validate(); !status.is_ok()) {
    return status;
  }
  if (Status status = policy.validate(); !status.is_ok()) {
    return status;
  }
  if (!plan_generation.valid()) {
    return Status::error(ErrorCode::kMissingRequiredField, "plan generation must be set");
  }
  if (bindings.size() > kMaxBindings) {
    return Status::error(ErrorCode::kTooManyItems, "binding count exceeds limit");
  }
  for (std::size_t index = 1; index < bindings.size(); ++index) {
    if (!(bindings[index - 1].participant < bindings[index].participant)) {
      return Status::error(ErrorCode::kInconsistentMetadata, "bindings are not in canonical order");
    }
  }
  for (const EndpointBinding& binding : bindings) {
    if (!is_valid_identifier(binding.participant.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "binding participant is not a valid identifier");
    }
    if (!is_valid_identifier(binding.node.value())) {
      return Status::error(ErrorCode::kInvalidCharacter, "binding node is not a valid identifier");
    }
    if (std::find(collective.participants.begin(), collective.participants.end(), binding.participant) ==
        collective.participants.end()) {
      return Status::error(ErrorCode::kUnknownReference,
                           "binding references participant " + binding.participant.value() +
                               " which the collective does not declare");
    }
    if (fabric.find_node(binding.node) == nullptr) {
      return Status::error(ErrorCode::kUnknownEndpointNode,
                           "binding references unknown node " + binding.node.value());
    }
    for (const FailureDomainId& domain : binding.allowed_failure_domains) {
      if (fabric.find_failure_domain(domain) == nullptr) {
        return Status::error(ErrorCode::kUnknownReference,
                             "binding references unknown failure domain " + domain.value());
      }
    }
    for (const TierId& tier : binding.allowed_tiers) {
      if (!is_valid_tier_name(tier.value())) {
        return Status::error(ErrorCode::kInvalidCharacter, "binding tier is not a valid tier name");
      }
    }
  }
  ByteWriter writer;
  encode_canonical(writer);
  if (writer.sha256() != canonical_digest) {
    return Status::error(ErrorCode::kDigestMismatch, "canonical digest does not match the encoded request");
  }
  return Status::ok();
}

Result<PlanningRequest> build_request(const RequestSpec& spec) {
  RequestSpec copy = spec;
  return build_request(std::move(copy));
}

Result<PlanningRequest> build_request(RequestSpec&& spec) {
  PlanningRequest request;

  auto collective = Collective::build(std::move(spec.collective));
  if (!collective.has_value()) {
    return Result<PlanningRequest>::failure(collective.status());
  }
  request.collective = std::move(collective.value());

  auto fabric = FabricGraph::build(std::move(spec.fabric));
  if (!fabric.has_value()) {
    return Result<PlanningRequest>::failure(fabric.status());
  }
  request.fabric = std::move(fabric.value());

  request.policy = std::move(spec.policy);
  request.policy.canonicalize();
  if (Status status = request.policy.validate(); !status.is_ok()) {
    return Result<PlanningRequest>::failure(status);
  }

  request.plan_generation = spec.plan_generation;
  if (!request.plan_generation.valid()) {
    return Result<PlanningRequest>::failure(ErrorCode::kMissingRequiredField, "plan generation must be set");
  }

  std::vector<EndpointBinding> bindings = std::move(spec.bindings);
  if (bindings.size() > kMaxBindings) {
    return Result<PlanningRequest>::failure(ErrorCode::kTooManyItems, "binding count exceeds limit");
  }
  for (EndpointBinding& binding : bindings) {
    if (!is_valid_identifier(binding.participant.value())) {
      return Result<PlanningRequest>::failure(ErrorCode::kInvalidCharacter,
                                              "binding participant is not a valid identifier");
    }
    if (!is_valid_identifier(binding.node.value())) {
      return Result<PlanningRequest>::failure(ErrorCode::kInvalidCharacter,
                                              "binding node is not a valid identifier");
    }
    if (binding.allowed_failure_domains.size() > kMaxPolicySetEntries ||
        binding.allowed_tiers.size() > kMaxPolicySetEntries) {
      return Result<PlanningRequest>::failure(ErrorCode::kTooManyItems, "binding set exceeds the configured bound");
    }
    std::sort(binding.allowed_failure_domains.begin(), binding.allowed_failure_domains.end());
    binding.allowed_failure_domains.erase(
        std::unique(binding.allowed_failure_domains.begin(), binding.allowed_failure_domains.end()),
        binding.allowed_failure_domains.end());
    std::sort(binding.allowed_tiers.begin(), binding.allowed_tiers.end());
    binding.allowed_tiers.erase(std::unique(binding.allowed_tiers.begin(), binding.allowed_tiers.end()),
                                binding.allowed_tiers.end());
  }
  std::sort(bindings.begin(), bindings.end(), [](const EndpointBinding& lhs, const EndpointBinding& rhs) {
    return lhs.participant < rhs.participant;
  });
  for (std::size_t index = 1; index < bindings.size(); ++index) {
    if (bindings[index].participant == bindings[index - 1].participant) {
      return Result<PlanningRequest>::failure(ErrorCode::kDuplicateBinding,
                                              "participant " + bindings[index].participant.value() +
                                                  " is bound more than once");
    }
  }
  request.bindings = std::move(bindings);

  for (const EndpointBinding& binding : request.bindings) {
    if (std::find(request.collective.participants.begin(), request.collective.participants.end(),
                  binding.participant) == request.collective.participants.end()) {
      return Result<PlanningRequest>::failure(ErrorCode::kUnknownReference,
                                              "binding references participant " + binding.participant.value() +
                                                  " which the collective does not declare");
    }
    if (request.fabric.find_node(binding.node) == nullptr) {
      return Result<PlanningRequest>::failure(ErrorCode::kUnknownEndpointNode,
                                              "binding references unknown node " + binding.node.value());
    }
    for (const FailureDomainId& domain : binding.allowed_failure_domains) {
      if (request.fabric.find_failure_domain(domain) == nullptr) {
        return Result<PlanningRequest>::failure(ErrorCode::kUnknownReference,
                                                "binding references unknown failure domain " + domain.value());
      }
    }
    for (const TierId& tier : binding.allowed_tiers) {
      if (!is_valid_tier_name(tier.value())) {
        return Result<PlanningRequest>::failure(ErrorCode::kInvalidCharacter,
                                                "binding tier is not a valid tier name");
      }
    }
  }

  ByteWriter writer;
  request.encode_canonical(writer);
  request.canonical_bytes = writer.size();
  request.canonical_digest = writer.sha256();

  if (Status status = request.validate(); !status.is_ok()) {
    return Result<PlanningRequest>::failure(status);
  }
  return Result<PlanningRequest>::success(std::move(request));
}

}  // namespace cpath
