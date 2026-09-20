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

## 8. Explicit non-claims

* No path produced by this library has been validated on physical hardware. The
  test suites use synthetic fabrics and label them SYNTHETIC.
* A plan does not reserve bandwidth and does not install a route.
* A persisted plan is a record of a past decision, not a statement about the
  present.
