// Rack Capacity - example: evaluating a candidate without placing it.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The point of this example is the three-valued verdict. A fit evaluation can
// answer "fits", "does not fit", or "cannot be decided from this evidence", and
// the third answer is not a failure: it is the honest result when a rack holds
// an occupant whose consumption is unknown.

#include <cstdint>
#include <iostream>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

[[nodiscard]] RackCapacityInputs build_rack(bool with_unknown_occupant) {
  RackCapacityInputs inputs;
  inputs.composition.rack = RackId::create("rack-b7").value();
  inputs.composition.generation = RackCompositionGeneration::trusted(2);
  inputs.composition.unit_count = 24;
  inputs.composition.reference.source = EvidenceSource::RackRegistry;
  inputs.composition.reference.evidence = EvidenceId::create("ev-composition").value();
  inputs.composition.reference.version = 2;
  inputs.composition.reference.observed_at = TimestampNs::trusted(kNow);

  inputs.policy.reference.policy = PolicyId::create("standard-2026").value();
  inputs.policy.reference.version = 1;

  PowerCapacityEvidence power;
  power.feed_count = 2;
  power.watts_per_feed = Watts::trusted(4000);
  power.redundancy = RedundancyMode::N2;
  power.reference.source = EvidenceSource::PowerCapacity;
  power.reference.evidence = EvidenceId::create("ev-power").value();
  power.reference.version = 1;
  power.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.power = power;

  AssetOccupancyEvidence installed;
  installed.asset = AssetId::create("a-0001").value();
  installed.span = SlotSet::parse("[1,3)").value().intervals().front();
  installed.presence = PresenceState::Present;
  installed.nameplate_draw = Watts::trusted(1000);
  installed.reference.source = EvidenceSource::AssetRegistry;
  installed.reference.evidence = EvidenceId::create("ev-a-0001").value();
  installed.reference.version = 1;
  installed.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.assets.push_back(installed);

  if (with_unknown_occupant) {
    // Something is mounted, but nothing is known about what it draws.
    AssetOccupancyEvidence uncertain;
    uncertain.asset = AssetId::create("a-0002").value();
    uncertain.span = SlotSet::parse("[5,7)").value().intervals().front();
    uncertain.presence = PresenceState::Present;
    uncertain.reference.source = EvidenceSource::AssetRegistry;
    uncertain.reference.evidence = EvidenceId::create("ev-a-0002").value();
    uncertain.reference.version = 1;
    uncertain.reference.observed_at = TimestampNs::trusted(kNow);
    inputs.assets.push_back(uncertain);
  }

  inputs.epoch = EvidenceEpoch::trusted(4);
  inputs.captured_at = TimestampNs::trusted(kNow);
  inputs.actor = ActorId::create("planner-1").value();
  inputs.request = RequestId::create("req-planner-1").value();
  return inputs;
}

void probe(const RackCapacitySnapshot& snapshot, const char* label, std::int64_t watts,
           std::uint32_t first_unit) {
  FitRequest request;
  request.expected = CapacityExpectation::of(snapshot);
  request.span = SlotInterval::for_units(first_unit, 1).value();
  request.draw = Watts::trusted(watts);
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot);
  std::cout << label << " (" << watts << " W at " << request.span->to_text() << "): "
            << fit_verdict_name(evaluation.value().verdict);
  if (evaluation.value().verdict != FitVerdict::Fits) {
    std::cout << " because " << dimension_name(evaluation.value().binding_constraint);
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  for (const bool with_unknown : {false, true}) {
    const RackCapacityInputs inputs = build_rack(with_unknown);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    if (!snapshot.has_value()) {
      std::cout << "evaluation failed: " << describe(snapshot.error()) << "\n";
      return 1;
    }
    std::cout << (with_unknown ? "with an occupant whose draw is unknown\n"
                               : "with every occupant fully described\n");
    if (snapshot.value().power.free.is_known()) {
      std::cout << "  free power is [" << snapshot.value().power.free.lower().value() << ", "
                << snapshot.value().power.free.upper().value() << "] W\n";
    }
    probe(snapshot.value(), "small candidate ", 500, 9);
    probe(snapshot.value(), "large candidate ", 9000, 9);
    probe(snapshot.value(), "occupied slot   ", 500, 1);
    std::cout << "\n";
  }

  // A full report for the undecided case, so the reasoning is visible.
  const RackCapacityInputs inputs = build_rack(true);
  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
      TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
  FitRequest request;
  request.expected = CapacityExpectation::of(snapshot.value());
  request.unit_height = 1;
  request.draw = Watts::trusted(2500);
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
  std::cout << evaluation.value().to_text();
  return 0;
}
