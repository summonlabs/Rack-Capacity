# Rack Capacity

Rack Capacity is the per-rack capacity accounting runtime.

It answers one question precisely: **how much capacity does this exact rack
generation have left across physical slots, power, cooling, weight and
serviceability, and which of those constraints becomes binding first?**

It answers it from evidence it does not own, with exact integer arithmetic, and
it reports what it does not know instead of inventing a number.

```cpp
RackCapacityInputs inputs = /* evidence from rack composition, assets, power,
                               cooling, weight, reservations and policy */;

Result<RackCapacitySnapshot> snapshot =
    evaluate_capacity(inputs, CapacityGeneration::trusted(1),
                      SnapshotRevision::trusted(1), now, EvidenceFreshness::Fresh);

snapshot.value().slots.free.lower();       // 74 slots, guaranteed free
snapshot.value().power.free.upper();       // 2980 W at most, and only if
                                          // nothing unknown is drawing power
snapshot.value().primary_binding;          // Dimension::Power
```

## 1. Systems boundary

### What it owns

* Per-rack capacity accounting: the remaining and headroom capacity of one
  exact rack generation, derived from evidence.
* Exact mount-slot occupancy accounting over the published rack coordinate
  space, including structural reservations, shared mounts and fragmentation.
* The electrical, thermal, static-load and serviceability envelopes of a rack,
  with physical derating, policy derating and policy headroom kept separate.
* Deterministic capacity snapshots with canonical digests, exact closure
  identities, stable explanations, diffs and revalidation.
* Fit evaluation: whether a candidate would be compatible with the remaining
  capacity of a rack, as a three-valued verdict, with no placement authority.
* Durable per-rack capacity state with transactional publication, recovery and
  writer authority.

### What it explicitly does not own

Rack Capacity does **not** become any of these, and does not duplicate their
authority:

| Adjacent runtime | What Rack Capacity does instead |
| --- | --- |
| Rack Registry | Consumes rack identity, height, structural reservations and serviceability limits as versioned evidence. It never defines what a rack is. |
| Physical Location Registry | Consumes site, hall, row and position as opaque references. It never resolves a location. |
| Asset Registry / Facility Placement Planner | Consumes installed occupants, mounts and nameplate data as evidence. It never creates, moves or removes an occupant, and never publishes a placement. |
| Power Capacity / Cooling Capacity | Consumes feed topology, nameplate envelopes and metered observations. It never actuates, never allocates a feed and never decides whether a rack may be energised. |
| Facility Capacity Reservation | Consumes committed reservations as evidence and subtracts them. It never creates, extends or releases a reservation. |
| Facility Capacity / Space Capacity | Accounts for one rack. It never aggregates racks into a hall, a room or a site. |
| Policy authority | Consumes a resolved policy and its reference. It never authors, merges or resolves policy. |

The two statements that matter most:

* **No placement authority.** A fit evaluation never chooses a position. When
  the caller names no position, the evaluation reports how many positions could
  host the candidate and stops there.
* **No reservation authority.** The library has no operation that reserves
  future capacity. Committed reservations enter as evidence and leave as
  subtracted load.

## 2. Architecture

```
include/rack_capacity/
  errors.hpp        stable error taxonomy and process exit codes
  result.hpp        explicit Result<T> / Status
  measures.hpp      exact integer measures (watts, grams, millimetres, basis
                    points, instants) and checked arithmetic
  bound.hpp         Bound<T>: unknown, exact, or a proven interval
  ids.hpp           identities, generations, epochs, revisions, attempts
  coordinates.hpp   mount slot intervals, normalised interval sets, the
                    published Rack Registry coordinate space
  evidence.hpp      typed evidence consumed from adjacent runtimes
  capacity.hpp      per-dimension accounting, snapshots, closure, pressure
  fit.hpp           three-valued fit evaluation
  diff.hpp          snapshot diffs and revalidation reports
  catalog.hpp       per-rack capacity authority (the product)
  persistence.hpp   durable store, publication protocol, writer fencing
  text.hpp          human-authorable specification format
src/                implementation, including the codec and platform layer
tools/rackcap.cpp   inspection and administration CLI
tools/rc_child.cpp  process-level proof harness (not installed)
```

