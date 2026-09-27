// Rack Capacity - catalog authority, preconditions, idempotency and lifecycle.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

Result<std::unique_ptr<CapacityCatalog>> open_catalog(const std::string& path, bool read_only = false,
                                                      bool create = true) {
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("test-writer").value();
  options.read_only = read_only;
  options.create_if_missing = create;
  return CapacityCatalog::open(options);
}

RackCapacityInputs input_for(const std::string& rack, std::uint64_t epoch,
                             const std::string& request) {
  return rctest::Bundle(rack, 42)
      .epoch(epoch)
      .request(request)
      .asset_units("a-0001", 1, 1)
      .asset_draw("a-0001", 1000)
      .build();
}

}  // namespace

RC_TEST(catalog_registers_and_quotes_capacity_with_preconditions) {
  rctest::CatalogFixture fixture("catalog-register");
  const RackCapacityInputs inputs = input_for("rack-a1", 1, "req-0001");
  const Result<MutationReceipt> receipt =
      fixture.catalog->register_rack(RegisterRackRequest{inputs, TimestampNs::trusted(kNow)});
  RC_REQUIRE_OK(receipt);
  RC_CHECK(receipt.value().outcome == MutationOutcome::Applied);
  RC_CHECK_EQ(receipt.value().capacity_generation.value(), 1u);
  RC_CHECK_EQ(receipt.value().revision.value(), 1u);
  RC_CHECK_EQ(receipt.value().sequence.value(), 1u);
  RC_CHECK(fixture.catalog->contains_rack(RackId::create("rack-a1").value()));
  RC_CHECK_EQ(fixture.catalog->rack_count(), 1u);

  const RackCapacityRecord record =
      fixture.catalog->record(RackId::create("rack-a1").value()).value();
  RC_CHECK(record.quotes_capacity());
  const Result<RackCapacitySnapshot> snapshot =
      fixture.catalog->snapshot(CapacityExpectation::of(record));
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().power.committed_known.value(), 1000);

  // A stale expectation is refused, and the refusal says what is current.
  CapacityExpectation stale = CapacityExpectation::of(record);
  stale.capacity_generation = CapacityGeneration::trusted(7);
  const Result<RackCapacitySnapshot> refused = fixture.catalog->snapshot(stale);
  RC_REQUIRE_CODE(refused, ErrorCode::StaleCapacityGeneration);
  RC_CHECK_EQ(refused.error().detail.expected, 1u);
  RC_CHECK_EQ(refused.error().detail.actual, 7u);

  stale = CapacityExpectation::of(record);
  stale.rack = RackId::create("rack-other").value();
  RC_REQUIRE_CODE(fixture.catalog->snapshot(stale), ErrorCode::UnknownRackId);

  // Registering the same rack twice is refused.
  RC_REQUIRE_CODE(fixture.catalog->register_rack(
                      RegisterRackRequest{input_for("rack-a1", 2, "req-0002"),
                                          TimestampNs::trusted(kNow)}),
                  ErrorCode::DuplicateRackId);
  RC_CHECK_EQ(fixture.catalog->rejections().size(), 1u);
}

