// cpath - Collective Path Planner command line interface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpath/dsl.hpp"
#include "cpath/explain.hpp"
#include "cpath/persistence.hpp"
#include "cpath/plan.hpp"
#include "cpath/planner.hpp"
#include "cpath/request.hpp"
#include "cpath/server.hpp"
#include "cpath/version.hpp"

namespace {

// Exit code contract of this tool. Every code names a distinct failure class so
// that scripts can react without parsing prose.
enum ExitCode : int {
  kExitOk = 0,
  kExitUsage = 1,
  kExitInput = 2,
  kExitDenied = 3,
  kExitPersistence = 4,
  kExitService = 5,
  kExitIo = 6,
};

int exit_code_for(cpath::ErrorCode code) {
  switch (cpath::category_of(code)) {
    case cpath::ErrorCategory::kNone:
      return kExitOk;
    case cpath::ErrorCategory::kInput:
      return kExitInput;
    case cpath::ErrorCategory::kAuthority:
    case cpath::ErrorCategory::kPlanning:
      return kExitDenied;
    case cpath::ErrorCategory::kPersistence:
      return kExitPersistence;
    case cpath::ErrorCategory::kProtocol:
      return kExitService;
    case cpath::ErrorCategory::kInternal:
      return kExitIo;
  }
  return kExitIo;
}

void print_version() { std::cout << cpath::build_summary() << "\n"; }

void print_usage() {
  std::cout <<
      "cpath " << cpath::version_string() << " - Collective Path Planner\n"
      "\n"
      "usage: cpath <command> [options]\n"
      "\n"
      "inspection\n"
      "  version                                     print build identification\n"
      "  validate   --request FILE                   parse and validate a request\n"
      "  canonical  --request FILE                   print the canonical request text\n"
      "  digest     --request FILE                   print the canonical request digest\n"
      "  plan       --request FILE [options]         produce a plan\n"
      "               --out FILE                     write the plan record\n"
      "               --json | --text | --explain    select the output form\n"
      "  explain    --plan FILE [--request FILE]     explain a plan record\n"
      "  compare    --a FILE --b FILE                compare two plan records\n"
      "\n"
      "durable store\n"
      "  store put        --store DIR --request FILE [--plan FILE]\n"
      "  store list       --store DIR [--json]\n"
      "  store show       --store DIR --id HEX [--text]\n"
      "  store revalidate --store DIR --id HEX --request FILE\n"
      "  store verify     --store DIR\n"
      "  store erase      --store DIR --id HEX\n"
      "  store bounds     --store DIR\n"
      "\n"
      "coordinator (real TCP to a cpathd process)\n"
      "  rpc ping       --endpoint HOST:PORT\n"
      "  rpc plan       --endpoint HOST:PORT --request FILE [--explain] [--out FILE]\n"
      "  rpc list       --endpoint HOST:PORT [--json]\n"
      "  rpc get        --endpoint HOST:PORT --id HEX [--out FILE]\n"
      "  rpc revalidate --endpoint HOST:PORT --id HEX --request FILE\n"
      "  rpc shutdown   --endpoint HOST:PORT --epoch N\n";
}

struct Arguments {
  std::vector<std::string> positional{};
  std::vector<std::pair<std::string, std::string>> options{};

  bool has(std::string_view name) const {
    for (const auto& option : options) {
      if (option.first == name) {
        return true;
      }
    }
    return false;
  }

  const std::string* get(std::string_view name) const {
    for (const auto& option : options) {
      if (option.first == name) {
        return &option.second;
      }
    }
    return nullptr;
  }
};

bool parse_arguments(const std::vector<std::string>& tokens, Arguments& out) {
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& token = tokens[index];
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      if (index + 1u < tokens.size() && tokens[index + 1u].compare(0, 2, "--") != 0) {
        out.options.emplace_back(name, tokens[index + 1u]);
        ++index;
      } else {
        out.options.emplace_back(name, std::string());
      }
    } else {
      out.positional.push_back(token);
    }
  }
  return true;
}

cpath::Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return cpath::Result<std::string>::failure(cpath::ErrorCode::kStoreIoFailure,
                                               "cannot open " + path + " for reading");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return cpath::Result<std::string>::failure(cpath::ErrorCode::kStoreIoFailure,
                                               "cannot determine the size of " + path);
  }
  if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(cpath::kMaxInputBytes)) {
    return cpath::Result<std::string>::failure(cpath::ErrorCode::kInputTooLarge,
                                               path + " exceeds the accepted input bound");
  }
  stream.seekg(0, std::ios::beg);
  std::string content(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    stream.read(content.data(), size);
    if (stream.gcount() != size) {
      return cpath::Result<std::string>::failure(cpath::ErrorCode::kStoreIoFailure,
                                                 "short read from " + path);
    }
  }
  return cpath::Result<std::string>::success(std::move(content));
}