The library is the product. The CLI is a thin shell over it: every decision it
makes is a library call and every failure it reports is a typed library error
rendered verbatim.

**Zero third-party dependencies.** C++20 standard library only. SHA-256,
CRC-32C, the canonical codec, the durable store, the specification parser and
the file locking layer are all implemented here, and all are covered by tests
against published vectors or against independent models.

## 3. Authority model

### Identities, generations and epochs

Every identity is a distinct C++ type and no two identity families convert into
one another. `RackId`, `AssetId`, `ReservationId`, `EvidenceId`, `PolicyId`,
`WriterId`, `ActorId` and `RequestId` are all validated at the construction
boundary: printable ASCII, no path separators, no shell metacharacters, no
colon, no traversal sequence, bounded length. Rejection is always preferred to
repair, because a silently rewritten identity is a different identity.

| Concept | Meaning |
| --- | --- |
| `RackCompositionGeneration` | The rack composition generation the evidence describes. Supplied by composition authority; never invented here. |
| `CapacityGeneration` | The generation of the capacity state this library authored for one rack. Monotonic across composition generations and never reset. |
| `SnapshotRevision` | Revision of one evaluated snapshot. |
| `EvidenceEpoch` | Epoch of the evidence bundle a snapshot was evaluated from. |
| `StoreEpoch` | Epoch of writer authority over a durable store. Advanced on every acquisition. |
| `StoreSequence` | Publication sequence. Advances by exactly one per publication. |
| `AttemptId` | Identity of one durable publication attempt. |
| `RequestId` | Identity of one mutation, used for bounded idempotent replay. |

### Preconditions are mandatory

Every operation that depends on current state carries a `CapacityExpectation` -
rack, composition generation, capacity generation and snapshot revision - and a
mismatch is refused with the exact expected and current values, never merged:

```
$ rackcap rack fit --path store --rack rack-a1 --units 1
stale_capacity_generation(301): the expectation was planned against a different
capacity generation [operation=catalog.precondition, subject=capacity_generation,
related=rack-a1, expected=3, actual=1]
```

### Idempotent retries

Replay is bounded and explicit. The catalog keeps a journal of the last
`kMaxIdempotencyRecords` (256) receipts inside the published state, so replay
coverage survives a restart, and the number of evicted receipts is itself
recorded. A replay requires the same request identity *and* the same evidence
facts:

* same identity, same facts: answered from the journal as `Replayed`, nothing is
  republished;
* same identity, different facts: `RequestIdConflict`;
* same facts, new identity, same evidence epoch: `NoChange`, nothing published.

### Evidence provenance

Every evidence record carries an `EvidenceReference`: source runtime, evidence
identity, version, the instant it was observed, and the producer. A snapshot
records the digest of the whole bundle and the reference of the policy it was
accounted under, so any snapshot can state exactly what it was derived from.

## 4. Capacity model

### Mount slot coordinates

Vertical position is expressed in mount slots: one rack unit is two slots, slot
`2U-1` is the lower half of unit `U` and slot `2U` is its upper half. This is
deliberately the coordinate space Rack Registry publishes, so a composition fact
carries across without reinterpretation. Every interval is half open, and every
interval set is normalised - sorted, disjoint, adjacent members merged - so a
given set of slots has exactly one representation, one text form and one digest.

### The five dimensions

| Dimension | Envelope | Consumed by |
| --- | --- | --- |
| Slot | rack height minus structurally reserved slots | present occupants, committed reservations, indeterminate occupancy |
| Power | feeds x contributing feeds, physical derate, policy derate, policy headroom | nameplate draw of present occupants, committed reservations |
| Cooling | nominal heat rejection, physical derate, policy derate, policy headroom | declared heat, or heat derived from draw by an explicit policy equivalence |
| Weight | static load limit, derates, headroom; optional per-unit point load limit | occupant mass, distributed across the units the occupant spans |
| Serviceability | front and rear clearance, highest serviceable unit | reported as constraints, never as capacity |

