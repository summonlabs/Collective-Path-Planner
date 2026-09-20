// Collective Path Planner - planning policy model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_POLICY_HPP
#define CPATH_POLICY_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/fabric.hpp"
#include "cpath/ids.hpp"
#include "cpath/limits.hpp"
#include "cpath/status.hpp"

namespace cpath {

// How strictly the paths chosen for one logical edge must avoid sharing
// physical resources with each other.
enum class Disjointness : std::uint8_t {
  kNone = 0,  // paths may share edges and nodes
  kEdge = 1,  // no two paths share a physical edge
  kNode = 2,  // no two paths share an intermediate physical node
};

const char* to_string(Disjointness value) noexcept;
bool parse_disjointness(std::string_view text, Disjointness& out) noexcept;

// Whether chosen paths must land in independent failure domains.
enum class DomainDiversity : std::uint8_t {
  kNone = 0,
  kPreferred = 1,  // used as a tie-break in scoring, never a hard failure
  kRequired = 2,   // hard failure when unsatisfiable
};

const char* to_string(DomainDiversity value) noexcept;
bool parse_domain_diversity(std::string_view text, DomainDiversity& out) noexcept;

// Minimum provenance accepted for the capacity used by a path.
enum class EvidenceRequirement : std::uint8_t {
  kAny = 0,
  kSyntheticOrBetter = 1,
  kMeasured = 2,
};

const char* to_string(EvidenceRequirement value) noexcept;
bool parse_evidence_requirement(std::string_view text, EvidenceRequirement& out) noexcept;

// The planning policy. A policy never grants authority: it only constrains
// which mappings this planner is willing to propose.
struct Policy {
  PolicyGeneration generation{};
  std::size_t max_hops{8};
  std::size_t paths_per_logical_edge{1};
  Disjointness disjointness{Disjointness::kNone};
  DomainDiversity domain_diversity{DomainDiversity::kNone};
  DomainKind domain_diversity_level{DomainKind::kRack};
  EvidenceRequirement evidence{EvidenceRequirement::kAny};
  // Fixed-point weights in thousandths. Cost = (latency_micros * latency_weight
  // + congestion_penalty * congestion_weight) / 1000, computed with saturating
  // arithmetic.
  std::uint64_t latency_weight_milli{1000};
  std::uint64_t congestion_weight_milli{0};
  std::uint64_t max_path_cost{kMaxPathCostLimit};
  // When false, an edge with zero verified capacity is unusable. When true such
  // an edge may carry a path, but the plan records a zero bottleneck and the
  // weakest evidence class it relied on, so the gap is explicit rather than
  // silently treated as capacity.
  bool allow_unverified_capacity{false};

  std::vector<NodeId> forbidden_nodes{};
  std::vector<TierId> forbidden_tiers{};
  std::vector<FailureDomainId> forbidden_failure_domains{};
  // When non-empty, every hop must sit on one of these tiers.
  std::vector<TierId> allowed_path_tiers{};

  // Search budget. Bounds planner work independently of graph size.
  std::size_t max_search_expansions{1u << 18};

  void encode_canonical(ByteWriter& writer) const;
  Status validate() const;
  // Sorts and de-duplicates every set-valued field. Called by the request builder.
  void canonicalize();
};

std::string encode_policy_section(const Policy& policy);

}  // namespace cpath

#endif  // CPATH_POLICY_HPP