cpath::Result<std::vector<std::uint8_t>> read_binary_file(const std::string& path, std::size_t max_bytes) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return cpath::Result<std::vector<std::uint8_t>>::failure(cpath::ErrorCode::kStoreIoFailure,
                                                             "cannot open " + path + " for reading");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return cpath::Result<std::vector<std::uint8_t>>::failure(cpath::ErrorCode::kStoreIoFailure,
                                                             "cannot determine the size of " + path);
  }
  if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(max_bytes)) {
    return cpath::Result<std::vector<std::uint8_t>>::failure(cpath::ErrorCode::kOversizedRecord,
                                                             path + " exceeds the accepted record bound");
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> content(static_cast<std::size_t>(size), 0u);
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(content.data()), size);
    if (stream.gcount() != size) {
      return cpath::Result<std::vector<std::uint8_t>>::failure(cpath::ErrorCode::kStoreIoFailure,
                                                               "short read from " + path);
    }
  }
  return cpath::Result<std::vector<std::uint8_t>>::success(std::move(content));
}

cpath::Status write_binary_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return cpath::Status::error(cpath::ErrorCode::kStoreIoFailure, "cannot open " + path + " for writing");
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  stream.flush();
  if (!stream) {
    return cpath::Status::error(cpath::ErrorCode::kStoreIoFailure, "failed to write " + path);
  }
  return cpath::Status::ok();
}

cpath::Result<cpath::Digest> digest_option(const Arguments& arguments) {
  const std::string* text = arguments.get("id");
  if (text == nullptr) {
    return cpath::Result<cpath::Digest>::failure(cpath::ErrorCode::kMissingRequiredField,
                                                 "--id is required for this command");
  }
  return cpath::Digest::from_hex(*text);
}

cpath::Result<cpath::PlanningRequest> request_option(const Arguments& arguments) {
  const std::string* path = arguments.get("request");
  if (path == nullptr) {
    return cpath::Result<cpath::PlanningRequest>::failure(cpath::ErrorCode::kMissingRequiredField,
                                                          "--request is required for this command");
  }
  auto text = read_text_file(*path);
  if (!text.has_value()) {
    return cpath::Result<cpath::PlanningRequest>::failure(text.status());
  }
  return cpath::parse_request_text(text.value());
}

cpath::Result<cpath::Plan> plan_option(const Arguments& arguments, std::string_view name) {
  const std::string* path = arguments.get(name);
  if (path == nullptr) {
    return cpath::Result<cpath::Plan>::failure(cpath::ErrorCode::kMissingRequiredField,
                                               "--" + std::string(name) + " is required for this command");
  }
  auto bytes = read_binary_file(*path, cpath::kMaxPlanRecordBytes);
  if (!bytes.has_value()) {
    return cpath::Result<cpath::Plan>::failure(bytes.status());
  }
  cpath::StoredPlanSummary summary;
  return cpath::decode_plan_record(bytes.value(), &summary);
}

std::vector<std::uint8_t> plan_record_for(const cpath::Plan& plan, const cpath::PlanningRequest& request) {
  return cpath::encode_plan_record(plan, request, cpath::Incarnation{1}, 1u, 0u);
}

void report(const cpath::Status& status, std::string_view context) {
  std::cerr << "cpath: " << context << ": " << status.to_string() << "\n";
}

// --- command handlers ------------------------------------------------------

int command_validate(const Arguments& arguments) {
  auto request = request_option(arguments);
  if (!request.has_value()) {
    report(request.status(), "validate");
    return exit_code_for(request.status().code());
  }
  std::cout << "request valid: collective " << request.value().collective.id.value() << " ("
            << cpath::to_string(request.value().collective.kind) << "), "
            << request.value().collective.logical_edges.size() << " logical edges, "
            << request.value().fabric.node_count() << " nodes, " << request.value().fabric.edge_count()
            << " edges, " << request.value().bindings.size() << " bindings\n";
  std::cout << "canonical digest " << request.value().canonical_digest.hex() << " (" 
            << request.value().canonical_bytes << " bytes)\n";
  return kExitOk;
}

