# Collective Path Planner - design notes

Copyright 2026 Summon Software Labs. Licensed under the Apache License 2.0.

These notes describe the design that is actually implemented in this repository.
They are not a roadmap.

## 1. Boundary

The planner owns exactly one decision: **given a supplied logical collective
structure and a supplied physical fabric, which deterministic set of physical
paths and stages should carry it, and why is that mapping valid under the exact
topology, failure-domain, capacity-evidence and policy generations in force?**

It produces *plans*. A plan is a candidate until an adjacent authority validates
and commits it.

The planner deliberately does **not** own:

| Adjacent system | Why the planner does not own it |
| --- | --- |
| Collective algorithm execution | The planner chooses paths; it never moves data. |
| Topology discovery | The fabric graph is supplied as input and is never inferred. |
| Route installation / switch programming | Installing a route is an effect on hardware. |
| Traffic admission and bandwidth brokerage | The plan *proposes* allocations; it never reserves them. |
| Congestion control | Congestion enters only as a cost term derived from evidence. |
| Workload scheduling | The collective structure is supplied, not chosen. |

Two consequences follow from that boundary and are enforced in code:

* A plan carries an `Allocation` table that is explicitly a *proposal*. Nothing
  in the library writes a reservation or claims that capacity now exists.
* The planner never reads a device, never opens a network connection to a
  fabric element, and never mutates its inputs.

## 2. Core model

### 2.1 Identities

All identities are strongly typed (`include/cpath/ids.hpp`). A default
constructed identity is an *invalid* sentinel - an empty string or a zero
generation - and no comparison in the library treats an invalid identity as a
match.

* Opaque string identities: `NodeId`, `EdgeId`, `ParticipantId`, `GroupId`,
  `CollectiveId`, `FailureDomainId`, `TierId`.
