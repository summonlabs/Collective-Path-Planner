// Collective Path Planner - bounded, integrity-checked plan persistence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/persistence.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cpath {
namespace {

constexpr char kRecordMagic[4] = {'C', 'P', 'P', 'R'};
constexpr char kMetaMagic[4] = {'C', 'P', 'P', 'S'};
constexpr std::size_t kMetaBytes = 34;      // magic + version + epoch + incarnation + sequence + crc
constexpr std::size_t kRecordDigests = 64;  // payload sha256 followed by the request digest
constexpr std::size_t kRecordFileHeaderBytes = 14;

std::uint64_t unix_micros_now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string hex_of(std::uint64_t value) {
  static const char* kDigits = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    out[15 - index] = kDigits[(value >> (4u * index)) & 0xFu];
  }
  return out;
}

std::string record_file_name(const Digest& digest) { return "plan-" + digest.hex() + ".rec"; }

Status filesystem_failure(const char* what, const std::filesystem::path& path, const std::error_code& error) {
  return Status::error(ErrorCode::kStoreIoFailure,
                       std::string(what) + " " + path.string() + ": " + error.message());
}

// Writes bytes to a staging file in the target directory, makes them durable when
// requested, then atomically replaces the target. No reader ever observes a
// partially written record.
Status write_file_atomic(const std::filesystem::path& target, const std::vector<std::uint8_t>& bytes,
                         bool durable) {
  static std::mutex counter_mutex;
  static std::uint64_t counter = 0;
  std::uint64_t token = 0;
  {
    std::lock_guard<std::mutex> lock(counter_mutex);
    token = ++counter;
  }
  const std::filesystem::path staging =
      target.parent_path() / ("tmp-" + hex_of(token) + "-" + hex_of(unix_micros_now()) + ".tmp");

  // Low-level writes are used deliberately: the durability flush must apply to
  // the very handle the bytes were written through, before the replace.
  {
    bool failed = false;
#ifdef _WIN32
    const HANDLE handle = ::CreateFileA(staging.string().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return Status::error(ErrorCode::kStoreIoFailure, "cannot create staging file " + staging.string());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
      const std::size_t remaining = bytes.size() - written;
      const DWORD chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
      DWORD produced = 0;
      if (::WriteFile(handle, bytes.data() + written, chunk, &produced, nullptr) == FALSE ||
          produced == 0) {
        failed = true;
        break;
      }
      written += static_cast<std::size_t>(produced);
    }
    if (!failed && durable) {
      failed = ::FlushFileBuffers(handle) == FALSE;
    }
    ::CloseHandle(handle);
#else
    const int descriptor = ::open(staging.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
      return Status::error(ErrorCode::kStoreIoFailure, "cannot create staging file " + staging.string());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
      const std::size_t remaining = bytes.size() - written;
      const std::size_t chunk = remaining > (1u << 20) ? (1u << 20) : remaining;
      const long result = static_cast<long>(::write(descriptor, bytes.data() + written, chunk));
      if (result <= 0) {
        failed = true;
        break;
      }
      written += static_cast<std::size_t>(result);
    }
    if (!failed && durable) {
      failed = ::fsync(descriptor) != 0;
    }
    (void)::close(descriptor);
#endif
    if (failed) {
      std::error_code ignored;
      std::filesystem::remove(staging, ignored);
      return Status::error(ErrorCode::kStoreIoFailure, "failed to write staging file " + staging.string());
    }
  }

#ifdef _WIN32
  if (::MoveFileExA(staging.string().c_str(), target.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return Status::error(ErrorCode::kAtomicReplaceFailed,
                         "atomic replace of " + target.string() + " failed: " + error.message());
  }
#else
  std::error_code error;
  std::filesystem::rename(staging, target, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return Status::error(ErrorCode::kAtomicReplaceFailed,
                         "atomic replace of " + target.string() + " failed: " + error.message());
  }
  if (durable) {
    const int directory = ::open(target.parent_path().string().c_str(), O_RDONLY);
    if (directory >= 0) {
      (void)::fsync(directory);
      (void)::close(directory);
    }
  }
#endif
  return Status::ok();
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::size_t max_bytes) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStoreIoFailure,
                                                      "cannot open " + path.string());
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStoreIoFailure,
                                                      "cannot size " + path.string());
  }
  if (size > static_cast<std::uintmax_t>(max_bytes)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kOversizedRecord,
                                                      path.string() + " exceeds the record bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size), 0u);
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
      return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kTruncatedRecord,
                                                        "short read from " + path.string());
    }
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::vector<std::uint8_t> encode_meta(const Epoch& epoch, const Incarnation& incarnation,
                                      std::uint64_t sequence) {
  ByteWriter writer;
  writer.raw(reinterpret_cast<const std::uint8_t*>(kMetaMagic), sizeof(kMetaMagic));
  writer.u16(kStoreFormatVersion);
  writer.u64(epoch.value());
  writer.u64(incarnation.value());
  writer.u64(sequence);
  const std::uint32_t crc = crc32(writer.bytes().data(), writer.bytes().size());
  writer.u32(crc);
  return writer.take();
}