Redundancy is accounted, not assumed: `None` counts every feed, `N+1` counts all
but one, `2N` counts one. Physical derating, policy derating and policy headroom
are applied in that order and reported separately, so a reader can always tell
how much of the gap between nominal and usable is physical and how much is
policy.

### Unknown never becomes free capacity

This is the invariant the whole library is built around.

* A missing power, cooling or weight envelope reports `free` as **unknown**, not
  as zero and not as unlimited.
* An occupant with an unknown draw, an unknown heat rejection or an unknown mass
  forces the conservative free figure to **zero** and leaves the optimistic upper
  bound in place; the snapshot reports both, and names the count of unknown
  occupants.
* An occupant whose *presence* cannot be established is not treated as absent:
  its slots are excluded from the conservative free set, and its consumption is
  unknown regardless of whether a nameplate is recorded.
* Heat is never inferred from power unless the policy states an equivalence
  factor, and every derived quantity is reported separately from declared ones.
* Metered values are observations. A measurement never increases authoritative
  free capacity; a stale or undated measurement is reported as stale.

An undecided question is answered as undecided. `FitVerdict` has three values,
and `Indeterminate` is a first-class answer:

| Verdict | Meaning |
| --- | --- |
| `Fits` | every constraint is satisfied under every reading of the evidence |
| `DoesNotFit` | at least one constraint is provably violated |
| `Indeterminate` | nothing is provably violated and something cannot be decided |

### Exact closure

Every evaluated snapshot must satisfy the accounting identities exactly, and the
check runs inside evaluation, inside decoding and inside inspection:

* the slot partition - structural, occupied, committed reservations,
  indeterminate, policy headroom, free - covers the rack extent exactly once,
  and the parts sum to the total plus the slots claimed twice;
* committed + reserved + free equals usable plus overcommit, for each measured
  dimension;
* an unknown contributor forces the conservative free figure to zero;
* the free bound equals the cardinality of the free sets.

A snapshot that does not close is refused rather than published.

### "Which constraint becomes binding first"

Each dimension reports a `ConstraintPressure`: the share of its usable envelope
already claimed by occupied and committed load, in basis points. The table is
ordered by binding first, then by utilisation, then by canonical dimension
order, and `primary_binding` names the dimension that becomes binding first
under additional load. Unknown envelopes are reported as unknown rather than
ordered as if they had a ratio.

## 5. Lifecycle, recovery standing and revalidation

A rack record is `Active`, `Retired` or `Quarantined`. Retirement is terminal:
capacity is never quoted for a retired rack again and no evidence is accepted
for it. Quarantine is entered only when a recovered record fails to reproduce
from its own evidence, and is left by applying fresh evidence.

Separately, a record is `Authoritative` or `PendingRevalidation`. **Every record
read from durable state is `PendingRevalidation`**, and quoting its capacity is
refused until it has been revalidated in the process that is asking. Persisted
evidence therefore never silently becomes current again after a restart.

Revalidation recomputes the snapshot from the evidence the record carries and
compares the result with what was recovered:

| Verdict | Meaning |
| --- | --- |
| `Current` | reproduces exactly, and the evidence is inside the freshness policy |
| `Stale` | reproduces exactly, but the evidence is older than the policy allows |
| `Diverged` | does not reproduce, or the stored digest does not match its content |

For inspection that must not write, `CapacityCatalog::inspect_snapshot`
recomputes and verifies a record without publishing anything - and it is at
least as strict as the authoritative path, because a record that does not
reproduce is refused rather than displayed.

## 6. Persistence, publication and recovery

The state file is a fixed-width, little-endian, length-prefixed frame with a
magic, a format version, a byte order marker, the coordinate model, the store
epoch and sequence, the rack and occupant counts, a 32-byte store incarnation, a
CRC-32C over the payload and a CRC-32C over the header. The payload is a
canonical encoding of the whole catalog state, including each record's evidence
bundle, its snapshot, and the replay journal.

Publication is one sequence:

