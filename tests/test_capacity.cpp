// Rack Capacity - capacity evaluation tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <iostream>
#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

Result<RackCapacitySnapshot> evaluate(const RackCapacityInputs& inputs,
                                      std::int64_t now = kNow) {
  return evaluate_capacity(inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
                           TimestampNs::trusted(now), EvidenceFreshness::Fresh);
}

}  // namespace

RC_TEST(empty_rack_reports_every_slot_free) {
  const Result<RackCapacitySnapshot> snapshot = evaluate(rctest::Bundle("rack-a1", 42).build());
  RC_REQUIRE_OK(snapshot);
  const SlotAccounting& slots = snapshot.value().slots;
  RC_CHECK_EQ(slots.total_slots, 84u);
  RC_CHECK_EQ(slots.structural_reserved_slots, 0u);
  RC_CHECK_EQ(slots.occupied_slots, 0u);
  RC_CHECK_EQ(slots.reserved_slots, 0u);
  RC_CHECK_EQ(slots.pending_slots, 0u);
  RC_CHECK_EQ(slots.indeterminate_slots, 0u);
  RC_CHECK_EQ(slots.free.lower(), 84u);
  RC_CHECK_EQ(slots.free.upper(), 84u);
  RC_CHECK(slots.free.is_exact());
  RC_CHECK_EQ(slots.fragmentation.free_run_count, 1u);
  RC_CHECK_EQ(slots.fragmentation.largest_free_run_slots, 84u);
  RC_CHECK(slots.free_lower_set.contains_slot(1));
  RC_CHECK(slots.free_lower_set.contains_slot(84));
  RC_CHECK_EQ(snapshot.value().primary_binding, Dimension::Slot);
  RC_CHECK(!snapshot.value().binding);
}

RC_TEST(occupied_reserved_and_submitted_are_accounted_separately) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .structural_reserved("[83,85)")
          .asset_units("a-0001", 1, 2)              // slots [1,5)
          .asset_units("a-0002", 11, 1)             // slots [21,23)
          .asset("a-0003", "[31,33)")               // one slot at 31
          .asset_presence("a-0003", PresenceState::Indeterminate)
          .reservation("r-0001", ReservationState::Committed)
          .reservation_span("r-0001", "[41,45)")
          .reservation("r-0002", ReservationState::Submitted)
          .reservation_span("r-0002", "[51,55)")
          .reservation("r-0003", ReservationState::Released)
          .reservation_span("r-0003", "[61,65)")
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const SlotAccounting& slots = snapshot.value().slots;
  RC_CHECK_EQ(slots.total_slots, 84u);
  RC_CHECK_EQ(slots.structural_reserved_slots, 2u);
  RC_CHECK_EQ(slots.occupied_slots, 6u);
  RC_CHECK_EQ(slots.indeterminate_slots, 2u);
  RC_CHECK_EQ(slots.reserved_slots, 4u);
  RC_CHECK_EQ(slots.pending_slots, 4u);
  // 84 - 2 structural - 6 occupied - 2 indeterminate - 4 reserved = 70.
  RC_CHECK_EQ(slots.free.lower(), 70u);
  RC_CHECK_EQ(slots.free.upper(), 72u);
  RC_CHECK(slots.free_lower_set.contains_slot(6));
  RC_CHECK(!slots.free_lower_set.contains_slot(31));
  RC_CHECK(slots.free_upper_set.contains_slot(31));
  RC_CHECK(!slots.free_upper_set.contains_slot(21));
  RC_CHECK(snapshot.value().has_code(ReasonCode::SlotPendingSubmitted));
  RC_CHECK(snapshot.value().has_code(ReasonCode::SlotIndeterminatePresence));
  RC_CHECK(snapshot.value().unknown.indeterminate_occupancy);
  RC_CHECK(snapshot.value().has_code(ReasonCode::SlotBlockedByStructure));
}

RC_TEST(absent_occupants_consume_nothing) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .asset_units("a-0001", 1, 2)
                                        .asset_absent("a-0001")
                                        .asset_draw("a-0001", 1200)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().slots.occupied_slots, 0u);
  RC_CHECK_EQ(snapshot.value().slots.free.lower(), 84u);
  RC_CHECK_EQ(snapshot.value().power.committed_known.value(), 0);
}

