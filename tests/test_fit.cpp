// Rack Capacity - fit evaluation tests.
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

struct Built {
  RackCapacitySnapshot snapshot{};
};

Result<RackCapacitySnapshot> evaluate(const RackCapacityInputs& inputs) {
  return evaluate_capacity(inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
                           TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
}

FitRequest base_request(const RackCapacitySnapshot& snapshot) {
  FitRequest request;
  request.expected = CapacityExpectation::of(snapshot);
  return request;
}

}  // namespace

RC_TEST(fit_accepts_a_free_span_and_reports_no_authority) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_draw("a-0001", 1000)
                                        .asset_heat("a-0001", 1000)
                                        .asset_mass("a-0001", 20000)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest request = base_request(snapshot.value());
  request.span = SlotInterval::for_units(5, 1).value();
  request.draw = Watts::trusted(1200);
  request.heat_rejection = Watts::trusted(1200);
  request.mass = Grams::trusted(25000);
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(evaluation);
  RC_CHECK(evaluation.value().verdict == FitVerdict::Fits);
  RC_CHECK_EQ(evaluation.value().binding_constraint, Dimension::None);
  RC_CHECK(!evaluation.value().placement_authority);
  RC_CHECK(!evaluation.value().reservation_authority);
  RC_CHECK_EQ(evaluation.value().constraints.size(), 4u);
  RC_CHECK(evaluation.value().explanations.size() == 1u);
  RC_CHECK(evaluation.value().explanations[0].code == ReasonCode::FitAccepted);
  RC_CHECK(evaluation.value().to_text().find("placement authority: none") != std::string::npos);
}

RC_TEST(fit_refuses_a_stale_precondition) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42).build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest request = base_request(snapshot.value());
  request.span = SlotInterval::for_units(5, 1).value();
  request.expected.capacity_generation = CapacityGeneration::trusted(9);
  RC_REQUIRE_CODE(evaluate_fit(request, snapshot.value()), ErrorCode::StaleCapacityGeneration);

  request.expected = CapacityExpectation::of(snapshot.value());
  request.expected.revision = SnapshotRevision::trusted(9);
  RC_REQUIRE_CODE(evaluate_fit(request, snapshot.value()), ErrorCode::StaleSnapshotRevision);

  request.expected = CapacityExpectation::of(snapshot.value());
  request.expected.rack = RackId::create("rack-other").value();
  RC_REQUIRE_CODE(evaluate_fit(request, snapshot.value()), ErrorCode::InvalidArgument);
}

RC_TEST(fit_distinguishes_occupied_from_indeterminate_slots) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .asset_units("a-0001", 1, 2)
                                        .asset_units("a-0002", 10, 1)
                                        .asset_presence("a-0002", PresenceState::Indeterminate)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest occupied = base_request(snapshot.value());
  occupied.span = SlotInterval::for_units(1, 1).value();
  const Result<FitEvaluation> occupied_result = evaluate_fit(occupied, snapshot.value());
  RC_REQUIRE_OK(occupied_result);
  RC_CHECK(occupied_result.value().verdict == FitVerdict::DoesNotFit);
  RC_CHECK_EQ(occupied_result.value().binding_constraint, Dimension::Slot);
  RC_CHECK_EQ(occupied_result.value().constraints[0].code, ReasonCode::FitSlotOccupied);

  FitRequest uncertain = base_request(snapshot.value());
  uncertain.span = SlotInterval::for_units(10, 1).value();
  const Result<FitEvaluation> uncertain_result = evaluate_fit(uncertain, snapshot.value());
  RC_REQUIRE_OK(uncertain_result);
  RC_CHECK(uncertain_result.value().verdict == FitVerdict::Indeterminate);
  RC_CHECK_EQ(uncertain_result.value().constraints[0].code, ReasonCode::FitSlotIndeterminate);

  FitRequest blocked = base_request(snapshot.value());
  RC_REQUIRE(validate_inputs(rctest::Bundle("rack-a1", 42).structural_reserved("[9,11)").build())
                 .has_value());
  const Result<RackCapacitySnapshot> with_structure =
      evaluate(rctest::Bundle("rack-a1", 42).structural_reserved("[9,11)").build());
  RC_REQUIRE_OK(with_structure);
  blocked.expected = CapacityExpectation::of(with_structure.value());
  blocked.span = SlotInterval::for_units(5, 1).value();
  const Result<FitEvaluation> blocked_result = evaluate_fit(blocked, with_structure.value());
  RC_REQUIRE_OK(blocked_result);
  RC_CHECK(blocked_result.value().verdict == FitVerdict::DoesNotFit);
  RC_CHECK_EQ(blocked_result.value().constraints[0].code, ReasonCode::FitSlotBlockedByStructure);
}