```
plan -> validate -> stage (create, write, flush) -> read back and verify
     -> publish the superseded generation as the retained previous
     -> rename the staged generation over the current one   <- commit point
     -> sync the directory -> retire residue
```

A crash at any point leaves either the previous generation or the new one
authoritative, never a mixture and never a file that decodes into a
half-applied change. This is proved by injecting a fault at each of the eight
named stages, and separately by killing a real child process at each of them
(section 11).

Recovery rules:

* the current file is verified; if it fails, the retained previous generation is
  used and the rejection is reported;
* if neither verifies, no state is accepted at all and the store reports why;
* temporary files left by an interrupted publication are retired, but only
  regular files whose name matches this store's staging pattern - a directory or
  a link with a matching name is left alone;
* a store whose recorded incarnation does not match the caller's expectation is
  refused as a swapped store;
* a state path that is a link or a reparse point is refused rather than followed.

Every decoded state is re-validated in full: bundle well-formedness, occupant
conflicts, snapshot digest, closure, generation agreement and record ordering. A
decoded count is checked against its documented bound before any container is
sized for it.

## 7. Concurrency and process authority

**Lock order, fixed before implementation and audited after:**

1. the catalog mutex - taken first, released last, one per catalog;
2. the capacity store writer mutex - the only lock ever taken while the catalog
   mutex is held.

The order is never reversed. The catalog calls only the store and pure functions
while its mutex is held. No callback, observer or hook is invoked while the
catalog mutex is held; the only hook in the library belongs to the store's
publication path and is documented there. Queries copy what they return, so no
reference into catalog state escapes. A mutation builds a candidate state,
publishes it durably, and only then makes it authoritative in memory.

The audit that accompanied this model checked, by reading every lock-taking
path: read-to-write acquisition on the same lock without dropping the read
guard; a write lock held while calling code that takes the same lock; mutex
re-entry through callbacks; event emission beneath internal locks; shutdown or
join while holding locks workers need; reversed nested ordering; cancellation
paths with reversed ordering; progress callbacks re-entering mutable state; and
shutdown waiting on work while preventing its completion.

**Process authority** is OS-level, not advisory. A writable store holds an
exclusive operating-system byte-range lock on `<path>.lock` for its lifetime.
The lock covers a byte far beyond the metadata the file carries, because an OS
byte-range lock also blocks reads of the locked bytes by other handles - locking
a distant byte keeps "who holds this store" readable by another process and by
the lock owner itself. Because the lock is held by the process, the operating
system releases it when the process dies for any reason, including an abrupt
crash. A live writer cannot be fenced by an operator: `force_release` refuses
while the OS lock is held, and succeeds once the holder is gone, advancing the
store epoch so any publication planned against the previous epoch is refused.

## 8. Errors

Errors are a stable, numeric, documented contract, grouped by fault domain.
Every rejection carries a machine-readable message plus structured context:
operation, subject, related identity, expected and current values, and an
ordered list of the specific items involved.

| Range | Domain |
| --- | --- |
| 1xx | input rejected at a construction or validation boundary |
| 2xx | identity uniqueness, existence, evidence well-formedness |
| 3xx | stale authority: generations, revisions, epochs, writer fencing |
| 4xx | lifecycle gates |
| 5xx | slot coordinates, intervals, structural reservation |
| 6xx | evidence, policy and derivation |
| 7xx | persistence, encoding and integrity |
| 8xx | bounds, arithmetic and capability |
| 9xx | writer ownership, OS-level locking and fencing |
| 10xx | internal invariants |

**Validation precedence is fixed and tested.** For a mutation the order is:
the requested instant, then specification or argument parsing, then canonical
ordering, then bundle validation (identity, bounds, conflicts, shared classes,
reference well-formedness, policy), then existence, then the replay journal,
then the precondition, then lifecycle, then staleness of composition generation
and evidence epoch, then evaluation, then closure, then publication. A test
asserts that a malformed bundle is reported as malformed even when the rack it
names does not exist.

The CLI exits with a documented code per fault class (0 ok, 2 usage, 3 not
found, 5 stale, 6 lifecycle, 7 persistence, 10 io, 11 writer lock, 12 evidence,
13 invariant) so a script never has to parse text.