Status decode_meta(const std::vector<std::uint8_t>& bytes, Epoch& epoch, Incarnation& incarnation,
                   std::uint64_t& sequence) {
  if (bytes.size() != kMetaBytes) {
    return Status::error(ErrorCode::kCorruptRecord, "store metadata has an unexpected size");
  }
  const std::uint32_t expected = crc32(bytes.data(), bytes.size() - 4u);
  const std::uint32_t stored = static_cast<std::uint32_t>(bytes[bytes.size() - 4u]) |
                               (static_cast<std::uint32_t>(bytes[bytes.size() - 3u]) << 8u) |
                               (static_cast<std::uint32_t>(bytes[bytes.size() - 2u]) << 16u) |
                               (static_cast<std::uint32_t>(bytes[bytes.size() - 1u]) << 24u);
  if (expected != stored) {
    return Status::error(ErrorCode::kCorruptRecord, "store metadata failed its integrity check");
  }
  ByteReader reader(bytes);
  std::span<const std::uint8_t> magic;
  reader.raw(sizeof(kMetaMagic), magic);
  if (Status status = reader.status(); !status.is_ok()) {
    return status;
  }
  if (std::memcmp(magic.data(), kMetaMagic, sizeof(kMetaMagic)) != 0) {
    return Status::error(ErrorCode::kCorruptRecord, "store metadata magic is wrong");
  }
  std::uint16_t version = 0;
  std::uint64_t epoch_value = 0;
  std::uint64_t incarnation_value = 0;
  reader.u16(version);
  reader.u64(epoch_value);
  reader.u64(incarnation_value);
  reader.u64(sequence);
  if (Status status = reader.status(); !status.is_ok()) {
    return status;
  }
  if (version != kStoreFormatVersion) {
    return Status::error(ErrorCode::kUnsupportedRecordVersion,
                         "store metadata format version " + std::to_string(version) + " is not supported");
  }
  epoch = Epoch{epoch_value};
  incarnation = Incarnation{incarnation_value};
  return Status::ok();
}

}  // namespace

const char* to_string(RecordIntegrity value) noexcept {
  switch (value) {
    case RecordIntegrity::kOk:
      return "ok";
    case RecordIntegrity::kMissing:
      return "missing";
    case RecordIntegrity::kCorrupt:
      return "corrupt";
    case RecordIntegrity::kTruncated:
      return "truncated";
    case RecordIntegrity::kOversized:
      return "oversized";
    case RecordIntegrity::kUnsupportedVersion:
      return "unsupported-version";
    case RecordIntegrity::kDigestMismatch:
      return "digest-mismatch";
    case RecordIntegrity::kIncompatible:
      return "incompatible";
  }
  return "missing";
}

