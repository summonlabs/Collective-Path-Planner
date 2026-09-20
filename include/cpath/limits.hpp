// Collective Path Planner - hard resource and format bounds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_LIMITS_HPP
#define CPATH_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace cpath {

// Wire, storage and canonical-encoding format versions owned by this library.
inline constexpr std::uint16_t kCanonicalFormatVersion = 1;
inline constexpr std::uint16_t kStoreFormatVersion = 1;
inline constexpr std::uint16_t kWireProtocolVersion = 1;

// Identity and grammar bounds.
inline constexpr std::size_t kMaxIdentifierBytes = 64;
inline constexpr std::size_t kMaxTierNameBytes = 32;
inline constexpr std::size_t kMaxPlacementLabelBytes = 32;
inline constexpr std::size_t kMaxTextLineBytes = 4096;
inline constexpr std::size_t kMaxInputBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kMaxDetailBytes = 512;

// Model bounds. Every externally supplied count is checked against these
// before it reaches an allocation.
inline constexpr std::size_t kMaxNodes = 65536;
inline constexpr std::size_t kMaxEdges = 1048576;
inline constexpr std::size_t kMaxFailureDomains = 65536;
inline constexpr std::size_t kMaxTiers = 256;
inline constexpr std::size_t kMaxParticipants = 65536;
inline constexpr std::size_t kMaxLogicalEdges = 262144;
inline constexpr std::size_t kMaxBindings = 65536;
inline constexpr std::size_t kMaxGroups = 4096;
inline constexpr std::size_t kMaxGroupMembers = 65536;
inline constexpr std::size_t kMaxPolicySetEntries = 4096;

// Planning bounds.
inline constexpr std::size_t kMinPathsPerLogicalEdge = 1;
inline constexpr std::size_t kMaxPathsPerLogicalEdge = 8;
inline constexpr std::size_t kMaxHops = 256;
inline constexpr std::size_t kMaxStages = 64;
inline constexpr std::size_t kMaxHierarchyLevels = 8;
// Upper bound on the retained candidate path set per logical edge. The
// combination search is exponential in this value, so it stays small.
inline constexpr std::size_t kMaxCandidatePaths = 16;
// Hard ceiling on planner search work per request, independent of graph size.
inline constexpr std::size_t kMaxSearchExpansions = 1u << 24;
// Hard ceilings on the completeness-oriented search bounds. Reaching one of
// these produces an INDETERMINATE result, never an infeasibility claim.
inline constexpr std::size_t kMaxExhaustivePaths = 512;
inline constexpr std::size_t kMaxCombinationSteps = 1u << 22;
inline constexpr std::size_t kMaxSetAlternatives = 64;
inline constexpr std::size_t kMaxGlobalSearchNodes = 1u << 24;

// Scoring bounds. Costs saturate below this value; nothing wraps.
inline constexpr std::uint64_t kCostCeiling = (1ull << 62);
inline constexpr std::uint64_t kMaxScoringWeightMilli = 1000000;
inline constexpr std::uint64_t kMaxCapacityMbps = (1ull << 40);
inline constexpr std::uint64_t kMaxLatencyMicros = (1ull << 40);
inline constexpr std::uint64_t kMaxPathCostLimit = kCostCeiling;

// Persistence bounds.
inline constexpr std::size_t kMaxPlanRecordBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxStoreRecords = 4096;
inline constexpr std::size_t kMaxStoreTotalBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t kMaxStorePathBytes = 4096;

// Protocol bounds.
inline constexpr std::size_t kMaxFramePayloadBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxFrameBytes = kMaxFramePayloadBytes + 128;
inline constexpr std::size_t kMaxSessions = 32;
inline constexpr std::size_t kMaxPendingRequestsPerSession = 64;
inline constexpr std::size_t kMaxSessionQueueDepth = 256;
inline constexpr std::size_t kMaxJsonishStringBytes = 4096;

}  // namespace cpath

#endif  // CPATH_LIMITS_HPP