RC_TEST(policy_slot_headroom_is_taken_from_the_top) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42).slot_headroom(4).build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const SlotAccounting& slots = snapshot.value().slots;
  RC_CHECK_EQ(slots.policy_headroom_slots, 4u);
  RC_CHECK_EQ(slots.free.lower(), 80u);
  RC_CHECK_EQ(slots.free.upper(), 80u);
  RC_CHECK(slots.policy_headroom_set.contains_slot(84));
  RC_CHECK(slots.policy_headroom_set.contains_slot(81));
  RC_CHECK(!slots.free_lower_set.contains_slot(81));
  RC_CHECK(slots.free_lower_set.contains_slot(80));
  RC_CHECK(snapshot.value().has_code(ReasonCode::SlotPolicyHeadroomHeld));
}

RC_TEST(power_derating_headroom_and_redundancy_are_reported_separately) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 2000)
          .power_policy_derate(500)
          .power_headroom(500)
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const PowerAccounting& power = snapshot.value().power;
  RC_CHECK(power.envelope_known);
  RC_CHECK_EQ(power.contributing_feeds, 1u);
  RC_CHECK_EQ(power.effective_nominal->value(), 8000);
  RC_CHECK_EQ(power.physically_derated->value(), 6400);
  // 6400 less a further 5 percent policy derate is 6080, less 500 W headroom.
  RC_CHECK_EQ(power.usable->value(), 5580);
  RC_CHECK_EQ(power.policy_headroom.value(), 500);
  RC_CHECK_EQ(power.free.lower().value(), 5580);
  RC_CHECK_EQ(power.free.upper().value(), 5580);
  RC_CHECK(power.free.is_exact());
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerRedundancyReductionApplied));
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerPhysicalDerateApplied));
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerPolicyDerateApplied));
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerPolicyHeadroomApplied));

  const RackCapacityInputs no_redundancy =
      rctest::Bundle("rack-a1", 42).power(2, 8000, RedundancyMode::None, 0).build();
  const Result<RackCapacitySnapshot> other = evaluate(no_redundancy);
  RC_REQUIRE_OK(other);
  RC_CHECK_EQ(other.value().power.contributing_feeds, 2u);
  RC_CHECK_EQ(other.value().power.effective_nominal->value(), 16000);

  const RackCapacityInputs n1 = rctest::Bundle("rack-a1", 42)
                                    .power(4, 8000, RedundancyMode::N1, 0)
                                    .build();
  const Result<RackCapacitySnapshot> n1_snapshot = evaluate(n1);
  RC_REQUIRE_OK(n1_snapshot);
  RC_CHECK_EQ(n1_snapshot.value().power.contributing_feeds, 3u);
  RC_CHECK_EQ(n1_snapshot.value().power.effective_nominal->value(), 24000);
}

RC_TEST(unknown_power_evidence_never_becomes_free_capacity) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42).no_power().build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const PowerAccounting& power = snapshot.value().power;
  RC_CHECK(!power.envelope_known);
  RC_CHECK(!power.free.is_known());
  RC_CHECK(!power.usable.has_value());
  RC_CHECK(snapshot.value().unknown.power_envelope);
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerEnvelopeUnknown));
  RC_CHECK_EQ(snapshot.value().primary_binding, Dimension::Slot);
}

RC_TEST(unknown_consumption_shrinks_free_capacity_to_a_range) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .power(2, 8000, RedundancyMode::N2, 0)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_draw("a-0001", 1000)
                                        .asset_units("a-0002", 2, 1)
                                        .build();  // draw unknown
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const PowerAccounting& power = snapshot.value().power;
  RC_CHECK(power.free.is_known());
  RC_CHECK_EQ(power.free.lower().value(), 0);
  RC_CHECK_EQ(power.free.upper().value(), 7000);
  RC_CHECK_EQ(power.unknown_draw_assets, 1u);
  RC_CHECK_EQ(power.committed_known.value(), 1000);
  RC_CHECK(snapshot.value().unknown.unknown_power_consumption);
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerUnknownConsumption));
  RC_CHECK(snapshot.value().binding == false);
}

RC_TEST(indeterminate_presence_makes_consumption_unknown) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_presence("a-0001", PresenceState::Indeterminate)
                                        .asset_draw("a-0001", 900)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().power.unknown_draw_assets, 1u);
  RC_CHECK_EQ(snapshot.value().power.committed_known.value(), 0);
  RC_CHECK_EQ(snapshot.value().power.free.lower().value(), 0);
}

