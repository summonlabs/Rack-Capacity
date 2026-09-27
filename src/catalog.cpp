// Rack Capacity - the capacity catalog: per-rack capacity authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency model, fixed before implementation and audited after:
//
//   * One mutex guards all catalog state. It is taken first and is released
//     last. While it is held the catalog calls the capacity store, which takes
//     its own writer mutex, and pure functions that take no lock at all. The
//     lock order is therefore catalog mutex, then capacity store writer mutex,
//     and it is never taken in reverse.
//   * No callback, observer or hook is invoked while the catalog mutex is held.
//     The only hook in the library belongs to the store's publication path and
//     is documented there.
//   * Queries copy what they return and release the mutex before returning, so
//     no reference into catalog state escapes.
//   * A mutation builds a candidate state, publishes it durably, and only then
//     makes it authoritative in memory. A refused publication leaves the
//     in-memory state exactly as it was.

#include "rack_capacity/catalog.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace rackcapacity {
namespace {

[[nodiscard]] CapacityError rejected(ErrorCode code, std::string message, std::string operation,
                                     std::string subject, std::string related = {},
                                     std::vector<std::string> items = {}) {
  return make_error(code, std::move(message),
                    ErrorDetail{std::move(operation), std::move(subject), std::move(related), 0, 0,
                                std::move(items)});
}

[[nodiscard]] bool record_less(const RackCapacityRecord& left,
                               const RackCapacityRecord& right) {
  return left.rack < right.rack;
}

}  // namespace

struct CapacityCatalog::Impl {
  CatalogOptions options{};
  std::unique_ptr<CapacityStore> store{};
  // The catalog lock. Taken first, released last; the store's writer mutex is
  // the only lock ever taken while it is held.
  mutable std::mutex mutex{};
  CatalogState state{};
  // The retained previous publication, loaded once at open for diffing. It is
  // never authoritative and never mutated.
  CatalogState previous{};
  bool has_previous = false;
  std::vector<RejectionRecord> rejections{};
  AttemptId next_attempt{AttemptId::trusted(1)};