RC_TEST(fit_counts_anchors_without_choosing_one) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 12)
                                        .asset("a-0001", "[1,3)")
                                        .asset("a-0002", "[5,7)")
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest request = base_request(snapshot.value());
  request.unit_height = 1u;
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(evaluation);
  RC_CHECK(evaluation.value().verdict == FitVerdict::Fits);
  RC_REQUIRE(evaluation.value().feasible_anchors_lower.has_value());
  // Occupied slots are 1, 2, 5 and 6. A one-unit candidate may start at any
  // odd slot whose pair is free: 3, then 7 through 23, which is ten anchors.
  RC_CHECK_EQ(evaluation.value().feasible_anchors_lower.value(), 10u);
  RC_CHECK_EQ(evaluation.value().feasible_anchors_upper.value(), 10u);
  RC_CHECK_EQ(evaluation.value().requested_span_slots.value(), 2u);

  FitRequest too_tall = base_request(snapshot.value());
  too_tall.unit_height = 12u;
  const Result<FitEvaluation> too_tall_result = evaluate_fit(too_tall, snapshot.value());
  RC_REQUIRE_OK(too_tall_result);
  RC_CHECK(too_tall_result.value().verdict == FitVerdict::DoesNotFit);
  RC_CHECK_EQ(too_tall_result.value().constraints[0].code, ReasonCode::FitSlotNoAnchor);

  FitRequest incomplete = base_request(snapshot.value());
  RC_REQUIRE_CODE(evaluate_fit(incomplete, snapshot.value()), ErrorCode::InvalidArgument);
  incomplete.unit_height = 1u;
  incomplete.span = SlotInterval::for_units(1, 1).value();
  RC_REQUIRE_CODE(evaluate_fit(incomplete, snapshot.value()), ErrorCode::InvalidArgument);
  FitRequest outside = base_request(snapshot.value());
  outside.span = SlotInterval::for_units(13, 1).value();
  RC_REQUIRE_CODE(evaluate_fit(outside, snapshot.value()), ErrorCode::SlotOutOfBounds);
}