RC_TEST(measured_power_is_an_observation_and_never_adds_capacity) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 0)
          .power_measurement(9000, kNow - 1000)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const PowerAccounting& power = snapshot.value().power;
  RC_CHECK(power.measured.has_value());
  RC_CHECK_EQ(power.measured->value(), 9000);
  RC_CHECK(power.measurement_standing == MeasurementStanding::Fresh);
  // The metered value exceeds declared load, which is reported, and the free
  // capacity still comes from declared committed load only.
  RC_CHECK_EQ(power.free.lower().value(), 7000);
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerMeasurementExceedsCommitted));

  const RackCapacityInputs stale =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 0)
          .power_measurement(500, kNow - 400'000'000'000ll)
          .build();
  const Result<RackCapacitySnapshot> stale_snapshot = evaluate(stale);
  RC_REQUIRE_OK(stale_snapshot);
  RC_CHECK(stale_snapshot.value().power.measurement_standing == MeasurementStanding::Stale);
  RC_CHECK(stale_snapshot.value().has_code(ReasonCode::PowerMeasurementStale));
}

RC_TEST(cooling_derives_heat_only_when_policy_declares_it) {
  const RackCapacityInputs without_policy =
      rctest::Bundle("rack-a1", 42)
          .cooling(20000, 0)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .build();
  const Result<RackCapacitySnapshot> first = evaluate(without_policy);
  RC_REQUIRE_OK(first);
  RC_CHECK_EQ(first.value().cooling.derived_heat_known.value(), 0);
  RC_CHECK_EQ(first.value().cooling.unknown_heat_assets, 1u);
  RC_CHECK_EQ(first.value().cooling.free.lower().value(), 0);

  const RackCapacityInputs with_policy =
      rctest::Bundle("rack-a1", 42)
          .cooling(20000, 0)
          .heat_equivalence(1'000'000)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .build();
  const Result<RackCapacitySnapshot> second = evaluate(with_policy);
  RC_REQUIRE_OK(second);
  RC_CHECK_EQ(second.value().cooling.derived_heat_known.value(), 1000);
  RC_CHECK_EQ(second.value().cooling.unknown_heat_assets, 0u);
  RC_CHECK_EQ(second.value().cooling.free.lower().value(), 19000);
  RC_CHECK(second.value().has_code(ReasonCode::CoolingHeatDerivedFromPolicy));

  // A declared heat figure always wins over a derived one.
  const RackCapacityInputs declared =
      rctest::Bundle("rack-a1", 42)
          .cooling(20000, 0)
          .heat_equivalence(1'000'000)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .asset_heat("a-0001", 1400)
          .build();
  const Result<RackCapacitySnapshot> third = evaluate(declared);
  RC_REQUIRE_OK(third);
  RC_CHECK_EQ(third.value().cooling.declared_heat_known.value(), 1400);
  RC_CHECK_EQ(third.value().cooling.derived_heat_known.value(), 0);
}

