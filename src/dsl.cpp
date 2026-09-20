// Collective Path Planner - strict canonical text DSL reader and writer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/dsl.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpath/buffer.hpp"

namespace cpath {
namespace {

constexpr std::string_view kBanner = "cpath-request";
constexpr std::uint64_t kBannerVersion = 1;

bool is_space(char ch) noexcept { return ch == ' ' || ch == '\t'; }

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return Result<std::uint64_t>::failure(ErrorCode::kMissingRequiredField, "expected a decimal number");
  }
  if (text.size() > 1 && text.front() == '0') {
    return Result<std::uint64_t>::failure(ErrorCode::kMalformedNumber, "numbers must not carry leading zeros");
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return Result<std::uint64_t>::failure(ErrorCode::kMalformedNumber, "expected a decimal number");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10u) {
      return Result<std::uint64_t>::failure(ErrorCode::kValueOutOfRange, "number does not fit in 64 bits");
    }
    value = value * 10u + digit;
  }
  return Result<std::uint64_t>::success(value);
}

Result<std::uint32_t> parse_u32(std::string_view text) {
  auto wide = parse_u64(text);
  if (!wide.has_value()) {
    return Result<std::uint32_t>::failure(wide.status());
  }
  if (wide.value() > 0xFFFFFFFFull) {
    return Result<std::uint32_t>::failure(ErrorCode::kValueOutOfRange, "number does not fit in 32 bits");
  }
  return Result<std::uint32_t>::success(static_cast<std::uint32_t>(wide.value()));
}

bool parse_bool(std::string_view text, bool& out) noexcept {
  if (text == "0") {
    out = false;
    return true;
  }
  if (text == "1") {
    out = true;
    return true;
  }
  return false;
}

std::vector<std::string_view> split_list(std::string_view text, char separator) {
  std::vector<std::string_view> items;
  if (text.empty()) {
    return items;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(separator, start);
    if (position == std::string_view::npos) {
      items.push_back(text.substr(start));
      break;
    }
    items.push_back(text.substr(start, position - start));
    start = position + 1u;
  }
  return items;
}

struct Assignment {
  std::string_view key{};
  std::string_view value{};
};

// Parses "key=value key=value ..." into an ordered assignment list. A repeated
// key is a hard error rather than a silent last-wins.
class AssignmentList {
 public:
  Status parse(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
      while (index < text.size() && is_space(text[index])) {
        ++index;
      }
      if (index >= text.size()) {
        break;
      }
      const std::size_t key_start = index;
      while (index < text.size() && !is_space(text[index]) && text[index] != '=') {
        ++index;
      }
      const std::string_view key = text.substr(key_start, index - key_start);
      if (index >= text.size() || text[index] != '=') {
        return Status::error(ErrorCode::kInvalidSyntax, "expected key=value, found " + std::string(key));
      }
      ++index;  // consume '='
      const std::size_t value_start = index;
      while (index < text.size() && !is_space(text[index])) {
        ++index;
      }
      const std::string_view value = text.substr(value_start, index - value_start);
      if (key.empty()) {
        return Status::error(ErrorCode::kInvalidSyntax, "empty key before '='");
      }
      if (value.empty()) {
        return Status::error(ErrorCode::kInvalidSyntax, "empty value for key " + std::string(key));
      }
      if (indexOf(key) >= 0) {
        return Status::error(ErrorCode::kDuplicateIdentifier, "repeated key " + std::string(key));
      }
      entries_.push_back(Assignment{key, value});
    }
    return Status::ok();
  }

  int indexOf(std::string_view key) const noexcept {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].key == key) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  bool has(std::string_view key) const noexcept { return indexOf(key) >= 0; }

  std::string_view get(std::string_view key) const noexcept {
    const int position = indexOf(key);
    return position < 0 ? std::string_view{} : entries_[static_cast<std::size_t>(position)].value;
  }

  // Reports any key that is not in the accepted set.
  Status reject_unknown(const std::vector<std::string_view>& accepted) const {
    for (const Assignment& entry : entries_) {
      bool known = false;
      for (const std::string_view candidate : accepted) {
        if (candidate == entry.key) {
          known = true;
          break;
        }
      }
      if (!known) {
        return Status::error(ErrorCode::kUnknownKeyword, "unknown key " + std::string(entry.key));
      }
    }
    return Status::ok();
  }

  Status require(std::string_view key) const {
    if (!has(key)) {
      return Status::error(ErrorCode::kMissingRequiredField, "missing required key " + std::string(key));
    }
    return Status::ok();
  }

  Result<std::uint64_t> number(std::string_view key, std::uint64_t fallback, bool required) const {
    if (!has(key)) {
      if (required) {
        return Result<std::uint64_t>::failure(ErrorCode::kMissingRequiredField,
                                              "missing required key " + std::string(key));
      }
      return Result<std::uint64_t>::success(fallback);
    }
    auto value = parse_u64(get(key));
    if (!value.has_value()) {
      return Result<std::uint64_t>::failure(value.status().code(),
                                            std::string(key) + ": " + value.status().detail());
    }
    return value;
  }

  Result<std::uint32_t> number32(std::string_view key, std::uint32_t fallback, bool required) const {
    if (!has(key)) {
      if (required) {
        return Result<std::uint32_t>::failure(ErrorCode::kMissingRequiredField,
                                              "missing required key " + std::string(key));
      }
      return Result<std::uint32_t>::success(fallback);
    }
    auto value = parse_u32(get(key));
    if (!value.has_value()) {
      return Result<std::uint32_t>::failure(value.status().code(),
                                            std::string(key) + ": " + value.status().detail());
    }
    return value;
  }

  Result<std::string> identifier(std::string_view key, bool required) const {
    if (!has(key)) {
      if (required) {
        return Result<std::string>::failure(ErrorCode::kMissingRequiredField,
                                            "missing required key " + std::string(key));
      }
      return Result<std::string>::success(std::string());
    }
    auto value = make_identifier(get(key));
    if (!value.has_value()) {
      return Result<std::string>::failure(value.status().code(),
                                          std::string(key) + ": " + value.status().detail());
    }
    return value;
  }

  Result<std::string> label(std::string_view key) const {
    if (!has(key)) {
      return Result<std::string>::success(std::string());
    }
    const std::string_view text = get(key);
    if (text.size() > kMaxPlacementLabelBytes || !is_valid_identifier(text)) {
      return Result<std::string>::failure(ErrorCode::kInvalidCharacter,
                                          std::string(key) + ": not a valid placement label");
    }
    return Result<std::string>::success(std::string(text));
  }

 private:
  std::vector<Assignment> entries_{};
};