* Dense derived identities: `LogicalEdgeId` (1-based index into the canonical
  logical edge list), `PathId` (1-based index into the plan's path list).
* `CollectivePlanId` is the 32-byte canonical input digest.

Identifier grammar is deliberately conservative: one to 64 ASCII bytes from
`[A-Za-z0-9_.:-]`, and the first byte must be alphanumeric or an underscore. Two
consequences matter: an identity is always safe to embed in the text DSL and in
a log line without quoting, and canonical byte order equals human sort order.

### 2.2 Generations

Five generations are bound into every plan (`GenerationBindings`):
`TopologyGeneration`, `FailureDomainGeneration`,
`CapacityEvidenceGeneration`, `PolicyGeneration`,
`CollectivePlanGeneration`.

The first four are *inputs*: they take part in the canonical request digest, so a
topology change necessarily produces a different plan identity. The fifth is a
caller-supplied sequencing label for the act of planning; it is bound into the
plan and compared by `assess_freshness`, but it is deliberately **not** part of
the canonical input digest. This is a considered decision: a plan produced from
the same fabric, collective and policy is the same plan, and asking for it again
under a new plan generation must not fabricate a different mapping.

`assess_freshness(plan, current)` classifies a plan against a live set of
generations as one of `current`, `stale-topology`, `stale-failure-domains`,
`stale-capacity-evidence`, `stale-policy`, `stale-plan-generation` or
`unverified`. `describe_freshness` maps each classification to its own
`ErrorCode`, so a caller never has to parse prose to learn why authority lapsed.

### 2.3 Fabric

A fabric is a directed multigraph (`include/cpath/fabric.hpp`):

* `Node` carries a placement (`site`/`pod`/`rack`/`host`/device index, all
  optional) and a declared tier.
* `Edge` carries verified `capacity_mbps`, `latency_micros`, capacity already
  `reserved_mbps` by an adjacent authority, a tier, a failure domain, an
  `EvidenceClass` and a declared eligibility flag.
* `FailureDomain` forms a containment hierarchy ordered
  `device < host < rack < pod < site`.

Canonical form is fixed: failure domains ordered by id, nodes ordered by id,
edges ordered by `(from, to, id)`. Two distinct edges with the same endpoints are
both retained - duplicate physical links are a real topology property, not an
input error. Self-loop edges, duplicate identifiers, unknown references, a
parent that is not strictly broader than its child, a containment chain deeper
than eight levels, and `reserved > capacity` are all rejected at build time with
specific codes.

Zero capacity is never treated as capacity. An edge with `capacity_mbps == 0` is
unusable unless the policy sets `allow_unverified_capacity`, and even then it is
never usable when some authority has already claimed a reservation on it. When
such an edge does carry a path, the path's bottleneck is reported as zero and the
plan's `weakest_evidence` records the gap.

### 2.4 Collective structure

A collective is declared as one or more *groups*, each with a hierarchy level and
a pattern (`include/cpath/collective.hpp`). Flat collectives are simply a single
group at level 0. Hierarchical collectives declare several levels.

Logical edges are *derived* from the groups, never supplied directly:

| Pattern | Derived logical edges |
| --- | --- |
| `ring` | consecutive members and a closing edge, in declared order |
| `tree` | root to member (forward), member to root (reverse), or both |
| `pairwise` | consecutive pairs in canonical order, both directions |
| `alltoall` | every ordered pair of distinct members |

A ring's declared order is semantically meaningful, so it is canonicalised by
rotation, not by sorting: the smallest member in byte order is moved to the
front. Rotations of one cycle therefore collapse to a single canonical encoding,
while a reversal - which reverses every logical edge - does not.

A logical edge implied by more than one group is de-duplicated and attributed to
the earliest stage that implies it. `stage_count` is one more than the highest
declared level, and every logical edge carries the stage index of the group that
produced it.

### 2.5 Policy

The policy (`include/cpath/policy.hpp`) constrains what the planner is willing to
propose: hop limit, paths per logical edge, disjointness
(`none`/`edge`/`node`), failure-domain diversity (`none`/`preferred`/`required`)
at a chosen granularity, minimum evidence class, latency and congestion weights,
a path cost ceiling, forbidden nodes/tiers/failure domains, an allowed tier set,
and a search budget.

A policy grants no authority. It only narrows the proposal space.

## 3. Canonicalisation and determinism

`build_request` performs the only canonicalisation in the library and is the only
way to obtain a `PlanningRequest`:

1. the collective, fabric and policy are validated and canonicalised;
2. endpoint bindings are validated, their set-valued fields sorted and
   de-duplicated, and the binding list sorted by participant with duplicates
   rejected;
3. the canonical encoding is produced and hashed with SHA-256;
4. the resulting request is re-validated, including a digest re-check.

The canonical encoding is a tag-delimited, little-endian, length-prefixed byte
stream (`ByteWriter`). Ordering inside the encoding is always canonical order, so
logically identical inputs declared in a different order produce byte-identical
encodings and therefore identical digests.

Because the plan identity *is* that digest, determinism follows: identical
canonical inputs produce identical plan bytes. This is asserted directly by the
property suite.

## 4. Planning

`plan_collective` is the single entry point (`include/cpath/planner.hpp`). It never
returns a partial plan: on any denial the `PlanningOutcome` carries denials and no
plan at all.

### 4.1 Eligibility

Nodes and edges are first filtered by the policy: declared eligibility, forbidden
nodes and tiers, the allowed tier set, forbidden failure domains (including
ancestor forbid) and the evidence requirement. Per-participant binding
constraints then apply to *every* hop of a path carrying that participant: when a
binding declares allowed failure domains or tiers, each hop must stay inside
them.

### 4.2 Search

Each logical edge is treated in canonical order. The path search is a
hop-limited Dijkstra over `(node, hops)` states, so a cheap but over-long path
can never mask a compliant one. The frontier is ordered by `(cost, hops, node)`
and relaxation is strict, so ties resolve deterministically without reference to
container iteration order.

Cost is fixed-point and saturating:

```
latency_term    = latency_micros * latency_weight_milli / 1000
utilisation     = (reserved_mbps + allocated_mbps) * 1000 / capacity_mbps
congestion_term = congestion_weight_milli * utilisation / (1000 - utilisation)
edge_cost       = latency_term + congestion_term        (saturating at 2^62)
```

`allocated_mbps` is what this request's earlier logical edges have already been
planned onto that edge, so congestion scoring is order-dependent by design and
reproducible by construction. Because costs are non-negative, a label whose cost
exceeds `max_path_cost` can be pruned together with every extension of it.

### 4.3 Candidate generation and selection

For each logical edge the planner retains a bounded candidate pool (at most
`kMaxCandidatePaths = 16`):

1. the cheapest compliant path;
2. a greedy diversity chain: block what the previous path used (edges, plus
   intermediate nodes when node-disjointness is required) and search again;
3. alternatives obtained by blocking one resource of the best path at a time,
   and, when diversity is in play, by blocking one of its failure domains.

Selection then enumerates combinations of the pool of exactly the required size
and keeps the cheapest combination that satisfies *all* constraints -
disjointness, failure-domain independence and the demand fit. The pool is
bounded, so the combination search is bounded too.

### 4.4 Denials

Every refusal carries an `ErrorCode`, a `ConflictKind` naming the constraint
class, the logical edge and participant involved, and a bounded witness set.
Where the reason can be certified, it is:

* When a disjointness requirement cannot be met, the planner runs a unit-capacity
  max-flow (on a node-split graph for node-disjointness) and reports the exact
  upper bound the fabric admits. "The fabric admits at most 1 edge-disjoint path
  but 2 were required" is a certificate, not a guess.
* Failure-domain independence is judged on the domains a path adds **beyond the
  attachment domains of its two endpoints**. Sibling paths necessarily sit in the
  source and destination domains, and a failure there takes the endpoint down
  regardless of which route was chosen, so those domains carry no discriminating
  information. Everything a path touches in between does: two edge-disjoint
  routes that both transit the same pod are refused under required pod
  diversity, and they are refused because the transit domains really overlap,
  not because two names matched. An edge with no declared failure domain makes
  the path unprovable, and under a required diversity it is refused rather than
  assumed independent.
* When no path exists, the planner classifies *why* by counting how the outgoing
  edges of the source endpoint were excluded - ineligible, forbidden tier,
  forbidden domain, insufficient evidence, no verified capacity, endpoint
  constraint - and reports the dominant class.
* When a path exists but every path exceeds the hop limit, that is detected by a
  second search under the model's maximum hop bound and reported as
  `hop_limit_exceeded` rather than a vague "no path".

### 4.5 Self-check

A produced plan is passed through `validate_plan` before it is returned. If the
self-check fails, the plan is discarded and the outcome reports an internal
error. Independently, `validate_plan` is public so that a consumer - or the
persistence layer - can re-derive every promise from the raw inputs. It checks
that every hop references an existing edge with matching endpoints, that hops are
contiguous and anchored at the bound endpoints, that no forbidden node, tier or
domain appears, that evidence and capacity requirements hold, that hop and path
counts match the policy, that sibling paths really are disjoint and
failure-domain independent, that the allocation table follows from the paths, and
that the recorded statistics match the paths they summarise.

## 5. Plan identity and audit

`Plan::encode` is a versioned, length-prefixed canonical encoding;
`Plan::decode` is strict - wrong tag, unsupported version, out-of-range counts,
inconsistent statistics and trailing bytes are all rejected with specific codes.

A plan records the weakest evidence class any of its paths relied on, so a plan
built partly on synthetic or unknown evidence says so on its face.

## 6. Persistence

See `include/cpath/persistence.hpp` and the README for the on-disk contract. The
authority rules that matter here:

* opening a store advances both its epoch and its incarnation, so a restarted
  process cannot resurrect authority held by its predecessor;
* a record read back from disk is reported as unverified until the caller
  revalidates it against a live request;
* an acknowledgement for a commit is only produced after the atomic replace has
  happened.

## 7. Service

`Coordinator` owns a planning epoch and a process incarnation and reports both to
clients. `ServiceClient` speaks the same framed protocol over a real TCP
connection.

Each session owns a reader thread and a worker thread and a bounded inbox. Lock
order is fixed and documented in `src/server.cpp`: coordinator state, then
session state, then store state. No callback runs under a session lock, the store
is never touched under a session lock, and shutdown is driven by a per-session
loopback wake-up socket rather than by a timeout, so no protocol decision depends
on an interval.

Sequence numbers advance strictly within a session; a repeat or a regression is
refused as a replay. Identity comes from the session envelope the coordinator
assigned, never from a client-supplied field.

## 9. The algorithmic contract, in full

This section is the authoritative statement of what the planner solves and what
it can prove. It exists because "I did not find a mapping" and "there is no
mapping" are different results, and a planner that conflates them is unsafe to
build on.

### 9.1 The supported problem class

Given a directed multigraph (the physical fabric), a set of logical edges, an
endpoint binding per participant and a policy, choose for every logical edge a
set of `paths_per_logical_edge` **simple** paths between its bound endpoints such
that every hard constraint holds:

| Constraint | Meaning |
| --- | --- |
| hop limit | at most `policy.max_hops` hops per path |
| static eligibility | the edge is eligible, its tier is not forbidden, it is inside the allowed tier set when one is declared, its failure domain and none of its ancestors is forbidden, and its evidence meets the policy floor |
| endpoint binding | every hop satisfies the allowed failure domains and allowed tiers of **both** bindings of the logical edge; an allowed domain contains a hop domain when the hop domain is that domain or a descendant of it |
| demand fit | every hop of a path for logical edge *e* has at least *d(e)* = ceil(demand / k) of verified spare capacity, where spare is `capacity_mbps - reserved_mbps` |
| capacity coherence | for every physical edge the total demand the whole request places on it does not exceed its verified spare capacity |
| disjointness | `kEdge`: no two sibling paths share a physical edge; `kNode`: no two sibling paths share an interior node |
| domain diversity | when required, sibling failure-domain signatures are pairwise disjoint |
| cost ceiling | every path cost is at most `policy.max_path_cost` |

Sibling paths are **distinct**: a path set is a set, so the same physical route
cannot be chosen twice. Two identical siblings would not divide the demand across
any independent resource, so a request that asks for more sibling paths than the
fabric can distinguish is refused rather than satisfied with copies.

A path is a **simple path**: no physical node and no physical edge appears twice.
The hop-limited search orders its frontier by `(cost, hops, node)` and returns on
the first pop of the target, and a walk that repeats a node contains a cycle
whose removal keeps the cost the same or lower while strictly reducing the hop
count, contradicting minimality. The planner also checks simplicity explicitly
when it materialises a candidate, and `validate_plan` re-derives it from the
emitted hops.

Zero verified capacity is never capacity. An edge with `capacity_mbps == 0` is
usable only when the policy sets `allow_unverified_capacity`, and never when an
adjacent authority has already claimed a reservation on it. Such an edge carries
no capacity-checked footprint, so the plan reports a zero bottleneck for any path
that depends on it and records `kUnknown` in its weakest-evidence label.

### 9.2 The objective

The planner minimises a lexicographic objective. The same total order is used for
partial comparisons, so nothing depends on container iteration, hash order,
thread scheduling or the order in which candidates happened to be discovered.

1. satisfy every hard constraint;
2. minimise the total path cost - the sum, over all chosen paths of all logical
   edges, of the path cost, where a path cost is the saturating sum of its edge
   costs and an edge cost is `latency_micros * latency_weight_milli / 1000` plus
   `congestion_weight_milli * utilisation / (1000 - utilisation)`, with utilisation
   taken from the capacity **adjacent authorities have already committed**;
3. minimise the number of sibling pairs that are not failure-domain independent
   (only ever non-zero when diversity is `preferred`, because `required` is a
   hard constraint);
4. minimise the largest single path cost;
5. maximise the smallest remaining verified capacity headroom;
6. break remaining ties by the canonical path encoding - the concatenation, in
   canonical logical-edge order, of each chosen path's sequence of hop edge
   indices, compared lexicographically.

Because the cost of an edge depends only on evidence supplied by adjacent
authorities and never on this request's own commitments, candidate generation
for one logical edge is completely independent of every other logical edge.

### 9.3 Completeness boundary

Per logical edge the planner enumerates simple paths **by increasing hop count**
and records whether the enumeration ran to completion. It is complete for a
logical edge when:

* the enumeration completed, so the path set is every feasible simple path; and
* the k-subset enumeration over that set completed within its budget.

Across the request it is complete when every logical edge was complete and the
global capacity-coherent assignment search ran to exhaustion.

`PlanningOutcome::optimal` reports that. When it is true, the chosen mapping is
provably the minimum of the objective above. When it is false the mapping is
still valid, still deterministic, and still byte-identical for identical inputs -
it is simply not claimed to be optimal.

### 9.4 What INFEASIBLE means

`DenialKind` separates the outcomes:

| Kind | Meaning |
| --- | --- |
| `kInvalidRequest` | the request is malformed or self-contradictory |
| `kStaleInput` | the request's generations do not authorise a plan |
| `kProvenInfeasible` | **no mapping exists**, and that has been proven |
| `kSearchLimitReached` | the search stopped at a declared bound: INDETERMINATE |
| `kUnsupportedConstraint` | a mode this planner does not implement |

Infeasibility is only ever claimed from an argument that holds for **all**
constraints at once, never from the failure of a bounded search:

* the destination is unreachable under the policy - proven by breadth-first
  reachability over exactly the same edge filters the search uses;
* the reachable distance exceeds `max_hops` - proven by the same breadth-first
  search, which computes the minimum hop count exactly;
* a disjointness requirement exceeds what the fabric admits - proven by a
  unit-capacity max-flow (on a node-split graph for node-disjointness) that
  accounts for the policy's filters, the endpoint bindings and the demand fit;
* every route out of the source is removed by one identifiable filter - reported
  as the narrowest filter that did the work, not as a union of causes;
* an exhaustive path set admits no k-subset satisfying disjointness, required
  domain diversity and the per-set capacity fit;
* an exhaustive assignment search finds no globally capacity-coherent choice.

Every other failure to find a mapping is reported as `kSearchLimitReached` with
`ErrorCode::kSearchBudgetExceeded`. It names the constraint class that blocked
progress so an operator still knows what to relax, but it does not claim that no
mapping exists.

A denial never carries a plan. `PlanningOutcome::proven_infeasible()` is true only
when every denial in the outcome is conclusive, and
`PlanningOutcome::indeterminate()` is true as soon as any denial is a search
limit.

### 9.5 Capacity decisions are global, not greedy

Earlier revisions committed per-path demand as they walked the logical edges in
canonical order, so adding an unrelated physical edge could change which route an
early logical edge picked and thereby withdraw a later one. The planner now:

1. generates each logical edge's candidate sets against the fabric's spare
   capacity only, never against this request's own prior commitments, so
   candidate generation is independent of logical-edge ordering;
2. takes each logical edge's individually optimal set and checks the union; if it
   fits, that assignment is the global optimum and is returned immediately;
3. otherwise searches the product of the per-logical-edge alternatives, best
   first, pruning every branch that would exceed any edge's spare capacity, and
   returns the best assignment found.

Consequently, adding usable unconstrained fabric resources cannot turn a feasible
request into an infeasibility claim: the richer fabric either yields a plan, or
the search that could not decide says so. A richer fabric may still yield a
*different* valid plan, and a bounded global search may return
`kSearchLimitReached` rather than the optimum; neither is a false denial.

### 9.6 What validate_plan proves independently

`validate_plan` does not replay planner assumptions. From the request alone it
re-derives: the plan's identity and its bound generations; that every logical
edge of the collective is present with exactly the required number of paths; that
each path is simple, contiguous, starts and ends at the bound endpoints, and
stays inside the hop and cost ceilings; that every hop names an existing physical
edge with matching direction; that no forbidden node, tier or failure domain
appears; that the evidence floor, the allowed tier set and both endpoint bindings
hold on every hop; that sibling paths satisfy the disjointness mode and, when
required, are failure-domain independent; that the failure-domain signature of
each path is exactly what its hops imply; that the allocation table follows from
the paths, names each physical edge at most once, and never exceeds verified
spare capacity; and that every recorded statistic matches the paths it
summarises.

## 8. Explicit non-claims

* No path produced by this library has been validated on physical hardware. The
  test suites use synthetic fabrics and label them SYNTHETIC.
* A plan does not reserve bandwidth and does not install a route.
* A persisted plan is a record of a past decision, not a statement about the
  present.