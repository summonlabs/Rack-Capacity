# Contributing to Rack Capacity

Thank you for your interest in contributing to Rack Capacity. This document
describes the conventions used in this repository so that every contribution
lands cleanly.

## Scope

Rack Capacity is the per-rack capacity accounting runtime of the Data Center
Control Plane (DCCP), Tranche 2 - Facility Capacity and Placement. It answers:

* How much capacity does one exact rack generation have left across physical
  slots, power, cooling, weight and serviceability?
* Which of those constraints becomes binding first, and by how much?
* What is occupied, what is reserved, what is merely submitted, and what is
  unknown - and therefore not free capacity?
* Is a candidate device compatible with the remaining capacity of a rack, when
  evaluated without taking any placement authority?
* Which evidence was used, which policy headroom was applied, and why exactly
  did an evaluation reach its verdict?
* Does a durable capacity record still match current evidence after a restart?

Rack Capacity deliberately does **not** become a Rack Registry, a Space
Capacity runtime, a Facility Capacity runtime, a Power Capacity runtime, a
Cooling Capacity runtime, an Asset Registry, a Physical Location Registry, a
Facility Placement Planner or a Facility Capacity Reservation. It publishes no
placement decision, reserves no future capacity, actuates no electrical or
cooling equipment, and performs no workload scheduling. Contributions that pull
those boundaries into Rack Capacity will be redirected.

## Development setup

Rack Capacity targets:

* C++20 (portable, standard library only, no third-party dependencies)
* Windows / MSVC 19.44 with CMake and Ninja, or GCC/Clang with CMake
* A local filesystem as the durable substrate for authoritative state

Clone the repository, then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite must be allowed to run to completion. It contains no timeouts
and no watchdog-success logic by design: a hanging test is a defect to diagnose
and repair, not something to mask.

## Coding guidelines

* Use the `rackcapacity` namespace.
* Use strong types for identities, generations, revisions, epochs and units.
  Never pass a raw integer where a `CapacityGeneration`, `SnapshotRevision`,
  `EvidenceEpoch` or `Watts` exists, and never convert between unrelated ones
  implicitly.
* Validate every externally supplied value at the construction boundary and
  return a machine-readable `ErrorCode`, not a silently normalized value.
* Keep behavior deterministic. Any randomized test must fix its seed and print
  it, and the library must never read a wall clock on its own.
* Never let an unknown quantity become free capacity. Distinguish zero from
  unknown, unsupported from unavailable, and committed from submitted.
* Fix the concurrency model in writing before changing it. The documented
  model is: `CapacityCatalog` is internally synchronized, mutations are
  serialized end to end including durable publication, and the lock order is
  catalog mutex, then capacity store writer mutex, never in reverse.
* Never emit callbacks while holding an internal lock; the library emits no
  callbacks while locked at all.
* Bound every externally influenced size before allocating, and bound every
  collection that can grow from untrusted input.
* The project compiles under MSVC `/W4 /WX` and under `-Wall -Wextra -Werror`
  on GCC/Clang. New code must be warning-free; fix the cause rather than
  suppressing the warning.
* There is no product TODO, placeholder handler, or dead code in this
  repository, and contributions must not add any.

## Testing expectations

Every behavioral change should come with proof:

* unit tests for the changed type or rule;
* property/invariant tests for anything with interval, accounting or count
  semantics;
* a persistence test through a real close/reopen when durable state changes;
* an adversarial test when new input is parsed.

Tests must not use timeouts, time-of-day dependence, or fixed sleeps that are
load-bearing. Where a test needs a timestamp, it supplies one explicitly.

## Repository hygiene

Do not commit build directories, install trees, benchmark residue, crash
dumps, logs, or temporary fixtures. `git status` must be clean before a change
is proposed.

## Submitting a contribution

1. Open an issue or start a discussion describing the change if it is larger
   than a small fix.
2. Keep the change focused; unrelated reformatting makes review harder.
3. Make sure Release and Debug builds are warning-free and the full test suite
   passes.
4. Open a pull request with a neutral, descriptive commit message.

## License

By submitting a contribution to this repository you agree that your
contribution is licensed under the Apache License, Version 2.0, as described in
the `LICENSE` file in this distribution. There is no Contributor License
Agreement (CLA) and no copyright assignment requirement: contributions are
accepted under the inbound-equals-outbound terms of Section 5 of the license.

Do not add co-author trailers, AI attribution, or generated-by lines to
commits in this repository.
