// Collective Path Planner - bounded, integrity-checked plan persistence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_PERSISTENCE_HPP
#define CPATH_PERSISTENCE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cpath/plan.hpp"
#include "cpath/request.hpp"
#include "cpath/status.hpp"

namespace cpath {

struct StoreConfig {
  std::string root{};  // directory holding the store
  std::size_t max_records{kMaxStoreRecords};
  std::size_t max_record_bytes{kMaxPlanRecordBytes};
  std::uint64_t max_total_bytes{kMaxStoreTotalBytes};
  // When true (default) a commit is durable before put() returns: temporary
  // file, flush, fsync, atomic replace, directory flush where supported.
  bool durable_commit{true};
};

// Summary of one durable record. Reading a summary does not make the plan
// current; freshness is a separate question answered by revalidate().
struct StoredPlanSummary {
  CollectivePlanId id{};
  CollectivePlanGeneration generation{};
  GenerationBindings bindings{};
  CollectiveId collective{};
  CollectiveKind kind{CollectiveKind::kRing};
  std::uint64_t stored_sequence{0};  // monotonic within this store
  std::uint64_t record_bytes{0};
  Incarnation written_incarnation{};  // process incarnation that committed it
  std::uint64_t written_unix_micros{0};

  friend bool operator==(const StoredPlanSummary&, const StoredPlanSummary&) = default;
};

enum class RecordIntegrity : std::uint8_t {
  kOk = 0,
  kMissing = 1,
  kCorrupt = 2,
  kTruncated = 3,
  kOversized = 4,
  kUnsupportedVersion = 5,
  kDigestMismatch = 6,
  kIncompatible = 7,
};

const char* to_string(RecordIntegrity value) noexcept;

// Outcome of asking whether a persisted plan still describes current inputs.
// A reopened plan is never assumed current: kUnverified until revalidate()
// succeeds against an explicit request.
struct RevalidationResult {
  bool still_valid{false};
  PlanFreshness freshness{PlanFreshness::kUnverified};
  ErrorCode code{ErrorCode::kOk};
  Digest current_input_digest{};
  Digest stored_input_digest{};
  std::optional<Plan> replanned{};
  std::string detail{};

  bool needs_replan() const noexcept { return !still_valid; }
};

// Durable plan store.
//
// Layout inside root:
//   store.meta           - versioned header: epoch, incarnation, sequence
//   plan-<digest>.rec    - one immutable plan record each
//   tmp-*.tmp            - transient staging files, never authoritative
//
// Authority rules enforced here:
//  - every record is written under a single incarnation and epoch;
//  - opening the store advances the incarnation, so a restart cannot resurrect
//    authority held by a previous process;
//  - a record read back from disk is reported as unverified until the caller
//    revalidates it against a live request;
//  - malformed, truncated, oversized or incompatible records are rejected
//    deterministically and never partially applied.
class PlanStore {
 public:
  explicit PlanStore(StoreConfig config);
  ~PlanStore();

  PlanStore(const PlanStore&) = delete;
  PlanStore& operator=(const PlanStore&) = delete;
  PlanStore(PlanStore&&) = delete;
  PlanStore& operator=(PlanStore&&) = delete;

  Status open();
  Status close();
  bool is_open() const noexcept;
  const StoreConfig& config() const noexcept { return config_; }
  Epoch epoch() const noexcept;
  Incarnation incarnation() const noexcept;
  std::size_t record_count() const noexcept;
  std::uint64_t total_bytes() const noexcept;

  // Commits the plan if and only if it was produced from request's canonical
  // digest. Returns the durable summary.
  Result<StoredPlanSummary> put(const Plan& plan, const PlanningRequest& request);

  Result<Plan> get(const CollectivePlanId& id) const;
  Result<StoredPlanSummary> summary(const CollectivePlanId& id) const;
  Result<std::vector<StoredPlanSummary>> list() const;

  // Re-checks a stored plan against current inputs. Also reports the freshness
  // classification, which is always non-current for a plan whose bound
  // generations differ from the request.
  Result<RevalidationResult> revalidate(const CollectivePlanId& id, const PlanningRequest& current) const;

  Status erase(const CollectivePlanId& id);
  // Full re-read of every record with integrity verification. Rebuilds the
  // in-memory index; records that fail verification are reported, not applied.
  Result<std::vector<StoredPlanSummary>> verify_all();
  // Drops records above the configured bound, oldest sequence first.
  Status enforce_bounds();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
  StoreConfig config_{};
};

// Low-level record codec, exposed for adversarial tests.
std::vector<std::uint8_t> encode_plan_record(const Plan& plan, const PlanningRequest& request,
                                             const Incarnation& incarnation, std::uint64_t sequence,
                                             std::uint64_t unix_micros);
Result<Plan> decode_plan_record(std::span<const std::uint8_t> bytes, StoredPlanSummary* summary_out);

}  // namespace cpath

#endif  // CPATH_PERSISTENCE_HPP
