// Collective Path Planner - strongly typed identities and generations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_IDS_HPP
#define CPATH_IDS_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "cpath/digest.hpp"
#include "cpath/limits.hpp"
#include "cpath/status.hpp"

namespace cpath {

// Strongly typed identifier over a representation type. Default-constructed
// values are *invalid* sentinels: an empty string or a zero generation. The
// library never treats an invalid identity as a match.
template <class Tag, class Rep>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongId() = default;
  constexpr explicit StrongId(Rep value) : value_(std::move(value)) {}

  const Rep& value() const noexcept { return value_; }
  Rep& mutable_value() noexcept { return value_; }

  bool valid() const noexcept {
    if constexpr (std::is_same_v<Rep, std::string>) {
      return !value_.empty();
    } else if constexpr (std::is_same_v<Rep, Digest>) {
      return !value_.is_zero();
    } else {
      return value_ != Rep{0};
    }
  }

  friend bool operator==(const StrongId&, const StrongId&) = default;
  friend std::strong_ordering operator<=>(const StrongId&, const StrongId&) = default;

 private:
  Rep value_{};
};

template <class Tag, class Rep>
struct StrongIdHash {
  std::size_t operator()(const StrongId<Tag, Rep>& id) const noexcept {
    if constexpr (std::is_same_v<Rep, Digest>) {
      return DigestHash{}(id.value());
    } else {
      return std::hash<Rep>{}(id.value());
    }
  }
};

// Dense index table keyed by a strong identity. Iteration order is never used
// for anything observable; canonical ordering always comes from sorted vectors.
template <class Id>
using IdMap = std::unordered_map<Id, std::uint32_t, StrongIdHash<typename Id::tag_type, typename Id::rep_type>>;

// --- Tag types -------------------------------------------------------------
struct NodeIdTag;
struct EdgeIdTag;
struct ParticipantIdTag;
struct LogicalEdgeIdTag;
struct PathIdTag;
struct CollectiveIdTag;
struct GroupIdTag;
struct FailureDomainIdTag;
struct TierIdTag;
struct CollectivePlanIdTag;
struct TopologyGenerationTag;
struct FailureDomainGenerationTag;
struct CapacityEvidenceGenerationTag;
struct PolicyGenerationTag;
struct CollectivePlanGenerationTag;
struct EpochTag;
struct IncarnationTag;
struct SessionIdTag;
struct RequestIdTag;

// --- Identities ------------------------------------------------------------
using NodeId = StrongId<NodeIdTag, std::string>;
using EdgeId = StrongId<EdgeIdTag, std::string>;
using ParticipantId = StrongId<ParticipantIdTag, std::string>;
using LogicalEdgeId = StrongId<LogicalEdgeIdTag, std::uint64_t>;
using PathId = StrongId<PathIdTag, std::uint64_t>;
using CollectiveId = StrongId<CollectiveIdTag, std::string>;
using GroupId = StrongId<GroupIdTag, std::string>;
using FailureDomainId = StrongId<FailureDomainIdTag, std::string>;
using TierId = StrongId<TierIdTag, std::string>;
using CollectivePlanId = StrongId<CollectivePlanIdTag, Digest>;
using SessionId = StrongId<SessionIdTag, std::uint64_t>;
using RequestId = StrongId<RequestIdTag, std::uint64_t>;

// --- Generations, epochs and incarnations ---------------------------------
// Every one of these is a monotonic fence. A plan binds all of them; an
// observed value that is not the expected value invalidates authority.
using TopologyGeneration = StrongId<TopologyGenerationTag, std::uint64_t>;
using FailureDomainGeneration = StrongId<FailureDomainGenerationTag, std::uint64_t>;
using CapacityEvidenceGeneration = StrongId<CapacityEvidenceGenerationTag, std::uint64_t>;
using PolicyGeneration = StrongId<PolicyGenerationTag, std::uint64_t>;
using CollectivePlanGeneration = StrongId<CollectivePlanGenerationTag, std::uint64_t>;
using Epoch = StrongId<EpochTag, std::uint64_t>;
using Incarnation = StrongId<IncarnationTag, std::uint64_t>;

// --- Identifier grammar ----------------------------------------------------
// Identifiers are opaque byte strings restricted to a conservative ASCII set
// so that they are safe in text formats, file names and logs. Canonical
// ordering is byte-wise lexicographic order.
bool is_valid_identifier(std::string_view text) noexcept;
bool is_valid_tier_name(std::string_view text) noexcept;

// Validates and returns the identifier, or a deterministic error.
Result<std::string> make_identifier(std::string_view text);
Result<std::string> make_tier_name(std::string_view text);

// Hex rendering of generation-valued ids for logs and machine output.
template <class Tag>
std::string to_decimal(const StrongId<Tag, std::uint64_t>& id) {
  return std::to_string(id.value());
}

std::string to_string(const NodeId& id);
std::string to_string(const EdgeId& id);
std::string to_string(const ParticipantId& id);
std::string to_string(const LogicalEdgeId& id);
std::string to_string(const PathId& id);
std::string to_string(const CollectiveId& id);
std::string to_string(const GroupId& id);
std::string to_string(const FailureDomainId& id);
std::string to_string(const TierId& id);
std::string to_string(const CollectivePlanId& id);
std::string to_string(const SessionId& id);

}  // namespace cpath

#endif  // CPATH_IDS_HPP