  [[nodiscard]] std::size_t find(const RackId& rack) const {
    std::size_t low = 0;
    std::size_t high = state.records.size();
    while (low < high) {
      const std::size_t middle = low + (high - low) / 2;
      if (state.records[middle].rack < rack) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    if (low < state.records.size() && state.records[low].rack == rack) {
      return low;
    }
    return state.records.size();
  }

  [[nodiscard]] const RackCapacityRecord* lookup(const RackId& rack) const {
    const std::size_t index = find(rack);
    return index < state.records.size() ? &state.records[index] : nullptr;
  }

  void note_rejection(const CapacityError& error, std::string operation, TimestampNs at) {
    RejectionRecord record;
    record.error = error;
    record.operation = std::move(operation);
    record.at = at;
    if (rejections.size() >= kMaxRejectionJournalEntries) {
      rejections.erase(rejections.begin());
    }
    rejections.push_back(std::move(record));
  }

  [[nodiscard]] Result<MutationReceipt> reject(const CapacityError& error, std::string operation,
                                               TimestampNs at) {
    note_rejection(error, std::move(operation), at);
    return error;
  }

  [[nodiscard]] Status check_precondition(const CapacityExpectation& expected,
                                          const RackCapacityRecord& record) const {
    if (expected.rack != record.rack) {
      return Status(rejected(ErrorCode::InvalidArgument,
                             "the expectation names a different rack", "catalog.precondition",
                             expected.rack.text(), record.rack.text()));
    }
    if (expected.composition_generation != record.composition_generation) {
      return Status(make_error(
          ErrorCode::StaleCompositionGeneration,
          "the expectation was planned against a different rack composition generation",
          ErrorDetail{"catalog.precondition", "composition_generation", record.rack.text(),
                      record.composition_generation.value(),
                      expected.composition_generation.value(), {}}));
    }
    if (expected.capacity_generation != record.capacity_generation) {
      return Status(make_error(
          ErrorCode::StaleCapacityGeneration,
          "the expectation was planned against a different capacity generation",
          ErrorDetail{"catalog.precondition", "capacity_generation", record.rack.text(),
                      record.capacity_generation.value(), expected.capacity_generation.value(),
                      {}}));
    }
    if (expected.revision != record.revision) {
      return Status(make_error(
          ErrorCode::StaleSnapshotRevision,
          "the expectation was planned against a different snapshot revision",
          ErrorDetail{"catalog.precondition", "revision", record.rack.text(),
                      record.revision.value(), expected.revision.value(), {}}));
    }
    return Status{};
  }

  [[nodiscard]] Result<MutationReceipt> publish_change(CatalogState& candidate,
                                                       const RackCapacityRecord& record,
                                                       MutationOutcome outcome,
                                                       TimestampNs accepted_at,
                                                       ReasonCode reason, std::string detail,
                                                       std::string operation) {
    if (outcome == MutationOutcome::Applied) {
      const Result<StoreSequence> expected_sequence = state.sequence.next();
      if (!expected_sequence.has_value()) {
        return reject(expected_sequence.error(), operation, accepted_at);
      }
      candidate.sequence = expected_sequence.value();
      candidate.written_at = accepted_at;
      const Result<StoreSequence> published = store->publish(candidate);
      if (!published.has_value()) {
        return reject(published.error(), operation, accepted_at);
      }
      state = candidate;
    }
    MutationReceipt receipt;
    receipt.outcome = outcome;
    receipt.rack = record.rack;
    receipt.lifecycle = record.lifecycle;
    receipt.capacity_generation = record.capacity_generation;
    receipt.revision = record.revision;
    receipt.composition_generation = record.composition_generation;
    receipt.evidence_epoch = record.snapshot.evidence_epoch;
    receipt.snapshot_digest = record.snapshot.digest;
    receipt.attempt = next_attempt;
    receipt.sequence = state.sequence;
    receipt.epoch = state.epoch;
    receipt.accepted_at = accepted_at;
    receipt.reason = reason;
    receipt.detail = std::move(detail);
    const Result<AttemptId> advanced = next_attempt.next();
    if (advanced.has_value()) {
      next_attempt = advanced.value();
    }
    return receipt;
  }

  [[nodiscard]] Result<MutationReceipt> apply_idempotent_replay(
      const IdempotencyRecord& entry, const CapacityError& conflict, std::string operation,
      TimestampNs at) {
    const RackCapacityRecord* record = lookup(entry.rack);
    if (record == nullptr) {
      return reject(rejected(ErrorCode::UnknownRackId,
                             "the recorded request names a rack the catalog does not hold",
                             operation, entry.rack.text()),
                    operation, at);
    }
    MutationReceipt receipt;
    receipt.outcome = MutationOutcome::Replayed;
    receipt.rack = record->rack;
    receipt.lifecycle = record->lifecycle;
    receipt.capacity_generation = entry.capacity_generation;
    receipt.revision = entry.revision;
    receipt.composition_generation = record->composition_generation;
    receipt.evidence_epoch = record->snapshot.evidence_epoch;
    receipt.snapshot_digest = entry.snapshot_digest;
    receipt.attempt = entry.attempt;
    receipt.sequence = entry.sequence;
    receipt.epoch = state.epoch;
    receipt.accepted_at = at;
    receipt.reason = ReasonCode::Ok;
    receipt.detail = "the request was already applied and was answered from the replay journal";
    if (entry.snapshot_digest != record->snapshot.digest) {
      receipt.detail += "; the rack has since moved to a different capacity state";
    }
    (void)conflict;
    return receipt;
  }
};

CapacityCatalog::CapacityCatalog(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CapacityCatalog::~CapacityCatalog() { (void)close(); }

Result<std::unique_ptr<CapacityCatalog>> CapacityCatalog::open(const CatalogOptions& options) {
  auto impl = std::make_unique<Impl>();
  impl->options = options;

  StoreOptions store_options;
  store_options.path = options.path;
  store_options.writer_id = options.writer_id;
  store_options.producer = options.producer;
  store_options.read_only = options.read_only;
  store_options.create_if_missing = options.create_if_missing;
  store_options.retain_previous = options.retain_previous;
  store_options.adopt_abandoned_lock = options.adopt_abandoned_lock;
  store_options.expected_incarnation = options.expected_incarnation;
  store_options.fault_hook = options.fault_hook;

  Result<std::unique_ptr<CapacityStore>> store = CapacityStore::open(store_options);
  if (!store.has_value()) {
    return store.error();
  }
  impl->store = std::move(store).value();
  impl->state = impl->store->state();
  std::sort(impl->state.records.begin(), impl->state.records.end(), record_less);

  if (options.retain_previous) {
    StoreOptions previous_options;
    previous_options.path = std::filesystem::path(options.path.string() + ".prev");
    previous_options.read_only = true;
    previous_options.create_if_missing = false;
    previous_options.retain_previous = false;
    Result<std::unique_ptr<CapacityStore>> previous = CapacityStore::open(previous_options);
    if (previous.has_value()) {
      impl->previous = previous.value()->state();
      std::sort(impl->previous.records.begin(), impl->previous.records.end(), record_less);
      impl->has_previous = true;
      (void)previous.value()->close();
    }
  }

  return std::unique_ptr<CapacityCatalog>(new CapacityCatalog(std::move(impl)));
}

Result<MutationReceipt> CapacityCatalog::register_rack(const RegisterRackRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const TimestampNs at = request.requested_at;
  if (at.value() <= 0) {
    return impl_->reject(rejected(ErrorCode::InvalidTimestamp,
                                 "a mutation must state the instant it was requested",
                                 "catalog.register_rack", request.inputs.composition.rack.text()),
                         "register_rack", at);
  }
  RackCapacityInputs inputs = request.inputs;
  const Status canonical = canonicalize_inputs(inputs);
  if (!canonical.has_value()) {
    return impl_->reject(canonical.error(), "register_rack", at);
  }
  const Status valid = validate_inputs(inputs);
  if (!valid.has_value()) {
    return impl_->reject(valid.error(), "register_rack", at);
  }
  const RackId rack = inputs.composition.rack;
  const StateDigest facts = inputs_digest(inputs);
  // The replay journal is consulted before the existence check, because a
  // genuine replay of a registration names a rack that exists precisely
  // because the request already succeeded.
  const std::optional<IdempotencyRecord> replay = impl_->state.idempotency.find(inputs.request);
  if (replay.has_value()) {
    if (replay->rack != rack || replay->inputs_digest != facts) {
      return impl_->reject(rejected(ErrorCode::RequestIdConflict,
                                   "the request identity was already used for a different request",
                                   "catalog.register_rack", rack.text()),
                           "register_rack", at);
    }
    return impl_->apply_idempotent_replay(replay.value(), CapacityError{}, "register_rack", at);
  }
  if (impl_->lookup(rack) != nullptr) {
    return impl_->reject(rejected(ErrorCode::DuplicateRackId,
                                 "a rack with this identity is already registered",
                                 "catalog.register_rack", rack.text()),
                         "register_rack", at);
  }

  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1), at,
      EvidenceFreshness::Fresh);
  if (!snapshot.has_value()) {
    return impl_->reject(snapshot.error(), "register_rack", at);
  }

