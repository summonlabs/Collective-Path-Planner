// Collective Path Planner - logical collective structure model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_COLLECTIVE_HPP
#define CPATH_COLLECTIVE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cpath/buffer.hpp"
#include "cpath/ids.hpp"
#include "cpath/limits.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Declared shape of a collective. Purely descriptive: it is bound into the plan
// digest so that a plan cannot be silently re-used for a different structure.
enum class CollectiveKind : std::uint8_t {
  kRing = 0,
  kTree = 1,
  kPairwise = 2,
  kAllToAll = 3,
  kHierarchical = 4,
};

const char* to_string(CollectiveKind value) noexcept;
bool parse_collective_kind(std::string_view text, CollectiveKind& out) noexcept;

// Direction of a tree-shaped group edge set.
enum class TreeDirection : std::uint8_t {
  kForward = 0,      // root -> member
  kReverse = 1,      // member -> root
  kBidirectional = 2,
};

const char* to_string(TreeDirection value) noexcept;
bool parse_tree_direction(std::string_view text, TreeDirection& out) noexcept;

// Pattern applied to the members of one group at one hierarchy level.
enum class GroupPattern : std::uint8_t {
  kRing = 0,
  kTree = 1,
  kPairwise = 2,
  kAllToAll = 3,
};

const char* to_string(GroupPattern value) noexcept;
bool parse_group_pattern(std::string_view text, GroupPattern& out) noexcept;

// One directed logical exchange between two participants.
struct LogicalEdge {
  ParticipantId src{};
  ParticipantId dst{};

  friend bool operator==(const LogicalEdge&, const LogicalEdge&) = default;
  friend std::strong_ordering operator<=>(const LogicalEdge&, const LogicalEdge&) = default;
};

// A declared group of participants at one hierarchy level.
struct GroupSpec {
  GroupId id{};
  std::uint32_t level{0};
  GroupPattern pattern{GroupPattern::kRing};
  TreeDirection direction{TreeDirection::kForward};
  ParticipantId root{};  // required by kTree, ignored otherwise
  std::vector<ParticipantId> members{};
};

// Unvalidated collective description.
struct CollectiveSpec {
  CollectiveId id{};
  CollectiveKind kind{CollectiveKind::kRing};
  // Declared nominal bandwidth each logical edge is expected to carry. Zero
  // means "not declared": scoring then ignores capacity sufficiency and the
  // plan proposes no allocation. A non-zero demand is a declaration, not a
  // reservation.
  std::uint64_t demand_mbps{0};
  std::vector<GroupSpec> groups{};
};

// Canonical collective: participants sorted by id, logical edges in canonical
// order with dense LogicalEdgeId values, and a stage index per logical edge
// derived from the declaring hierarchy level.
struct Collective {
  CollectiveId id{};
  CollectiveKind kind{CollectiveKind::kRing};
  std::uint64_t demand_mbps{0};
  std::vector<ParticipantId> participants{};
  std::vector<LogicalEdge> logical_edges{};
  // Stage of logical edge i, parallel to logical_edges.
  std::vector<std::uint32_t> logical_edge_stage{};
  std::vector<GroupSpec> groups{};  // canonical order: (level, id)
  std::uint32_t stage_count{1};

  LogicalEdgeId logical_edge_id(std::size_t index) const noexcept {
    return LogicalEdgeId{static_cast<std::uint64_t>(index) + 1u};
  }

  static Result<Collective> build(const CollectiveSpec& spec);
  static Result<Collective> build(CollectiveSpec&& spec);

  void encode_canonical(ByteWriter& writer) const;
  Status validate() const;
};

std::string encode_collective_section(const Collective& collective);

}  // namespace cpath

#endif  // CPATH_COLLECTIVE_HPP
