// Rack Capacity - independent downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is built out of tree against an installed Rack Capacity package.
// It exercises a real minimal lifecycle end to end: create a store, register a
// rack from an evidence bundle, quote capacity, evaluate a candidate, publish a
// change, release the store, open it again from scratch, revalidate the
// recovered record and quote it once more. If any of that needed something the
// package does not export, this program would not build.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

int g_failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cout << "FAIL " << what << "\n";
    ++g_failures;
  }
}

[[nodiscard]] RackCapacityInputs bundle(std::uint64_t epoch, const std::string& request,
                                        std::int64_t draw) {
  RackCapacityInputs inputs;
  inputs.composition.rack = RackId::create("rack-downstream").value();
  inputs.composition.generation = RackCompositionGeneration::trusted(1);
  inputs.composition.unit_count = 24;
  inputs.composition.serviceability.front_clearance = Millimetres::trusted(900);
  inputs.composition.reference.source = EvidenceSource::RackRegistry;
  inputs.composition.reference.evidence = EvidenceId::create("ev-composition").value();
  inputs.composition.reference.version = 1;
  inputs.composition.reference.observed_at = TimestampNs::trusted(kNow);

  inputs.policy.reference.policy = PolicyId::create("downstream-policy").value();
  inputs.policy.reference.version = 1;

  PowerCapacityEvidence power;
  power.feed_count = 2;
  power.watts_per_feed = Watts::trusted(8000);
  power.redundancy = RedundancyMode::N2;
  power.reference.source = EvidenceSource::PowerCapacity;
  power.reference.evidence = EvidenceId::create("ev-power").value();
  power.reference.version = 1;
  power.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.power = power;

  AssetOccupancyEvidence asset;
  asset.asset = AssetId::create("a-0001").value();
  asset.span = SlotSet::parse("[1,3)").value().intervals().front();
  asset.presence = PresenceState::Present;
  asset.nameplate_draw = Watts::trusted(draw);
  asset.reference.source = EvidenceSource::AssetRegistry;
  asset.reference.evidence = EvidenceId::create("ev-a-0001").value();
  asset.reference.version = 1;
  asset.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.assets.push_back(asset);

  inputs.epoch = EvidenceEpoch::trusted(epoch);
  inputs.captured_at = TimestampNs::trusted(kNow);
  inputs.actor = ActorId::create("downstream-actor").value();
  inputs.request = RequestId::create(request).value();
  return inputs;
}

}  // namespace

int main() {
  std::cout << "Rack Capacity " << version_string() << " consumed from an installed package\n";

  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(error) / "rcap-downstream";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::string path = (directory / "capacity.rcstate").string();

  {
    CatalogOptions options;
    options.path = path;
    options.writer_id = WriterId::create("downstream-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
    check(catalog.has_value(), "the catalog opens");
    if (!catalog.has_value()) {
      std::cout << describe(catalog.error()) << "\n";
      return 1;
    }

    const Result<MutationReceipt> registered = catalog.value()->register_rack(
        RegisterRackRequest{bundle(1, "req-0001", 1200), TimestampNs::trusted(kNow)});
    check(registered.has_value(), "a rack registers");
    if (!registered.has_value()) {
      std::cout << describe(registered.error()) << "\n";
      return 1;
    }

    const RackId rack = RackId::create("rack-downstream").value();
    const RackCapacityRecord record = catalog.value()->record(rack).value();
    const Result<RackCapacitySnapshot> snapshot =
        catalog.value()->snapshot(CapacityExpectation::of(record));
    check(snapshot.has_value(), "capacity is quoted");
    if (snapshot.has_value()) {
      check(snapshot.value().power.usable.has_value(), "the envelope is known");
      check(snapshot.value().power.free.is_exact(), "the free power is exact");
      check(snapshot.value().slots.free.lower() == 46u, "the free slot count is right");
      check(verify_closure(snapshot.value()).holds, "the snapshot closes exactly");
    }

    FitRequest fit;
    fit.expected = CapacityExpectation::of(record);
    fit.unit_height = 1;
    fit.draw = Watts::trusted(500);
    const Result<FitEvaluation> evaluation = catalog.value()->evaluate_fit(fit);
    check(evaluation.has_value(), "a fit is evaluated");
    if (evaluation.has_value()) {
      check(evaluation.value().verdict == FitVerdict::Fits, "the candidate fits");
      check(!evaluation.value().placement_authority, "no placement authority is claimed");
    }

    const Result<MutationReceipt> applied = catalog.value()->apply_evidence(ApplyEvidenceRequest{
        CapacityExpectation::of(record), bundle(2, "req-0002", 2600),
        TimestampNs::trusted(kNow + 1)});
    check(applied.has_value(), "a change publishes");
    check(catalog.value()->storage().sequence.value() == 2u, "the store sequence advanced");
    (void)catalog.value()->close();
  }

  {
    CatalogOptions options;
    options.path = path;
    options.writer_id = WriterId::create("downstream-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
    check(catalog.has_value(), "the store reopens in a new object graph");
    if (!catalog.has_value()) {
      std::cout << describe(catalog.error()) << "\n";
      return 1;
    }
    check(catalog.value()->rack_count() == 1u, "the published rack is still there");
    const RackId rack = RackId::create("rack-downstream").value();
    const RackCapacityRecord recovered = catalog.value()->record(rack).value();
    check(recovered.standing == RecoveryStanding::PendingRevalidation,
          "a recovered record is not authoritative yet");

    const Result<RackCapacitySnapshot> refused =
        catalog.value()->snapshot(CapacityExpectation::of(recovered));
    check(!refused.has_value(), "quoting a recovered record is refused");
    check(refused.error().code == ErrorCode::RevalidationRequired, "the refusal is typed");

    const Result<InspectedSnapshot> inspected = catalog.value()->inspect_snapshot(
        CapacityExpectation::of(recovered), TimestampNs::trusted(kNow + 2));
    check(inspected.has_value(), "the record can be inspected without being promoted");
    if (inspected.has_value()) {
      check(inspected.value().verdict != RevalidationVerdict::Diverged,
            "the recovered record reproduces from its own evidence");
    }

    const Result<MutationReceipt> promoted = catalog.value()->revalidate(RevalidateRequest{
        CapacityExpectation::of(recovered), TimestampNs::trusted(kNow + 3),
        ActorId::create("downstream-actor").value(), RequestId::create("req-0003").value(),
        TimestampNs::trusted(kNow + 3)});
    check(promoted.has_value(), "revalidation promotes the record");
    const RackCapacityRecord promoted_record = catalog.value()->record(rack).value();
    check(promoted_record.quotes_capacity(), "the promoted record quotes capacity");
    (void)catalog.value()->close();
  }

  std::filesystem::remove_all(directory, error);

  if (g_failures != 0) {
    std::cout << g_failures << " downstream check(s) failed\n";
    return 1;
  }
  std::cout << "downstream consumer: all checks passed\n";
  return 0;
}