RC_TEST(catalog_generations_advance_monotonically_and_refuse_regression) {
  rctest::CatalogFixture fixture("catalog-generations");
  const RackId rack = RackId::create("rack-a1").value();
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const RackCapacityRecord first = fixture.catalog->record(rack).value();
  RC_CHECK_EQ(first.capacity_generation.value(), 1u);
  RC_CHECK_EQ(first.revision.value(), 1u);
  RC_CHECK_EQ(first.composition_generation.value(), 1u);

  // The same evidence epoch carrying different facts is refused rather than
  // merged, because an epoch names one exact body of evidence.
  RackCapacityInputs conflicting = rctest::Bundle("rack-a1", 42)
                                       .epoch(1)
                                       .request("req-0002")
                                       .asset_units("a-0002", 3, 1)
                                       .build();
  RC_REQUIRE_CODE(fixture.catalog->apply_evidence(
                      ApplyEvidenceRequest{CapacityExpectation::of(first), conflicting,
                                           TimestampNs::trusted(kNow + 1)}),
                  ErrorCode::StaleEvidenceEpoch);

  // The same epoch carrying exactly the facts already held confirms the state.
  RackCapacityInputs same_facts = input_for("rack-a1", 1, "req-0003");
  const Result<MutationReceipt> confirmation = fixture.catalog->apply_evidence(
      ApplyEvidenceRequest{CapacityExpectation::of(first), same_facts,
                           TimestampNs::trusted(kNow + 2)});
  RC_REQUIRE_OK(confirmation);
  RC_CHECK(confirmation.value().outcome == MutationOutcome::NoChange);
  RC_CHECK_EQ(confirmation.value().sequence.value(), 1u);

  // A newer composition generation is adopted and both counter families move.
  RackCapacityInputs newer_composition = rctest::Bundle("rack-a1", 42)
                                             .composition_generation(2)
                                             .epoch(2)
                                             .request("req-0004")
                                             .build();
  const Result<MutationReceipt> adopted = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
      CapacityExpectation::of(first), newer_composition, TimestampNs::trusted(kNow + 3)});
  RC_REQUIRE_OK(adopted);
  RC_CHECK_EQ(adopted.value().composition_generation.value(), 2u);
  RC_CHECK_EQ(adopted.value().capacity_generation.value(), 2u);
  RC_CHECK_EQ(adopted.value().revision.value(), 2u);
  RC_CHECK_EQ(adopted.value().sequence.value(), 2u);

  const RackCapacityRecord second = fixture.catalog->record(rack).value();
  RC_CHECK_EQ(second.composition_generation.value(), 2u);

  // An older composition generation is refused as a regression.
  RackCapacityInputs older_composition = rctest::Bundle("rack-a1", 42)
                                             .composition_generation(1)
                                             .epoch(3)
                                             .request("req-0005")
                                             .build();
  RC_REQUIRE_CODE(fixture.catalog->apply_evidence(
                      ApplyEvidenceRequest{CapacityExpectation::of(second), older_composition,
                                           TimestampNs::trusted(kNow + 4)}),
                  ErrorCode::StaleCompositionGeneration);

  // Older evidence is refused as well.
  RackCapacityInputs older_epoch = rctest::Bundle("rack-a1", 42)
                                      .composition_generation(2)
                                      .epoch(1)
                                      .request("req-0006")
                                      .build();
  RC_REQUIRE_CODE(fixture.catalog->apply_evidence(
                      ApplyEvidenceRequest{CapacityExpectation::of(second), older_epoch,
                                           TimestampNs::trusted(kNow + 5)}),
                  ErrorCode::StaleEvidenceEpoch);

  // A refused mutation leaves the record exactly as it was.
  const RackCapacityRecord unchanged = fixture.catalog->record(rack).value();
  RC_CHECK_EQ(unchanged.capacity_generation.value(), 2u);
  RC_CHECK_EQ(unchanged.revision.value(), 2u);
  RC_CHECK_EQ(unchanged.composition_generation.value(), 2u);

  // A genuine advance is applied and the sequence moves by exactly one.
  RackCapacityInputs next = rctest::Bundle("rack-a1", 42)
                                .composition_generation(2)
                                .epoch(3)
                                .request("req-0007")
                                .asset_units("a-0001", 1, 1)
                                .asset_draw("a-0001", 2000)
                                .build();
  const Result<MutationReceipt> applied = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
      CapacityExpectation::of(unchanged), next, TimestampNs::trusted(kNow + 6)});
  RC_REQUIRE_OK(applied);
  RC_CHECK_EQ(applied.value().capacity_generation.value(), 3u);
  RC_CHECK_EQ(applied.value().revision.value(), 3u);
  RC_CHECK_EQ(applied.value().sequence.value(), 3u);

  // The same facts under the same epoch confirm the state again.
  const RackCapacityRecord advanced = fixture.catalog->record(rack).value();
  RackCapacityInputs confirm_again = next;
  confirm_again.request = RequestId::create("req-0008").value();
  const Result<MutationReceipt> no_change = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
      CapacityExpectation::of(advanced), confirm_again, TimestampNs::trusted(kNow + 7)});
  RC_REQUIRE_OK(no_change);
  RC_CHECK(no_change.value().outcome == MutationOutcome::NoChange);
  RC_CHECK_EQ(no_change.value().revision.value(), 3u);
  RC_CHECK_EQ(no_change.value().sequence.value(), 3u);
}
RC_TEST(catalog_replay_is_exactly_bounded) {
  rctest::CatalogFixture fixture("catalog-replay");
  const RackCapacityInputs inputs = input_for("rack-a1", 1, "req-0001");
  const Result<MutationReceipt> first =
      fixture.catalog->register_rack(RegisterRackRequest{inputs, TimestampNs::trusted(kNow)});
  RC_REQUIRE_OK(first);

  // The same request identity with the same facts is answered from the journal.
  const Result<MutationReceipt> replay =
      fixture.catalog->register_rack(RegisterRackRequest{inputs, TimestampNs::trusted(kNow + 1)});
  RC_REQUIRE_OK(replay);
  RC_CHECK(replay.value().outcome == MutationOutcome::Replayed);
  RC_CHECK_EQ(replay.value().snapshot_digest, first.value().snapshot_digest);
  RC_CHECK_EQ(replay.value().attempt.value(), first.value().attempt.value());
  RC_CHECK_EQ(fixture.catalog->rack_count(), 1u);

  // The same request identity with different facts is a conflict, not a replay.
  RackCapacityInputs different = rctest::Bundle("rack-a2", 42)
                                     .epoch(1)
                                     .request("req-0001")
                                     .asset_units("a-0009", 1, 1)
                                     .build();
  RC_REQUIRE_CODE(fixture.catalog->register_rack(
                      RegisterRackRequest{different, TimestampNs::trusted(kNow + 2)}),
                  ErrorCode::RequestIdConflict);
  RC_CHECK_EQ(fixture.catalog->rack_count(), 1u);

  // Every receipt survives a restart, so replay coverage is not lost.
  const std::string path = fixture.store_path();
  RC_REQUIRE(fixture.catalog->close().has_value());
  fixture.catalog.reset();
  const Result<std::unique_ptr<CapacityCatalog>> reopened = open_catalog(path);
  RC_REQUIRE_OK(reopened);
  const Result<MutationReceipt> after_restart = reopened.value()->register_rack(
      RegisterRackRequest{inputs, TimestampNs::trusted(kNow + 3)});
  RC_REQUIRE_OK(after_restart);
  RC_CHECK(after_restart.value().outcome == MutationOutcome::Replayed);
  RC_CHECK_EQ(reopened.value()->stats().idempotency_records, 1u);
}