int command_canonical(const Arguments& arguments) {
  auto request = request_option(arguments);
  if (!request.has_value()) {
    report(request.status(), "canonical");
    return exit_code_for(request.status().code());
  }
  std::cout << cpath::encode_request_text(request.value());
  return kExitOk;
}

int command_digest(const Arguments& arguments) {
  auto request = request_option(arguments);
  if (!request.has_value()) {
    report(request.status(), "digest");
    return exit_code_for(request.status().code());
  }
  std::cout << request.value().canonical_digest.hex() << "\n";
  return kExitOk;
}

int command_plan(const Arguments& arguments) {
  auto request = request_option(arguments);
  if (!request.has_value()) {
    report(request.status(), "plan");
    return exit_code_for(request.status().code());
  }
  const cpath::PlanningOutcome outcome = cpath::plan_collective(request.value());
  if (!outcome.ok()) {
    std::cerr << cpath::explain_denials(outcome);
    return exit_code_for(outcome.primary_code());
  }
  const cpath::Plan& plan = *outcome.plan;
  const std::string* out_path = arguments.get("out");
  if (out_path != nullptr && !out_path->empty()) {
    const std::vector<std::uint8_t> record = plan_record_for(plan, request.value());
    if (cpath::Status status = write_binary_file(*out_path, record); !status.is_ok()) {
      report(status, "plan");
      return kExitIo;
    }
  }
  if (arguments.has("json")) {
    std::cout << cpath::render_plan_json(plan) << "\n";
  } else if (arguments.has("text") || arguments.has("explain")) {
    std::cout << cpath::encode_plan_text(plan);
  } else {
    std::cout << "plan " << plan.id.value().hex() << " paths " << plan.stats.path_count << " hops "
              << plan.stats.hop_count << " total-cost " << plan.stats.total_cost << " weakest-evidence "
              << cpath::to_string(plan.weakest_evidence) << " search-expansions " << outcome.search_expansions
              << "\n";
  }
  if (out_path != nullptr && !out_path->empty()) {
    std::cout << "wrote " << *out_path << "\n";
  }
  return kExitOk;
}

int command_explain(const Arguments& arguments) {
  auto plan = plan_option(arguments, "plan");
  if (!plan.has_value()) {
    report(plan.status(), "explain");
    return exit_code_for(plan.status().code());
  }
  std::optional<cpath::PlanningRequest> request;
  if (arguments.has("request")) {
    auto loaded = request_option(arguments);
    if (!loaded.has_value()) {
      report(loaded.status(), "explain");
      return exit_code_for(loaded.status().code());
    }
    request = std::move(loaded.value());
  }
  std::cout << cpath::explain_plan(plan.value(), request.has_value() ? &(*request) : nullptr);
  return kExitOk;
}

int command_compare(const Arguments& arguments) {
  auto left = plan_option(arguments, "a");
  if (!left.has_value()) {
    report(left.status(), "compare");
    return exit_code_for(left.status().code());
  }
  auto right = plan_option(arguments, "b");
  if (!right.has_value()) {
    report(right.status(), "compare");
    return exit_code_for(right.status().code());
  }
  const cpath::PlanComparison comparison = cpath::compare_plans(left.value(), right.value());
  std::cout << comparison.summary << "\n";
  std::cout << "same-input-digest " << (comparison.same_input_digest ? "yes" : "no") << "\n";
  std::cout << "same-collective " << (comparison.same_collective ? "yes" : "no") << "\n";
  for (const cpath::PathDifference& difference : comparison.differences) {
    std::cout << "  logical edge " << difference.logical_edge.value() << ": " << difference.summary << "\n";
  }
  return comparison.identical ? kExitOk : kExitOk;
}

