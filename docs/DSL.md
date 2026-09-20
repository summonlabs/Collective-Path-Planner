# The request DSL

Copyright 2026 Summon Software Labs. Licensed under the Apache License 2.0.

The request DSL is the only text input the library accepts. It is line oriented,
strict, and treated as untrusted input: unknown sections, unknown keys, duplicate
identifiers, out-of-range numbers, over-long lines and trailing content are all
refused with a specific `ErrorCode` and a line number.

Identifiers are one to 64 ASCII bytes from `[A-Za-z0-9_.:-]`, and the first byte
must be alphanumeric or an underscore.

## Skeleton

```
cpath-request 1
plan-generation 2
topology-generation 7
failure-domain-generation 3
capacity-evidence-generation 11
policy-generation 5

[collective]
id=c0 kind=ring demand=1000

[groups]
g0 level=0 pattern=ring members=p0,p1,p2,p3

[domains]
S0 kind=site
R0 kind=rack parent=S0

[nodes]
n0 site=S0 rack=R0 host=H0 device=0 tier=LEAF

[edges]
e0 from=n0 to=n1 capacity=25000 latency=100 tier=LEAF domain=R0 evidence=synthetic

[bindings]
p0 node=n0

[policy]
max-hops=8
paths-per-logical-edge=2
disjointness=edge

[forbidden]
nodes=n9
tiers=MGMT
domains=R9
```

## Rules

* The first non-comment, non-empty line must be the banner `cpath-request 1`.
* Header lines are `KEY VALUE` (a single space). Sections are `[name]`.
  Everything inside a section is `key=value`, space separated.
* Blank lines are ignored. A line whose first non-space character is `#` is a
  comment.
* A line may not exceed 4096 bytes; the whole input may not exceed 32 MiB.
* Numbers are unsigned decimal without a sign and without leading zeros.
* Booleans are `0` or `1`.
* A repeated key on one line is an error, not a last-wins.
* A repeated section is an error.
* Ordering inside a section never matters: the builder canonicalises. Only the
  ring member list is order meaningful, and it is canonicalised by rotation.
* Everything after the last recognised line must be blank or a comment.

## Sections

### Header

| Key | Meaning |
| --- | --- |
| `plan-generation` | sequencing label for the act of planning |
| `topology-generation` | physical topology generation |
| `failure-domain-generation` | failure-domain hierarchy generation |
| `capacity-evidence-generation` | capacity evidence generation |
| `policy-generation` | policy generation |

### `[collective]`

One line: `ID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `kind` | no | `ring`, `tree`, `pairwise`, `alltoall`, `hierarchical` (default `ring`) |
| `demand` | no | declared nominal bandwidth per logical edge in Mbit/s; `0` (default) means "not declared" |

### `[groups]`

One line per group: `GROUPID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `level` | yes | hierarchy level, 0 to 7; it becomes the stage index |
| `pattern` | yes | `ring`, `tree`, `pairwise` or `alltoall` |
| `root` | for `tree` | participant that roots the tree |
| `direction` | no | `forward` (default), `reverse` or `bidirectional` |
| `members` | yes | comma separated participant ids |

### `[domains]`

One line per failure domain: `DOMAINID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `kind` | no | `device` (default), `host`, `rack`, `pod`, `site` |
| `parent` | no | containing domain; must be strictly broader |

### `[nodes]`

One line per node: `NODEID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `site`, `pod`, `rack`, `host` | no | placement labels |
| `device` | no | device index within the host (default 0) |
| `tier` | no | declared tier name, at most 32 bytes |
| `eligible` | no | `0` or `1` (default `1`) |

### `[edges]`

One line per **directed** edge: `EDGEID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `from`, `to` | yes | endpoint node ids; a self loop is rejected |
| `capacity` | no | verified capacity in Mbit/s (default 0 = no verified capacity) |
| `latency` | no | one-way latency in microseconds (default 0) |
| `reserved` | no | capacity already committed by an adjacent authority |
| `tier` | no | tier name |
| `domain` | no | failure domain of the edge |
| `evidence` | no | `unknown` (default), `synthetic` or `measured` |
| `eligible` | no | `0` or `1` |

A bidirectional link is declared as two lines.

### `[bindings]`

One line per participant: `PARTICIPANTID key=value ...`

| Key | Required | Meaning |
| --- | --- | --- |
| `node` | yes | the endpoint node the participant is attached to |
| `domains` | no | allowed failure domains; when present every hop must stay inside one of them |
| `tiers` | no | allowed tiers; when present every hop must use one of them |

A participant with no binding is a planning denial, not an implicit default.

### `[policy]`

| Key | Default | Meaning |
| --- | --- | --- |
| `max-hops` | 8 | maximum physical hops per path (1 to 256) |
| `paths-per-logical-edge` | 1 | paths proposed per logical edge (1 to 8) |
| `disjointness` | `none` | `none`, `edge` or `node` |
| `domain-diversity` | `none` | `none`, `preferred` or `required`; independence is judged on the domains a path adds beyond its two endpoint attachment domains |
| `domain-diversity-level` | `rack` | `device`, `host`, `rack`, `pod` or `site` |
| `evidence` | `any` | `any`, `synthetic-or-better` or `measured` |
| `latency-weight-milli` | 1000 | latency weight in thousandths |
| `congestion-weight-milli` | 0 | congestion weight in thousandths |
| `max-path-cost` | 2^62 | path cost ceiling |
| `allow-unverified-capacity` | 0 | whether a zero-capacity edge may carry a path |
| `max-search-expansions` | 262144 | search budget for the whole request |
| `allowed-tiers` | empty | comma separated tiers; when present every hop must use one of them |

### `[forbidden]`

At most one line per key.

| Key | Meaning |
| --- | --- |
| `nodes` | comma separated forbidden node ids |
| `tiers` | comma separated forbidden tier names |
| `domains` | comma separated forbidden failure domains; a forbidden domain also forbids every domain contained in it |

## Canonical form

`encode_request_text` re-renders a parsed request in this same DSL, in canonical
order, so that `parse_request_text(encode_request_text(x))` yields the same
canonical digest. The CLI exposes it as `cpath canonical --request FILE`.
