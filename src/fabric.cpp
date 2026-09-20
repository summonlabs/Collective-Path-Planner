// Collective Path Planner - physical fabric graph model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/fabric.hpp"

#include <algorithm>
#include <utility>

namespace cpath {
namespace {

bool valid_optional_label(std::string_view text) noexcept {
  return text.empty() || (text.size() <= kMaxPlacementLabelBytes && is_valid_identifier(text));
}

}  // namespace

const char* to_string(EvidenceClass value) noexcept {
  switch (value) {
    case EvidenceClass::kUnknown:
      return "unknown";
    case EvidenceClass::kSynthetic:
      return "synthetic";
    case EvidenceClass::kMeasured:
      return "measured";
  }
  return "unknown";
}

bool parse_evidence_class(std::string_view text, EvidenceClass& out) noexcept {
  if (text == "unknown") {
    out = EvidenceClass::kUnknown;
    return true;
  }
  if (text == "synthetic") {
    out = EvidenceClass::kSynthetic;
    return true;
  }
  if (text == "measured") {
    out = EvidenceClass::kMeasured;
    return true;
  }
  return false;
}

const char* to_string(DomainKind value) noexcept {
  switch (value) {
    case DomainKind::kDevice:
      return "device";
    case DomainKind::kHost:
      return "host";
    case DomainKind::kRack:
      return "rack";
    case DomainKind::kPod:
      return "pod";
    case DomainKind::kSite:
      return "site";
  }
  return "device";
}

bool parse_domain_kind(std::string_view text, DomainKind& out) noexcept {
  if (text == "device") {
    out = DomainKind::kDevice;
    return true;
  }
  if (text == "host") {
    out = DomainKind::kHost;
    return true;
  }
  if (text == "rack") {
    out = DomainKind::kRack;
    return true;
  }
  if (text == "pod") {
    out = DomainKind::kPod;
    return true;
  }
  if (text == "site") {
    out = DomainKind::kSite;
    return true;
  }
  return false;
}

Result<FabricGraph> FabricGraph::build(const FabricSpec& spec) {
  FabricSpec copy = spec;
  return build(std::move(copy));
}

Result<FabricGraph> FabricGraph::build(FabricSpec&& spec) {
  if (spec.failure_domains.size() > kMaxFailureDomains) {
    return Result<FabricGraph>::failure(ErrorCode::kTooManyItems, "failure domain count exceeds limit");
  }
  if (spec.nodes.size() > kMaxNodes) {
    return Result<FabricGraph>::failure(ErrorCode::kTooManyItems, "node count exceeds limit");
  }
  if (spec.edges.size() > kMaxEdges) {
    return Result<FabricGraph>::failure(ErrorCode::kTooManyItems, "edge count exceeds limit");
  }

  FabricGraph graph;
  graph.topology_generation_ = spec.topology_generation;
  graph.failure_domain_generation_ = spec.failure_domain_generation;
  graph.capacity_evidence_generation_ = spec.capacity_evidence_generation;

  // --- failure domains ---
  for (const FailureDomain& domain : spec.failure_domains) {
    if (!is_valid_identifier(domain.id.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "failure domain id is not a valid identifier");
    }
    if (domain.parent.valid() && !is_valid_identifier(domain.parent.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter,
                                          "failure domain parent is not a valid identifier");
    }
  }
  std::sort(spec.failure_domains.begin(), spec.failure_domains.end(),
            [](const FailureDomain& lhs, const FailureDomain& rhs) { return lhs.id < rhs.id; });
  for (std::size_t index = 1; index < spec.failure_domains.size(); ++index) {
    if (spec.failure_domains[index].id == spec.failure_domains[index - 1].id) {
      return Result<FabricGraph>::failure(ErrorCode::kDuplicateIdentifier,
                                          "duplicate failure domain id " + spec.failure_domains[index].id.value());
    }
  }
  graph.failure_domains_ = std::move(spec.failure_domains);
  graph.rebuild_index();

  for (std::size_t index = 0; index < graph.failure_domains_.size(); ++index) {
    const FailureDomain& domain = graph.failure_domains_[index];
    if (!domain.parent.valid()) {
      continue;
    }
    const auto found = graph.domain_index_.find(domain.parent);
    if (found == graph.domain_index_.end()) {
      return Result<FabricGraph>::failure(ErrorCode::kUnknownReference,
                                          "failure domain " + domain.id.value() + " references unknown parent " +
                                              domain.parent.value());
    }
    const FailureDomain& parent = graph.failure_domains_[found->second];
    if (!(static_cast<std::uint8_t>(parent.kind) > static_cast<std::uint8_t>(domain.kind))) {
      return Result<FabricGraph>::failure(
          ErrorCode::kInconsistentMetadata,
          "failure domain " + domain.id.value() + " must be strictly contained by its parent " + parent.id.value());
    }
  }

  // Containment chains are bounded; a longer chain means a cycle or a hierarchy
  // deeper than the model allows. Both are rejected outright.
  graph.domain_chains_.assign(graph.failure_domains_.size(), {});
  for (std::size_t index = 0; index < graph.failure_domains_.size(); ++index) {
    std::vector<FailureDomainId> chain;
    FailureDomainId cursor = graph.failure_domains_[index].id;
    while (cursor.valid()) {
      if (chain.size() >= kMaxHierarchyLevels) {
        return Result<FabricGraph>::failure(ErrorCode::kInconsistentMetadata,
                                            "failure domain containment chain exceeds " +
                                                std::to_string(kMaxHierarchyLevels) + " levels");
      }
      chain.push_back(cursor);
      const auto found = graph.domain_index_.find(cursor);
      if (found == graph.domain_index_.end()) {
        break;
      }
      cursor = graph.failure_domains_[found->second].parent;
    }
    graph.domain_chains_[index] = std::move(chain);
  }

  // --- nodes ---
  for (const Node& node : spec.nodes) {
    if (!is_valid_identifier(node.id.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "node id is not a valid identifier");
    }
    if (node.tier.valid() && !is_valid_tier_name(node.tier.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "node tier is not a valid tier name");
    }
    if (!valid_optional_label(node.placement.site) || !valid_optional_label(node.placement.pod) ||
        !valid_optional_label(node.placement.rack) || !valid_optional_label(node.placement.host)) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "node placement label is not a valid label");
    }
  }
  std::sort(spec.nodes.begin(), spec.nodes.end(),
            [](const Node& lhs, const Node& rhs) { return lhs.id < rhs.id; });
  for (std::size_t index = 1; index < spec.nodes.size(); ++index) {
    if (spec.nodes[index].id == spec.nodes[index - 1].id) {
      return Result<FabricGraph>::failure(ErrorCode::kDuplicateIdentifier,
                                          "duplicate node id " + spec.nodes[index].id.value());
    }
  }
  graph.nodes_ = std::move(spec.nodes);
  graph.rebuild_index();

  // --- edges ---
  for (const Edge& edge : spec.edges) {
    if (!is_valid_identifier(edge.id.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "edge id is not a valid identifier");
    }
    if (!is_valid_identifier(edge.from.value()) || !is_valid_identifier(edge.to.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "edge endpoint is not a valid identifier");
    }
    if (edge.tier.valid() && !is_valid_tier_name(edge.tier.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter, "edge tier is not a valid tier name");
    }
    if (edge.failure_domain.valid() && !is_valid_identifier(edge.failure_domain.value())) {
      return Result<FabricGraph>::failure(ErrorCode::kInvalidCharacter,
                                          "edge failure domain is not a valid identifier");
    }
    if (edge.capacity_mbps > kMaxCapacityMbps) {
      return Result<FabricGraph>::failure(ErrorCode::kValueOutOfRange, "edge capacity exceeds the model ceiling");
    }
    if (edge.latency_micros > kMaxLatencyMicros) {
      return Result<FabricGraph>::failure(ErrorCode::kValueOutOfRange, "edge latency exceeds the model ceiling");
    }
    if (edge.from == edge.to) {
      return Result<FabricGraph>::failure(ErrorCode::kInconsistentMetadata,
                                          "edge " + edge.id.value() + " is a self loop");
    }
    if (edge.reserved_mbps > edge.capacity_mbps) {
      return Result<FabricGraph>::failure(ErrorCode::kInconsistentMetadata,
                                          "edge " + edge.id.value() +
                                              " has reserved capacity above its verified capacity");
    }
    if (graph.node_index_.find(edge.from) == graph.node_index_.end()) {
      return Result<FabricGraph>::failure(ErrorCode::kUnknownReference,
                                          "edge " + edge.id.value() + " references unknown node " + edge.from.value());
    }
    if (graph.node_index_.find(edge.to) == graph.node_index_.end()) {
      return Result<FabricGraph>::failure(ErrorCode::kUnknownReference,
                                          "edge " + edge.id.value() + " references unknown node " + edge.to.value());
    }
    if (edge.failure_domain.valid() && graph.domain_index_.find(edge.failure_domain) == graph.domain_index_.end()) {
      return Result<FabricGraph>::failure(ErrorCode::kUnknownReference,
                                          "edge " + edge.id.value() + " references unknown failure domain " +
                                              edge.failure_domain.value());
    }
  }
  std::sort(spec.edges.begin(), spec.edges.end(), [](const Edge& lhs, const Edge& rhs) {
    if (lhs.from != rhs.from) {
      return lhs.from < rhs.from;
    }
    if (lhs.to != rhs.to) {
      return lhs.to < rhs.to;
    }
    return lhs.id < rhs.id;
  });
  // Edge identities are checked across the whole vector, not only between
  // canonically adjacent rows: two edges with the same id but different
  // endpoints are not adjacent after this sort and would otherwise slip
  // through, leaving find_edge() to return an arbitrary one of them.
  {
    std::vector<std::string_view> edge_ids;
    edge_ids.reserve(spec.edges.size());
    for (const Edge& edge : spec.edges) {
      edge_ids.push_back(edge.id.value());
    }
    std::sort(edge_ids.begin(), edge_ids.end());
    const auto duplicate = std::adjacent_find(edge_ids.begin(), edge_ids.end());
    if (duplicate != edge_ids.end()) {
      return Result<FabricGraph>::failure(ErrorCode::kDuplicateIdentifier,
                                          "duplicate edge id " + std::string(*duplicate));
    }
  }
  graph.edges_ = std::move(spec.edges);
  graph.rebuild_index();

  if (Status status = graph.validate(); !status.is_ok()) {
    return Result<FabricGraph>::failure(status);
  }
  return Result<FabricGraph>::success(std::move(graph));
}