int command_store(const Arguments& arguments) {
  const std::string* root = arguments.get("store");
  if (root == nullptr) {
    std::cerr << "cpath: store: --store is required\n";
    return kExitUsage;
  }
  if (arguments.positional.empty()) {
    std::cerr << "cpath: store: an action is required\n";
    return kExitUsage;
  }
  const std::string& action = arguments.positional.front();

  cpath::StoreConfig config;
  config.root = *root;
  cpath::PlanStore store(config);
  if (cpath::Status status = store.open(); !status.is_ok()) {
    report(status, "store open");
    return exit_code_for(status.code());
  }

  int result = kExitOk;
  if (action == "put") {
    auto request = request_option(arguments);
    if (!request.has_value()) {
      report(request.status(), "store put");
      return exit_code_for(request.status().code());
    }
    auto plan = plan_option(arguments, "plan");
    cpath::Plan produced;
    if (plan.has_value()) {
      produced = std::move(plan.value());
    } else {
      const cpath::PlanningOutcome outcome = cpath::plan_collective(request.value());
      if (!outcome.ok()) {
        std::cerr << cpath::explain_denials(outcome);
        store.close();
        return exit_code_for(outcome.primary_code());
      }
      produced = *outcome.plan;
      if (arguments.has("plan")) {
        std::cerr << "cpath: store put: --plan could not be read (" << plan.status().to_string()
                  << "); planning instead\n";
      }
    }
    auto summary = store.put(produced, request.value());
    if (!summary.has_value()) {
      report(summary.status(), "store put");
      result = exit_code_for(summary.status().code());
    } else {
      std::cout << "stored " << summary.value().id.value().hex() << " bytes "
                << summary.value().record_bytes << " sequence " << summary.value().stored_sequence << "\n";
    }
  } else if (action == "list") {
    auto plans = store.list();
    if (!plans.has_value()) {
      report(plans.status(), "store list");
      result = exit_code_for(plans.status().code());
    } else if (arguments.has("json")) {
      std::cout << "[";
      for (std::size_t index = 0; index < plans.value().size(); ++index) {
        if (index > 0) {
          std::cout << ",";
        }
        std::cout << "{\"plan_id\":\"" << plans.value()[index].id.value().hex() << "\",\"collective\":\""
                  << plans.value()[index].collective.value() << "\",\"kind\":\""
                  << cpath::to_string(plans.value()[index].kind) << "\",\"generation\":"
                  << plans.value()[index].generation.value() << ",\"sequence\":"
                  << plans.value()[index].stored_sequence << ",\"bytes\":"
                  << plans.value()[index].record_bytes << ",\"written_incarnation\":"
                  << plans.value()[index].written_incarnation.value() << "}";
      }
      std::cout << "]\n";
    } else {
      for (const cpath::StoredPlanSummary& entry : plans.value()) {
        std::cout << entry.id.value().hex() << " " << entry.collective.value() << " "
                  << cpath::to_string(entry.kind) << " generation " << entry.generation.value()
                  << " sequence " << entry.stored_sequence << " bytes " << entry.record_bytes
                  << " written-incarnation " << entry.written_incarnation.value() << "\n";
      }
      std::cout << plans.value().size() << " plan(s) in " << *root << "\n";
    }
  } else if (action == "show") {
    auto id = digest_option(arguments);
    if (!id.has_value()) {
      report(id.status(), "store show");
      result = exit_code_for(id.status().code());
    } else {
      auto plan = store.get(cpath::CollectivePlanId{id.value()});
      if (!plan.has_value()) {
        report(plan.status(), "store show");
        result = exit_code_for(plan.status().code());
      } else if (arguments.has("text")) {
        std::cout << cpath::encode_plan_text(plan.value());
      } else {
        std::cout << cpath::explain_plan(plan.value(), nullptr);
        std::cout << "note: a plan reopened from storage is unverified until revalidated against "
                     "current inputs\n";
      }
    }
  } else if (action == "revalidate") {
    auto id = digest_option(arguments);
    auto request = request_option(arguments);
    if (!id.has_value() || !request.has_value()) {
      const cpath::Status status = !id.has_value() ? id.status() : request.status();
      report(status, "store revalidate");
      result = exit_code_for(status.code());
    } else {
      auto outcome = store.revalidate(cpath::CollectivePlanId{id.value()}, request.value());
      if (!outcome.has_value()) {
        report(outcome.status(), "store revalidate");
        result = exit_code_for(outcome.status().code());
      } else {
        std::cout << "still-valid " << (outcome.value().still_valid ? "yes" : "no") << " freshness "
                  << cpath::to_string(outcome.value().freshness) << "\n";
        if (!outcome.value().detail.empty()) {
          std::cout << "  " << outcome.value().detail << "\n";
        }
        if (outcome.value().replanned.has_value()) {
          std::cout << "  replanned under current inputs: "
                    << outcome.value().replanned->id.value().hex() << "\n";
        }
      }
    }
  } else if (action == "verify") {
    auto verified = store.verify_all();
    if (!verified.has_value()) {
      report(verified.status(), "store verify");
      result = exit_code_for(verified.status().code());
    } else {
      std::cout << verified.value().size() << " verified record(s) of " << store.record_count()
                << " reachable record(s); store bytes " << store.total_bytes() << "\n";
    }
  } else if (action == "erase") {
    auto id = digest_option(arguments);
    if (!id.has_value()) {
      report(id.status(), "store erase");
      result = exit_code_for(id.status().code());
    } else if (cpath::Status status = store.erase(cpath::CollectivePlanId{id.value()}); !status.is_ok()) {
      report(status, "store erase");
      result = exit_code_for(status.code());
    } else {
      std::cout << "erased " << id.value().hex() << "\n";
    }
  } else if (action == "bounds") {
    if (cpath::Status status = store.enforce_bounds(); !status.is_ok()) {
      report(status, "store bounds");
      result = exit_code_for(status.code());
    } else {
      std::cout << store.record_count() << " record(s), " << store.total_bytes() << " bytes, epoch "
                << store.epoch().value() << ", incarnation " << store.incarnation().value() << "\n";
    }
  } else {
    std::cerr << "cpath: store: unknown action " << action << "\n";
    store.close();
    return kExitUsage;
  }

  if (cpath::Status status = store.close(); !status.is_ok() && result == kExitOk) {
    report(status, "store close");
    result = exit_code_for(status.code());
  }
  return result;
}

