// Collective Path Planner - logical collective structure model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/collective.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "cpath/buffer.hpp"

namespace cpath {
namespace {

// Canonical rotation of a directed cycle: the smallest member in byte order
// comes first. Rotations of the same cycle therefore collapse to one encoding,
// while a reversal (which changes the direction of every logical edge) does not.
void rotate_ring_canonical(std::vector<ParticipantId>& members) {
  if (members.empty()) {
    return;
  }
  const auto smallest = std::min_element(members.begin(), members.end());
  std::rotate(members.begin(), smallest, members.end());
}

Status append_group_edges(const GroupSpec& group, std::vector<std::pair<LogicalEdge, std::uint32_t>>& out) {
  const std::uint32_t stage = group.level;
  switch (group.pattern) {
    case GroupPattern::kRing: {
      if (group.members.size() < 2) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "ring group " + group.id.value() + " needs at least two members");
      }
      for (std::size_t index = 0; index < group.members.size(); ++index) {
        const ParticipantId& src = group.members[index];
        const ParticipantId& dst = group.members[(index + 1u) % group.members.size()];
        out.emplace_back(LogicalEdge{src, dst}, stage);
      }
      return Status::ok();
    }
    case GroupPattern::kTree: {
      if (group.members.size() < 2) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "tree group " + group.id.value() + " needs at least two members");
      }
      if (!group.root.valid()) {
        return Status::error(ErrorCode::kMissingRequiredField,
                             "tree group " + group.id.value() + " needs a root participant");
      }
      const bool root_is_member = std::find(group.members.begin(), group.members.end(), group.root) !=
                                  group.members.end();
      if (!root_is_member) {
        return Status::error(ErrorCode::kUnknownReference,
                             "tree group " + group.id.value() + " root is not a member of the group");
      }
      for (const ParticipantId& member : group.members) {
        if (member == group.root) {
          continue;
        }
        if (group.direction == TreeDirection::kForward || group.direction == TreeDirection::kBidirectional) {
          out.emplace_back(LogicalEdge{group.root, member}, stage);
        }
        if (group.direction == TreeDirection::kReverse || group.direction == TreeDirection::kBidirectional) {
          out.emplace_back(LogicalEdge{member, group.root}, stage);
        }
      }
      return Status::ok();
    }
    case GroupPattern::kPairwise: {
      if (group.members.size() % 2u != 0u) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "pairwise group " + group.id.value() + " needs an even number of members");
      }
      for (std::size_t index = 0; index + 1u < group.members.size(); index += 2u) {
        const ParticipantId& first = group.members[index];
        const ParticipantId& second = group.members[index + 1u];
        out.emplace_back(LogicalEdge{first, second}, stage);
        out.emplace_back(LogicalEdge{second, first}, stage);
      }
      return Status::ok();
    }
    case GroupPattern::kAllToAll: {
      if (group.members.size() < 2) {
        return Status::error(ErrorCode::kInconsistentMetadata,
                             "all-to-all group " + group.id.value() + " needs at least two members");
      }
      for (const ParticipantId& src : group.members) {
        for (const ParticipantId& dst : group.members) {
          if (!(src == dst)) {
            out.emplace_back(LogicalEdge{src, dst}, stage);
          }
        }
      }
      return Status::ok();
    }
  }
  return Status::error(ErrorCode::kInternalError, "unhandled group pattern");
}

}  // namespace

const char* to_string(CollectiveKind value) noexcept {
  switch (value) {
    case CollectiveKind::kRing:
      return "ring";
    case CollectiveKind::kTree:
      return "tree";
    case CollectiveKind::kPairwise:
      return "pairwise";
    case CollectiveKind::kAllToAll:
      return "alltoall";
    case CollectiveKind::kHierarchical:
      return "hierarchical";
  }
  return "ring";
}

bool parse_collective_kind(std::string_view text, CollectiveKind& out) noexcept {
  if (text == "ring") {
    out = CollectiveKind::kRing;
    return true;
  }
  if (text == "tree") {
    out = CollectiveKind::kTree;
    return true;
  }
  if (text == "pairwise") {
    out = CollectiveKind::kPairwise;
    return true;
  }
  if (text == "alltoall") {
    out = CollectiveKind::kAllToAll;
    return true;
  }
  if (text == "hierarchical") {
    out = CollectiveKind::kHierarchical;
    return true;
  }
  return false;
}

const char* to_string(TreeDirection value) noexcept {
  switch (value) {
    case TreeDirection::kForward:
      return "forward";
    case TreeDirection::kReverse:
      return "reverse";
    case TreeDirection::kBidirectional:
      return "bidirectional";
  }
  return "forward";
}

bool parse_tree_direction(std::string_view text, TreeDirection& out) noexcept {
  if (text == "forward") {
    out = TreeDirection::kForward;
    return true;
  }
  if (text == "reverse") {
    out = TreeDirection::kReverse;
    return true;
  }
  if (text == "bidirectional") {
    out = TreeDirection::kBidirectional;
    return true;
  }
  return false;
}