std::vector<std::uint8_t> encode_plan_record(const Plan& plan, const PlanningRequest& request,
                                             const Incarnation& incarnation, std::uint64_t sequence,
                                             std::uint64_t unix_micros) {
  const std::vector<std::uint8_t> payload_plan = plan.encode();
  const Digest payload_digest = Sha256::hash(payload_plan.data(), payload_plan.size());

  ByteWriter header;
  header.raw(reinterpret_cast<const std::uint8_t*>(kRecordMagic), sizeof(kRecordMagic));
  header.u16(kStoreFormatVersion);
  const std::size_t header_length_position = header.size();
  header.u32(0u);  // header length field, back-patched once the header is complete
  header.u32(static_cast<std::uint32_t>(payload_plan.size() + kRecordDigests));
  header.u64(incarnation.value());
  header.u64(sequence);
  header.u64(unix_micros);
  header.u64(plan.generation.value());
  header.u64(plan.bindings.topology.value());
  header.u64(plan.bindings.failure_domains.value());
  header.u64(plan.bindings.capacity_evidence.value());
  header.u64(plan.bindings.policy.value());
  header.u64(plan.bindings.plan.value());
  header.string(plan.collective.value());
  header.u8(static_cast<std::uint8_t>(plan.kind));
  header.digest(plan.input_digest);

  std::vector<std::uint8_t> header_bytes = header.take();
  const std::uint32_t header_length = static_cast<std::uint32_t>(header_bytes.size());
  header_bytes[header_length_position] = static_cast<std::uint8_t>(header_length & 0xFFu);
  header_bytes[header_length_position + 1u] = static_cast<std::uint8_t>((header_length >> 8u) & 0xFFu);
  header_bytes[header_length_position + 2u] = static_cast<std::uint8_t>((header_length >> 16u) & 0xFFu);
  header_bytes[header_length_position + 3u] = static_cast<std::uint8_t>((header_length >> 24u) & 0xFFu);
  const std::uint32_t header_crc = crc32(header_bytes.data(), header_bytes.size());

  ByteWriter writer;
  writer.raw(header_bytes);
  writer.u32(header_crc);
  writer.raw(payload_plan);
  writer.digest(payload_digest);
  writer.digest(request.canonical_digest);
  return writer.take();
}

Result<Plan> decode_plan_record(std::span<const std::uint8_t> bytes, StoredPlanSummary* summary_out) {
  if (bytes.size() > kMaxPlanRecordBytes + 4096u) {
    return Result<Plan>::failure(ErrorCode::kOversizedRecord, "record exceeds the accepted bound");
  }
  if (bytes.size() < kRecordFileHeaderBytes) {
    return Result<Plan>::failure(ErrorCode::kTruncatedRecord, "record is shorter than its file header");
  }
  if (std::memcmp(bytes.data(), kRecordMagic, sizeof(kRecordMagic)) != 0) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "record magic is wrong");
  }

  ByteReader prefix(bytes);
  std::span<const std::uint8_t> magic;
  prefix.raw(sizeof(kRecordMagic), magic);
  std::uint16_t version = 0;
  std::uint32_t header_length = 0;
  std::uint32_t payload_length = 0;
  prefix.u16(version);
  prefix.u32(header_length);
  prefix.u32(payload_length);
  if (Status status = prefix.status(); !status.is_ok()) {
    return Result<Plan>::failure(status);
  }
  if (version != kStoreFormatVersion) {
    return Result<Plan>::failure(ErrorCode::kUnsupportedRecordVersion,
                                 "record format version " + std::to_string(version) + " is not supported");
  }
  if (payload_length > kMaxPlanRecordBytes || header_length > kMaxPlanRecordBytes) {
    return Result<Plan>::failure(ErrorCode::kOversizedRecord, "record declares an oversize section");
  }
  if (payload_length < kRecordDigests) {
    return Result<Plan>::failure(ErrorCode::kTruncatedRecord, "record payload cannot hold its digests");
  }
  const std::size_t total =
      static_cast<std::size_t>(header_length) + 4u + static_cast<std::size_t>(payload_length);
  if (bytes.size() < total) {
    return Result<Plan>::failure(ErrorCode::kTruncatedRecord, "record is truncated");
  }
  if (bytes.size() > total) {
    return Result<Plan>::failure(ErrorCode::kTrailingGarbage,
                                 std::to_string(bytes.size() - total) + " trailing bytes after the record");
  }
  const std::uint32_t expected_crc = crc32(bytes.data(), header_length);
  const std::uint32_t stored_crc = static_cast<std::uint32_t>(bytes[header_length]) |
                                   (static_cast<std::uint32_t>(bytes[header_length + 1u]) << 8u) |
                                   (static_cast<std::uint32_t>(bytes[header_length + 2u]) << 16u) |
                                   (static_cast<std::uint32_t>(bytes[header_length + 3u]) << 24u);
  if (expected_crc != stored_crc) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "record header failed its integrity check");
  }

  ByteReader reader(std::span<const std::uint8_t>(bytes.data(), header_length));
  std::span<const std::uint8_t> header_magic;
  reader.raw(sizeof(kRecordMagic), header_magic);
  std::uint16_t header_version = 0;
  std::uint32_t declared_header_length = 0;
  std::uint32_t declared_payload_length = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t unix_micros = 0;
  std::uint64_t generation = 0;
  GenerationBindings bindings;
  reader.u16(header_version);
  reader.u32(declared_header_length);
  reader.u32(declared_payload_length);
  reader.u64(incarnation);
  reader.u64(sequence);
  reader.u64(unix_micros);
  reader.u64(generation);
  reader.u64(bindings.topology.mutable_value());
  reader.u64(bindings.failure_domains.mutable_value());
  reader.u64(bindings.capacity_evidence.mutable_value());
  reader.u64(bindings.policy.mutable_value());
  reader.u64(bindings.plan.mutable_value());
  std::string collective;
  reader.string(collective, kMaxIdentifierBytes);
  std::uint8_t kind = 0;
  reader.u8(kind);
  Digest input_digest;
  reader.digest(input_digest);
  if (Status status = reader.require_end(); !status.is_ok()) {
    return Result<Plan>::failure(status);
  }
  if (declared_header_length != header_length || declared_payload_length != payload_length) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "record header disagrees with its own layout");
  }
  if (header_version != version) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "record header version is inconsistent");
  }
  if (kind > static_cast<std::uint8_t>(CollectiveKind::kHierarchical)) {
    return Result<Plan>::failure(ErrorCode::kCorruptRecord, "record declares an unknown collective kind");
  }

  const std::size_t payload_offset = static_cast<std::size_t>(header_length) + 4u;
  const std::size_t plan_bytes = static_cast<std::size_t>(payload_length) - kRecordDigests;
  const std::uint8_t* payload = bytes.data() + payload_offset;
  Digest stored_payload_digest;
  std::memcpy(stored_payload_digest.bytes.data(), payload + plan_bytes, kDigestBytes);
  Digest stored_request_digest;
  std::memcpy(stored_request_digest.bytes.data(), payload + plan_bytes + kDigestBytes, kDigestBytes);

  const Digest computed = Sha256::hash(payload, plan_bytes);
  if (!(computed == stored_payload_digest)) {
    return Result<Plan>::failure(ErrorCode::kDigestMismatch, "record payload failed its digest check");
  }

  auto plan = Plan::decode(std::span<const std::uint8_t>(payload, plan_bytes));
  if (!plan.has_value()) {
    return Result<Plan>::failure(plan.status());
  }
  if (!(plan.value().input_digest == stored_request_digest)) {
    return Result<Plan>::failure(ErrorCode::kDigestMismatch,
                                 "record request digest does not match the plan it carries");
  }
  if (summary_out != nullptr) {
    summary_out->id = plan.value().id;
    summary_out->generation = plan.value().generation;
    summary_out->bindings = plan.value().bindings;
    summary_out->collective = plan.value().collective;
    summary_out->kind = plan.value().kind;
    summary_out->stored_sequence = sequence;
    summary_out->record_bytes = bytes.size();
    summary_out->written_incarnation = Incarnation{incarnation};
    summary_out->written_unix_micros = unix_micros;
  }
  return plan;
}