## 9. Building, installing and consuming

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `RC_BUILD_TESTS`, `RC_BUILD_TOOLS`, `RC_BUILD_EXAMPLES`,
`RC_BUILD_BENCHMARKS`, `RC_WARNINGS_AS_ERRORS` (default `ON`), `RC_SANITIZERS`.

```
cmake --install build --prefix /some/prefix
```

installs the headers, the static library, the `rackcap` tool, the documentation
and a versioned CMake package:

```cmake
find_package(RackCapacity 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE RackCapacity::rack_capacity)
```

The package exports `RackCapacity_VERSION`,
`RackCapacity_STATE_FORMAT_VERSION`, `RackCapacity_SNAPSHOT_LAYOUT_VERSION` and
`RackCapacity_MOUNT_SLOTS_PER_RACK_UNIT`. `tests/downstream` is an independent
consumer that is configured against an install prefix, finds the package, builds
from sources outside this tree and runs a real lifecycle (section 11).

Three runnable examples are built with the project:

| Example | Shows |
| --- | --- |
| `example_capacity_basics` | building one rack's evidence, reading its capacity, and the difference between a conservative and an optimistic free figure |
| `example_fit_evaluation` | the three-valued verdict, including why an unknown occupant makes a small request undecided rather than accepted |
| `example_durable_recovery` | durable publication, a real reopen, the refusal to quote recovered capacity, inspection without promotion, explicit revalidation and a diff between two published generations |

There is no GUI and no telemetry: the library opens no network socket, performs
no name resolution and contacts no remote service.

## 10. Inspection tool

```
rackcap version
rackcap store init          --path P
rackcap store inspect       --path P          # header, incarnation, counts
rackcap store verify        --path P          # decodes, checksums and validates
rackcap store lock-status   --path P
rackcap store force-release --path P [--operator ID]
rackcap rack list           --path P
rackcap rack show           --path P --rack R
rackcap rack apply          --path P --spec FILE
rackcap rack fit            --path P --rack R (--units N | --span "[a,b)") [constraints]
rackcap rack retire         --path P --rack R [--reason TEXT]
rackcap rack revalidate     --path P --rack R
rackcap rack diff           --path P --rack R
rackcap rejections          --path P
```

`store verify` and `store inspect` are at least as strict as opening the store:
they verify the header, both integrity checks, the canonical encoding, every
record and every closure identity, and they never modify anything.

Evidence bundles are authored in a line-oriented specification format that is
strict about everything - unknown keys, duplicate keys, duplicate sections,
malformed values and out-of-range numbers are all rejected with a line number -
and that round-trips exactly:

```
rcap-spec 1
[composition]
rack = rack-a1
generation = 4
units = 48
structural-reserved = [95,97)
...
[policy]
policy = standard-2026
policy-version = 3
power-headroom-w = 500
...
[asset a-0001]
span = [1,5)
kind = full
presence = present
nameplate-draw-w = 1200
...
```

## 11. Validation performed

Everything below was run on Windows with MSVC 19.44, CMake 4.3 and Ninja, in
this repository, on the commit this file ships with.

### Builds and warnings

| Configuration | Result |
| --- | --- |
| Debug (`/Od /RTC1`) | clean build, zero first-party warnings under `/W4 /WX /permissive-` |
| Release (`/O2`) | clean build, zero first-party warnings under `/W4 /WX /permissive-` |
| RelWithDebInfo + `/fsanitize=address` | clean build, zero first-party warnings |

### Suites

Twelve suites, 100+ named cases, run to completion with no timeout, no watchdog
and no process kill anywhere in the harness. All twelve pass in Debug, in
Release and under AddressSanitizer.