void FabricGraph::rebuild_index() {
  node_index_.clear();
  node_index_.reserve(nodes_.size() * 2u + 1u);
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    node_index_.emplace(nodes_[index].id, static_cast<std::uint32_t>(index));
  }
  edge_index_.clear();
  edge_index_.reserve(edges_.size() * 2u + 1u);
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    edge_index_.emplace(edges_[index].id, static_cast<std::uint32_t>(index));
  }
  domain_index_.clear();
  domain_index_.reserve(failure_domains_.size() * 2u + 1u);
  for (std::size_t index = 0; index < failure_domains_.size(); ++index) {
    domain_index_.emplace(failure_domains_[index].id, static_cast<std::uint32_t>(index));
  }
  out_edges_.assign(nodes_.size(), {});
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    const auto found = node_index_.find(edges_[index].from);
    if (found != node_index_.end()) {
      out_edges_[found->second].push_back(static_cast<std::uint32_t>(index));
    }
  }
}

const Node* FabricGraph::find_node(const NodeId& id) const noexcept {
  const auto found = node_index_.find(id);
  return found == node_index_.end() ? nullptr : &nodes_[found->second];
}

const Edge* FabricGraph::find_edge(const EdgeId& id) const noexcept {
  const auto found = edge_index_.find(id);
  return found == edge_index_.end() ? nullptr : &edges_[found->second];
}