// ---------------------------------------------------------------------------
// PlanStore
// ---------------------------------------------------------------------------

struct PlanStore::Impl {
  struct Entry {
    StoredPlanSummary summary{};
    std::filesystem::path path{};
  };

  explicit Impl(StoreConfig value) : config(std::move(value)) {}

  StoreConfig config{};
  // One mutex guards every mutable member below. No user code is called while it
  // is held, and no other lock in the library is acquired while it is held.
  mutable std::mutex mutex{};
  bool open{false};
  Epoch epoch{};
  Incarnation incarnation{};
  std::uint64_t sequence{0};
  std::map<Digest, Entry> index{};

  std::filesystem::path meta_path() const { return std::filesystem::path(config.root) / "store.meta"; }

  Status persist_meta() {
    return write_file_atomic(meta_path(), encode_meta(epoch, incarnation, sequence), config.durable_commit);
  }

  // Rebuilds the index from the directory, verifying every record. A record that
  // fails verification is reported by omission and is never indexed.
  Status rebuild_index() {
    index.clear();
    std::error_code error;
    std::filesystem::directory_iterator iterator(std::filesystem::path(config.root), error);
    if (error) {
      return filesystem_failure("cannot enumerate", config.root, error);
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
      const std::string name = entry.path().filename().string();
      std::error_code kind_error;
      if (!entry.is_regular_file(kind_error) || kind_error) {
        kind_error.clear();
        continue;
      }
      if (name.rfind("plan-", 0) != 0 || entry.path().extension() != ".rec") {
        continue;
      }
      auto bytes = read_file(entry.path(), config.max_record_bytes);
      if (!bytes.has_value()) {
        continue;
      }
      StoredPlanSummary summary;
      auto plan = decode_plan_record(bytes.value(), &summary);
      if (!plan.has_value()) {
        continue;
      }
      index[summary.id.value()] = Entry{summary, entry.path()};
    }
    return Status::ok();
  }
};