RC_TEST(weight_point_load_is_distributed_with_an_exact_remainder_rule) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .weight(900'000, 0)
          .weight_point_limit(30'000)
          .asset_units("a-0001", 1, 4)
          .asset_mass("a-0001", 40'000)  // 10 kg per unit over four units
          .asset_units("a-0002", 5, 1)
          .asset_mass("a-0002", 31'000)  // over the point limit on its own unit
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const WeightAccounting& weight = snapshot.value().weight;
  RC_CHECK_EQ(weight.occupied_known.value(), 71'000);
  RC_CHECK_EQ(weight.max_unit_load->value(), 31'000);
  RC_CHECK_EQ(weight.units_at_or_above_point_limit, 1u);
  RC_CHECK(snapshot.value().has_code(ReasonCode::WeightPointLoadExceeded));
  RC_CHECK_EQ(weight.free.lower().value(), 900'000 - 71'000);
}

RC_TEST(weight_remainder_lands_on_the_lowest_touched_unit) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .weight(900'000, 0)
                                        .weight_point_limit(100'000)
                                        .asset_units("a-0001", 1, 3)
                                        .asset_mass("a-0001", 10'000)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  // 10000 over three units is 3333 each with a remainder of 1 charged to U1.
  RC_CHECK_EQ(snapshot.value().weight.max_unit_load->value(), 3334);
}

RC_TEST(unknown_weight_never_becomes_free_capacity) {
  const RackCapacityInputs no_limit = rctest::Bundle("rack-a1", 42).no_weight().build();
  const Result<RackCapacitySnapshot> first = evaluate(no_limit);
  RC_REQUIRE_OK(first);
  RC_CHECK(!first.value().weight.envelope_known);
  RC_CHECK(!first.value().weight.free.is_known());
  RC_CHECK(first.value().unknown.weight_envelope);

  const RackCapacityInputs unknown_mass = rctest::Bundle("rack-a1", 42)
                                              .weight(900'000, 0)
                                              .asset_units("a-0001", 1, 1)
                                              .build();
  const Result<RackCapacitySnapshot> second = evaluate(unknown_mass);
  RC_REQUIRE_OK(second);
  RC_CHECK_EQ(second.value().weight.free.lower().value(), 0);
  RC_CHECK_EQ(second.value().weight.free.upper().value(), 900'000);
  RC_CHECK_EQ(second.value().weight.unknown_mass_assets, 1u);
}

RC_TEST(overcommitment_is_reported_rather_than_hidden) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 0)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 7000)
          .reservation("r-0001", ReservationState::Committed)
          .reservation_draw("r-0001", 2000)
          .reservation_span("r-0001", "[5,7)")
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().power.usable->value(), 8000);
  RC_CHECK_EQ(snapshot.value().power.overcommit.value(), 1000);
  RC_CHECK_EQ(snapshot.value().power.free.upper().value(), 0);
  RC_CHECK(snapshot.value().has_code(ReasonCode::PowerOvercommitted));
  RC_CHECK(snapshot.value().binding);
  RC_CHECK_EQ(snapshot.value().primary_binding, Dimension::Power);
}

RC_TEST(slot_reservation_over_an_occupant_is_an_overcommit_not_a_double_count) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .asset_units("a-0001", 1, 2)
          .reservation("r-0001", ReservationState::Committed)
          .reservation_span("r-0001", "[1,5)")
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().slots.occupied_slots, 4u);
  RC_CHECK_EQ(snapshot.value().slots.reserved_slots, 4u);
  RC_CHECK_EQ(snapshot.value().slots.overcommitted_slots, 4u);
  RC_CHECK_EQ(snapshot.value().slots.free.lower(), 80u);
  RC_CHECK(snapshot.value().has_code(ReasonCode::SlotOvercommitted));
}

RC_TEST(shared_mount_classes_report_remaining_shares) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .shared_asset("a-0001", "[81,83)", "psu-bay", 2)
          .shared_asset("a-0002", "[81,83)", "psu-bay", 2)
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_REQUIRE(snapshot.value().shared_classes.size() == 1u);
  const SharedClassUtilization& entry = snapshot.value().shared_classes[0];
  RC_CHECK_EQ(entry.share_capacity, 2u);
  RC_CHECK_EQ(entry.used, 2u);
  RC_CHECK_EQ(entry.remaining, 0u);
  RC_CHECK_EQ(entry.overcommitted, 0u);
  RC_CHECK(snapshot.value().has_code(ReasonCode::SharedClassCapacityExhausted));
  // A shared span consumes its slots once, not once per occupant.
  RC_CHECK_EQ(snapshot.value().slots.occupied_slots, 2u);
  RC_CHECK_EQ(snapshot.value().slots.shared_occupied_slots, 2u);
}

RC_TEST(fragmentation_measures_the_largest_usable_run) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 12)
                                        .asset("a-0001", "[1,3)")
                                        .asset("a-0002", "[4,5)")
                                        .asset("a-0003", "[8,9)")
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const SlotFragmentation& fragmentation = snapshot.value().slots.fragmentation;
  RC_CHECK_EQ(snapshot.value().slots.free.lower(), 24u - 4u);
  RC_CHECK_EQ(fragmentation.free_run_count, 3u);
  RC_CHECK_EQ(fragmentation.isolated_free_slots, 1u);
  RC_CHECK_EQ(fragmentation.largest_free_run_slots, 16u);
}

RC_TEST(serviceability_reports_occupancy_above_the_service_height_limit) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 12)
                                        .service_height_limit(8)
                                        .asset_units("a-0001", 4, 1)
                                        .asset_units("a-0002", 10, 1)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().serviceability.service_height_limit_unit.value(), 8u);
  RC_CHECK_EQ(snapshot.value().serviceability.occupied_units_above_service_height, 1u);
  RC_CHECK(snapshot.value().has_code(ReasonCode::ServiceOccupancyAboveServiceHeight));
  RC_CHECK(snapshot.value().has_code(ReasonCode::ServiceClearanceRecorded));
}

