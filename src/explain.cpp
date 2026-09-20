// Collective Path Planner - plan explanation and comparison.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/explain.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace cpath {
namespace {

void append_json_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20u) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(ch >> 4u) & 0x0Fu]);
          out.push_back(kHex[ch & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_key(std::string& out, std::string_view key) {
  append_json_string(out, key);
  out.push_back(':');
}

std::string hops_to_string(const PathPlan& path) {
  std::string out;
  if (path.hops.empty()) {
    return "(local)";
  }
  out += path.hops.front().from.value();
  for (const Hop& hop : path.hops) {
    out += " -> ";
    out += hop.to.value();
  }
  return out;
}

}  // namespace

std::string explain_plan(const Plan& plan, const PlanningRequest* request) {
  std::string out;
  out += "plan " + plan.id.value().hex() + "\n";
  out += "  collective        : " + plan.collective.value() + " (" + to_string(plan.kind) + ")\n";
  out += "  plan generation   : " + std::to_string(plan.generation.value()) + "\n";
  out += "  bound generations : topology=" + std::to_string(plan.bindings.topology.value()) +
         " failure-domains=" + std::to_string(plan.bindings.failure_domains.value()) +
         " capacity-evidence=" + std::to_string(plan.bindings.capacity_evidence.value()) +
         " policy=" + std::to_string(plan.bindings.policy.value()) + "\n";
  out += "  weakest evidence  : " + std::string(to_string(plan.weakest_evidence)) + "\n";
  out += "  stages            : " + std::to_string(plan.stages.size()) + "\n";
  out += "  logical edges     : " + std::to_string(plan.stats.logical_edge_count) + "\n";
  out += "  physical paths    : " + std::to_string(plan.stats.path_count) + "\n";
  out += "  physical hops     : " + std::to_string(plan.stats.hop_count) + "\n";
  out += "  total cost        : " + std::to_string(plan.stats.total_cost) + "\n";
  out += "  max path cost     : " + std::to_string(plan.stats.max_path_cost) + "\n";
  out += "  bottleneck (mbps) : " + std::to_string(plan.stats.bottleneck_mbps) + "\n";

  std::map<std::uint32_t, std::vector<const PathPlan*>> by_stage;
  for (const PathPlan& path : plan.paths) {
    by_stage[path.stage].push_back(&path);
  }
  for (const auto& [stage, paths] : by_stage) {
    out += "  stage " + std::to_string(stage) + ":\n";
    for (const PathPlan* path : paths) {
      out += "    logical edge " + std::to_string(path->logical_edge.value()) + " (" + path->src.value() +
             " -> " + path->dst.value() + ")\n";
      out += "      path " + std::to_string(path->id.value()) + " cost " + std::to_string(path->cost) +
             " bottleneck-mbps " + std::to_string(path->bottleneck_mbps) + " evidence " +
             to_string(path->weakest_evidence) + "\n";
      out += "      route " + hops_to_string(*path) + "\n";
      for (const Hop& hop : path->hops) {
        out += "        hop " + hop.edge.value() + " " + hop.from.value() + " -> " + hop.to.value() + "\n";
      }
      if (!path->domain_signature.empty()) {
        out += "      failure domains ";
        for (std::size_t index = 0; index < path->domain_signature.size(); ++index) {
          if (index > 0) {
            out += ", ";
          }
          out += path->domain_signature[index].value();
        }
        out += "\n";
      }
    }
  }

  if (!plan.allocations.empty()) {
    out += "  proposed allocations (not admission):\n";
    for (const Allocation& allocation : plan.allocations) {
      out += "    " + allocation.edge.value() + " " + std::to_string(allocation.planned_mbps) + " mbps\n";
    }
  }

  if (request != nullptr) {
    out += "  re-check against supplied inputs:\n";
    const Status status = validate_plan(plan, *request);
    if (status.is_ok()) {
      out += "    all plan invariants hold for these inputs\n";
    } else {
      out += "    DIVERGENCE: " + status.to_string() + "\n";
    }
    const PlanFreshness freshness = assess_freshness(plan, request->current_bindings());
    ErrorCode code = ErrorCode::kOk;
    out += std::string("    freshness: ") + to_string(freshness) + " - " +
           describe_freshness(freshness, code) + "\n";
  }
  return out;
}

std::string explain_denials(const PlanningOutcome& outcome) {
  std::string out;
  if (outcome.ok()) {
    out += "planning succeeded; no denials\n";
    return out;
  }
  out += "planning refused: " + std::to_string(outcome.denials.size()) + " denial(s)\n";
  for (const DenialDetail& denial : outcome.denials) {
    out += "  " + std::string(code_symbol(denial.code)) + " (" + to_string(denial.code) + ")";
    out += " conflict=" + std::string(to_string(denial.conflict));
    if (denial.logical_edge.valid()) {
      out += " logical-edge=" + std::to_string(denial.logical_edge.value());
    }
    if (denial.participant.valid()) {
      out += " participant=" + denial.participant.value();
    }
    out += "\n";
    if (!denial.message.empty()) {
      out += "    " + denial.message + "\n";
    }
    if (!denial.nodes.empty()) {
      out += "    nodes:";
      for (const NodeId& node : denial.nodes) {
        out += " " + node.value();
      }
      out += "\n";
    }
    if (!denial.edges.empty()) {
      out += "    edges:";
      for (const EdgeId& edge : denial.edges) {
        out += " " + edge.value();
      }
      out += "\n";
    }
    if (!denial.failure_domains.empty()) {
      out += "    failure domains:";
      for (const FailureDomainId& domain : denial.failure_domains) {
        out += " " + domain.value();
      }
      out += "\n";
    }
    if (!denial.tiers.empty()) {
      out += "    tiers:";
      for (const TierId& tier : denial.tiers) {
        out += " " + tier.value();
      }
      out += "\n";
    }
  }
  return out;
}

std::string render_plan_json(const Plan& plan) {
  std::string out;
  out += "{";
  append_key(out, "plan_id");
  append_json_string(out, plan.id.value().hex());
  out += ",";
  append_key(out, "collective");
  append_json_string(out, plan.collective.value());
  out += ",";
  append_key(out, "kind");
  append_json_string(out, to_string(plan.kind));
  out += ",";
  append_key(out, "generation");
  out += std::to_string(plan.generation.value());
  out += ",";
  append_key(out, "bindings");
  out += "{\"topology\":" + std::to_string(plan.bindings.topology.value()) +
         ",\"failure_domains\":" + std::to_string(plan.bindings.failure_domains.value()) +
         ",\"capacity_evidence\":" + std::to_string(plan.bindings.capacity_evidence.value()) +
         ",\"policy\":" + std::to_string(plan.bindings.policy.value()) +
         ",\"plan\":" + std::to_string(plan.bindings.plan.value()) + "}";
  out += ",";
  append_key(out, "weakest_evidence");
  append_json_string(out, to_string(plan.weakest_evidence));
  out += ",";
  append_key(out, "stage_count");
  out += std::to_string(plan.stage_count);
  out += ",";
  append_key(out, "stats");
  out += "{\"paths\":" + std::to_string(plan.stats.path_count) +
         ",\"hops\":" + std::to_string(plan.stats.hop_count) +
         ",\"logical_edges\":" + std::to_string(plan.stats.logical_edge_count) +
         ",\"total_cost\":" + std::to_string(plan.stats.total_cost) +
         ",\"max_path_cost\":" + std::to_string(plan.stats.max_path_cost) +
         ",\"bottleneck_mbps\":" + std::to_string(plan.stats.bottleneck_mbps) + "}";
  out += ",";
  append_key(out, "paths");
  out += "[";
  for (std::size_t index = 0; index < plan.paths.size(); ++index) {
    const PathPlan& path = plan.paths[index];
    if (index > 0) {
      out += ",";
    }
    out += "{";
    append_key(out, "id");
    out += std::to_string(path.id.value());
    out += ",";
    append_key(out, "logical_edge");
    out += std::to_string(path.logical_edge.value());
    out += ",";
    append_key(out, "stage");
    out += std::to_string(path.stage);
    out += ",";
    append_key(out, "src");
    append_json_string(out, path.src.value());
    out += ",";
    append_key(out, "dst");
    append_json_string(out, path.dst.value());
    out += ",";
    append_key(out, "cost");
    out += std::to_string(path.cost);
    out += ",";
    append_key(out, "bottleneck_mbps");
    out += std::to_string(path.bottleneck_mbps);
    out += ",";
    append_key(out, "evidence");
    append_json_string(out, to_string(path.weakest_evidence));
    out += ",";
    append_key(out, "hops");
    out += "[";
    for (std::size_t hop_index = 0; hop_index < path.hops.size(); ++hop_index) {
      const Hop& hop = path.hops[hop_index];
      if (hop_index > 0) {
        out += ",";
      }
      out += "{\"edge\":";
      append_json_string(out, hop.edge.value());
      out += ",\"from\":";
      append_json_string(out, hop.from.value());
      out += ",\"to\":";
      append_json_string(out, hop.to.value());
      out += "}";
    }
    out += "]";
    out += ",";
    append_key(out, "failure_domains");
    out += "[";
    for (std::size_t domain_index = 0; domain_index < path.domain_signature.size(); ++domain_index) {
      if (domain_index > 0) {
        out += ",";
      }
      append_json_string(out, path.domain_signature[domain_index].value());
    }
    out += "]";
    out += "}";
  }
  out += "]";
  out += ",";
  append_key(out, "allocations");
  out += "[";
  for (std::size_t index = 0; index < plan.allocations.size(); ++index) {
    if (index > 0) {
      out += ",";
    }
    out += "{\"edge\":";
    append_json_string(out, plan.allocations[index].edge.value());
    out += ",\"planned_mbps\":" + std::to_string(plan.allocations[index].planned_mbps) + "}";
  }
  out += "]";
  out += "}";
  return out;
}

std::string render_outcome_json(const PlanningOutcome& outcome) {
  std::string out;
  out += "{";
  append_key(out, "ok");
  out += outcome.ok() ? "true" : "false";
  out += ",";
  append_key(out, "search_expansions");
  out += std::to_string(outcome.search_expansions);
  out += ",";
  append_key(out, "denials");
  out += "[";
  for (std::size_t index = 0; index < outcome.denials.size(); ++index) {
    const DenialDetail& denial = outcome.denials[index];
    if (index > 0) {
      out += ",";
    }
    out += "{";
    append_key(out, "code");
    append_json_string(out, code_symbol(denial.code));
    out += ",";
    append_key(out, "conflict");
    append_json_string(out, to_string(denial.conflict));
    out += ",";
    append_key(out, "logical_edge");
    out += std::to_string(denial.logical_edge.value());
    out += ",";
    append_key(out, "participant");
    append_json_string(out, denial.participant.value());
    out += ",";
    append_key(out, "message");
    append_json_string(out, denial.message);
    out += "}";
  }
  out += "]";
  if (outcome.plan.has_value()) {
    out += ",";
    append_key(out, "plan");
    out += render_plan_json(*outcome.plan);
  }
  out += "}";
  return out;
}

std::string render_freshness_json(const Plan& plan, PlanFreshness freshness) {
  ErrorCode code = ErrorCode::kOk;
  const char* description = describe_freshness(freshness, code);
  std::string out;
  out += "{";
  append_key(out, "plan_id");
  append_json_string(out, plan.id.value().hex());
  out += ",";
  append_key(out, "freshness");
  append_json_string(out, to_string(freshness));
  out += ",";
  append_key(out, "code");
  append_json_string(out, code_symbol(code));
  out += ",";
  append_key(out, "current");
  out += freshness == PlanFreshness::kCurrent ? "true" : "false";
  out += ",";
  append_key(out, "description");
  append_json_string(out, description);
  out += "}";
  return out;
}

PlanComparison compare_plans(const Plan& left, const Plan& right) {
  PlanComparison comparison;
  comparison.same_input_digest = left.input_digest == right.input_digest;
  comparison.same_collective = left.collective == right.collective && left.kind == right.kind;
  comparison.identical = comparison.same_input_digest && left.body_digest() == right.body_digest();
  comparison.left_is_replan_of_right =
      comparison.same_collective && !comparison.same_input_digest &&
      left.bindings.plan.value() > right.bindings.plan.value();

  std::map<std::uint64_t, std::vector<const PathPlan*>> by_edge_left;
  std::map<std::uint64_t, std::vector<const PathPlan*>> by_edge_right;
  for (const PathPlan& path : left.paths) {
    by_edge_left[path.logical_edge.value()].push_back(&path);
  }
  for (const PathPlan& path : right.paths) {
    by_edge_right[path.logical_edge.value()].push_back(&path);
  }

  std::vector<std::uint64_t> edges;
  for (const auto& [edge, unused] : by_edge_left) {
    (void)unused;
    edges.push_back(edge);
  }
  for (const auto& [edge, unused] : by_edge_right) {
    (void)unused;
    edges.push_back(edge);
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  for (const std::uint64_t edge : edges) {
    const std::vector<const PathPlan*>& left_paths = by_edge_left[edge];
    const std::vector<const PathPlan*>& right_paths = by_edge_right[edge];
    const std::size_t shared = std::min(left_paths.size(), right_paths.size());
    bool changed = left_paths.size() != right_paths.size();
    for (std::size_t index = 0; index < shared; ++index) {
      if (!(left_paths[index]->hops == right_paths[index]->hops)) {
        changed = true;
        PathDifference difference;
        difference.logical_edge = LogicalEdgeId{edge};
        difference.left = left_paths[index]->id;
        difference.right = right_paths[index]->id;
        difference.left_present = true;
        difference.right_present = true;
        difference.identical = false;
        difference.summary = "path rewritten: " + hops_to_string(*left_paths[index]) + " became " +
                             hops_to_string(*right_paths[index]);
        comparison.differences.push_back(std::move(difference));
        ++comparison.rewritten_paths;
      }
    }
    if (left_paths.size() > right_paths.size()) {
      comparison.removed_paths += left_paths.size() - right_paths.size();
      for (std::size_t index = shared; index < left_paths.size(); ++index) {
        PathDifference difference;
        difference.logical_edge = LogicalEdgeId{edge};
        difference.left = left_paths[index]->id;
        difference.left_present = true;
        difference.summary = "path removed: " + hops_to_string(*left_paths[index]);
        comparison.differences.push_back(std::move(difference));
      }
    } else if (right_paths.size() > left_paths.size()) {
      comparison.added_paths += right_paths.size() - left_paths.size();
      for (std::size_t index = shared; index < right_paths.size(); ++index) {
        PathDifference difference;
        difference.logical_edge = LogicalEdgeId{edge};
        difference.right = right_paths[index]->id;
        difference.right_present = true;
        difference.summary = "path added: " + hops_to_string(*right_paths[index]);
        comparison.differences.push_back(std::move(difference));
      }
    }
    if (changed) {
      ++comparison.changed_logical_edges;
    }
  }

  comparison.summary = comparison.identical
                           ? "plans are byte-identical"
                           : ("plans differ: " + std::to_string(comparison.changed_logical_edges) +
                              " logical edge(s) changed, " + std::to_string(comparison.added_paths) +
                              " path(s) added, " + std::to_string(comparison.removed_paths) +
                              " path(s) removed, " + std::to_string(comparison.rewritten_paths) +
                              " path(s) rewritten");
  return comparison;
}

}  // namespace cpath