PlanStore::PlanStore(StoreConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))), config_(impl_->config) {}

PlanStore::~PlanStore() {
  if (impl_ != nullptr) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->open = false;
    impl_->index.clear();
  }
}

Status PlanStore::open() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->open) {
    return Status::error(ErrorCode::kStoreAlreadyOpen, "the store is already open");
  }
  if (impl_->config.root.empty()) {
    return Status::error(ErrorCode::kStoreIoFailure, "the store has no root directory");
  }

  std::error_code error;
  std::filesystem::create_directories(impl_->config.root, error);
  if (error && !std::filesystem::is_directory(impl_->config.root)) {
    return filesystem_failure("cannot create the store directory", impl_->config.root, error);
  }

  // Staging files are never authoritative, so they are removed before anything
  // else is read.
  {
    std::error_code listing_error;
    std::filesystem::directory_iterator iterator(std::filesystem::path(impl_->config.root), listing_error);
    if (listing_error) {
      return filesystem_failure("cannot enumerate", impl_->config.root, listing_error);
    }
    std::vector<std::filesystem::path> stale;
    for (const std::filesystem::directory_entry& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("tmp-", 0) == 0 && entry.path().extension() == ".tmp") {
        stale.push_back(entry.path());
      }
    }
    for (const std::filesystem::path& path : stale) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }

  const std::filesystem::path meta = impl_->meta_path();
  std::error_code exists_error;
  if (std::filesystem::exists(meta, exists_error) && !exists_error) {
    auto bytes = read_file(meta, kMetaBytes);
    if (!bytes.has_value()) {
      return bytes.status();
    }
    Epoch epoch;
    Incarnation incarnation;
    std::uint64_t sequence = 0;
    if (Status status = decode_meta(bytes.value(), epoch, incarnation, sequence); !status.is_ok()) {
      return status;
    }
    // Opening the store advances both fences: nothing written by a previous open
    // can be treated as current.
    impl_->epoch = Epoch{epoch.value() + 1u};
    impl_->incarnation = Incarnation{incarnation.value() + 1u};
    impl_->sequence = sequence;
  } else {
    impl_->epoch = Epoch{1};
    impl_->incarnation = Incarnation{1};
    impl_->sequence = 0;
  }
  if (Status status = impl_->persist_meta(); !status.is_ok()) {
    return status;
  }
  if (Status status = impl_->rebuild_index(); !status.is_ok()) {
    return status;
  }
  impl_->open = true;
  return Status::ok();
}

Status PlanStore::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Status::ok();
  }
  const Status status = impl_->persist_meta();
  impl_->index.clear();
  impl_->open = false;
  return status;
}

bool PlanStore::is_open() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->open;
}

Epoch PlanStore::epoch() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->epoch;
}

Incarnation PlanStore::incarnation() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->incarnation;
}

std::size_t PlanStore::record_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->index.size();
}

std::uint64_t PlanStore::total_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::uint64_t total = 0;
  for (const auto& item : impl_->index) {
    total += item.second.summary.record_bytes;
  }
  return total;
}