  RackCapacityRecord record;
  record.rack = rack;
  record.composition_generation = inputs.composition.generation;
  record.capacity_generation = CapacityGeneration::trusted(1);
  record.revision = SnapshotRevision::trusted(1);
  record.lifecycle = RackLifecycle::Active;
  record.standing = RecoveryStanding::Authoritative;
  record.inputs = inputs;
  record.snapshot = snapshot.value();
  record.accepted_at = at;
  record.last_attempt = impl_->next_attempt;
  record.last_actor = inputs.actor;

  CatalogState candidate = impl_->state;
  candidate.records.push_back(record);
  std::sort(candidate.records.begin(), candidate.records.end(), record_less);
  if (candidate.records.size() > kMaxRacks) {
    return impl_->reject(rejected(ErrorCode::LimitExceeded,
                                 "the catalog holds as many racks as the documented bound allows",
                                 "catalog.register_rack", rack.text()),
                         "register_rack", at);
  }
  IdempotencyRecord entry;
  entry.request = inputs.request;
  entry.rack = rack;
  entry.attempt = impl_->next_attempt;
  entry.outcome = MutationOutcome::Applied;
  entry.capacity_generation = record.capacity_generation;
  entry.revision = record.revision;
  entry.inputs_digest = facts;
  entry.snapshot_digest = record.snapshot.digest;
  entry.accepted_at = at;
  const Status journaled = candidate.idempotency.record(entry);
  if (!journaled.has_value()) {
    return impl_->reject(journaled.error(), "register_rack", at);
  }