int command_rpc(const Arguments& arguments) {
  const std::string* endpoint = arguments.get("endpoint");
  if (endpoint == nullptr) {
    std::cerr << "cpath: rpc: --endpoint is required\n";
    return kExitUsage;
  }
  if (arguments.positional.empty()) {
    std::cerr << "cpath: rpc: an action is required\n";
    return kExitUsage;
  }
  std::string host;
  auto port = cpath::parse_endpoint(*endpoint, host);
  if (!port.has_value()) {
    report(port.status(), "rpc");
    return kExitUsage;
  }

  cpath::ServiceClient client;
  if (cpath::Status status = client.connect(host, port.value()); !status.is_ok()) {
    report(status, "rpc connect");
    return kExitService;
  }
  auto handshake = client.handshake("cpath-cli");
  if (!handshake.has_value()) {
    report(handshake.status(), "rpc handshake");
    return kExitService;
  }

  const std::string& action = arguments.positional.front();
  int result = kExitOk;
  if (action == "ping") {
    if (cpath::Status status = client.ping(); !status.is_ok()) {
      report(status, "rpc ping");
      result = kExitService;
    } else {
      std::cout << "pong from " << handshake.value().server_label << " session "
                << handshake.value().session_id << " epoch " << handshake.value().epoch << " incarnation "
                << handshake.value().incarnation << "\n";
    }
  } else if (action == "plan") {
    auto request = request_option(arguments);
    if (!request.has_value()) {
      report(request.status(), "rpc plan");
      result = exit_code_for(request.status().code());
    } else {
      std::string text = cpath::encode_request_text(request.value());
      auto response = client.plan(text, arguments.has("explain"));
      if (!response.has_value()) {
        report(response.status(), "rpc plan");
        result = kExitService;
      } else if (!response.value().has_plan) {
        std::cerr << "planning refused by coordinator: code "
                  << cpath::code_symbol(static_cast<cpath::ErrorCode>(response.value().code)) << "\n";
        for (const cpath::DenialPayload& denial : response.value().denials) {
          std::cerr << "  " << cpath::code_symbol(static_cast<cpath::ErrorCode>(denial.code)) << " "
                    << cpath::to_string(static_cast<cpath::DenialKind>(denial.kind)) << " conflict "
                    << denial.conflict << " logical-edge " << denial.logical_edge << " " << denial.message
                    << "\n";
        }
        result = kExitDenied;
      } else {
        cpath::StoredPlanSummary summary;
        auto plan = cpath::decode_plan_record(response.value().plan_record, &summary);
        if (!plan.has_value()) {
          report(plan.status(), "rpc plan");
          result = kExitService;
        } else {
          const std::string* out_path = arguments.get("out");
          if (out_path != nullptr && !out_path->empty()) {
            if (cpath::Status status = write_binary_file(*out_path, response.value().plan_record);
                !status.is_ok()) {
              report(status, "rpc plan");
              result = kExitIo;
            }
          }
          if (!response.value().explanation.empty()) {
            std::cout << response.value().explanation;
          } else {
            std::cout << cpath::encode_plan_text(plan.value());
          }
          std::cout << "coordinator search-expansions " << response.value().search_expansions << "\n";
        }
      }
    }
  } else if (action == "list") {
    auto response = client.list_plans();
    if (!response.has_value()) {
      report(response.status(), "rpc list");
      result = kExitService;
    } else {
      for (const cpath::PlanSummaryPayload& entry : response.value().plans) {
        std::cout << entry.plan_id.hex() << " " << entry.collective << " generation " << entry.generation
                  << " sequence " << entry.stored_sequence << " bytes " << entry.record_bytes << "\n";
      }
      std::cout << response.value().plans.size() << " plan(s) held by the coordinator\n";
    }
  } else if (action == "get") {
    auto id = digest_option(arguments);
    if (!id.has_value()) {
      report(id.status(), "rpc get");
      result = exit_code_for(id.status().code());
    } else {
      auto response = client.get_plan(id.value());
      if (!response.has_value()) {
        report(response.status(), "rpc get");
        result = kExitService;
      } else if (!response.value().has_plan) {
        std::cerr << "cpath: rpc get: "
                  << cpath::code_symbol(static_cast<cpath::ErrorCode>(response.value().code)) << "\n";
        result = kExitPersistence;
      } else {
        const std::string* out_path = arguments.get("out");
        if (out_path != nullptr && !out_path->empty()) {
          if (cpath::Status status = write_binary_file(*out_path, response.value().plan_record);
              !status.is_ok()) {
            report(status, "rpc get");
            result = kExitIo;
          } else {
            std::cout << "wrote " << *out_path << "\n";
          }
        } else {
          cpath::StoredPlanSummary summary;
          auto plan = cpath::decode_plan_record(response.value().plan_record, &summary);
          if (!plan.has_value()) {
            report(plan.status(), "rpc get");
            result = kExitService;
          } else {
            std::cout << cpath::explain_plan(plan.value(), nullptr);
          }
        }
      }
    }
  } else if (action == "revalidate") {
    auto id = digest_option(arguments);
    auto request = request_option(arguments);
    if (!id.has_value() || !request.has_value()) {
      const cpath::Status status = !id.has_value() ? id.status() : request.status();
      report(status, "rpc revalidate");
      result = exit_code_for(status.code());
    } else {
      auto response = client.revalidate(id.value(), cpath::encode_request_text(request.value()));
      if (!response.has_value()) {
        report(response.status(), "rpc revalidate");
        result = kExitService;
      } else {
        std::cout << "still-valid " << (response.value().still_valid ? "yes" : "no") << " freshness "
                  << cpath::to_string(static_cast<cpath::PlanFreshness>(response.value().freshness)) << "\n";
        if (!response.value().detail.empty()) {
          std::cout << "  " << response.value().detail << "\n";
        }
      }
    }
  } else if (action == "shutdown") {
    const std::string* epoch_text = arguments.get("epoch");
    if (epoch_text == nullptr) {
      std::cerr << "cpath: rpc shutdown: --epoch is required\n";
      result = kExitUsage;
    } else {
      std::uint64_t epoch = 0;
      try {
        epoch = static_cast<std::uint64_t>(std::stoull(*epoch_text));
      } catch (const std::exception&) {
        std::cerr << "cpath: rpc shutdown: --epoch must be a number\n";
        client.close();
        return kExitUsage;
      }
      auto response = client.shutdown(epoch);
      if (!response.has_value()) {
        report(response.status(), "rpc shutdown");
        result = kExitService;
      } else {
        std::cout << "shutdown acknowledged for epoch " << response.value().expected_epoch << "\n";
      }
    }
  } else {
    std::cerr << "cpath: rpc: unknown action " << action << "\n";
    result = kExitUsage;
  }
  client.close();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens(argv + 1, argv + argc);
  if (tokens.empty()) {
    print_usage();
    return kExitUsage;
  }
  const std::string command = tokens.front();
  if (command == "--help" || command == "-h" || command == "help") {
    print_usage();
    return kExitOk;
  }
  if (command == "--version" || command == "version") {
    print_version();
    return kExitOk;
  }

  Arguments arguments;
  parse_arguments(std::vector<std::string>(tokens.begin() + 1, tokens.end()), arguments);

  if (command == "validate") {
    return command_validate(arguments);
  }
  if (command == "canonical") {
    return command_canonical(arguments);
  }
  if (command == "digest") {
    return command_digest(arguments);
  }
  if (command == "plan") {
    return command_plan(arguments);
  }
  if (command == "explain") {
    return command_explain(arguments);
  }
  if (command == "compare") {
    return command_compare(arguments);
  }
  if (command == "store") {
    return command_store(arguments);
  }
  if (command == "rpc") {
    return command_rpc(arguments);
  }

  std::cerr << "cpath: unknown command " << command << "\n";
  print_usage();
  return kExitUsage;
}