| Suite | What it proves |
| --- | --- |
| `test_coordinates` | interval construction and normalisation, set algebra, anchor counting, and a property test of the interval set against an independent per-slot bitmap model over 400 seeded cases |
| `test_measures` | range-checked construction, checked arithmetic at the boundaries, exact derating and flooring, heat derivation, SHA-256 against published vectors, CRC-32C against its published check value, canonical encode/decode round trip, digest sensitivity, closure identities |
| `test_evidence` | every validation rule, specification round trip with byte-identical re-rendering, and rejection of malformed documents, unknown keys, duplicate keys and out-of-range values |
| `test_capacity` | occupied vs reserved vs submitted, absent occupants, policy slot headroom, the full derating chain, redundancy, unknown envelopes, unknown consumption, metering, point-load distribution, serviceability, overcommit, fragmentation, pressure ordering, determinism |
| `test_fit` | the three-valued verdict per dimension, anchor counting without choosing, point load without guessing a distribution, serviceability, and the boundary where a request stops being provably satisfiable |
| `test_catalog` | registration, monotonic generations, refusal of epoch and composition regression, exact replay, retirement, retained-generation diffs, read-only stores, validation precedence, statistics |
| `test_persistence` | real close and reopen, recovery from a damaged current generation, truncation and oversize refusal, swapped-store detection, link refusal, residue retirement, strict creation and inspection, epoch and sequence fencing, whole-state-only publication, and a fault injected at every one of the eight durable stages |
| `test_property` | 900 seeded cases comparing slot accounting with an independent bitmap model, envelope accounting with direct arithmetic, monotonicity of free capacity under added load, fit verdicts against the free sets they report, and randomised catalog mutation sequences |
| `test_adversarial` | 400 single-byte corruptions of a real state file, structural hostility (wrong magic, swapped byte order, foreign versions, trailing bytes, truncation, enormous declared sizes), declared-count attacks, identity and path edges, hostile specifications and measure boundaries |
| `test_concurrency` | four readers against a live writer with every observation closure-checked, exactly one winner among four concurrent registrations, fit verdicts consistent with their preconditions under concurrent mutation |
| `test_multiprocess` | real operating-system processes: exclusive writer authority across processes, lock release on both clean exit and abrupt death, a child's publication read by a parent, a real crash at each of the eight durable stages followed by a whole-generation recovery, operator release refused while a live holder exists, and two writers serialising on the store |
| `test_cli` | the tool as a real process: version, init, inspect, verify, lock status, force release, apply, list, show, fit (fits, does not fit, not found, usage), retire, rejections, and typed exit codes |

### Multiprocess and crash recovery

Proved with independent processes, not threads:

* a second process is refused `WriterLockHeld` while the first holds authority,
  and the lock record names the live holder's identity and process;
* killing the holder as the operating system would on a crash releases the lock
  and the store opens immediately, with nothing to repair;
* a child process that dies abruptly at each of the eight durable stages leaves
  exactly one whole generation - the previous one or a state that decodes,
  validates and closes - with no staging residue, and the store accepts a new
  publication afterwards;
* an operator release of writer authority is refused with `WriterLockHeld`
  while a live holder exists, and succeeds afterwards, advancing the epoch.

### Sanitizers

The full suite runs clean under MSVC AddressSanitizer (`/fsanitize=address`,
RelWithDebInfo, all twelve suites passing, no diagnostic emitted). No
UndefinedBehaviorSanitizer equivalent was exercised on this toolchain, and no
claim is made about one.

### Install, export and downstream

The Release build installs headers, library, tool, documentation and a versioned
CMake package. `tests/downstream` is configured out of tree against the install
prefix, finds the package with `find_package(RackCapacity 1.0 REQUIRED)`, builds
with `/W4 /WX` and runs a real lifecycle: create a store, register a rack, quote
capacity and check the exact free slot count, evaluate a fit, publish a change,
release the store, reopen it in a new object graph, observe that a recovered
record is refused until revalidated, inspect it without promoting it, revalidate
it explicitly and quote it again.

### What is not claimed

* No physical hardware was involved. No PDU, UPS, generator, cooling unit, GPU,
  switch or rack was instrumented, read or controlled. Every envelope in every
  test and benchmark is synthetic.
* Only Windows/MSVC was exercised. The POSIX paths in the platform layer are
  written and compiled by inspection only; they were not built or run here, and
  no claim is made about them.