  return impl_->publish_change(candidate, record, MutationOutcome::Applied, at, ReasonCode::Ok,
                               "the rack was registered with its first capacity snapshot",
                               "register_rack");
}

Result<MutationReceipt> CapacityCatalog::apply_evidence(const ApplyEvidenceRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const TimestampNs at = request.requested_at;
  if (at.value() <= 0) {
    return impl_->reject(rejected(ErrorCode::InvalidTimestamp,
                                 "a mutation must state the instant it was requested",
                                 "catalog.apply_evidence", request.expected.rack.text()),
                         "apply_evidence", at);
  }
  RackCapacityInputs inputs = request.inputs;
  const Status canonical = canonicalize_inputs(inputs);
  if (!canonical.has_value()) {
    return impl_->reject(canonical.error(), "apply_evidence", at);
  }
  const Status valid = validate_inputs(inputs);
  if (!valid.has_value()) {
    return impl_->reject(valid.error(), "apply_evidence", at);
  }

  const std::size_t index = impl_->find(request.expected.rack);
  if (index >= impl_->state.records.size()) {
    return impl_->reject(rejected(ErrorCode::UnknownRackId,
                                 "no rack with this identity is registered",
                                 "catalog.apply_evidence", request.expected.rack.text()),
                         "apply_evidence", at);
  }
  const RackCapacityRecord current = impl_->state.records[index];
  const Status precondition = impl_->check_precondition(request.expected, current);
  if (!precondition.has_value()) {
    return impl_->reject(precondition.error(), "apply_evidence", at);
  }
  if (inputs.composition.rack != current.rack) {
    return impl_->reject(rejected(ErrorCode::InvalidArgument,
                                 "the evidence names a different rack than the precondition",
                                 "catalog.apply_evidence", current.rack.text(),
                                 inputs.composition.rack.text()),
                         "apply_evidence", at);
  }
  if (current.lifecycle == RackLifecycle::Retired) {
    return impl_->reject(rejected(ErrorCode::RackRetired,
                                 "a retired rack accepts no further evidence; its capacity is "
                                 "never quoted again",
                                 "catalog.apply_evidence", current.rack.text()),
                         "apply_evidence", at);
  }
  if (inputs.composition.generation < current.composition_generation) {
    return impl_->reject(
        make_error(ErrorCode::StaleCompositionGeneration,
                   "the evidence describes an older rack composition generation",
                   ErrorDetail{"catalog.apply_evidence", "composition_generation",
                               current.rack.text(), current.composition_generation.value(),
                               inputs.composition.generation.value(), {}}),
        "apply_evidence", at);
  }
  if (inputs.epoch.value() < current.snapshot.evidence_epoch.value()) {
    return impl_->reject(
        make_error(ErrorCode::StaleEvidenceEpoch,
                   "the evidence epoch is older than the one already applied",
                   ErrorDetail{"catalog.apply_evidence", "evidence_epoch", current.rack.text(),
                               current.snapshot.evidence_epoch.value(), inputs.epoch.value(), {}}),
        "apply_evidence", at);
  }

  const StateDigest facts = inputs_digest(inputs);
  const std::optional<IdempotencyRecord> replay = impl_->state.idempotency.find(inputs.request);
  if (replay.has_value()) {
    if (replay->rack != current.rack || replay->inputs_digest != facts) {
      return impl_->reject(rejected(ErrorCode::RequestIdConflict,
                                   "the request identity was already used for a different request",
                                   "catalog.apply_evidence", current.rack.text()),
                           "apply_evidence", at);
    }
    return impl_->apply_idempotent_replay(replay.value(), CapacityError{}, "apply_evidence", at);
  }

  if (inputs.epoch == current.snapshot.evidence_epoch && facts == current.snapshot.inputs_digest) {
    MutationReceipt receipt;
    receipt.outcome = MutationOutcome::NoChange;
    receipt.rack = current.rack;
    receipt.lifecycle = current.lifecycle;
    receipt.capacity_generation = current.capacity_generation;
    receipt.revision = current.revision;
    receipt.composition_generation = current.composition_generation;
    receipt.evidence_epoch = current.snapshot.evidence_epoch;
    receipt.snapshot_digest = current.snapshot.digest;
    receipt.attempt = impl_->next_attempt;
    receipt.sequence = impl_->state.sequence;
    receipt.epoch = impl_->state.epoch;
    receipt.accepted_at = at;
    receipt.reason = ReasonCode::Ok;
    receipt.detail = "the evidence bundle describes exactly the state the record already holds";
    return receipt;
  }
  if (inputs.epoch == current.snapshot.evidence_epoch) {
    return impl_->reject(
        make_error(ErrorCode::StaleEvidenceEpoch,
                   "the evidence epoch repeats the current one but describes different facts; an "
                   "unchanged epoch may only confirm the state it already names",
                   ErrorDetail{"catalog.apply_evidence", "evidence_epoch", current.rack.text(),
                               current.snapshot.evidence_epoch.value(), inputs.epoch.value(), {}}),
        "apply_evidence", at);
  }

  const Result<CapacityGeneration> next_generation = current.capacity_generation.next();
  if (!next_generation.has_value()) {
    return impl_->reject(next_generation.error(), "apply_evidence", at);
  }
  const Result<SnapshotRevision> next_revision = current.revision.next();
  if (!next_revision.has_value()) {
    return impl_->reject(next_revision.error(), "apply_evidence", at);
  }
  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, next_generation.value(), next_revision.value(), at, EvidenceFreshness::Fresh);
  if (!snapshot.has_value()) {
    return impl_->reject(snapshot.error(), "apply_evidence", at);
  }

  RackCapacityRecord record = current;
  record.composition_generation = inputs.composition.generation;
  record.capacity_generation = next_generation.value();
  record.revision = next_revision.value();
  record.inputs = inputs;
  record.snapshot = snapshot.value();
  record.accepted_at = at;
  record.last_attempt = impl_->next_attempt;
  record.last_actor = inputs.actor;
  // Fresh evidence is the repair path for a quarantined record: the record is
  // rebuilt from evidence that has been validated, so it becomes usable again.
  record.lifecycle = RackLifecycle::Active;
  record.standing = RecoveryStanding::Authoritative;

  CatalogState candidate = impl_->state;
  candidate.records[index] = record;
  IdempotencyRecord entry;
  entry.request = inputs.request;
  entry.rack = record.rack;
  entry.attempt = impl_->next_attempt;
  entry.outcome = MutationOutcome::Applied;
  entry.capacity_generation = record.capacity_generation;
  entry.revision = record.revision;
  entry.inputs_digest = facts;
  entry.snapshot_digest = record.snapshot.digest;
  entry.accepted_at = at;
  const Status journaled = candidate.idempotency.record(entry);
  if (!journaled.has_value()) {
    return impl_->reject(journaled.error(), "apply_evidence", at);
  }

  return impl_->publish_change(candidate, record, MutationOutcome::Applied, at, ReasonCode::Ok,
                               "the evidence bundle replaced the previous one and the capacity "
                               "snapshot was recomputed",
                               "apply_evidence");
}