struct SectionState {
  std::string name{};
};

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Result<RequestSpec> run() {
    if (text_.size() > kMaxInputBytes) {
      return Result<RequestSpec>::failure(ErrorCode::kInputTooLarge, "input exceeds the accepted size bound");
    }
    if (text_.empty()) {
      return Result<RequestSpec>::failure(ErrorCode::kEmptyInput, "input is empty");
    }

    bool banner_seen = false;
    std::size_t offset = 0;
    while (offset <= text_.size()) {
      const std::size_t newline = text_.find('\n', offset);
      std::string_view raw =
          newline == std::string_view::npos ? text_.substr(offset) : text_.substr(offset, newline - offset);
      offset = newline == std::string_view::npos ? text_.size() + 1u : newline + 1u;
      ++line_number_;

      if (raw.size() > kMaxTextLineBytes) {
        return Result<RequestSpec>::failure(ErrorCode::kLineTooLong,
                                            "line " + std::to_string(line_number_) + " exceeds " +
                                                std::to_string(kMaxTextLineBytes) + " bytes");
      }
      if (!raw.empty() && raw.back() == '\r') {
        raw.remove_suffix(1);
      }
      std::size_t begin = 0;
      while (begin < raw.size() && is_space(raw[begin])) {
        ++begin;
      }
      std::size_t end = raw.size();
      while (end > begin && is_space(raw[end - 1])) {
        --end;
      }
      const std::string_view line = raw.substr(begin, end - begin);
      if (line.empty() || line.front() == '#') {
        if (newline == std::string_view::npos) {
          break;
        }
        continue;
      }

      Status status = Status::ok();
      if (line.front() == '[') {
        status = handle_section(line);
      } else if (!banner_seen) {
        status = handle_banner(line);
        banner_seen = true;
      } else if (section_.name.empty()) {
        status = handle_header(line);
      } else {
        status = handle_entry(line);
      }
      if (!status.is_ok()) {
        return Result<RequestSpec>::failure(status.code(),
                                            "line " + std::to_string(line_number_) + ": " + status.detail());
      }
      if (newline == std::string_view::npos) {
        break;
      }
    }

    if (!banner_seen) {
      return Result<RequestSpec>::failure(ErrorCode::kMissingRequiredField,
                                          "input does not start with the cpath-request banner");
    }
    return Result<RequestSpec>::success(std::move(spec_));
  }

 private:
  Status handle_banner(std::string_view line) {
    std::size_t index = 0;
    while (index < line.size() && !is_space(line[index])) {
      ++index;
    }
    const std::string_view keyword = line.substr(0, index);
    if (keyword != kBanner) {
      return Status::error(ErrorCode::kMissingRequiredField,
                           "expected the banner '" + std::string(kBanner) + " <version>'");
    }
    while (index < line.size() && is_space(line[index])) {
      ++index;
    }
    const std::string_view version = line.substr(index);
    auto parsed = parse_u64(version);
    if (!parsed.has_value()) {
      return Status::error(parsed.status().code(), "banner version: " + parsed.status().detail());
    }
    if (parsed.value() != kBannerVersion) {
      return Status::error(ErrorCode::kUnsupportedFormatVersion,
                           "unsupported request DSL version " + std::to_string(parsed.value()));
    }
    banner_seen_ = true;
    return Status::ok();
  }

  Status handle_header(std::string_view line) {
    std::size_t index = 0;
    while (index < line.size() && !is_space(line[index])) {
      ++index;
    }
    const std::string_view keyword = line.substr(0, index);
    while (index < line.size() && is_space(line[index])) {
      ++index;
    }
    const std::string_view value = line.substr(index);
    if (value.empty()) {
      return Status::error(ErrorCode::kInvalidSyntax, "header line requires a value");
    }
    auto number = parse_u64(value);
    if (!number.has_value()) {
      return Status::error(number.status().code(), std::string(keyword) + ": " + number.status().detail());
    }
    if (keyword == "plan-generation") {
      spec_.plan_generation = CollectivePlanGeneration{number.value()};
    } else if (keyword == "topology-generation") {
      spec_.fabric.topology_generation = TopologyGeneration{number.value()};
    } else if (keyword == "failure-domain-generation") {
      spec_.fabric.failure_domain_generation = FailureDomainGeneration{number.value()};
    } else if (keyword == "capacity-evidence-generation") {
      spec_.fabric.capacity_evidence_generation = CapacityEvidenceGeneration{number.value()};
    } else if (keyword == "policy-generation") {
      spec_.policy.generation = PolicyGeneration{number.value()};
    } else {
      return Status::error(ErrorCode::kUnknownKeyword, "unknown header keyword " + std::string(keyword));
    }
    return Status::ok();
  }

  Status handle_section(std::string_view line) {
    if (line.size() < 3u || line.back() != ']') {
      return Status::error(ErrorCode::kInvalidSyntax, "malformed section header");
    }
    const std::string_view name = line.substr(1, line.size() - 2u);
    if (name.empty()) {
      return Status::error(ErrorCode::kInvalidSyntax, "empty section name");
    }
    static const std::vector<std::string_view> kSections = {
        "collective", "groups", "domains", "nodes", "edges", "bindings", "policy", "forbidden"};
    bool known = false;
    for (const std::string_view candidate : kSections) {
      if (candidate == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Status::error(ErrorCode::kUnknownKeyword, "unknown section [" + std::string(name) + "]");
    }
    if (!section_seen_.insert(std::string(name)).second) {
      return Status::error(ErrorCode::kDuplicateIdentifier, "repeated section [" + std::string(name) + "]");
    }
    section_.name = std::string(name);
    return Status::ok();
  }

  Status handle_entry(std::string_view line) {
    if (section_.name == "policy") {
      return handle_policy(line);
    }
    if (section_.name == "forbidden") {
      return handle_forbidden(line);
    }
    if (section_.name == "collective") {
      return handle_collective(line);
    }

    // Every other section is "<id> key=value ...".
    std::size_t index = 0;
    while (index < line.size() && !is_space(line[index])) {
      ++index;
    }
    const std::string_view id = line.substr(0, index);
    auto identifier = make_identifier(id);
    if (!identifier.has_value()) {
      return Status::error(identifier.status().code(), "identifier: " + identifier.status().detail());
    }
    AssignmentList assignments;
    if (Status status = assignments.parse(line.substr(index)); !status.is_ok()) {
      return status;
    }

    const std::string name = identifier.value();
    if (section_.name == "groups") {
      return handle_group(name, assignments);
    }
    if (section_.name == "domains") {
      return handle_domain(name, assignments);
    }
    if (section_.name == "nodes") {
      return handle_node(name, assignments);
    }
    if (section_.name == "edges") {
      return handle_edge(name, assignments);
    }
    if (section_.name == "bindings") {
      return handle_binding(name, assignments);
    }
    return Status::error(ErrorCode::kInternalError, "unhandled section");
  }

  // The collective entry names its identifier as an assignment ("id=<id>"),
  // because that is how encode_request_text renders the section: the reader
  // must accept exactly what the canonical writer produces.
  Status handle_collective(std::string_view line) {
    AssignmentList assignments;
    if (Status status = assignments.parse(line); !status.is_ok()) {
      return status;
    }
    if (Status status = assignments.reject_unknown({"id", "kind", "demand"}); !status.is_ok()) {
      return status;
    }
    if (Status status = assignments.require("id"); !status.is_ok()) {
      return status;
    }
    auto identifier = make_identifier(assignments.get("id"));
    if (!identifier.has_value()) {
      return Status::error(identifier.status().code(), "id: " + identifier.status().detail());
    }
    if (collective_seen_) {
      return Status::error(ErrorCode::kDuplicateIdentifier, "collective is declared more than once");
    }
    collective_seen_ = true;
    spec_.collective.id = CollectiveId{identifier.value()};
    if (assignments.has("kind")) {
      CollectiveKind kind = CollectiveKind::kRing;
      if (!parse_collective_kind(assignments.get("kind"), kind)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown collective kind " +
                                                             std::string(assignments.get("kind")));
      }
      spec_.collective.kind = kind;
    }
    auto demand = assignments.number("demand", 0, false);
    if (!demand.has_value()) {
      return demand.status();
    }
    if (demand.value() > kMaxCapacityMbps) {
      return Status::error(ErrorCode::kValueOutOfRange, "declared demand exceeds the model ceiling");
    }
    spec_.collective.demand_mbps = demand.value();
    return Status::ok();
  }

  Status handle_group(const std::string& id, const AssignmentList& assignments) {
    if (Status status = assignments.reject_unknown({"level", "pattern", "root", "direction", "members"}); !status.is_ok()) {
      return status;
    }
    if (spec_.collective.groups.size() >= kMaxGroups) {
      return Status::error(ErrorCode::kTooManyItems, "group count exceeds limit");
    }
    for (const GroupSpec& group : spec_.collective.groups) {
      if (group.id.value() == id) {
        return Status::error(ErrorCode::kDuplicateIdentifier, "duplicate group id " + id);
      }
    }
    GroupSpec group;
    group.id = GroupId{id};
    auto level = assignments.number32("level", 0, true);
    if (!level.has_value()) {
      return level.status();
    }
    group.level = level.value();
    if (Status status = assignments.require("pattern"); !status.is_ok()) {
      return status;
    }
    if (!parse_group_pattern(assignments.get("pattern"), group.pattern)) {
      return Status::error(ErrorCode::kValueOutOfRange, "unknown group pattern");
    }
    if (assignments.has("direction")) {
      if (!parse_tree_direction(assignments.get("direction"), group.direction)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown tree direction");
      }
    }
    if (assignments.has("root")) {
      auto root = make_identifier(assignments.get("root"));
      if (!root.has_value()) {
        return Status::error(root.status().code(), "root: " + root.status().detail());
      }
      group.root = ParticipantId{root.value()};
    }
    if (Status status = assignments.require("members"); !status.is_ok()) {
      return status;
    }
    for (const std::string_view member : split_list(assignments.get("members"), ',')) {
      auto parsed = make_identifier(member);
      if (!parsed.has_value()) {
        return Status::error(parsed.status().code(), "members: " + parsed.status().detail());
      }
      if (group.members.size() >= kMaxGroupMembers) {
        return Status::error(ErrorCode::kTooManyItems, "group declares too many members");
      }
      group.members.push_back(ParticipantId{parsed.value()});
    }
    spec_.collective.groups.push_back(std::move(group));
    return Status::ok();
  }

  Status handle_domain(const std::string& id, const AssignmentList& assignments) {
    if (Status status = assignments.reject_unknown({"kind", "parent"}); !status.is_ok()) {
      return status;
    }
    if (spec_.fabric.failure_domains.size() >= kMaxFailureDomains) {
      return Status::error(ErrorCode::kTooManyItems, "failure domain count exceeds limit");
    }
    FailureDomain domain;
    domain.id = FailureDomainId{id};
    if (assignments.has("kind")) {
      if (!parse_domain_kind(assignments.get("kind"), domain.kind)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown failure domain kind");
      }
    }
    if (assignments.has("parent")) {
      auto parent = make_identifier(assignments.get("parent"));
      if (!parent.has_value()) {
        return Status::error(parent.status().code(), "parent: " + parent.status().detail());
      }
      domain.parent = FailureDomainId{parent.value()};
    }
    spec_.fabric.failure_domains.push_back(std::move(domain));
    return Status::ok();
  }

  Status handle_node(const std::string& id, const AssignmentList& assignments) {
    if (Status status = assignments.reject_unknown(
            {"site", "pod", "rack", "host", "device", "tier", "eligible"});
        !status.is_ok()) {
      return status;
    }
    if (spec_.fabric.nodes.size() >= kMaxNodes) {
      return Status::error(ErrorCode::kTooManyItems, "node count exceeds limit");
    }
    Node node;
    node.id = NodeId{id};
    auto site = assignments.label("site");
    auto pod = assignments.label("pod");
    auto rack = assignments.label("rack");
    auto host = assignments.label("host");
    if (!site.has_value() || !pod.has_value() || !rack.has_value() || !host.has_value()) {
      const Status& failure = !site.has_value() ? site.status()
                                                : (!pod.has_value() ? pod.status()
                                                                    : (!rack.has_value() ? rack.status()
                                                                                         : host.status()));
      return failure;
    }
    node.placement.site = site.value();
    node.placement.pod = pod.value();
    node.placement.rack = rack.value();
    node.placement.host = host.value();
    node.placement.supplied = assignments.has("site") || assignments.has("pod") || assignments.has("rack") ||
                              assignments.has("host") || assignments.has("device");
    auto device = assignments.number32("device", 0, false);
    if (!device.has_value()) {
      return device.status();
    }
    node.placement.device_index = device.value();
    if (assignments.has("tier")) {
      auto tier = make_tier_name(assignments.get("tier"));
      if (!tier.has_value()) {
        return Status::error(tier.status().code(), "tier: " + tier.status().detail());
      }
      node.tier = TierId{tier.value()};
    }
    if (assignments.has("eligible")) {
      if (!parse_bool(assignments.get("eligible"), node.eligible)) {
        return Status::error(ErrorCode::kValueOutOfRange, "eligible must be 0 or 1");
      }
    }
    spec_.fabric.nodes.push_back(std::move(node));
    return Status::ok();
  }

  Status handle_edge(const std::string& id, const AssignmentList& assignments) {
    if (Status status = assignments.reject_unknown(
            {"from", "to", "capacity", "latency", "reserved", "tier", "domain", "evidence", "eligible"});
        !status.is_ok()) {
      return status;
    }
    if (spec_.fabric.edges.size() >= kMaxEdges) {
      return Status::error(ErrorCode::kTooManyItems, "edge count exceeds limit");
    }
    Edge edge;
    edge.id = EdgeId{id};
    if (Status status = assignments.require("from"); !status.is_ok()) {
      return status;
    }
    if (Status status = assignments.require("to"); !status.is_ok()) {
      return status;
    }
    auto from = make_identifier(assignments.get("from"));
    auto to = make_identifier(assignments.get("to"));
    if (!from.has_value() || !to.has_value()) {
      return !from.has_value() ? from.status() : to.status();
    }
    edge.from = NodeId{from.value()};
    edge.to = NodeId{to.value()};
    auto capacity = assignments.number("capacity", 0, false);
    auto latency = assignments.number("latency", 0, false);
    auto reserved = assignments.number("reserved", 0, false);
    if (!capacity.has_value() || !latency.has_value() || !reserved.has_value()) {
      return !capacity.has_value() ? capacity.status()
                                   : (!latency.has_value() ? latency.status() : reserved.status());
    }
    edge.capacity_mbps = capacity.value();
    edge.latency_micros = latency.value();
    edge.reserved_mbps = reserved.value();
    if (assignments.has("tier")) {
      auto tier = make_tier_name(assignments.get("tier"));
      if (!tier.has_value()) {
        return Status::error(tier.status().code(), "tier: " + tier.status().detail());
      }
      edge.tier = TierId{tier.value()};
    }
    if (assignments.has("domain")) {
      auto domain = make_identifier(assignments.get("domain"));
      if (!domain.has_value()) {
        return Status::error(domain.status().code(), "domain: " + domain.status().detail());
      }
      edge.failure_domain = FailureDomainId{domain.value()};
    }
    if (assignments.has("evidence")) {
      if (!parse_evidence_class(assignments.get("evidence"), edge.evidence)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown evidence class");
      }
    }
    if (assignments.has("eligible")) {
      if (!parse_bool(assignments.get("eligible"), edge.eligible)) {
        return Status::error(ErrorCode::kValueOutOfRange, "eligible must be 0 or 1");
      }
    }
    spec_.fabric.edges.push_back(std::move(edge));
    return Status::ok();
  }

  Status handle_binding(const std::string& id, const AssignmentList& assignments) {
    if (Status status = assignments.reject_unknown({"node", "domains", "tiers"}); !status.is_ok()) {
      return status;
    }
    if (spec_.bindings.size() >= kMaxBindings) {
      return Status::error(ErrorCode::kTooManyItems, "binding count exceeds limit");
    }
    EndpointBinding binding;
    binding.participant = ParticipantId{id};
    if (Status status = assignments.require("node"); !status.is_ok()) {
      return status;
    }
    auto node = make_identifier(assignments.get("node"));
    if (!node.has_value()) {
      return Status::error(node.status().code(), "node: " + node.status().detail());
    }
    binding.node = NodeId{node.value()};
    if (assignments.has("domains")) {
      for (const std::string_view domain : split_list(assignments.get("domains"), ',')) {
        auto parsed = make_identifier(domain);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "domains: " + parsed.status().detail());
        }
        binding.allowed_failure_domains.push_back(FailureDomainId{parsed.value()});
      }
    }
    if (assignments.has("tiers")) {
      for (const std::string_view tier : split_list(assignments.get("tiers"), ',')) {
        auto parsed = make_tier_name(tier);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "tiers: " + parsed.status().detail());
        }
        binding.allowed_tiers.push_back(TierId{parsed.value()});
      }
    }
    spec_.bindings.push_back(std::move(binding));
    return Status::ok();
  }

  Status handle_policy(std::string_view line) {
    AssignmentList assignments;
    if (Status status = assignments.parse(line); !status.is_ok()) {
      return status;
    }
    if (Status status = assignments.reject_unknown({"max-hops",
                                                    "paths-per-logical-edge",
                                                    "disjointness",
                                                    "domain-diversity",
                                                    "domain-diversity-level",
                                                    "evidence",
                                                    "latency-weight-milli",
                                                    "congestion-weight-milli",
                                                    "max-path-cost",
                                                    "allow-unverified-capacity",
                                                    "max-search-expansions",
                                                    "allowed-tiers"});
        !status.is_ok()) {
      return status;
    }
    Policy& policy = spec_.policy;
    auto max_hops = assignments.number("max-hops", policy.max_hops, false);
    auto paths = assignments.number("paths-per-logical-edge", policy.paths_per_logical_edge, false);
    auto latency = assignments.number("latency-weight-milli", policy.latency_weight_milli, false);
    auto congestion = assignments.number("congestion-weight-milli", policy.congestion_weight_milli, false);
    auto max_cost = assignments.number("max-path-cost", policy.max_path_cost, false);
    auto expansions = assignments.number("max-search-expansions", policy.max_search_expansions, false);
    if (!max_hops.has_value() || !paths.has_value() || !latency.has_value() || !congestion.has_value() ||
        !max_cost.has_value() || !expansions.has_value()) {
      return Status::error(ErrorCode::kMalformedNumber, "policy value is not a decimal number");
    }
    policy.max_hops = static_cast<std::size_t>(max_hops.value());
    policy.paths_per_logical_edge = static_cast<std::size_t>(paths.value());
    policy.latency_weight_milli = latency.value();
    policy.congestion_weight_milli = congestion.value();
    policy.max_path_cost = max_cost.value();
    policy.max_search_expansions = static_cast<std::size_t>(expansions.value());
    if (assignments.has("disjointness")) {
      if (!parse_disjointness(assignments.get("disjointness"), policy.disjointness)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown disjointness mode");
      }
    }
    if (assignments.has("domain-diversity")) {
      if (!parse_domain_diversity(assignments.get("domain-diversity"), policy.domain_diversity)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown domain diversity mode");
      }
    }
    if (assignments.has("domain-diversity-level")) {
      if (!parse_domain_kind(assignments.get("domain-diversity-level"), policy.domain_diversity_level)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown domain diversity level");
      }
    }
    if (assignments.has("evidence")) {
      if (!parse_evidence_requirement(assignments.get("evidence"), policy.evidence)) {
        return Status::error(ErrorCode::kValueOutOfRange, "unknown evidence requirement");
      }
    }
    if (assignments.has("allow-unverified-capacity")) {
      if (!parse_bool(assignments.get("allow-unverified-capacity"), policy.allow_unverified_capacity)) {
        return Status::error(ErrorCode::kValueOutOfRange, "allow-unverified-capacity must be 0 or 1");
      }
    }
    if (assignments.has("allowed-tiers")) {
      for (const std::string_view tier : split_list(assignments.get("allowed-tiers"), ',')) {
        auto parsed = make_tier_name(tier);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "allowed-tiers: " + parsed.status().detail());
        }
        if (policy.allowed_path_tiers.size() >= kMaxPolicySetEntries) {
          return Status::error(ErrorCode::kTooManyItems, "allowed tier set exceeds the bound");
        }
        policy.allowed_path_tiers.push_back(TierId{parsed.value()});
      }
    }
    return Status::ok();
  }

  Status handle_forbidden(std::string_view line) {
    AssignmentList assignments;
    if (Status status = assignments.parse(line); !status.is_ok()) {
      return status;
    }
    if (Status status = assignments.reject_unknown({"nodes", "tiers", "domains"}); !status.is_ok()) {
      return status;
    }
    Policy& policy = spec_.policy;
    if (assignments.has("nodes")) {
      for (const std::string_view node : split_list(assignments.get("nodes"), ',')) {
        auto parsed = make_identifier(node);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "nodes: " + parsed.status().detail());
        }
        if (policy.forbidden_nodes.size() >= kMaxPolicySetEntries) {
          return Status::error(ErrorCode::kTooManyItems, "forbidden node set exceeds the bound");
        }
        policy.forbidden_nodes.push_back(NodeId{parsed.value()});
      }
    }
    if (assignments.has("tiers")) {
      for (const std::string_view tier : split_list(assignments.get("tiers"), ',')) {
        auto parsed = make_tier_name(tier);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "tiers: " + parsed.status().detail());
        }
        if (policy.forbidden_tiers.size() >= kMaxPolicySetEntries) {
          return Status::error(ErrorCode::kTooManyItems, "forbidden tier set exceeds the bound");
        }
        policy.forbidden_tiers.push_back(TierId{parsed.value()});
      }
    }
    if (assignments.has("domains")) {
      for (const std::string_view domain : split_list(assignments.get("domains"), ',')) {
        auto parsed = make_identifier(domain);
        if (!parsed.has_value()) {
          return Status::error(parsed.status().code(), "domains: " + parsed.status().detail());
        }
        if (policy.forbidden_failure_domains.size() >= kMaxPolicySetEntries) {
          return Status::error(ErrorCode::kTooManyItems, "forbidden failure domain set exceeds the bound");
        }
        policy.forbidden_failure_domains.push_back(FailureDomainId{parsed.value()});
      }
    }
    return Status::ok();
  }

  std::string_view text_{};
  std::size_t line_number_{0};
  bool banner_seen_{false};
  bool collective_seen_{false};
  SectionState section_{};
  std::set<std::string> section_seen_{};
  RequestSpec spec_{};
};

