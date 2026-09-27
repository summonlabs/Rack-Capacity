// Rack Capacity - example: reading one rack's remaining capacity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This example builds one rack's evidence bundle in memory and reads the
// capacity back. It deliberately includes an occupant whose power draw is not
// known, because that is the case where naive accounting reports free capacity
// that does not exist.

#include <cstdint>
#include <iostream>
#include <string>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

[[nodiscard]] RackCapacityInputs build_rack() {
  RackCapacityInputs inputs;
  inputs.composition.rack = RackId::create("rack-a1").value();
  inputs.composition.generation = RackCompositionGeneration::trusted(4);
  inputs.composition.unit_count = 48;
  inputs.composition.structural_reserved_slots = SlotSet::parse("[95,97)").value();
  inputs.composition.serviceability.front_clearance = Millimetres::trusted(900);
  inputs.composition.serviceability.rear_clearance = Millimetres::trusted(700);
  inputs.composition.serviceability.service_height_limit_unit = 42;
  inputs.composition.reference.source = EvidenceSource::RackRegistry;
  inputs.composition.reference.evidence = EvidenceId::create("ev-composition").value();
  inputs.composition.reference.version = 11;
  inputs.composition.reference.observed_at = TimestampNs::trusted(kNow);

  inputs.policy.reference.policy = PolicyId::create("standard-2026").value();
  inputs.policy.reference.version = 3;
  inputs.policy.headroom.power_headroom = Watts::trusted(500);
  inputs.policy.headroom.slot_headroom = 2;
  inputs.policy.headroom.power_derate = BasisPoints::trusted(500);
  inputs.policy.freshness.max_envelope_age = DurationNs::trusted(3'600'000'000'000ll);

  PowerCapacityEvidence power;
  power.feed_count = 2;
  power.watts_per_feed = Watts::trusted(8000);
  power.redundancy = RedundancyMode::N2;
  power.derate = BasisPoints::trusted(2000);
  power.reference.source = EvidenceSource::PowerCapacity;
  power.reference.evidence = EvidenceId::create("ev-power").value();
  power.reference.version = 7;
  power.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.power = power;

  CoolingCapacityEvidence cooling;
  cooling.nominal_heat_rejection = Watts::trusted(20000);
  cooling.derate = BasisPoints::trusted(1000);
  cooling.reference.source = EvidenceSource::CoolingCapacity;
  cooling.reference.evidence = EvidenceId::create("ev-cooling").value();
  cooling.reference.version = 3;
  cooling.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.cooling = cooling;

  WeightCapacityEvidence weight;
  weight.static_load_limit = Grams::trusted(900'000);
  weight.per_unit_point_load_limit = Grams::trusted(250'000);
  weight.reference.source = EvidenceSource::CoolingCapacity;
  weight.reference.evidence = EvidenceId::create("ev-weight").value();
  weight.reference.version = 2;
  weight.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.weight = weight;

  const auto occupant = [&inputs](const char* id, const char* span, std::int64_t draw,
                                  std::int64_t heat, std::int64_t mass, std::uint32_t version) {
    AssetOccupancyEvidence asset;
    asset.asset = AssetId::create(id).value();
    asset.span = SlotSet::parse(span).value().intervals().front();
    asset.presence = PresenceState::Present;
    asset.nameplate_draw = Watts::trusted(draw);
    asset.declared_heat_rejection = Watts::trusted(heat);
    asset.mass = Grams::trusted(mass);
    asset.reference.source = EvidenceSource::AssetRegistry;
    asset.reference.evidence = EvidenceId::create(std::string("ev-") + id).value();
    asset.reference.version = version;
    asset.reference.observed_at = TimestampNs::trusted(kNow);
    inputs.assets.push_back(asset);
  };
  occupant("a-0001", "[1,5)", 1200, 1200, 32'000, 5);
  occupant("a-0002", "[21,29)", 900, 900, 41'000, 6);

  // One occupant whose nameplate draw is not known. Nothing about it may be
  // assumed, and the capacity report must say so.
  AssetOccupancyEvidence unknown;
  unknown.asset = AssetId::create("a-0003").value();
  unknown.span = SlotSet::parse("[41,45)").value().intervals().front();
  unknown.presence = PresenceState::Present;
  unknown.mass = Grams::trusted(28'000);
  unknown.reference.source = EvidenceSource::AssetRegistry;
  unknown.reference.evidence = EvidenceId::create("ev-a-0003").value();
  unknown.reference.version = 1;
  unknown.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.assets.push_back(unknown);

  ReservationEvidence reservation;
  reservation.reservation = ReservationId::create("r-0001").value();
  reservation.state = ReservationState::Committed;
  reservation.span = SlotSet::parse("[51,53)").value().intervals().front();
  reservation.draw = Watts::trusted(500);
  reservation.heat_rejection = Watts::trusted(500);
  reservation.mass = Grams::trusted(9000);
  reservation.reference.source = EvidenceSource::FacilityCapacityReservation;
  reservation.reference.evidence = EvidenceId::create("ev-r-0001").value();
  reservation.reference.version = 2;
  reservation.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.reservations.push_back(reservation);

  inputs.epoch = EvidenceEpoch::trusted(12);
  inputs.captured_at = TimestampNs::trusted(kNow);
  inputs.actor = ActorId::create("operator-1").value();
  inputs.request = RequestId::create("req-0001").value();
  return inputs;
}

}  // namespace

int main() {
  const RackCapacityInputs inputs = build_rack();
  const Status valid = validate_inputs(inputs);
  if (!valid.has_value()) {
    std::cout << "the bundle is not valid: " << describe(valid.error()) << "\n";
    return 1;
  }

  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
      TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
  if (!snapshot.has_value()) {
    std::cout << "evaluation failed: " << describe(snapshot.error()) << "\n";
    return 1;
  }

  std::cout << snapshot.value().to_text();

  const ClosureReport closure = verify_closure(snapshot.value());
  std::cout << "\n" << closure.to_text();

  const RackCapacitySnapshot& value = snapshot.value();
  std::cout << "\nsummary\n";
  std::cout << "  slots free (conservative) : " << value.slots.free.lower() << "\n";
  std::cout << "  slots free (optimistic)   : " << value.slots.free.upper() << "\n";
  std::cout << "  power free                : "
            << (value.power.free.is_known()
                    ? std::to_string(value.power.free.lower().value()) + ".." +
                          std::to_string(value.power.free.upper().value()) + " W"
                    : std::string("unknown"))
            << "\n";
  std::cout << "  the constraint that becomes binding first is "
            << dimension_name(value.primary_binding) << "\n";
  return 0;
}