Result<MutationReceipt> CapacityCatalog::retire_rack(const RetireRackRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const TimestampNs at = request.requested_at;
  if (at.value() <= 0) {
    return impl_->reject(rejected(ErrorCode::InvalidTimestamp,
                                 "a mutation must state the instant it was requested",
                                 "catalog.retire_rack", request.expected.rack.text()),
                         "retire_rack", at);
  }
  if (request.request.empty()) {
    return impl_->reject(rejected(ErrorCode::EmptyValue,
                                 "a retirement must carry a request identity for bounded "
                                 "idempotent replay",
                                 "catalog.retire_rack", request.expected.rack.text()),
                         "retire_rack", at);
  }
  const std::size_t index = impl_->find(request.expected.rack);
  if (index >= impl_->state.records.size()) {
    return impl_->reject(rejected(ErrorCode::UnknownRackId,
                                 "no rack with this identity is registered",
                                 "catalog.retire_rack", request.expected.rack.text()),
                         "retire_rack", at);
  }
  const RackCapacityRecord current = impl_->state.records[index];
  const Status precondition = impl_->check_precondition(request.expected, current);
  if (!precondition.has_value()) {
    return impl_->reject(precondition.error(), "retire_rack", at);
  }
  const std::optional<IdempotencyRecord> replay = impl_->state.idempotency.find(request.request);
  if (replay.has_value()) {
    if (replay->rack != current.rack) {
      return impl_->reject(rejected(ErrorCode::RequestIdConflict,
                                   "the request identity was already used for a different request",
                                   "catalog.retire_rack", current.rack.text()),
                           "retire_rack", at);
    }
    return impl_->apply_idempotent_replay(replay.value(), CapacityError{}, "retire_rack", at);
  }
  if (current.lifecycle == RackLifecycle::Retired) {
    return impl_->reject(rejected(ErrorCode::LifecycleTransitionNotAllowed,
                                 "the rack is already retired", "catalog.retire_rack",
                                 current.rack.text()),
                         "retire_rack", at);
  }

  RackCapacityRecord record = current;
  record.lifecycle = RackLifecycle::Retired;
  record.standing = RecoveryStanding::Authoritative;
  record.accepted_at = at;
  record.last_attempt = impl_->next_attempt;
  record.last_actor = request.actor;

  CatalogState candidate = impl_->state;
  candidate.records[index] = record;
  IdempotencyRecord entry;
  entry.request = request.request;
  entry.rack = record.rack;
  entry.attempt = impl_->next_attempt;
  entry.outcome = MutationOutcome::Applied;
  entry.capacity_generation = record.capacity_generation;
  entry.revision = record.revision;
  entry.inputs_digest = record.snapshot.inputs_digest;
  entry.snapshot_digest = record.snapshot.digest;
  entry.accepted_at = at;
  const Status journaled = candidate.idempotency.record(entry);
  if (!journaled.has_value()) {
    return impl_->reject(journaled.error(), "retire_rack", at);
  }
  std::string detail = "the rack was retired; its record is retained for audit and its capacity is "
                       "never quoted again";
  if (!request.reason.empty()) {
    detail += ": ";
    detail += request.reason;
  }
  return impl_->publish_change(candidate, record, MutationOutcome::Applied, at,
                               ReasonCode::Ok, std::move(detail), "retire_rack");
}

