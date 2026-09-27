// Rack Capacity - evidence validation and specification text tests.
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

const char* kMinimalSpec = R"(rcap-spec 1
[composition]
rack = rack-a1
generation = 4
units = 48
structural-reserved = [95,97)
front-clearance-mm = 900
rear-clearance-mm = 700
service-height-limit-u = 42
site = dc1
hall = hall-a
row = row-7
position = pos-12
source = rack-registry
evidence = ev-composition
version = 11
observed-at-ns = 1700000000000000000
[policy]
policy = standard-2026
policy-version = 3
power-headroom-w = 500
cooling-headroom-w = 500
weight-headroom-g = 20000
slot-headroom = 2
power-derate-bp = 500
max-envelope-age-ns = 3600000000000
max-measurement-age-ns = 300000000000
heat-per-power-ppm = 1000000
[power]
feeds = 2
watts-per-feed = 8000
redundancy = 2n
derate-bp = 2000
measured-draw-w = 4100
measured-at-ns = 1700000000000000000
source = power-capacity
evidence = ev-power
version = 7
observed-at-ns = 1700000000000000000
[cooling]
nominal-heat-rejection-w = 20000
derate-bp = 1000
supply-air-temp-mc = 24000
source = cooling-capacity
evidence = ev-cooling
version = 3
observed-at-ns = 1700000000000000000
[weight]
static-load-limit-g = 900000
derate-bp = 0
per-unit-point-load-limit-g = 250000
source = cooling-capacity
evidence = ev-weight
version = 2
observed-at-ns = 1700000000000000000
[asset a-0001]
span = [1,5)
kind = full
presence = present
nameplate-draw-w = 1200
declared-heat-w = 1200
mass-g = 32000
source = asset-registry
evidence = ev-asset-0001
version = 5
observed-at-ns = 1700000000000000000
[asset a-0002]
span = [21,29)
kind = full
presence = present
nameplate-draw-w = 900
mass-g = 41000
source = asset-registry
evidence = ev-asset-0002
version = 6
observed-at-ns = 1700000000000000000
[reservation r-0001]
state = committed
span = [31,33)
kind = full
draw-w = 500
heat-w = 500
mass-g = 9000
source = facility-capacity-reservation
evidence = ev-reservation-0001
version = 2
observed-at-ns = 1700000000000000000
[request]
epoch = 12
captured-at-ns = 1700000000000000000
actor = operator-1
request = req-0001
)";

}  // namespace

RC_TEST(specification_parses_and_round_trips) {
  const Result<RackCapacityInputs> parsed = parse_specification(kMinimalSpec);
  RC_REQUIRE_OK(parsed);
  const RackCapacityInputs& inputs = parsed.value();
  RC_CHECK_EQ(inputs.composition.rack.text(), std::string("rack-a1"));
  RC_CHECK_EQ(inputs.composition.generation.value(), 4u);
  RC_CHECK_EQ(inputs.composition.unit_count, 48u);
  RC_CHECK_EQ(inputs.composition.structural_reserved_slots.to_text(), std::string("[95,97)"));
  RC_CHECK(inputs.composition.serviceability.service_height_limit_unit.has_value());
  RC_CHECK_EQ(inputs.composition.serviceability.service_height_limit_unit.value(), 42u);
  RC_CHECK_EQ(inputs.policy.reference.policy.text(), std::string("standard-2026"));
  RC_CHECK_EQ(inputs.policy.headroom.slot_headroom, 2u);
  RC_CHECK_EQ(inputs.policy.headroom.power_headroom.value(), 500);
  RC_CHECK_EQ(inputs.policy.heat_per_power_ppm, 1'000'000u);
  RC_REQUIRE(inputs.power.has_value());
  RC_CHECK_EQ(inputs.power->feed_count, 2u);
  RC_CHECK(inputs.power->redundancy == RedundancyMode::N2);
  RC_REQUIRE(inputs.cooling.has_value());
  RC_CHECK(inputs.cooling->supply_air_temp.has_value());
  RC_CHECK_EQ(inputs.cooling->supply_air_temp->value(), 24000);
  RC_REQUIRE(inputs.weight.has_value());
  RC_CHECK(inputs.weight->per_unit_point_load_limit.has_value());
  RC_CHECK_EQ(inputs.assets.size(), 2u);
  RC_CHECK_EQ(inputs.assets[0].asset.text(), std::string("a-0001"));
  RC_CHECK(inputs.assets[0].declared_heat_rejection.has_value());
  RC_CHECK(!inputs.assets[1].declared_heat_rejection.has_value());
  RC_CHECK_EQ(inputs.reservations.size(), 1u);
  RC_CHECK(inputs.reservations[0].state == ReservationState::Committed);
  RC_CHECK_EQ(inputs.epoch.value(), 12u);
  RC_CHECK_EQ(inputs.actor.text(), std::string("operator-1"));

  // The rendering must parse back into exactly the same facts.
  const std::string rendered = render_specification(inputs);
  const Result<RackCapacityInputs> reparsed = parse_specification(rendered);
  RC_REQUIRE_OK(reparsed);
  RC_CHECK_EQ(inputs_full_digest(reparsed.value()), inputs_full_digest(inputs));
  RC_CHECK_EQ(render_specification(reparsed.value()), rendered);
}