RC_TEST(catalog_recovered_records_are_not_authoritative_until_revalidated) {
  rctest::CatalogFixture fixture("catalog-revalidate");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const std::string path = fixture.store_path();
  RC_REQUIRE(fixture.catalog->close().has_value());
  fixture.catalog.reset();

  const Result<std::unique_ptr<CapacityCatalog>> reopened = open_catalog(path);
  RC_REQUIRE_OK(reopened);
  const RackId rack = RackId::create("rack-a1").value();
  const RackCapacityRecord recovered = reopened.value()->record(rack).value();
  RC_CHECK(recovered.standing == RecoveryStanding::PendingRevalidation);
  RC_CHECK(!recovered.quotes_capacity());
  RC_REQUIRE_CODE(reopened.value()->snapshot(CapacityExpectation::of(recovered)),
                  ErrorCode::RevalidationRequired);

  // Revalidating against an instant two hours after the evidence was captured
  // reports it as stale, because the policy allows one hour.
  const std::int64_t later = kNow + 7'200'000'000'000ll;
  const Result<MutationReceipt> revalidated = reopened.value()->revalidate(
      RevalidateRequest{CapacityExpectation::of(recovered), TimestampNs::trusted(later), {},
                        RequestId::create("req-0002").value(), TimestampNs::trusted(later + 1)});
  RC_REQUIRE_OK(revalidated);
  RC_CHECK(revalidated.value().lifecycle == RackLifecycle::Active);
  const RackCapacityRecord after = reopened.value()->record(rack).value();
  RC_CHECK(after.standing == RecoveryStanding::Authoritative);
  RC_CHECK(after.quotes_capacity());
  RC_CHECK_EQ(after.snapshot.revision.value(), recovered.revision.value() + 1u);
  RC_CHECK(after.snapshot.freshness == EvidenceFreshness::Stale);

  const Result<RackCapacitySnapshot> quoted =
      reopened.value()->snapshot(CapacityExpectation::of(after));
  RC_REQUIRE_OK(quoted);

  // A second revalidation changes nothing.
  const Result<MutationReceipt> again = reopened.value()->revalidate(
      RevalidateRequest{CapacityExpectation::of(after), TimestampNs::trusted(later), {},
                        RequestId::create("req-0003").value(), TimestampNs::trusted(later + 2)});
  RC_REQUIRE_OK(again);
  RC_CHECK(again.value().outcome == MutationOutcome::NoChange);
}