Result<MutationReceipt> CapacityCatalog::revalidate(const RevalidateRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const TimestampNs at = request.requested_at;
  if (at.value() <= 0 || request.now.value() <= 0) {
    return impl_->reject(rejected(ErrorCode::InvalidTimestamp,
                                 "revalidation must state both the instant it was requested and "
                                 "the instant freshness is judged against",
                                 "catalog.revalidate", request.expected.rack.text()),
                         "revalidate", at);
  }
  const std::size_t index = impl_->find(request.expected.rack);
  if (index >= impl_->state.records.size()) {
    return impl_->reject(rejected(ErrorCode::UnknownRackId,
                                 "no rack with this identity is registered",
                                 "catalog.revalidate", request.expected.rack.text()),
                         "revalidate", at);
  }
  const RackCapacityRecord current = impl_->state.records[index];
  const Status precondition = impl_->check_precondition(request.expected, current);
  if (!precondition.has_value()) {
    return impl_->reject(precondition.error(), "revalidate", at);
  }
  if (current.standing != RecoveryStanding::PendingRevalidation) {
    MutationReceipt receipt;
    receipt.outcome = MutationOutcome::NoChange;
    receipt.rack = current.rack;
    receipt.lifecycle = current.lifecycle;
    receipt.capacity_generation = current.capacity_generation;
    receipt.revision = current.revision;
    receipt.composition_generation = current.composition_generation;
    receipt.evidence_epoch = current.snapshot.evidence_epoch;
    receipt.snapshot_digest = current.snapshot.digest;
    receipt.attempt = impl_->next_attempt;
    receipt.sequence = impl_->state.sequence;
    receipt.epoch = impl_->state.epoch;
    receipt.accepted_at = at;
    receipt.reason = ReasonCode::Ok;
    receipt.detail = "the record is already authoritative in this process";
    return receipt;
  }

  const RevalidationReport report = revalidate_record(current, request.now);
  RackCapacityRecord record = current;
  record.accepted_at = at;
  record.last_attempt = impl_->next_attempt;
  record.last_actor = request.actor;
  record.standing = RecoveryStanding::Authoritative;
  ReasonCode reason = ReasonCode::Ok;
  std::string detail;
  if (report.verdict == RevalidationVerdict::Diverged) {
    record.lifecycle = RackLifecycle::Quarantined;
    reason = ReasonCode::ClosureIdentityViolated;
    detail = "the recovered record does not reproduce from the evidence it carries and was "
             "quarantined; it returns to service only when fresh evidence is applied";
  } else {
    const Result<SnapshotRevision> next_revision = current.revision.next();
    if (!next_revision.has_value()) {
      return impl_->reject(next_revision.error(), "revalidate", at);
    }
    const EvidenceFreshness freshness = report.verdict == RevalidationVerdict::Current
                                            ? EvidenceFreshness::Fresh
                                            : EvidenceFreshness::Stale;
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        record.inputs, record.capacity_generation, next_revision.value(), request.now, freshness);
    if (!snapshot.has_value()) {
      return impl_->reject(snapshot.error(), "revalidate", at);
    }
    record.revision = next_revision.value();
    record.snapshot = snapshot.value();
    detail = report.verdict == RevalidationVerdict::Current
                 ? "the recovered record reproduces exactly from its evidence and that evidence is "
                   "current; the record is authoritative again"
                 : "the recovered record reproduces exactly from its evidence, which is older than "
                   "the freshness policy allows; the record is authoritative but stale";
  }

  CatalogState candidate = impl_->state;
  candidate.records[index] = record;
  return impl_->publish_change(candidate, record, MutationOutcome::Applied, at, reason,
                               std::move(detail), "revalidate");
}