Result<StoredPlanSummary> PlanStore::put(const Plan& plan, const PlanningRequest& request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<StoredPlanSummary>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  if (!(plan.input_digest == request.canonical_digest)) {
    return Result<StoredPlanSummary>::failure(
        ErrorCode::kRecordIncompatible,
        "the plan was not produced from this request's canonical digest");
  }
  if (Status status = validate_plan(plan, request); !status.is_ok()) {
    return Result<StoredPlanSummary>::failure(status);
  }

  const std::uint64_t next_sequence = impl_->sequence + 1u;
  const std::vector<std::uint8_t> record =
      encode_plan_record(plan, request, impl_->incarnation, next_sequence, unix_micros_now());
  if (record.size() > impl_->config.max_record_bytes) {
    return Result<StoredPlanSummary>::failure(ErrorCode::kOversizedRecord,
                                              "the encoded plan record exceeds the configured bound");
  }

  const Digest digest = plan.id.value();
  const auto existing = impl_->index.find(digest);
  const bool replacing = existing != impl_->index.end();
  const std::uint64_t existing_bytes = replacing ? existing->second.summary.record_bytes : 0u;

  // Bounds are enforced before the commit becomes visible.
  std::vector<Digest> evictions;
  {
    std::size_t projected_count = impl_->index.size() + (replacing ? 0u : 1u);
    std::uint64_t projected_bytes = static_cast<std::uint64_t>(record.size()) - existing_bytes;
    for (const auto& item : impl_->index) {
      projected_bytes += item.second.summary.record_bytes;
    }
    std::vector<std::pair<std::uint64_t, Digest>> ordered;
    ordered.reserve(impl_->index.size());
    for (const auto& item : impl_->index) {
      if (replacing && item.first == digest) {
        continue;
      }
      ordered.emplace_back(item.second.summary.stored_sequence, item.first);
    }
    std::sort(ordered.begin(), ordered.end());
    for (const auto& item : ordered) {
      if (projected_count <= impl_->config.max_records && projected_bytes <= impl_->config.max_total_bytes) {
        break;
      }
      evictions.push_back(item.second);
      projected_count -= 1u;
      projected_bytes -= impl_->index[item.second].summary.record_bytes;
    }
    if (projected_count > impl_->config.max_records || projected_bytes > impl_->config.max_total_bytes) {
      return Result<StoredPlanSummary>::failure(
          ErrorCode::kStoreCapacityExceeded,
          "the store cannot make room for this record within its configured bounds");
    }
  }

  std::vector<std::filesystem::path> removals;
  removals.reserve(evictions.size());
  for (const Digest& key : evictions) {
    removals.push_back(impl_->index[key].path);
  }

  const std::filesystem::path target = std::filesystem::path(impl_->config.root) / record_file_name(digest);
  if (Status status = write_file_atomic(target, record, impl_->config.durable_commit); !status.is_ok()) {
    return Result<StoredPlanSummary>::failure(status);
  }

  // The commit is durable; only now may the in-memory state acknowledge it.
  StoredPlanSummary summary;
  summary.id = plan.id;
  summary.generation = plan.generation;
  summary.bindings = plan.bindings;
  summary.collective = plan.collective;
  summary.kind = plan.kind;
  summary.stored_sequence = next_sequence;
  summary.record_bytes = record.size();
  summary.written_incarnation = impl_->incarnation;
  summary.written_unix_micros = unix_micros_now();

  impl_->sequence = next_sequence;
  impl_->index[digest] = Impl::Entry{summary, target};
  for (const Digest& key : evictions) {
    impl_->index.erase(key);
  }
  for (const std::filesystem::path& path : removals) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  if (Status status = impl_->persist_meta(); !status.is_ok()) {
    return Result<StoredPlanSummary>::failure(status);
  }
  return Result<StoredPlanSummary>::success(summary);
}

Result<Plan> PlanStore::get(const CollectivePlanId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<Plan>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  const auto found = impl_->index.find(id.value());
  if (found == impl_->index.end()) {
    return Result<Plan>::failure(ErrorCode::kNotFound, "no record holds digest " + id.value().hex());
  }
  auto bytes = read_file(found->second.path, impl_->config.max_record_bytes);
  if (!bytes.has_value()) {
    return Result<Plan>::failure(bytes.status());
  }
  return decode_plan_record(bytes.value(), nullptr);
}

Result<StoredPlanSummary> PlanStore::summary(const CollectivePlanId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<StoredPlanSummary>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  const auto found = impl_->index.find(id.value());
  if (found == impl_->index.end()) {
    return Result<StoredPlanSummary>::failure(ErrorCode::kNotFound,
                                              "no record holds digest " + id.value().hex());
  }
  return Result<StoredPlanSummary>::success(found->second.summary);
}

Result<std::vector<StoredPlanSummary>> PlanStore::list() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<std::vector<StoredPlanSummary>>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  std::vector<StoredPlanSummary> summaries;
  summaries.reserve(impl_->index.size());
  for (const auto& item : impl_->index) {
    summaries.push_back(item.second.summary);
  }
  return Result<std::vector<StoredPlanSummary>>::success(std::move(summaries));
}

