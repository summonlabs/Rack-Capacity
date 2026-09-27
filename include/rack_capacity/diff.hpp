// Rack Capacity - snapshot diffs and revalidation reports.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/digest.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/measures.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Diffs
// ---------------------------------------------------------------------------

// One changed field. `path` is a stable, dotted, machine-readable locator such
// as "power.usable" or "slots.free.upper"; `before` and `after` are the
// canonical renderings of the two values. Changes are emitted in ascending
// path order so a diff is byte-for-byte reproducible.
struct FieldChange {
  std::string path{};
  std::string before{};
  std::string after{};
  Dimension dimension = Dimension::None;

  [[nodiscard]] bool operator<(const FieldChange& other) const noexcept {
    return path < other.path;
  }
  [[nodiscard]] bool operator==(const FieldChange& other) const noexcept {
    return path == other.path && before == other.before && after == other.after &&
           dimension == other.dimension;
  }
};

struct CapacityDiff {
  RackId rack{};
  CapacityGeneration from_generation{};
  CapacityGeneration to_generation{};
  SnapshotRevision from_revision{};
  SnapshotRevision to_revision{};
  StateDigest from_digest{};
  StateDigest to_digest{};
  bool identical = false;
  std::vector<FieldChange> changes{};
  // Explanations that appeared in `to` and not in `from`.
  std::vector<Explanation> new_explanations{};
  // Explanations that appeared in `from` and not in `to`.
  std::vector<Explanation> resolved_explanations{};
  Dimension primary_binding_before = Dimension::None;
  Dimension primary_binding_after = Dimension::None;

  [[nodiscard]] std::string to_text() const;
};

// Computes the difference between two snapshots of the same rack. Snapshots of
// different racks are refused.
[[nodiscard]] RACK_CAPACITY_API Result<CapacityDiff> diff_snapshots(
    const RackCapacitySnapshot& from, const RackCapacitySnapshot& to);

// ---------------------------------------------------------------------------
// Revalidation
// ---------------------------------------------------------------------------

enum class RevalidationVerdict : std::uint8_t {
  // The recovered snapshot reproduces exactly from the evidence it carries and
  // that evidence is inside the freshness policy.
  Current = 0,
  // The recovered snapshot reproduces exactly but its evidence is older than
  // the policy allows. The capacity is real but not current.
  Stale = 1,
  // The recovered snapshot does not reproduce from the evidence it carries, or
  // its recorded digest does not match its content. The record is quarantined.
  Diverged = 2,
  // The record has not been revalidated at all in this process.
  Unrevalidated = 3,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view revalidation_verdict_name(
    RevalidationVerdict verdict) noexcept;

struct RevalidationReport {
  RackId rack{};
  RevalidationVerdict verdict = RevalidationVerdict::Unrevalidated;
  CapacityExpectation revalidated{};
  EvidenceFreshness freshness = EvidenceFreshness::Unrevalidated;
  bool digest_matches = false;
  bool reproduces = false;
  // Ordered, human-readable descriptions of every difference found. Empty when
  // the snapshot reproduces exactly.
  std::vector<std::string> differences{};
  std::vector<Explanation> explanations{};

  [[nodiscard]] std::string to_text() const;
};

// Revalidates one record: recomputes its snapshot from the evidence it carries
// and compares the result with what was recovered. This proves that a durable
// record is internally consistent; it does not and cannot prove that the
// evidence itself is still current, which is what the freshness field reports.
[[nodiscard]] RACK_CAPACITY_API RevalidationReport revalidate_record(
    const RackCapacityRecord& record, TimestampNs now);

}  // namespace rackcapacity