bool CapacityCatalog::contains_rack(const RackId& rack) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->lookup(rack) != nullptr;
}

std::size_t CapacityCatalog::rack_count() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.records.size();
}

Result<RackCapacityRecord> CapacityCatalog::record(const RackId& rack) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const RackCapacityRecord* found = impl_->lookup(rack);
  if (found == nullptr) {
    return rejected(ErrorCode::UnknownRackId, "no rack with this identity is registered",
                    "catalog.record", rack.text());
  }
  return *found;
}

std::vector<RackCapacityRecord> CapacityCatalog::records() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.records;
}

Result<RackCapacitySnapshot> CapacityCatalog::snapshot(const CapacityExpectation& expected) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const RackCapacityRecord* found = impl_->lookup(expected.rack);
  if (found == nullptr) {
    return rejected(ErrorCode::UnknownRackId, "no rack with this identity is registered",
                    "catalog.snapshot", expected.rack.text());
  }
  const Status precondition = impl_->check_precondition(expected, *found);
  if (!precondition.has_value()) {
    return precondition.error();
  }
  if (found->lifecycle == RackLifecycle::Retired) {
    return rejected(ErrorCode::RackRetired,
                    "the rack is retired; no capacity is quoted for it", "catalog.snapshot",
                    expected.rack.text());
  }
  if (found->lifecycle == RackLifecycle::Quarantined) {
    return rejected(ErrorCode::RackQuarantined,
                    "the record is quarantined after a failed revalidation", "catalog.snapshot",
                    expected.rack.text());
  }
  if (found->standing != RecoveryStanding::Authoritative) {
    return rejected(ErrorCode::RevalidationRequired,
                    "the record was recovered from durable state and must be revalidated in this "
                    "process before its capacity may be quoted",
                    "catalog.snapshot", expected.rack.text());
  }
  return found->snapshot;
}

Result<InspectedSnapshot> CapacityCatalog::inspect_snapshot(const CapacityExpectation& expected,
                                                           TimestampNs now) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const RackCapacityRecord* found = impl_->lookup(expected.rack);
  if (found == nullptr) {
    return rejected(ErrorCode::UnknownRackId, "no rack with this identity is registered",
                    "catalog.inspect_snapshot", expected.rack.text());
  }
  const Status precondition = impl_->check_precondition(expected, *found);
  if (!precondition.has_value()) {
    return precondition.error();
  }
  if (found->lifecycle == RackLifecycle::Retired) {
    return rejected(ErrorCode::RackRetired, "the rack is retired; no capacity is quoted for it",
                    "catalog.inspect_snapshot", expected.rack.text());
  }
  const RevalidationReport verification = revalidate_record(*found, now);
  if (verification.verdict == RevalidationVerdict::Diverged) {
    return make_error(ErrorCode::CorruptState,
                      "the record does not reproduce from the evidence it carries and is not "
                      "displayed as authoritative",
                      ErrorDetail{"catalog.inspect_snapshot", "record", expected.rack.text(), 0, 0,
                                  verification.differences});
  }
  const EvidenceFreshness freshness = verification.verdict == RevalidationVerdict::Current
                                          ? EvidenceFreshness::Fresh
                                          : EvidenceFreshness::Stale;
  // The recomputed snapshot keeps the record's own generations: inspection
  // re-derives one state, it never advances one.
  const Result<RackCapacitySnapshot> recomputed =
      evaluate_capacity(found->inputs, found->capacity_generation, found->revision, now, freshness);
  if (!recomputed.has_value()) {
    return recomputed.error();
  }
  InspectedSnapshot inspected;
  inspected.record = *found;
  inspected.snapshot = recomputed.value();
  inspected.verdict = verification.verdict;
  inspected.freshness = freshness;
  inspected.recovered = found->standing != RecoveryStanding::Authoritative;
  return inspected;
}

