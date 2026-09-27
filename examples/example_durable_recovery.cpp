// Rack Capacity - example: durable capacity state across a real restart.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This example writes capacity state to a real store, releases it, opens it
// again from scratch, and shows the two rules that make recovered state safe:
// a recovered record is not authoritative until it has been revalidated, and
// the difference between two published generations is a first-class value.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

[[nodiscard]] std::string temporary_directory() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::filesystem::path path =
      (error ? std::filesystem::path(".") : base) / "rcap-example-durable";
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

[[nodiscard]] RackCapacityInputs bundle(std::uint64_t epoch, const std::string& request,
                                        std::int64_t first_draw, bool add_second) {
  RackCapacityInputs inputs;
  inputs.composition.rack = RackId::create("rack-a1").value();
  inputs.composition.generation = RackCompositionGeneration::trusted(1);
  inputs.composition.unit_count = 42;
  inputs.composition.reference.source = EvidenceSource::RackRegistry;
  inputs.composition.reference.evidence = EvidenceId::create("ev-composition").value();
  inputs.composition.reference.version = 1;
  inputs.composition.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.policy.reference.policy = PolicyId::create("standard-2026").value();
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

  AssetOccupancyEvidence readme;
  readme.asset = AssetId::create("a-0001").value();
  readme.span = SlotSet::parse("[1,3)").value().intervals().front();
  readme.presence = PresenceState::Present;
  readme.nameplate_draw = Watts::trusted(first_draw);
  readme.reference.source = EvidenceSource::AssetRegistry;
  readme.reference.evidence = EvidenceId::create("ev-a-0001").value();
  readme.reference.version = 2;
  readme.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.assets.push_back(readme);

  if (add_second) {
    AssetOccupancyEvidence second;
    second.asset = AssetId::create("a-0002").value();
    second.span = SlotSet::parse("[9,13)").value().intervals().front();
    second.presence = PresenceState::Present;
    second.nameplate_draw = Watts::trusted(2400);
    second.reference.source = EvidenceSource::AssetRegistry;
    second.reference.evidence = EvidenceId::create("ev-a-0002").value();
    second.reference.version = 1;
    second.reference.observed_at = TimestampNs::trusted(kNow);
    inputs.assets.push_back(second);
  }

  inputs.epoch = EvidenceEpoch::trusted(epoch);
  inputs.captured_at = TimestampNs::trusted(kNow);
  inputs.actor = ActorId::create("operator-1").value();
  inputs.request = RequestId::create(request).value();
  return inputs;
}

[[nodiscard]] Result<std::unique_ptr<CapacityCatalog>> open_catalog(const std::string& path) {
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("example-writer").value();
  return CapacityCatalog::open(options);
}

}  // namespace

int main() {
  const std::string directory = temporary_directory();
  const std::string store_path = (std::filesystem::path(directory) / "capacity.rcstate").string();

  std::cout << "store: " << store_path << "\n\n";

  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = open_catalog(store_path);
    if (!catalog.has_value()) {
      std::cout << "open failed: " << describe(catalog.error()) << "\n";
      return 1;
    }
    const Result<MutationReceipt> registered = catalog.value()->register_rack(
        RegisterRackRequest{bundle(1, "req-0001", 1200, false), TimestampNs::trusted(kNow)});
    if (!registered.has_value()) {
      std::cout << "register failed: " << describe(registered.error()) << "\n";
      return 1;
    }
    std::cout << registered.value().to_text() << "\n";

    const RackCapacityRecord record =
        catalog.value()->record(RackId::create("rack-a1").value()).value();
    const Result<MutationReceipt> applied = catalog.value()->apply_evidence(ApplyEvidenceRequest{
        CapacityExpectation::of(record), bundle(2, "req-0002", 3100, true),
        TimestampNs::trusted(kNow + 1)});
    if (!applied.has_value()) {
      std::cout << "apply failed: " << describe(applied.error()) << "\n";
      return 1;
    }
    std::cout << applied.value().to_text() << "\n";
    std::cout << catalog.value()->storage().to_text() << "\n";
    (void)catalog.value()->close();
  }

  std::cout << "--- the process forgets everything and opens the store again ---\n\n";

  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = open_catalog(store_path);
    if (!catalog.has_value()) {
      std::cout << "reopen failed: " << describe(catalog.error()) << "\n";
      return 1;
    }
    const RackId rack = RackId::create("rack-a1").value();
    const RackCapacityRecord recovered = catalog.value()->record(rack).value();
    std::cout << "recovered standing: " << recovery_standing_name(recovered.standing) << "\n";

    // Quoting capacity from a recovered record is refused until it has been
    // revalidated, so persisted evidence never silently becomes current.
    const Result<RackCapacitySnapshot> refused =
        catalog.value()->snapshot(CapacityExpectation::of(recovered));
    std::cout << "quoting it directly: " << describe(refused.error()) << "\n";

    // The inspection path verifies the record against its own evidence without
    // publishing anything.
    const Result<InspectedSnapshot> inspected = catalog.value()->inspect_snapshot(
        CapacityExpectation::of(recovered), TimestampNs::trusted(kNow + 2));
    if (!inspected.has_value()) {
      std::cout << "inspection failed: " << describe(inspected.error()) << "\n";
      return 1;
    }
    std::cout << "inspected verdict: " << revalidation_verdict_name(inspected.value().verdict)
              << ", freshness " << evidence_freshness_name(inspected.value().freshness) << "\n";

    // Promotion is an explicit mutation that publishes a new revision.
    const Result<MutationReceipt> promoted = catalog.value()->revalidate(
        RevalidateRequest{CapacityExpectation::of(recovered), TimestampNs::trusted(kNow + 3),
                          ActorId::create("operator-1").value(),
                          RequestId::create("req-0003").value(), TimestampNs::trusted(kNow + 3)});
    if (!promoted.has_value()) {
      std::cout << "revalidation failed: " << describe(promoted.error()) << "\n";
      return 1;
    }
    std::cout << "\n" << promoted.value().to_text() << "\n";

    const RackCapacityRecord current = catalog.value()->record(rack).value();
    const Result<RackCapacitySnapshot> quoted =
        catalog.value()->snapshot(CapacityExpectation::of(current));
    if (!quoted.has_value()) {
      std::cout << "quoting failed: " << describe(quoted.error()) << "\n";
      return 1;
    }
    std::cout << "free power after revalidation: "
              << quoted.value().power.free.lower().value() << " W\n";
    (void)catalog.value()->close();
  }

  std::cout << "--- the difference between the two published generations ---\n\n";

  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = open_catalog(store_path);
    if (!catalog.has_value()) {
      std::cout << "reopen failed: " << describe(catalog.error()) << "\n";
      return 1;
    }
    const Result<CapacityDiff> diff =
        catalog.value()->diff_with_previous(RackId::create("rack-a1").value());
    if (!diff.has_value()) {
      std::cout << "diff failed: " << describe(diff.error()) << "\n";
      return 1;
    }
    std::cout << diff.value().to_text();
    (void)catalog.value()->close();
  }

  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::cout << "\nresidue removed: " << (error ? error.message() : "yes") << "\n";
  return 0;
}
