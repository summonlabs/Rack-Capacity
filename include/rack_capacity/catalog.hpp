// Rack Capacity - the capacity catalog: per-rack capacity authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/diff.hpp"
#include "rack_capacity/errors.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/fit.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/persistence.hpp"
#include "rack_capacity/requests.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

struct CatalogOptions {
  std::filesystem::path path{};
  WriterId writer_id{};
  std::optional<SourceReference> producer{};
  bool read_only = false;
  bool create_if_missing = true;
  bool retain_previous = true;
  bool adopt_abandoned_lock = true;
  std::optional<std::string> expected_incarnation{};
  FaultHook fault_hook{};
};

struct RejectionRecord {
  CapacityError error{};
  std::string operation{};
  TimestampNs at{};

  [[nodiscard]] std::string to_text() const;
};

// One record read back through the inspection path.
struct InspectedSnapshot {
  RackCapacityRecord record{};
  // The snapshot recomputed from the record's own evidence at the instant the
  // caller supplied. It is never published.
  RackCapacitySnapshot snapshot{};
  // Current or Stale. A record that does not reproduce is reported as an error
  // rather than through this field.
  RevalidationVerdict verdict = RevalidationVerdict::Unrevalidated;
  EvidenceFreshness freshness = EvidenceFreshness::Unrevalidated;
  // True when the record was recovered from durable state and has not been
  // promoted to authoritative in this process.
  bool recovered = false;

  [[nodiscard]] std::string to_text() const;
};

// Storage standing of an open catalog.
struct CatalogStorage {
  std::string incarnation{};
  StoreEpoch epoch{};
  StoreSequence sequence{};
  bool read_only = false;
  bool holds_writer_authority = false;
  RecoveryAction recovery_action = RecoveryAction::FreshStore;
  WriterLockInfo writer_lock{};
  // True when a retained previous publication is available for diffing.
  bool has_previous = false;

  [[nodiscard]] std::string to_text() const;
};

struct CatalogStats {
  std::size_t rack_count = 0;
  std::size_t active_racks = 0;
  std::size_t retired_racks = 0;
  std::size_t quarantined_racks = 0;
  std::size_t pending_revalidation_racks = 0;
  std::size_t asset_count = 0;
  std::size_t reservation_count = 0;
  std::size_t rejection_count = 0;
  std::size_t idempotency_records = 0;
  std::uint64_t idempotency_evictions = 0;

  [[nodiscard]] std::string to_text() const;
};

// Per-rack capacity accounting authority.
//
// The catalog owns the capacity state of every registered rack, in memory and
// durably. It does not place, schedule, reserve or actuate anything: it
// accounts for capacity, reports it, and refuses claims that depend on
// authority it does not have.
//
// Concurrency model, fixed before implementation:
//
//   * One mutex guards all catalog state. It is a leaf with respect to the
//     library: while it is held the catalog calls only into the capacity store,
//     which takes its own writer mutex, and into pure functions that take no
//     locks at all. The lock order is therefore catalog mutex, then capacity
//     store writer mutex, and it is never taken in reverse.
//   * Reads copy the immutable record they need while the mutex is held and
//     then release it. No reference into catalog state escapes a query.
//   * Mutations are serialized end to end, including the durable publication,
//     so a mutation that cannot be published leaves no trace in memory: the
//     in-memory state never runs ahead of the durable state.
//   * No callback, observer or hook is ever invoked while the internal mutex is
//     held. The fault-injection hook used by tests runs inside the store's
//     publication path, not while the catalog mutex is held by a query.
class RACK_CAPACITY_API CapacityCatalog {
 public:
  [[nodiscard]] static Result<std::unique_ptr<CapacityCatalog>> open(
      const CatalogOptions& options);

  ~CapacityCatalog();
  CapacityCatalog(CapacityCatalog&&) = delete;
  CapacityCatalog& operator=(CapacityCatalog&&) = delete;
  CapacityCatalog(const CapacityCatalog&) = delete;
  CapacityCatalog& operator=(const CapacityCatalog&) = delete;

  // -------------------------------------------------------------------------
  // Mutations. Each carries an explicit precondition, is validated, evaluated
  // and durably published before it is reported as applied.
  // -------------------------------------------------------------------------

  [[nodiscard]] Result<MutationReceipt> register_rack(const RegisterRackRequest& request);
  [[nodiscard]] Result<MutationReceipt> apply_evidence(const ApplyEvidenceRequest& request);
  [[nodiscard]] Result<MutationReceipt> retire_rack(const RetireRackRequest& request);
  [[nodiscard]] Result<MutationReceipt> revalidate(const RevalidateRequest& request);

  // -------------------------------------------------------------------------
  // Queries.
  // -------------------------------------------------------------------------

  [[nodiscard]] bool contains_rack(const RackId& rack) const;
  [[nodiscard]] std::size_t rack_count() const;
  // The whole record, including lifecycle and recovery standing. This is the
  // audit path: it returns quarantined and recovered-pending records verbatim.
  [[nodiscard]] Result<RackCapacityRecord> record(const RackId& rack) const;
  [[nodiscard]] std::vector<RackCapacityRecord> records() const;
  // The capacity snapshot of one exact state. Refuses a stale precondition,
  // a retired rack, a quarantined record and a record that has not been
  // revalidated in this process.
  [[nodiscard]] Result<RackCapacitySnapshot> snapshot(const CapacityExpectation& expected) const;

  // The inspection path: recomputes one record's snapshot from the evidence the
  // record carries, verifies that the recomputation reproduces the published
  // one, and reports it with freshness judged at `now`. It never publishes and
  // never changes the record, so a recovered record can be inspected without
  // first being promoted to authoritative.
  //
  // It is deliberately at least as strict as snapshot(): a record whose
  // published snapshot does not reproduce is refused rather than displayed.
  [[nodiscard]] Result<InspectedSnapshot> inspect_snapshot(const CapacityExpectation& expected,
                                                           TimestampNs now) const;
  // Runs the same verification without choosing a snapshot revision, for a
  // caller that only wants to know whether a record is still consistent.
  [[nodiscard]] Result<RevalidationReport> inspect_revalidation(const RackId& rack,
                                                                TimestampNs now) const;
  [[nodiscard]] Result<FitEvaluation> evaluate_fit(const FitRequest& request) const;
  // Diff between the retained previous publication and the current one for one
  // rack. Fails with NoPreviousState when nothing is retained.
  [[nodiscard]] Result<CapacityDiff> diff_with_previous(const RackId& rack) const;
  [[nodiscard]] std::vector<RejectionRecord> rejections() const;
  [[nodiscard]] CatalogStats stats() const;
  [[nodiscard]] CatalogStorage storage() const;

  // Releases writer authority and retires store-owned temporaries. Idempotent.
  [[nodiscard]] Status close();

 private:
  struct Impl;
  explicit CapacityCatalog(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rackcapacity