Result<RevalidationReport> CapacityCatalog::inspect_revalidation(const RackId& rack,
                                                                TimestampNs now) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const RackCapacityRecord* found = impl_->lookup(rack);
  if (found == nullptr) {
    return rejected(ErrorCode::UnknownRackId, "no rack with this identity is registered",
                    "catalog.inspect_revalidation", rack.text());
  }
  return revalidate_record(*found, now);
}
Result<FitEvaluation> CapacityCatalog::evaluate_fit(const FitRequest& request) const {
  Result<RackCapacitySnapshot> current = snapshot(request.expected);
  if (!current.has_value()) {
    return current.error();
  }
  return rackcapacity::evaluate_fit(request, current.value());
}

Result<CapacityDiff> CapacityCatalog::diff_with_previous(const RackId& rack) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->has_previous) {
    return rejected(ErrorCode::NoPreviousState,
                    "no retained previous publication is available for diffing", "catalog.diff",
                    rack.text());
  }
  const RackCapacityRecord* current = impl_->lookup(rack);
  if (current == nullptr) {
    return rejected(ErrorCode::UnknownRackId, "no rack with this identity is registered",
                    "catalog.diff", rack.text());
  }
  const auto previous = std::lower_bound(
      impl_->previous.records.begin(), impl_->previous.records.end(), rack,
      [](const RackCapacityRecord& record, const RackId& id) { return record.rack < id; });
  if (previous == impl_->previous.records.end() || previous->rack != rack) {
    return rejected(ErrorCode::NoPreviousState,
                    "the retained previous publication does not hold this rack", "catalog.diff",
                    rack.text());
  }
  return diff_snapshots(previous->snapshot, current->snapshot);
}

std::vector<RejectionRecord> CapacityCatalog::rejections() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->rejections;
}

CatalogStats CapacityCatalog::stats() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  CatalogStats stats;
  stats.rack_count = impl_->state.records.size();
  for (const RackCapacityRecord& record : impl_->state.records) {
    switch (record.lifecycle) {
      case RackLifecycle::Active:
        ++stats.active_racks;
        break;
      case RackLifecycle::Retired:
        ++stats.retired_racks;
        break;
      case RackLifecycle::Quarantined:
        ++stats.quarantined_racks;
        break;
    }
    if (record.standing != RecoveryStanding::Authoritative) {
      ++stats.pending_revalidation_racks;
    }
    stats.asset_count += record.inputs.assets.size();
    stats.reservation_count += record.inputs.reservations.size();
  }
  stats.rejection_count = impl_->rejections.size();
  stats.idempotency_records = impl_->state.idempotency.records.size();
  stats.idempotency_evictions = impl_->state.idempotency.evicted;
  return stats;
}

CatalogStorage CapacityCatalog::storage() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  CatalogStorage storage;
  storage.incarnation = impl_->state.incarnation;
  storage.epoch = impl_->state.epoch;
  storage.sequence = impl_->state.sequence;
  storage.read_only = impl_->store->is_read_only();
  storage.holds_writer_authority = impl_->store->holds_writer_authority();
  storage.recovery_action = impl_->store->recovery().action;
  storage.writer_lock = impl_->store->writer_lock();
  storage.has_previous = impl_->has_previous;
  return storage;
}

Status CapacityCatalog::close() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->store->close();
}

}  // namespace rackcapacity