RC_TEST(fit_verdicts_are_three_valued_for_every_measured_dimension) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .power(2, 8000, RedundancyMode::N2, 0)  // usable 8000
                                        .cooling(10000, 0)                      // usable 10000
                                        .weight(100'000, 0)                     // usable 100000
                                        .asset_units("a-0001", 1, 1)
                                        .asset_draw("a-0001", 1000)
                                        .asset_heat("a-0001", 1000)
                                        .asset_mass("a-0001", 10'000)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().power.free.lower().value(), 7'000);
  RC_CHECK_EQ(snapshot.value().cooling.free.lower().value(), 9'000);
  RC_CHECK_EQ(snapshot.value().weight.free.lower().value(), 90'000);

  FitRequest small = base_request(snapshot.value());
  small.span = SlotInterval::for_units(5, 1).value();
  small.draw = Watts::trusted(1000);
  small.heat_rejection = Watts::trusted(1000);
  small.mass = Grams::trusted(10'000);
  const Result<FitEvaluation> small_result = evaluate_fit(small, snapshot.value());
  RC_REQUIRE_OK(small_result);
  RC_CHECK(small_result.value().verdict == FitVerdict::Fits);

  FitRequest large = small;
  large.draw = Watts::trusted(20'000);
  large.heat_rejection = Watts::trusted(50'000);
  large.mass = Grams::trusted(500'000);
  const Result<FitEvaluation> large_result = evaluate_fit(large, snapshot.value());
  RC_REQUIRE_OK(large_result);
  RC_CHECK(large_result.value().verdict == FitVerdict::DoesNotFit);
  RC_CHECK_EQ(large_result.value().binding_constraint, Dimension::Power);
  RC_CHECK_EQ(large_result.value().constraints[1].code, ReasonCode::FitPowerInsufficient);
  RC_CHECK_EQ(large_result.value().constraints[2].code, ReasonCode::FitCoolingInsufficient);
  RC_CHECK_EQ(large_result.value().constraints[3].code, ReasonCode::FitWeightInsufficient);

  // The same boundary read with one occupant whose draw cannot be established.
  const RackCapacityInputs uncertain_inputs =
      rctest::Bundle("rack-a1", 42)
          .power(2, 8000, RedundancyMode::N2, 0)
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .asset_units("a-0002", 2, 1)
          .build();
  const Result<RackCapacitySnapshot> uncertain_snapshot = evaluate(uncertain_inputs);
  RC_REQUIRE_OK(uncertain_snapshot);
  RC_CHECK_EQ(uncertain_snapshot.value().power.free.lower().value(), 0);
  RC_CHECK_EQ(uncertain_snapshot.value().power.free.upper().value(), 7'000);

  FitRequest between = base_request(uncertain_snapshot.value());
  between.span = SlotInterval::for_units(5, 1).value();
  between.draw = Watts::trusted(6500);
  const Result<FitEvaluation> between_result = evaluate_fit(between, uncertain_snapshot.value());
  RC_REQUIRE_OK(between_result);
  RC_CHECK(between_result.value().verdict == FitVerdict::Indeterminate);
  RC_CHECK_EQ(between_result.value().constraints[1].code, ReasonCode::FitPowerIndeterminate);
  RC_REQUIRE(between_result.value().constraints[1].available_lower.has_value());
  RC_REQUIRE(between_result.value().constraints[1].available_upper.has_value());
  RC_CHECK_EQ(between_result.value().constraints[1].available_lower.value(), 0);
  RC_CHECK_EQ(between_result.value().constraints[1].available_upper.value(), 7'000);

  FitRequest beyond = between;
  beyond.draw = Watts::trusted(7001);
  const Result<FitEvaluation> beyond_result = evaluate_fit(beyond, uncertain_snapshot.value());
  RC_REQUIRE_OK(beyond_result);
  RC_CHECK(beyond_result.value().verdict == FitVerdict::DoesNotFit);
}
RC_TEST(fit_reports_an_unknown_envelope_as_indeterminate) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42).no_power().no_cooling().no_weight().build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest request = base_request(snapshot.value());
  request.span = SlotInterval::for_units(5, 1).value();
  request.draw = Watts::trusted(1);
  request.heat_rejection = Watts::trusted(1);
  request.mass = Grams::trusted(1);
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(evaluation);
  RC_CHECK(evaluation.value().verdict == FitVerdict::Indeterminate);
  RC_CHECK_EQ(evaluation.value().constraints[1].code, ReasonCode::FitPowerIndeterminate);
  RC_CHECK_EQ(evaluation.value().constraints[2].code, ReasonCode::FitCoolingIndeterminate);
  RC_CHECK_EQ(evaluation.value().constraints[3].code, ReasonCode::FitWeightIndeterminate);
}

RC_TEST(fit_reports_serviceability_constraints) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .service_height_limit(20)
                                        .clearance(800, 600)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest high = base_request(snapshot.value());
  high.span = SlotInterval::for_units(30, 1).value();
  high.requires_service_access = true;
  high.required_front_clearance = Millimetres::trusted(900);
  high.required_rear_clearance = Millimetres::trusted(500);
  const Result<FitEvaluation> high_result = evaluate_fit(high, snapshot.value());
  RC_REQUIRE_OK(high_result);
  RC_CHECK(high_result.value().verdict == FitVerdict::DoesNotFit);
  RC_CHECK_EQ(high_result.value().binding_constraint, Dimension::Serviceability);
  bool saw_height = false;
  bool saw_clearance = false;
  for (const ConstraintOutcome& outcome : high_result.value().constraints) {
    if (outcome.code == ReasonCode::FitServiceHeightExceeded) {
      saw_height = true;
    }
    if (outcome.code == ReasonCode::FitServiceClearanceInsufficient) {
      saw_clearance = true;
    }
  }
  RC_CHECK(saw_height);
  RC_CHECK(saw_clearance);
  // The slot dimension is free: the candidate would fit physically, and only
  // the serviceability rules reject it.
  RC_CHECK(high_result.value().constraints[0].verdict == FitVerdict::Fits);

  FitRequest low = base_request(snapshot.value());
  low.unit_height = 1u;
  low.requires_service_access = true;
  const Result<FitEvaluation> low_result = evaluate_fit(low, snapshot.value());
  RC_REQUIRE_OK(low_result);
  RC_CHECK(low_result.value().verdict == FitVerdict::Fits);
  RC_REQUIRE(low_result.value().feasible_anchors_lower.has_value());
  // Only rack units at or below the service height limit are counted.
  RC_CHECK_EQ(low_result.value().feasible_anchors_lower.value(), 20u);
}

RC_TEST(fit_reports_point_load_without_guessing_the_distribution) {
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .weight(900'000, 0)
                                        .weight_point_limit(50'000)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_mass("a-0001", 40'000)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);

  FitRequest fits = base_request(snapshot.value());
  fits.span = SlotInterval::for_units(5, 1).value();
  fits.mass = Grams::trusted(5'000);
  const Result<FitEvaluation> fits_result = evaluate_fit(fits, snapshot.value());
  RC_REQUIRE_OK(fits_result);
  RC_CHECK(fits_result.value().verdict == FitVerdict::Fits);

  FitRequest uncertain = fits;
  uncertain.mass = Grams::trusted(20'000);
  const Result<FitEvaluation> uncertain_result = evaluate_fit(uncertain, snapshot.value());
  RC_REQUIRE_OK(uncertain_result);
  RC_CHECK(uncertain_result.value().verdict == FitVerdict::Indeterminate);
  bool saw_point_load = false;
  for (const ConstraintOutcome& outcome : uncertain_result.value().constraints) {
    if (outcome.code == ReasonCode::FitPointLoadIndeterminate) {
      saw_point_load = true;
    }
  }
  RC_CHECK(saw_point_load);
}

RC_TEST(fit_never_converts_an_unknown_into_an_acceptance) {
  // One occupant with an unknown draw makes every request above the known
  // remaining capacity undecided rather than accepted.
  const RackCapacityInputs inputs = rctest::Bundle("rack-a1", 42)
                                        .power(2, 4000, RedundancyMode::N2, 0)
                                        .asset_units("a-0001", 1, 1)
                                        .asset_draw("a-0001", 1000)
                                        .asset_units("a-0002", 2, 1)
                                        .build();
  const Result<RackCapacitySnapshot> snapshot = evaluate(inputs);
  RC_REQUIRE_OK(snapshot);
  RC_CHECK_EQ(snapshot.value().power.free.lower().value(), 0);
  RC_CHECK_EQ(snapshot.value().power.free.upper().value(), 3000);

  FitRequest request = base_request(snapshot.value());
  request.span = SlotInterval::for_units(5, 1).value();
  request.draw = Watts::trusted(3001);
  const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(evaluation);
  RC_CHECK(evaluation.value().verdict == FitVerdict::DoesNotFit);

  request.draw = Watts::trusted(3000);
  const Result<FitEvaluation> boundary = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(boundary);
  RC_CHECK(boundary.value().verdict == FitVerdict::Indeterminate);

  request.draw = Watts::trusted(0);
  const Result<FitEvaluation> zero = evaluate_fit(request, snapshot.value());
  RC_REQUIRE_OK(zero);
  RC_CHECK(zero.value().verdict == FitVerdict::Fits);
}