const FailureDomain* FabricGraph::find_failure_domain(const FailureDomainId& id) const noexcept {
  const auto found = domain_index_.find(id);
  return found == domain_index_.end() ? nullptr : &failure_domains_[found->second];
}

std::uint32_t FabricGraph::node_index(const NodeId& id) const noexcept {
  const auto found = node_index_.find(id);
  return found == node_index_.end() ? kInvalidIndex : found->second;
}

const std::vector<std::uint32_t>& FabricGraph::out_edges(std::uint32_t index) const noexcept {
  static const std::vector<std::uint32_t> kEmpty{};
  return index < out_edges_.size() ? out_edges_[index] : kEmpty;
}

const std::vector<FailureDomainId>& FabricGraph::domain_chain(const FailureDomainId& id) const noexcept {
  static const std::vector<FailureDomainId> kEmpty{};
  const auto found = domain_index_.find(id);
  return found == domain_index_.end() ? kEmpty : domain_chains_[found->second];
}

bool FabricGraph::domain_contains(const FailureDomainId& ancestor, const FailureDomainId& candidate) const noexcept {
  if (!ancestor.valid() || !candidate.valid()) {
    return false;
  }
  for (const FailureDomainId& entry : domain_chain(candidate)) {
    if (entry == ancestor) {
      return true;
    }
  }
  return false;
}