void append_list(std::string& out, const std::vector<std::string>& values) {
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      out.push_back(',');
    }
    out += values[index];
  }
}

}  // namespace

Result<RequestSpec> parse_request_spec(std::string_view text) {
  Parser parser(text);
  return parser.run();
}

Result<PlanningRequest> parse_request_text(std::string_view text) {
  auto spec = parse_request_spec(text);
  if (!spec.has_value()) {
    return Result<PlanningRequest>::failure(spec.status());
  }
  return build_request(std::move(spec.value()));
}

std::string encode_fabric_section(const FabricGraph& fabric) {
  std::string out;
  for (const FailureDomain& domain : fabric.failure_domains()) {
    out += domain.id.value();
    out += " kind=";
    out += to_string(domain.kind);
    if (domain.parent.valid()) {
      out += " parent=";
      out += domain.parent.value();
    }
    out += '\n';
  }
  return out;
}

std::string encode_collective_section(const Collective& collective) {
  std::string out;
  out += "id=";
  out += collective.id.value();
  out += " kind=";
  out += to_string(collective.kind);
  if (collective.demand_mbps > 0) {
    out += " demand=";
    out += std::to_string(collective.demand_mbps);
  }
  out += '\n';
  return out;
}

std::string encode_policy_section(const Policy& policy) {
  std::string out;
  out += "max-hops=" + std::to_string(policy.max_hops) + "\n";
  out += "paths-per-logical-edge=" + std::to_string(policy.paths_per_logical_edge) + "\n";
  out += std::string("disjointness=") + to_string(policy.disjointness) + "\n";
  out += std::string("domain-diversity=") + to_string(policy.domain_diversity) + "\n";
  out += std::string("domain-diversity-level=") + to_string(policy.domain_diversity_level) + "\n";
  out += std::string("evidence=") + to_string(policy.evidence) + "\n";
  out += "latency-weight-milli=" + std::to_string(policy.latency_weight_milli) + "\n";
  out += "congestion-weight-milli=" + std::to_string(policy.congestion_weight_milli) + "\n";
  out += "max-path-cost=" + std::to_string(policy.max_path_cost) + "\n";
  out += "allow-unverified-capacity=" + std::to_string(policy.allow_unverified_capacity ? 1 : 0) + "\n";
  out += "max-search-expansions=" + std::to_string(policy.max_search_expansions) + "\n";
  if (!policy.allowed_path_tiers.empty()) {
    out += "allowed-tiers=";
    std::vector<std::string> tiers;
    tiers.reserve(policy.allowed_path_tiers.size());
    for (const TierId& tier : policy.allowed_path_tiers) {
      tiers.push_back(tier.value());
    }
    append_list(out, tiers);
    out += '\n';
  }
  return out;
}