* No test asserts anything about timing, and no timeout, watchdog or
  forced-termination-as-success mechanism exists anywhere in the harness. A test
  that hangs is a defect to diagnose.

## 12. Benchmarks

`bench_rack_capacity` measures **completed** operations: an evaluation that
returned a verified snapshot, a fit that returned a verdict, a publication that
was flushed, verified and published. Nothing is timed from submission. Durable
measurements include the flush, the read-back verification and the atomic
publish, because that is what a durable publication costs; they are reported
separately from in-memory measurements and are never mixed with them. After the
timed regions, the benchmark re-runs the operation and verifies the state it
created rather than trusting the timing, and it removes every store it made.

The workload is **SYNTHETIC**: racks, occupants and envelopes are generated in
the process, with a fixed published seed, and describe no real hardware. Figures
are the median of completed operations after warm-up, measured on the machine
that produced this release, in Release (`/O2`), with 300 in-memory runs and 50
durable runs:

| Operation | Median | Min | Max |
| --- | --- | --- | --- |
| Snapshot evaluation, 8 occupants | 17.9 us | 17.5 us | 130.9 us |
| Snapshot evaluation, 23 occupants | 36.3 us | 23.9 us | 291.1 us |
| Snapshot evaluation, 25 occupants | 36.8 us | 24.7 us | 67.8 us |
| Fit evaluation of one candidate | 1.0 us | 0.6 us | 3.3 us |
| Canonical encode plus digest of one snapshot | 8.3 us | 5.9 us | 17.2 us |
| Durable register of one rack (flush, verify, publish) | 7.48 ms | 5.12 ms | 26.13 ms |
| Durable open and full verification of a 52-rack store | 3.03 ms | 2.59 ms | 4.02 ms |
| Durable open with writer authority (decode, validate, lock) | 9.27 ms | 8.23 ms | 12.71 ms |

These are single-machine figures with no before/after pair and no comparative
claim. A durable publication is dominated by the flush, as it must be: the
honest cost of not losing an acknowledged change is a write that has reached
storage. The store used for the durable figures held 52 racks and 228 066 bytes
after the timed publications, and it was verified rather than assumed before the
figures were reported.

## 13. Genuine limitations

* **One rack per record.** Cross-rack and facility-wide capacity aggregation
  belongs to Facility Capacity and is not implemented here.
* **Vertical coordinates only.** Occupancy is accounted in the two half-unit
  mount slots of the published coordinate space. Lateral splits within one slot
  and depth or rail-plane occupancy (front versus rear mounting) are not
  modelled; a shared mount covers co-occupancy through its declared shared
  class.
* **Point load is decided conservatively.** A per-unit point-load limit is
  checked against the worst case that a candidate's whole mass lands on the
  busiest unit it touches, so a request that would only fit with a favourable
  distribution is reported as undecided rather than accepted.
* **Weight is distributed by a fixed convention.** An occupant's mass is spread
  evenly over the units it spans, with the remainder charged to its lowest unit.
  That is a documented modelling choice, not a measurement of rail loading.
* **Cooling is a heat budget.** The thermal envelope is accounted in watts of
  heat rejection. Airflow, delta-T, hot-aisle containment and per-unit
  temperature are not modelled; a supply-air observation is carried for
  reporting and never influences authoritative accounting.
* **Heat is never inferred without policy.** If an occupant declares no heat
  rejection and the policy declares no power-to-heat equivalence, its heat is
  unknown and the conservative free cooling is zero.
* **Freshness is the caller's clock.** The library never reads a clock itself:
  every evaluation takes the instant it should reason about as an argument, and
  `system_now()` exists only for callers such as the CLI.
* **Mutations copy the authoritative state.** A mutation builds a candidate
  state, so its cost is proportional to the size of the catalog. This is what
  makes a refused publication leave no trace, and it is the deliberate trade.
* **The POSIX platform paths are unexercised.** They are implemented and
  reviewed but were neither compiled nor run on this platform.
* **No hardware proof of any kind.** See section 11.
* **The specification format is version 1.** A document declaring another
  version is refused rather than upgraded.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