const char* to_string(GroupPattern value) noexcept {
  switch (value) {
    case GroupPattern::kRing:
      return "ring";
    case GroupPattern::kTree:
      return "tree";
    case GroupPattern::kPairwise:
      return "pairwise";
    case GroupPattern::kAllToAll:
      return "alltoall";
  }
  return "ring";
}

bool parse_group_pattern(std::string_view text, GroupPattern& out) noexcept {
  if (text == "ring") {
    out = GroupPattern::kRing;
    return true;
  }
  if (text == "tree") {
    out = GroupPattern::kTree;
    return true;
  }
  if (text == "pairwise") {
    out = GroupPattern::kPairwise;
    return true;
  }
  if (text == "alltoall") {
    out = GroupPattern::kAllToAll;
    return true;
  }
  return false;
}

Result<Collective> Collective::build(const CollectiveSpec& spec) {
  CollectiveSpec copy = spec;
  return build(std::move(copy));
}

Result<Collective> Collective::build(CollectiveSpec&& spec) {
  if (!is_valid_identifier(spec.id.value())) {
    return Result<Collective>::failure(ErrorCode::kInvalidCharacter, "collective id is not a valid identifier");
  }
  if (spec.groups.empty()) {
    return Result<Collective>::failure(ErrorCode::kEmptyCollective, "collective declares no groups");
  }
  if (spec.groups.size() > kMaxGroups) {
    return Result<Collective>::failure(ErrorCode::kTooManyItems, "group count exceeds limit");
  }

  if (spec.demand_mbps > kMaxCapacityMbps) {
    return Result<Collective>::failure(ErrorCode::kValueOutOfRange,
                                       "declared collective demand exceeds the model ceiling");
  }

  Collective result;
  result.id = spec.id;
  result.kind = spec.kind;
  result.demand_mbps = spec.demand_mbps;

  // --- groups ---
  for (GroupSpec& group : spec.groups) {
    if (!is_valid_identifier(group.id.value())) {
      return Result<Collective>::failure(ErrorCode::kInvalidCharacter, "group id is not a valid identifier");
    }
    if (group.level >= kMaxHierarchyLevels) {
      return Result<Collective>::failure(ErrorCode::kValueOutOfRange,
                                         "group level exceeds the supported hierarchy depth");
    }
    if (group.members.empty()) {
      return Result<Collective>::failure(ErrorCode::kEmptyCollective,
                                         "group " + group.id.value() + " declares no members");
    }
    if (group.members.size() > kMaxGroupMembers) {
      return Result<Collective>::failure(ErrorCode::kTooManyItems,
                                         "group " + group.id.value() + " declares too many members");
    }
    for (const ParticipantId& member : group.members) {
      if (!is_valid_identifier(member.value())) {
        return Result<Collective>::failure(ErrorCode::kInvalidCharacter,
                                           "participant id is not a valid identifier");
      }
    }
    if (group.root.valid() && !is_valid_identifier(group.root.value())) {
      return Result<Collective>::failure(ErrorCode::kInvalidCharacter, "group root is not a valid identifier");
    }

    // Member order is semantically meaningful only for rings, where it defines
    // the cycle. Every other pattern is order-insensitive by construction.
    if (group.pattern == GroupPattern::kRing) {
      std::vector<ParticipantId> sorted = group.members;
      std::sort(sorted.begin(), sorted.end());
      if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return Result<Collective>::failure(ErrorCode::kDuplicateIdentifier,
                                           "ring group " + group.id.value() + " repeats a participant");
      }
      rotate_ring_canonical(group.members);
    } else {
      std::sort(group.members.begin(), group.members.end());
      if (std::adjacent_find(group.members.begin(), group.members.end()) != group.members.end()) {
        return Result<Collective>::failure(ErrorCode::kDuplicateIdentifier,
                                           "group " + group.id.value() + " repeats a participant");
      }
    }
  }

  std::sort(spec.groups.begin(), spec.groups.end(), [](const GroupSpec& lhs, const GroupSpec& rhs) {
    if (lhs.level != rhs.level) {
      return lhs.level < rhs.level;
    }
    return lhs.id < rhs.id;
  });
  for (std::size_t index = 1; index < spec.groups.size(); ++index) {
    if (spec.groups[index].id == spec.groups[index - 1].id) {
      return Result<Collective>::failure(ErrorCode::kDuplicateIdentifier,
                                         "duplicate group id " + spec.groups[index].id.value());
    }
  }
  result.groups = std::move(spec.groups);

  // --- derive logical edges ---
  std::vector<std::pair<LogicalEdge, std::uint32_t>> derived;
  for (const GroupSpec& group : result.groups) {
    if (Status status = append_group_edges(group, derived); !status.is_ok()) {
      return Result<Collective>::failure(status);
    }
  }
  if (derived.empty()) {
    return Result<Collective>::failure(ErrorCode::kEmptyCollective, "collective derives no logical edges");
  }
  if (derived.size() > kMaxLogicalEdges) {
    return Result<Collective>::failure(ErrorCode::kTooManyItems, "derived logical edge count exceeds limit");
  }

  // A logical edge may be implied by more than one group. It is one logical
  // exchange either way, so it is de-duplicated and attributed to the earliest
  // stage that implies it.
  std::sort(derived.begin(), derived.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.first.src != rhs.first.src) {
      return lhs.first.src < rhs.first.src;
    }
    if (lhs.first.dst != rhs.first.dst) {
      return lhs.first.dst < rhs.first.dst;
    }
    return lhs.second < rhs.second;
  });

  std::uint32_t stage_count = 1;
  for (std::size_t index = 0; index < derived.size(); ++index) {
    if (derived[index].first.src == derived[index].first.dst) {
      return Result<Collective>::failure(ErrorCode::kSelfLoopLogicalEdge,
                                         "logical edge from " + derived[index].first.src.value() + " to itself");
    }
    if (index > 0 && derived[index].first == derived[index - 1].first) {
      continue;  // duplicate from a later group; attributed to the first stage
    }
    result.logical_edges.push_back(derived[index].first);
    result.logical_edge_stage.push_back(derived[index].second);
    stage_count = std::max(stage_count, derived[index].second + 1u);
  }
  result.stage_count = stage_count;

  // --- participants ---
  std::vector<ParticipantId> participants;
  participants.reserve(result.logical_edges.size() * 2u);
  for (const LogicalEdge& edge : result.logical_edges) {
    participants.push_back(edge.src);
    participants.push_back(edge.dst);
  }
  std::sort(participants.begin(), participants.end());
  participants.erase(std::unique(participants.begin(), participants.end()), participants.end());
  if (participants.size() > kMaxParticipants) {
    return Result<Collective>::failure(ErrorCode::kTooManyItems, "participant count exceeds limit");
  }
  result.participants = std::move(participants);

  if (Status status = result.validate(); !status.is_ok()) {
    return Result<Collective>::failure(status);
  }
  return Result<Collective>::success(std::move(result));
}