std::string encode_request_text(const PlanningRequest& request) {
  const FabricGraph& fabric = request.fabric;
  const Collective& collective = request.collective;
  const Policy& policy = request.policy;

  std::string out;
  out += "cpath-request 1\n";
  out += "plan-generation " + std::to_string(request.plan_generation.value()) + "\n";
  out += "topology-generation " + std::to_string(fabric.topology_generation().value()) + "\n";
  out += "failure-domain-generation " + std::to_string(fabric.failure_domain_generation().value()) + "\n";
  out += "capacity-evidence-generation " + std::to_string(fabric.capacity_evidence_generation().value()) + "\n";
  out += "policy-generation " + std::to_string(policy.generation.value()) + "\n";

  out += "\n[collective]\n";
  out += encode_collective_section(collective);

  out += "\n[groups]\n";
  for (const GroupSpec& group : collective.groups) {
    out += group.id.value();
    out += " level=" + std::to_string(group.level);
    out += std::string(" pattern=") + to_string(group.pattern);
    if (group.pattern == GroupPattern::kTree) {
      out += " root=" + group.root.value();
      out += std::string(" direction=") + to_string(group.direction);
    }
    out += " members=";
    std::vector<std::string> members;
    members.reserve(group.members.size());
    for (const ParticipantId& member : group.members) {
      members.push_back(member.value());
    }
    append_list(out, members);
    out += '\n';
  }

  if (!fabric.failure_domains().empty()) {
    out += "\n[domains]\n";
    out += encode_fabric_section(fabric);
  }

  out += "\n[nodes]\n";
  for (const Node& node : fabric.nodes()) {
    out += node.id.value();
    if (node.placement.supplied) {
      if (!node.placement.site.empty()) {
        out += " site=" + node.placement.site;
      }
      if (!node.placement.pod.empty()) {
        out += " pod=" + node.placement.pod;
      }
      if (!node.placement.rack.empty()) {
        out += " rack=" + node.placement.rack;
      }
      if (!node.placement.host.empty()) {
        out += " host=" + node.placement.host;
      }
      out += " device=" + std::to_string(node.placement.device_index);
    }
    if (node.tier.valid()) {
      out += " tier=" + node.tier.value();
    }
    if (!node.eligible) {
      out += " eligible=0";
    }
    out += '\n';
  }

  out += "\n[edges]\n";
  for (const Edge& edge : fabric.edges()) {
    out += edge.id.value();
    out += " from=" + edge.from.value();
    out += " to=" + edge.to.value();
    out += " capacity=" + std::to_string(edge.capacity_mbps);
    out += " latency=" + std::to_string(edge.latency_micros);
    if (edge.reserved_mbps > 0) {
      out += " reserved=" + std::to_string(edge.reserved_mbps);
    }
    if (edge.tier.valid()) {
      out += " tier=" + edge.tier.value();
    }
    if (edge.failure_domain.valid()) {
      out += " domain=" + edge.failure_domain.value();
    }
    out += std::string(" evidence=") + to_string(edge.evidence);
    if (!edge.eligible) {
      out += " eligible=0";
    }
    out += '\n';
  }

  if (!request.bindings.empty()) {
    out += "\n[bindings]\n";
    for (const EndpointBinding& binding : request.bindings) {
      out += binding.participant.value();
      out += " node=" + binding.node.value();
      if (!binding.allowed_failure_domains.empty()) {
        out += " domains=";
        std::vector<std::string> domains;
        domains.reserve(binding.allowed_failure_domains.size());
        for (const FailureDomainId& domain : binding.allowed_failure_domains) {
          domains.push_back(domain.value());
        }
        append_list(out, domains);
      }
      if (!binding.allowed_tiers.empty()) {
        out += " tiers=";
        std::vector<std::string> tiers;
        tiers.reserve(binding.allowed_tiers.size());
        for (const TierId& tier : binding.allowed_tiers) {
          tiers.push_back(tier.value());
        }
        append_list(out, tiers);
      }
      out += '\n';
    }
  }

  out += "\n[policy]\n";
  out += encode_policy_section(policy);

  if (!policy.forbidden_nodes.empty() || !policy.forbidden_tiers.empty() ||
      !policy.forbidden_failure_domains.empty()) {
    out += "\n[forbidden]\n";
    if (!policy.forbidden_nodes.empty()) {
      out += "nodes=";
      std::vector<std::string> nodes;
      nodes.reserve(policy.forbidden_nodes.size());
      for (const NodeId& node : policy.forbidden_nodes) {
        nodes.push_back(node.value());
      }
      append_list(out, nodes);
      out += '\n';
    }
    if (!policy.forbidden_tiers.empty()) {
      out += "tiers=";
      std::vector<std::string> tiers;
      tiers.reserve(policy.forbidden_tiers.size());
      for (const TierId& tier : policy.forbidden_tiers) {
        tiers.push_back(tier.value());
      }
      append_list(out, tiers);
      out += '\n';
    }
    if (!policy.forbidden_failure_domains.empty()) {
      out += "domains=";
      std::vector<std::string> domains;
      domains.reserve(policy.forbidden_failure_domains.size());
      for (const FailureDomainId& domain : policy.forbidden_failure_domains) {
        domains.push_back(domain.value());
      }
      append_list(out, domains);
      out += '\n';
    }
  }
  return out;
}