Result<RevalidationResult> PlanStore::revalidate(const CollectivePlanId& id,
                                                 const PlanningRequest& current) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<RevalidationResult>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  const auto found = impl_->index.find(id.value());
  if (found == impl_->index.end()) {
    return Result<RevalidationResult>::failure(ErrorCode::kNotFound,
                                               "no record holds digest " + id.value().hex());
  }
  auto bytes = read_file(found->second.path, impl_->config.max_record_bytes);
  if (!bytes.has_value()) {
    return Result<RevalidationResult>::failure(bytes.status());
  }
  auto plan = decode_plan_record(bytes.value(), nullptr);
  if (!plan.has_value()) {
    return Result<RevalidationResult>::failure(plan.status());
  }

  RevalidationResult result;
  result.stored_input_digest = plan.value().input_digest;
  result.current_input_digest = current.canonical_digest;
  result.freshness = assess_freshness(plan.value(), current.current_bindings());
  result.still_valid = (result.stored_input_digest == result.current_input_digest) &&
                       (plan.value().bindings == current.current_bindings());

  std::string detail;
  if (result.still_valid) {
    result.code = ErrorCode::kOk;
    detail = "the stored plan was produced from exactly these generations";
  } else {
    ErrorCode code = ErrorCode::kOk;
    detail = describe_freshness(result.freshness, code);
    result.code = code;
    if (!(result.stored_input_digest == result.current_input_digest)) {
      detail += "; the current inputs hash to " + result.current_input_digest.hex();
    }
    const auto replanned = replan(current);
    if (replanned.has_value()) {
      detail += "; a plan for the current inputs is available as " + replanned.value().id.value().hex();
      result.replanned = replanned.value();
    } else {
      detail += "; replanning under the current inputs is refused: " + replanned.status().to_string();
    }
  }
  if (!(found->second.summary.written_incarnation == impl_->incarnation)) {
    detail += "; the record was written under incarnation " +
              std::to_string(found->second.summary.written_incarnation.value()) +
              " and this store now runs under incarnation " + std::to_string(impl_->incarnation.value());
  }
  result.detail = std::move(detail);
  return Result<RevalidationResult>::success(std::move(result));
}

Status PlanStore::erase(const CollectivePlanId& id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Status::error(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  const auto found = impl_->index.find(id.value());
  if (found == impl_->index.end()) {
    return Status::error(ErrorCode::kNotFound, "no record holds digest " + id.value().hex());
  }
  const std::filesystem::path path = found->second.path;
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return filesystem_failure("cannot remove", path, error);
  }
  impl_->index.erase(found);
  return impl_->persist_meta();
}

Result<std::vector<StoredPlanSummary>> PlanStore::verify_all() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Result<std::vector<StoredPlanSummary>>::failure(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  if (Status status = impl_->rebuild_index(); !status.is_ok()) {
    return Result<std::vector<StoredPlanSummary>>::failure(status);
  }
  std::vector<StoredPlanSummary> verified;
  verified.reserve(impl_->index.size());
  for (const auto& item : impl_->index) {
    verified.push_back(item.second.summary);
  }
  return Result<std::vector<StoredPlanSummary>>::success(std::move(verified));
}

Status PlanStore::enforce_bounds() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    return Status::error(ErrorCode::kStoreNotOpen, "the store is not open");
  }
  std::vector<std::pair<std::uint64_t, Digest>> ordered;
  ordered.reserve(impl_->index.size());
  std::uint64_t projected_bytes = 0;
  for (const auto& item : impl_->index) {
    ordered.emplace_back(item.second.summary.stored_sequence, item.first);
    projected_bytes += item.second.summary.record_bytes;
  }
  std::sort(ordered.begin(), ordered.end());
  std::size_t projected_count = impl_->index.size();
  for (const auto& item : ordered) {
    if (projected_count <= impl_->config.max_records && projected_bytes <= impl_->config.max_total_bytes) {
      break;
    }
    const auto found = impl_->index.find(item.second);
    if (found == impl_->index.end()) {
      continue;
    }
    const std::uint64_t size = found->second.summary.record_bytes;
    const std::filesystem::path path = found->second.path;
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
      return filesystem_failure("cannot remove", path, error);
    }
    impl_->index.erase(found);
    projected_count -= 1u;
    projected_bytes -= size;
  }
  return impl_->persist_meta();
}

}  // namespace cpath