void Collective::encode_canonical(ByteWriter& writer) const {
  writer.tagged("collective.v1");
  writer.string(id.value());
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u64(demand_mbps);
  writer.u32(static_cast<std::uint32_t>(stage_count));
  writer.u32(static_cast<std::uint32_t>(participants.size()));
  for (const ParticipantId& participant : participants) {
    writer.string(participant.value());
  }
  writer.u32(static_cast<std::uint32_t>(logical_edges.size()));
  for (std::size_t index = 0; index < logical_edges.size(); ++index) {
    writer.string(logical_edges[index].src.value());
    writer.string(logical_edges[index].dst.value());
    writer.u32(logical_edge_stage[index]);
  }
  writer.u32(static_cast<std::uint32_t>(groups.size()));
  for (const GroupSpec& group : groups) {
    writer.string(group.id.value());
    writer.u32(group.level);
    writer.u8(static_cast<std::uint8_t>(group.pattern));
    writer.u8(static_cast<std::uint8_t>(group.direction));
    writer.string(group.root.value());
    writer.u32(static_cast<std::uint32_t>(group.members.size()));
    for (const ParticipantId& member : group.members) {
      writer.string(member.value());
    }
  }
}

Status Collective::validate() const {
  if (logical_edges.empty()) {
    return Status::error(ErrorCode::kEmptyCollective, "collective has no logical edges");
  }
  if (logical_edges.size() > kMaxLogicalEdges) {
    return Status::error(ErrorCode::kTooManyItems, "logical edge count exceeds limit");
  }
  if (logical_edge_stage.size() != logical_edges.size()) {
    return Status::error(ErrorCode::kInternalError, "stage index is not parallel to the logical edge list");
  }
  for (std::size_t index = 1; index < logical_edges.size(); ++index) {
    if (!(logical_edges[index - 1] < logical_edges[index])) {
      return Status::error(ErrorCode::kInconsistentMetadata, "logical edges are not in canonical order");
    }
  }
  for (const LogicalEdge& edge : logical_edges) {
    if (edge.src == edge.dst) {
      return Status::error(ErrorCode::kSelfLoopLogicalEdge, "logical edge is a self loop");
    }
  }
  for (std::size_t index = 1; index < participants.size(); ++index) {
    if (!(participants[index - 1] < participants[index])) {
      return Status::error(ErrorCode::kInconsistentMetadata, "participants are not in canonical order");
    }
  }
  if (stage_count == 0) {
    return Status::error(ErrorCode::kInconsistentMetadata, "collective reports zero stages");
  }
  for (const std::uint32_t stage : logical_edge_stage) {
    if (stage >= stage_count) {
      return Status::error(ErrorCode::kInconsistentMetadata, "logical edge stage is out of range");
    }
  }
  return Status::ok();
}

}  // namespace cpath