void FabricGraph::encode_canonical(ByteWriter& writer) const {
  writer.tagged("fabric.v1");
  writer.u64(topology_generation_.value());
  writer.u64(failure_domain_generation_.value());
  writer.u64(capacity_evidence_generation_.value());
  writer.u32(static_cast<std::uint32_t>(failure_domains_.size()));
  for (const FailureDomain& domain : failure_domains_) {
    writer.string(domain.id.value());
    writer.string(domain.parent.value());
    writer.u8(static_cast<std::uint8_t>(domain.kind));
  }
  writer.u32(static_cast<std::uint32_t>(nodes_.size()));
  for (const Node& node : nodes_) {
    writer.string(node.id.value());
    writer.string(node.placement.site);
    writer.string(node.placement.pod);
    writer.string(node.placement.rack);
    writer.string(node.placement.host);
    writer.u32(node.placement.device_index);
    writer.boolean(node.placement.supplied);
    writer.string(node.tier.value());
    writer.boolean(node.eligible);
  }
  writer.u32(static_cast<std::uint32_t>(edges_.size()));
  for (const Edge& edge : edges_) {
    writer.string(edge.id.value());
    writer.string(edge.from.value());
    writer.string(edge.to.value());
    writer.u64(edge.capacity_mbps);
    writer.u64(edge.latency_micros);
    writer.u64(edge.reserved_mbps);
    writer.string(edge.tier.value());
    writer.string(edge.failure_domain.value());
    writer.u8(static_cast<std::uint8_t>(edge.evidence));
    writer.boolean(edge.eligible);
  }
}