RC_TEST(catalog_retirement_is_terminal_and_idempotent) {
  rctest::CatalogFixture fixture("catalog-retire");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const RackId rack = RackId::create("rack-a1").value();
  const RackCapacityRecord record = fixture.catalog->record(rack).value();

  RetireRackRequest retire;
  retire.expected = CapacityExpectation::of(record);
  retire.reason = "decommissioned";
  retire.request = RequestId::create("req-0002").value();
  retire.actor = ActorId::create("operator-2").value();
  retire.requested_at = TimestampNs::trusted(kNow + 1);
  const Result<MutationReceipt> retired = fixture.catalog->retire_rack(retire);
  RC_REQUIRE_OK(retired);
  RC_CHECK(retired.value().lifecycle == RackLifecycle::Retired);
  RC_CHECK(retired.value().detail.find("decommissioned") != std::string::npos);

  const RackCapacityRecord after = fixture.catalog->record(rack).value();
  RC_CHECK(after.lifecycle == RackLifecycle::Retired);
  RC_CHECK(!after.quotes_capacity());
  RC_REQUIRE_CODE(fixture.catalog->snapshot(CapacityExpectation::of(after)),
                  ErrorCode::RackRetired);

  // Evidence is refused for a retired rack.
  RC_REQUIRE_CODE(fixture.catalog->apply_evidence(
                      ApplyEvidenceRequest{CapacityExpectation::of(after),
                                           input_for("rack-a1", 4, "req-0003"),
                                           TimestampNs::trusted(kNow + 2)}),
                  ErrorCode::RackRetired);
  // Replaying the retirement request answers from the journal; a fresh
  // retirement request for an already retired rack is refused.
  RetireRackRequest replay = retire;
  replay.expected = CapacityExpectation::of(after);
  replay.requested_at = TimestampNs::trusted(kNow + 3);
  const Result<MutationReceipt> replayed = fixture.catalog->retire_rack(replay);
  RC_REQUIRE_OK(replayed);
  RC_CHECK(replayed.value().outcome == MutationOutcome::Replayed);

  RetireRackRequest second = retire;
  second.expected = CapacityExpectation::of(after);
  second.request = RequestId::create("req-0004").value();
  second.requested_at = TimestampNs::trusted(kNow + 4);
  RC_REQUIRE_CODE(fixture.catalog->retire_rack(second),
                  ErrorCode::LifecycleTransitionNotAllowed);
}

RC_TEST(catalog_diffs_the_retained_previous_publication) {
  rctest::CatalogFixture fixture("catalog-diff");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 42)
                                                         .epoch(1)
                                                         .request("req-0001")
                                                         .asset_units("a-0001", 1, 1)
                                                         .asset_draw("a-0001", 1000)
                                                         .build(),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const RackId rack = RackId::create("rack-a1").value();
  const RackCapacityRecord first = fixture.catalog->record(rack).value();

  const Result<MutationReceipt> applied = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
      CapacityExpectation::of(first),
      rctest::Bundle("rack-a1", 42)
          .epoch(2)
          .request("req-0002")
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 3000)
          .asset_units("a-0002", 3, 2)
          .asset_draw("a-0002", 2000)
          .build(),
      TimestampNs::trusted(kNow + 1)});
  RC_REQUIRE_OK(applied);

  // The comparison is against the publication that was authoritative when the
  // catalog was opened, so reopen to see the retained generation.
  RC_REQUIRE(fixture.catalog->close().has_value());
  fixture.catalog.reset();
  const Result<std::unique_ptr<CapacityCatalog>> reopened = open_catalog(fixture.store_path());
  RC_REQUIRE_OK(reopened);
  const Result<CapacityDiff> diff = reopened.value()->diff_with_previous(rack);
  RC_REQUIRE_OK(diff);
  RC_CHECK(!diff.value().identical);
  RC_CHECK_EQ(diff.value().from_revision.value(), 1u);
  RC_CHECK_EQ(diff.value().to_revision.value(), 2u);
  bool saw_committed = false;
  for (const FieldChange& change : diff.value().changes) {
    if (change.path == "power.committed_known") {
      saw_committed = true;
      RC_CHECK_EQ(change.before, std::string("1000"));
      RC_CHECK_EQ(change.after, std::string("5000"));
    }
  }
  RC_CHECK(saw_committed);
  RC_CHECK(diff.value().to_text().find("power.committed_known") != std::string::npos);

  const Result<CapacityDiff> missing =
      reopened.value()->diff_with_previous(RackId::create("rack-none").value());
  RC_REQUIRE_CODE(missing, ErrorCode::UnknownRackId);
}

