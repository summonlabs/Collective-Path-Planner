// Collective Path Planner - strict canonical text DSL reader.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#ifndef CPATH_DSL_HPP
#define CPATH_DSL_HPP

#include <string>
#include <string_view>

#include "cpath/plan.hpp"
#include "cpath/request.hpp"
#include "cpath/status.hpp"

namespace cpath {

// The request DSL is a strict, line-oriented text format. It is the only text
// input the library accepts and it is treated as untrusted:
//
//   cpath-request 1
//   plan-generation 3
//   topology-generation 7
//   failure-domain-generation 2
//   capacity-evidence-generation 5
//   policy-generation 4
//
//   [collective]
//   id=c0
//   kind=ring
//
//   [groups]
//   g0 level=0 pattern=ring members=p0,p1,p2,p3
//
//   [domains]
//   FD-A kind=rack parent=FD-ROOT
//
//   [nodes]
//   n0 site=S1 rack=R1 host=H1 device=0 tier=TOR
//
//   [edges]
//   e0 from=n0 to=n1 capacity=25000 latency=120 tier=TOR domain=FD-A evidence=measured
//
//   [bindings]
//   p0 node=n0
//
//   [policy]
//   max-hops=8
//   paths-per-logical-edge=2
//   disjointness=edge
//
//   [forbidden]
//   nodes=n9
//   tiers=MGMT
//   domains=FD-Z
//
// Rules enforced by the reader:
//  - the first non-comment line must be the version banner;
//  - every line is at most kMaxTextLineBytes bytes and the input at most
//    kMaxInputBytes bytes;
//  - identifiers are at most kMaxIdentifierBytes bytes and match the
//    conservative grammar in ids.hpp;
//  - unknown sections, unknown keys, duplicate ids and unknown references are
//    rejected with deterministic codes;
//  - ordering inside a section is irrelevant; canonicalisation sorts;
//  - trailing content that is not a comment is rejected.
Result<RequestSpec> parse_request_spec(std::string_view text);
Result<PlanningRequest> parse_request_text(std::string_view text);

// Re-renders a parsed request in the same DSL. Round-trips.
std::string encode_request_text(const PlanningRequest& request);

// Plan rendering for audit and diffing.
std::string encode_plan_text(const Plan& plan);

}  // namespace cpath

#endif  // CPATH_DSL_HPP
