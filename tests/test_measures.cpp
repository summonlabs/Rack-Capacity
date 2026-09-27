// Rack Capacity - exact arithmetic, digests and canonical encoding tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>
#include <iostream>
#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

RC_TEST(measures_reject_out_of_range_and_clamp_trusted) {
  RC_CHECK(Watts::create(0).has_value());
  RC_CHECK(Watts::create(kMaxWatts).has_value());
  RC_CHECK(!Watts::create(kMaxWatts + 1).has_value());
  RC_CHECK(!Watts::create(-1).has_value());
  RC_CHECK(!Grams::create(-1).has_value());
  RC_CHECK(!BasisPoints::create(10'001).has_value());
  RC_CHECK(!TimestampNs::create(-1).has_value());
  RC_CHECK(MilliCelsius::create(-40'000).has_value());
  RC_CHECK(!MilliCelsius::create(kMaxMilliCelsius + 1).has_value());

  RC_CHECK_EQ(Watts::trusted(kMaxWatts + 5).value(), kMaxWatts);
  RC_CHECK_EQ(Watts::trusted(-5).value(), 0);
  RC_CHECK(!Watts::is_valid(-1));
  RC_CHECK(Watts::is_valid(0));
}

RC_TEST(measure_arithmetic_refuses_to_leave_the_range) {
  const Watts large = Watts::trusted(kMaxWatts);
  const Watts one = Watts::trusted(1);
  RC_CHECK(!large.add(one).has_value());
  RC_CHECK(!one.subtract(large).has_value());
  RC_CHECK(!Watts::trusted(kMaxWatts).multiply(2).has_value());

  const Result<Watts> sum = Watts::trusted(1200).add(Watts::trusted(800));
  RC_REQUIRE_OK(sum);
  RC_CHECK_EQ(sum.value().value(), 2000);

  const Result<Watts> difference = Watts::trusted(1200).subtract(Watts::trusted(800));
  RC_REQUIRE_OK(difference);
  RC_CHECK_EQ(difference.value().value(), 400);

  RC_CHECK_EQ(Watts::trusted(100).saturating_subtract(Watts::trusted(250)).value(), 0);
  RC_CHECK_EQ(Watts::trusted(300).saturating_subtract(Watts::trusted(250)).value(), 50);
}

RC_TEST(derating_is_exact_and_floors_toward_zero) {
  const Result<Watts> derated = derate(Watts::trusted(8000), BasisPoints::trusted(2000));
  RC_REQUIRE_OK(derated);
  RC_CHECK_EQ(derated.value().value(), 6400);

  // 333 * 0.75 = 249.75, which floors to 249 rather than rounding up.
  const Result<Watts> floored = derate(Watts::trusted(333), BasisPoints::trusted(2500));
  RC_REQUIRE_OK(floored);
  RC_CHECK_EQ(floored.value().value(), 249);

  const Result<Watts> untouched = derate(Watts::trusted(1234), BasisPoints::trusted(0));
  RC_REQUIRE_OK(untouched);
  RC_CHECK_EQ(untouched.value().value(), 1234);

  const Result<Watts> removed = derate(Watts::trusted(1234), BasisPoints::trusted(10'000));
  RC_REQUIRE_OK(removed);
  RC_CHECK_EQ(removed.value().value(), 0);

  const Result<Grams> weight = derate(Grams::trusted(900'000), BasisPoints::trusted(1000));
  RC_REQUIRE_OK(weight);
  RC_CHECK_EQ(weight.value().value(), 810'000);
}

RC_TEST(heat_derivation_requires_an_explicit_equivalence) {
  const Result<Watts> none = heat_from_power(Watts::trusted(1200), 0);
  RC_REQUIRE_OK(none);
  RC_CHECK_EQ(none.value().value(), 0);

  const Result<Watts> unity = heat_from_power(Watts::trusted(1200), 1'000'000);
  RC_REQUIRE_OK(unity);
  RC_CHECK_EQ(unity.value().value(), 1200);

  const Result<Watts> fractional = heat_from_power(Watts::trusted(1001), 500'000);
  RC_REQUIRE_OK(fractional);
  RC_CHECK_EQ(fractional.value().value(), 500);

  RC_CHECK(!heat_from_power(Watts::trusted(10), 1'000'001).has_value());
}

RC_TEST(ratio_and_division_rules_are_exact) {
  RC_CHECK_EQ(ratio_basis_points(50, 100).value(), 5'000);
  RC_CHECK_EQ(ratio_basis_points(150, 100).value(), 15'000);
  RC_CHECK_EQ(ratio_basis_points(1, 3).value(), 3'333);
  RC_CHECK_EQ(ratio_basis_points(0, 10).value(), 0);
  RC_CHECK(!ratio_basis_points(1, 0).has_value());
  RC_CHECK(!ratio_basis_points(-1, 10).has_value());

  RC_CHECK_EQ(divide_floor(7, 2).value(), 3);
  RC_CHECK_EQ(divide_floor(-7, 2).value(), -4);
  RC_CHECK(!divide_floor(1, 0).has_value());
}

RC_TEST(sha256_matches_published_vectors) {
  const auto digest_empty = StateDigest::of(nullptr, 0);
  RC_CHECK_EQ(digest_empty.to_hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  const std::string abc = "abc";
  const auto digest_abc = StateDigest::of(reinterpret_cast<const std::uint8_t*>(abc.data()),
                                          abc.size());
  RC_CHECK_EQ(digest_abc.to_hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  // The 448-bit message that exercises multi-block padding.
  const std::string long_message =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const auto digest_long = StateDigest::of(
      reinterpret_cast<const std::uint8_t*>(long_message.data()), long_message.size());
  RC_CHECK_EQ(digest_long.to_hex(),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const Result<StateDigest> parsed = StateDigest::from_hex(digest_abc.to_hex());
  RC_REQUIRE_OK(parsed);
  RC_CHECK_EQ(parsed.value(), digest_abc);
  RC_CHECK(!StateDigest::from_hex("abc").has_value());
  RC_CHECK(!StateDigest::from_hex(std::string(64, 'z')).has_value());
  RC_CHECK(StateDigest{}.is_zero());
  RC_CHECK_EQ(digest_abc.to_short_hex().size(), 16u);

  // Domain separation: the same payload under two names never collides.
  const std::vector<std::uint8_t> payload{1, 2, 3};
  RC_CHECK(StateDigest::domain("a", payload) != StateDigest::domain("b", payload));
}

RC_TEST(crc32c_matches_the_published_check_value) {
  const std::string check = "123456789";
  RC_CHECK_EQ(crc32c(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()),
              0xE3069283u);
  RC_CHECK_EQ(crc32c(nullptr, 0), 0u);
  const std::uint8_t one[1] = {0x00u};
  RC_CHECK_EQ(crc32c(one, 1), 0x527D5351u);
}

RC_TEST(canonical_encoding_round_trips_every_bundle) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 42)
          .structural_reserved("[83,85)")
          .service_height_limit(40)
          .location("dc1", "hall-a", "row-7", "pos-12")
          .power(2, 8000, RedundancyMode::N2, 2000)
          .power_measurement(4100, 1'700'000'000'000'000'000ll)
          .cooling(20000, 1000)
          .weight(900'000, 500)
          .weight_point_limit(250'000)
          .asset_units("a-0001", 1, 2)
          .asset_draw("a-0001", 1200)
          .asset_heat("a-0001", 1200)
          .asset_mass("a-0001", 32'000)
          .asset_units("a-0002", 21, 4)
          .asset_draw("a-0002", 800)
          .asset_mass("a-0002", 40'000)
          .shared_asset("a-0003", "[81,83)", "psu-bay", 2)
          .zero_u_asset("a-0004")
          .reservation("r-0001", ReservationState::Committed)
          .reservation_span("r-0001", "[31,33)")
          .reservation_draw("r-0001", 500)
          .reservation_mass("r-0001", 12'000)
          .reservation("r-0002", ReservationState::Submitted)
          .reservation_span("r-0002", "[33,35)")
          .build();

  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(3), SnapshotRevision::trusted(4),
      TimestampNs::trusted(1'700'000'000'000'000'000ll), EvidenceFreshness::Fresh);
  RC_REQUIRE_OK(snapshot);

  // The digest is stable across repeated evaluation of the same facts.
  const Result<RackCapacitySnapshot> again = evaluate_capacity(
      inputs, CapacityGeneration::trusted(3), SnapshotRevision::trusted(4),
      TimestampNs::trusted(1'700'000'000'000'000'000ll), EvidenceFreshness::Fresh);
  RC_REQUIRE_OK(again);
  RC_CHECK_EQ(snapshot.value().digest, again.value().digest);
  RC_CHECK_EQ(compute_snapshot_digest(snapshot.value()), snapshot.value().digest);

  const ClosureReport closure = verify_closure(snapshot.value());
  RC_CHECK(closure.holds);
  if (!closure.holds) {
    std::cout << closure.to_text();
  }
}

RC_TEST(snapshot_digest_detects_every_single_field_change) {
  const RackCapacityInputs base = rctest::Bundle("rack-a1", 24).asset_units("a-0001", 1, 1).build();
  const Result<RackCapacitySnapshot> first = evaluate_capacity(
      base, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
      TimestampNs::trusted(1000), EvidenceFreshness::Fresh);
  RC_REQUIRE_OK(first);

  RackCapacitySnapshot mutated = first.value();
  mutated.slots.free = Bound<std::uint32_t>::between(1, 2);
  RC_CHECK(compute_snapshot_digest(mutated) != first.value().digest);

  RackCapacitySnapshot mutated2 = first.value();
  mutated2.binding = !mutated2.binding;
  RC_CHECK(compute_snapshot_digest(mutated2) != first.value().digest);

  RackCapacitySnapshot mutated3 = first.value();
  mutated3.power.usable = Watts::trusted(1);
  RC_CHECK(compute_snapshot_digest(mutated3) != first.value().digest);
}

RC_TEST(closure_identities_are_stated_for_every_dimension) {
  const RackCapacityInputs inputs =
      rctest::Bundle("rack-a1", 12).asset_units("a-0001", 2, 1).asset_draw("a-0001", 300).build();
  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
      TimestampNs::trusted(1000), EvidenceFreshness::Fresh);
  RC_REQUIRE_OK(snapshot);
  const ClosureReport closure = verify_closure(snapshot.value());
  RC_CHECK(closure.holds);
  RC_CHECK(closure.identities.size() >= 20u);
  RC_CHECK(closure.violations.empty());

  // A snapshot whose slot partition has been tampered with must fail closure.
  RackCapacitySnapshot broken = snapshot.value();
  broken.slots.free_lower_set = SlotSet::parse("{}").value();
  const ClosureReport broken_report = verify_closure(broken);
  RC_CHECK(!broken_report.holds);
  RC_CHECK(!broken_report.violations.empty());
}