Status FabricGraph::validate() const {
  if (nodes_.size() > kMaxNodes) {
    return Status::error(ErrorCode::kTooManyItems, "node count exceeds limit");
  }
  if (edges_.size() > kMaxEdges) {
    return Status::error(ErrorCode::kTooManyItems, "edge count exceeds limit");
  }
  if (failure_domains_.size() > kMaxFailureDomains) {
    return Status::error(ErrorCode::kTooManyItems, "failure domain count exceeds limit");
  }
  for (std::size_t index = 1; index < failure_domains_.size(); ++index) {
    if (!(failure_domains_[index - 1].id < failure_domains_[index].id)) {
      return Status::error(ErrorCode::kInconsistentMetadata, "failure domains are not in canonical order");
    }
  }
  for (std::size_t index = 1; index < nodes_.size(); ++index) {
    if (!(nodes_[index - 1].id < nodes_[index].id)) {
      return Status::error(ErrorCode::kInconsistentMetadata, "nodes are not in canonical order");
    }
  }
  for (std::size_t index = 1; index < edges_.size(); ++index) {
    const Edge& previous = edges_[index - 1];
    const Edge& current = edges_[index];
    const bool ordered = previous.from < current.from ||
                         (previous.from == current.from &&
                          (previous.to < current.to || (previous.to == current.to && previous.id < current.id)));
    if (!ordered) {
      return Status::error(ErrorCode::kInconsistentMetadata, "edges are not in canonical order");
    }
  }
  if (domain_chains_.size() != failure_domains_.size()) {
    return Status::error(ErrorCode::kInternalError, "failure domain chain index is inconsistent");
  }
  // Identity uniqueness is re-proved here as well, because validate() is used by
  // the persistence layer and by consumers that may build a graph by other means.
  {
    std::vector<std::string_view> edge_ids;
    edge_ids.reserve(edges_.size());
    for (const Edge& edge : edges_) {
      edge_ids.push_back(edge.id.value());
    }
    std::sort(edge_ids.begin(), edge_ids.end());
    if (std::adjacent_find(edge_ids.begin(), edge_ids.end()) != edge_ids.end()) {
      return Status::error(ErrorCode::kDuplicateIdentifier, "the fabric repeats an edge identity");
    }
    std::vector<std::string_view> node_ids;
    node_ids.reserve(nodes_.size());
    for (const Node& node : nodes_) {
      node_ids.push_back(node.id.value());
    }
    std::sort(node_ids.begin(), node_ids.end());
    if (std::adjacent_find(node_ids.begin(), node_ids.end()) != node_ids.end()) {
      return Status::error(ErrorCode::kDuplicateIdentifier, "the fabric repeats a node identity");
    }
  }
  for (const Edge& edge : edges_) {
    if (find_node(edge.from) == nullptr) {
      return Status::error(ErrorCode::kUnknownReference, "edge " + edge.id.value() + " references unknown node");
    }
    if (find_node(edge.to) == nullptr) {
      return Status::error(ErrorCode::kUnknownReference, "edge " + edge.id.value() + " references unknown node");
    }
    if (edge.from == edge.to) {
      return Status::error(ErrorCode::kInconsistentMetadata, "edge " + edge.id.value() + " is a self loop");
    }
    if (edge.reserved_mbps > edge.capacity_mbps) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "edge " + edge.id.value() + " reserves more than its capacity");
    }
    if (edge.failure_domain.valid() && find_failure_domain(edge.failure_domain) == nullptr) {
      return Status::error(ErrorCode::kUnknownReference,
                           "edge " + edge.id.value() + " references an unknown failure domain");
    }
  }
  for (const FailureDomain& domain : failure_domains_) {
    if (!domain.parent.valid()) {
      continue;
    }
    const FailureDomain* parent = find_failure_domain(domain.parent);
    if (parent == nullptr) {
      return Status::error(ErrorCode::kUnknownReference,
                           "failure domain " + domain.id.value() + " references an unknown parent");
    }
    if (!(static_cast<std::uint8_t>(parent->kind) > static_cast<std::uint8_t>(domain.kind))) {
      return Status::error(ErrorCode::kInconsistentMetadata,
                           "failure domain " + domain.id.value() + " is not strictly contained");
    }
  }
  for (const auto& [id, index] : node_index_) {
    if (index >= nodes_.size() || !(nodes_[index].id == id)) {
      return Status::error(ErrorCode::kInternalError, "node index is inconsistent");
    }
  }
  for (const auto& [id, index] : edge_index_) {
    if (index >= edges_.size() || !(edges_[index].id == id)) {
      return Status::error(ErrorCode::kInternalError, "edge index is inconsistent");
    }
  }
  return Status::ok();
}

}  // namespace cpath