RC_TEST(specification_rejects_malformed_documents) {
  RC_CHECK(!parse_specification("").has_value());
  RC_CHECK(!parse_specification("not-a-header\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 2\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\nrack = rack-a1\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\n[composition]\nno-equals\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\n[unknown]\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\n[composition ]\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\n[asset]\n").has_value());
  RC_CHECK(!parse_specification("rcap-spec 1\n[composition x]\n").has_value());

  // A missing key is an error, not a default.
  std::string missing_key = kMinimalSpec;
  const std::size_t position = missing_key.find("units = 48\n");
  missing_key.erase(position, std::string("units = 48\n").size());
  RC_CHECK(!parse_specification(missing_key).has_value());

  // A duplicate key is an error, not a last-one-wins overwrite.
  std::string duplicate = kMinimalSpec;
  duplicate += "[composition]\nrack = rack-a2\n";
  RC_CHECK(!parse_specification(duplicate).has_value());

  // An unknown key inside a known section is an error.
  std::string unknown_key = kMinimalSpec;
  const std::size_t insert_at = unknown_key.find("[policy]");
  unknown_key.insert(insert_at, "mystery = 1\n");
  RC_CHECK(!parse_specification(unknown_key).has_value());

  // Out-of-range and malformed values are refused.
  std::string bad_units = kMinimalSpec;
  const std::size_t units_position = bad_units.find("units = 48");
  bad_units.replace(units_position, 10, "units = 0 ");
  RC_CHECK(!parse_specification(bad_units).has_value());

  std::string negative = kMinimalSpec;
  const std::size_t draw_position = negative.find("nameplate-draw-w = 1200");
  negative.replace(draw_position, 23, "nameplate-draw-w = -12");
  RC_CHECK(!parse_specification(negative).has_value());

  std::string comma_identity = kMinimalSpec;
  const std::size_t actor_position = comma_identity.find("actor = operator-1");
  comma_identity.replace(actor_position, 19, "actor = operator,1");
  RC_CHECK(!parse_specification(comma_identity).has_value());
}