std::string encode_plan_text(const Plan& plan) {
  std::string out;
  out += "plan " + plan.id.value().hex() + "\n";
  out += "collective " + plan.collective.value() + " kind " + to_string(plan.kind) + "\n";
  out += "generation " + std::to_string(plan.generation.value()) + "\n";
  out += "bindings topology=" + std::to_string(plan.bindings.topology.value()) +
         " failure-domains=" + std::to_string(plan.bindings.failure_domains.value()) +
         " capacity-evidence=" + std::to_string(plan.bindings.capacity_evidence.value()) +
         " policy=" + std::to_string(plan.bindings.policy.value()) +
         " plan=" + std::to_string(plan.bindings.plan.value()) + "\n";
  out += "weakest-evidence " + std::string(to_string(plan.weakest_evidence)) + "\n";
  out += "stats paths=" + std::to_string(plan.stats.path_count) + " hops=" + std::to_string(plan.stats.hop_count) +
         " logical-edges=" + std::to_string(plan.stats.logical_edge_count) +
         " total-cost=" + std::to_string(plan.stats.total_cost) +
         " max-path-cost=" + std::to_string(plan.stats.max_path_cost) +
         " bottleneck-mbps=" + std::to_string(plan.stats.bottleneck_mbps) + "\n";
  for (const Stage& stage : plan.stages) {
    out += "stage " + std::to_string(stage.index) + " logical-edges=" + std::to_string(stage.logical_edges.size()) +
           " paths=" + std::to_string(stage.paths.size()) + "\n";
  }
  for (const PathPlan& path : plan.paths) {
    out += "path " + std::to_string(path.id.value()) + " logical-edge " + std::to_string(path.logical_edge.value()) +
           " stage " + std::to_string(path.stage) + " " + path.src.value() + " -> " + path.dst.value() +
           " cost " + std::to_string(path.cost) + " bottleneck-mbps " + std::to_string(path.bottleneck_mbps) +
           " evidence " + std::string(to_string(path.weakest_evidence)) + "\n";
    if (path.hops.empty()) {
      out += "  hop none (both participants bound to the same endpoint node)\n";
    }
    for (const Hop& hop : path.hops) {
      out += "  hop " + hop.edge.value() + " " + hop.from.value() + " -> " + hop.to.value() + "\n";
    }
    if (!path.domain_signature.empty()) {
      out += "  domains ";
      std::vector<std::string> domains;
      domains.reserve(path.domain_signature.size());
      for (const FailureDomainId& domain : path.domain_signature) {
        domains.push_back(domain.value());
      }
      append_list(out, domains);
      out += '\n';
    }
  }
  for (const Allocation& allocation : plan.allocations) {
    out += "allocation " + allocation.edge.value() + " planned-mbps " +
           std::to_string(allocation.planned_mbps) + "\n";
  }
  return out;
}

}  // namespace cpath