RC_TEST(catalog_read_only_open_refuses_mutations) {
  rctest::CatalogFixture fixture("catalog-readonly");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const std::string path = fixture.store_path();
  RC_REQUIRE(fixture.catalog->close().has_value());
  fixture.catalog.reset();

  const Result<std::unique_ptr<CapacityCatalog>> read_only = open_catalog(path, true, false);
  RC_REQUIRE_OK(read_only);
  RC_CHECK(read_only.value()->storage().read_only);
  RC_CHECK(!read_only.value()->storage().holds_writer_authority);
  RC_CHECK_EQ(read_only.value()->rack_count(), 1u);
  const Result<MutationReceipt> refused = read_only.value()->register_rack(
      RegisterRackRequest{input_for("rack-a2", 1, "req-0002"), TimestampNs::trusted(kNow)});
  RC_REQUIRE_CODE(refused, ErrorCode::ReadOnlyStore);
}

RC_TEST(catalog_validates_input_before_it_looks_for_the_rack) {
  // Precedence: a malformed bundle is reported as malformed even when the rack
  // it names does not exist, because input validation is the first gate.
  rctest::CatalogFixture fixture("catalog-precedence");
  RackCapacityInputs malformed = rctest::Bundle("rack-missing", 42).build_unchecked();
  malformed.composition.unit_count = 0;
  const Result<MutationReceipt> refused =
      fixture.catalog->register_rack(RegisterRackRequest{malformed, TimestampNs::trusted(kNow)});
  RC_REQUIRE_CODE(refused, ErrorCode::InvalidRange);

  // A missing rack is reported as missing once the bundle itself is valid.
  const Result<MutationReceipt> unknown = fixture.catalog->apply_evidence(
      ApplyEvidenceRequest{CapacityExpectation{},
                           input_for("rack-missing", 1, "req-0001"),
                           TimestampNs::trusted(kNow)});
  RC_REQUIRE_CODE(unknown, ErrorCode::UnknownRackId);

  // A missing instant is refused before anything else.
  RC_REQUIRE_CODE(fixture.catalog->register_rack(
                      RegisterRackRequest{input_for("rack-a1", 1, "req-0002"),
                                          TimestampNs::trusted(0)}),
                  ErrorCode::InvalidTimestamp);
}

RC_TEST(catalog_stats_and_storage_are_reported) {
  rctest::CatalogFixture fixture("catalog-stats");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a2", 1, "req-0002"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const CatalogStats stats = fixture.catalog->stats();
  RC_CHECK_EQ(stats.rack_count, 2u);
  RC_CHECK_EQ(stats.active_racks, 2u);
  RC_CHECK_EQ(stats.retired_racks, 0u);
  RC_CHECK_EQ(stats.asset_count, 2u);
  RC_CHECK_EQ(stats.idempotency_records, 2u);

  const CatalogStorage storage = fixture.catalog->storage();
  RC_CHECK_EQ(storage.incarnation.size(), 32u);
  RC_CHECK_EQ(storage.epoch.value(), 1u);
  RC_CHECK_EQ(storage.sequence.value(), 2u);
  RC_CHECK(storage.holds_writer_authority);
  RC_CHECK(!storage.read_only);
  RC_CHECK(storage.writer_lock.writer_id == WriterId::create("test-writer").value());
  RC_CHECK(storage.writer_lock.state == WriterLockState::HeldByThisProcess);
  RC_CHECK(storage.to_text().find("incarnation") != std::string::npos);
  RC_CHECK(fixture.catalog->stats().to_text().find("active") != std::string::npos);
}

RC_TEST(catalog_fit_evaluation_resolves_the_precondition_itself) {
  rctest::CatalogFixture fixture("catalog-fit");
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  const RackCapacityRecord record =
      fixture.catalog->record(RackId::create("rack-a1").value()).value();

  FitRequest request;
  request.expected = CapacityExpectation::of(record);
  request.span = SlotInterval::for_units(5, 1).value();
  request.draw = Watts::trusted(100);
  const Result<FitEvaluation> evaluation = fixture.catalog->evaluate_fit(request);
  RC_REQUIRE_OK(evaluation);
  RC_CHECK(evaluation.value().verdict == FitVerdict::Fits);

  request.expected.capacity_generation = CapacityGeneration::trusted(11);
  RC_REQUIRE_CODE(fixture.catalog->evaluate_fit(request), ErrorCode::StaleCapacityGeneration);
}