RC_TEST(evidence_validation_enforces_the_documented_rules) {
  // Occupants outside the rack extent.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8).asset("a-0001", "[1,20)").build_unchecked())
                .has_value());
  // Two exclusive occupants over the same slots.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8)
                                .asset("a-0001", "[1,5)")
                                .asset("a-0002", "[3,7)")
                                .build_unchecked())
                .has_value());
  // Adjacent occupants are legal.
  RC_CHECK(validate_inputs(rctest::Bundle("rack-a1", 8)
                               .asset("a-0001", "[1,5)")
                               .asset("a-0002", "[5,7)")
                               .build_unchecked())
               .has_value());
  // An occupant inside a structurally reserved slot.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8)
                                .structural_reserved("[1,3)")
                                .asset("a-0001", "[1,5)")
                                .build_unchecked())
                .has_value());
  // Duplicate occupant identity: two entries carrying the same identity.
  RackCapacityInputs duplicated = rctest::Bundle("rack-a1", 8)
                                      .asset("a-0001", "[1,3)")
                                      .asset("a-0002", "[3,5)")
                                      .build_unchecked();
  duplicated.assets.back().asset = duplicated.assets.front().asset;
  RC_CHECK(!validate_inputs(duplicated).has_value());
  RC_CHECK(canonicalize_inputs(duplicated).has_value());
  RC_CHECK(!validate_inputs(duplicated).has_value());
  // An exclusive mount may not declare a shared class.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8)
                                .shared_asset("a-0001", "[1,3)", "psu-bay", 2)
                                .asset("a-0001", "[1,3)")
                                .build_unchecked())
                .has_value());
  // Shared occupants beyond the declared capacity.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8)
                                .shared_asset("a-0001", "[1,3)", "psu-bay", 2)
                                .shared_asset("a-0002", "[1,3)", "psu-bay", 2)
                                .shared_asset("a-0003", "[1,3)", "psu-bay", 2)
                                .build_unchecked())
                .has_value());
  // Two shared occupants within capacity are legal.
  RC_CHECK(validate_inputs(rctest::Bundle("rack-a1", 8)
                               .shared_asset("a-0001", "[1,3)", "psu-bay", 2)
                               .shared_asset("a-0002", "[1,3)", "psu-bay", 2)
                               .build_unchecked())
               .has_value());
  // Different shared classes over the same span conflict.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 8)
                                .shared_asset("a-0001", "[1,3)", "psu-bay", 2)
                                .shared_asset("a-0002", "[1,3)", "fan-bay", 2)
                                .build_unchecked())
                .has_value());
  // Non-canonical ordering is refused so that a bundle has exactly one encoding.
  RackCapacityInputs unordered = rctest::Bundle("rack-a1", 8)
                                     .asset("a-0002", "[3,5)")
                                     .asset("a-0001", "[1,3)")
                                     .build_unchecked();
  RC_CHECK(!validate_inputs(unordered).has_value());
  RC_CHECK(canonicalize_inputs(unordered).has_value());
  RC_CHECK(validate_inputs(unordered).has_value());
  RC_CHECK_EQ(unordered.assets[0].asset.text(), std::string("a-0001"));

  // Redundancy needs at least two feeds.
  RC_CHECK(!validate_inputs(
                rctest::Bundle("rack-a1", 8).power(1, 8000, RedundancyMode::N2, 0).build_unchecked())
                .has_value());
  // A power envelope with no watts per feed declares no capacity.
  RC_CHECK(!validate_inputs(
                rctest::Bundle("rack-a1", 8).power(2, 0, RedundancyMode::N2, 0).build_unchecked())
                .has_value());
  // Evidence must carry a version and an instant.
  RackCapacityInputs no_version = rctest::Bundle("rack-a1", 8).build_unchecked();
  no_version.composition.reference.version = 0;
  RC_CHECK(!validate_inputs(no_version).has_value());
  // The policy reference is mandatory.
  RackCapacityInputs no_policy = rctest::Bundle("rack-a1", 8).build_unchecked();
  no_policy.policy.reference.version = 0;
  RC_CHECK(!validate_inputs(no_policy).has_value());
  // An evidence epoch starts at one.
  RackCapacityInputs no_epoch = rctest::Bundle("rack-a1", 8).epoch(0).build_unchecked();
  RC_CHECK(!validate_inputs(no_epoch).has_value());
}

RC_TEST(evidence_digests_distinguish_facts_from_request_identity) {
  const RackCapacityInputs first = rctest::Bundle("rack-a1", 8).request("req-0001").build();
  const RackCapacityInputs replayed = rctest::Bundle("rack-a1", 8).request("req-0002").build();
  RC_CHECK_EQ(inputs_digest(first), inputs_digest(replayed));
  RC_CHECK(inputs_full_digest(first) != inputs_full_digest(replayed));

  const RackCapacityInputs changed =
      rctest::Bundle("rack-a1", 8).request("req-0001").asset_units("a-0001", 1, 1).build();
  RC_CHECK(inputs_digest(first) != inputs_digest(changed));
}