RC_TEST(stale_evidence_is_reported_and_freshness_is_supplied_by_the_caller) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42).envelope_age_limit(1000).evidence_observed_at(kNow - 5000).build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK(snapshot.value().freshness == EvidenceFreshness::Stale);
  RC_CHECK(snapshot.value().has_code(ReasonCode::EvidenceStale));
  RC_CHECK_EQ(snapshot.value().evidence_observed_at.value(), kNow - 5000);

  const RackCapacityInputs fresh =
      rctest::Bundle("rack-a1", 42).envelope_age_limit(1000).evidence_observed_at(kNow - 10).build();
  const Result<RackCapacitySnapshot> fresh_snapshot = evaluate(fresh);
  RC_REQUIRE_OK(fresh_snapshot);
  RC_CHECK(fresh_snapshot.value().freshness == EvidenceFreshness::Fresh);
  RC_CHECK(fresh_snapshot.value().has_code(ReasonCode::EvidenceFresh));

  // A zero age bound means the policy declares no bound at all.
  const RackCapacityInputs unbounded = rctest::Bundle("rack-a1", 42)
                                           .no_freshness_bounds()
                                           .evidence_observed_at(kNow - 10'000'000)
                                           .build();
  const Result<RackCapacitySnapshot> unbounded_snapshot = evaluate(unbounded);
  RC_REQUIRE_OK(unbounded_snapshot);
  RC_CHECK(unbounded_snapshot.value().freshness == EvidenceFreshness::Fresh);
}

RC_TEST(unevaluated_recovery_marks_a_snapshot_as_unrevalidated) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42).build();
  const Result<RackCapacitySnapshot> snapshot =
      evaluate_capacity(inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
                        TimestampNs::trusted(kNow), EvidenceFreshness::Unrevalidated);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK(snapshot.value().freshness == EvidenceFreshness::Unrevalidated);
  RC_CHECK(snapshot.value().has_code(ReasonCode::EvidenceUnrevalidated));
}

RC_TEST(constraint_pressure_orders_dimensions_by_utilisation) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 0)   // usable 8000
          .cooling(20000, 0)                        // usable 20000
          .weight(900'000, 0)                       // usable 900000
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 6000)               // 75 percent of power
          .asset_heat("a-0001", 4000)               // 20 percent of cooling
          .asset_mass("a-0001", 90'000)             // 10 percent of weight
          .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_REQUIRE(snapshot.value().pressures.size() >= 4u);
  RC_CHECK_EQ(snapshot.value().pressures[0].dimension, Dimension::Power);
  RC_CHECK_EQ(snapshot.value().pressures[0].utilization_bp, 7'500);
  RC_CHECK_EQ(snapshot.value().primary_binding, Dimension::Power);
  RC_CHECK(!snapshot.value().binding);

  // Slot pressure counts the occupied slots against the usable slot space.
  const auto slot_entry =
      std::find_if(snapshot.value().pressures.begin(), snapshot.value().pressures.end(),
                   [](const ConstraintPressure& entry) { return entry.dimension == Dimension::Slot; });
  RC_REQUIRE(slot_entry != snapshot.value().pressures.end());
  RC_CHECK_EQ(slot_entry->utilization_bp, 238);
}

RC_TEST(evaluation_is_deterministic_and_refuses_invalid_bundles) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_draw("a-0001", 100)
                                        .build();
  const Result<RackCapacitySnapshot> first = evaluate(inputs);
  const Result<RackCapacitySnapshot> second = evaluate(inputs);
  RC_REQUIRE_OK(first);
  RC_REQUIRE_OK(second);
  RC_CHECK_EQ(first.value().digest, second.value().digest);
  RC_CHECK(first.value().to_text().size() > 100u);

  RackCapacityInputs invalid = rctest::Bundle("rack-a1", 42).build_unchecked();
  invalid.composition.unit_count = 0;
  RC_REQUIRE_CODE(evaluate(invalid), ErrorCode::InvalidRange);
}

RC_TEST(explanation_text_is_stable_and_machine_readable) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42).no_power().build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  const std::vector<Explanation> unknown = snapshot.value().explanations_for(
      ReasonCode::PowerEnvelopeUnknown);
  RC_REQUIRE(unknown.size() == 1u);
  const std::string text = unknown[0].to_text();
  RC_CHECK(text.find("power_envelope_unknown") != std::string::npos);
  RC_CHECK(text.find("unknown") != std::string::npos);
  RC_CHECK_EQ(std::string(reason_code_name(ReasonCode::PowerEnvelopeUnknown)),
              std::string("power_envelope_unknown"));
}
