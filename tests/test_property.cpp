// Rack Capacity - deterministic property tests against independent models.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every check here compares the library with a model written independently in
// this file: per-slot bitmaps for occupancy, direct integer arithmetic for the
// envelopes. The models share no code with the library, so agreement between
// them is evidence rather than restatement. Seeds are fixed and printed, so a
// failure is reproducible exactly.

#include <algorithm>
#include <string>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

// Independent occupancy model: one entry per mount slot.
struct SlotModel {
  std::vector<bool> blocked;
  std::vector<bool> occupied;
  std::vector<bool> uncertain;
  std::vector<bool> reserved;
  std::uint32_t extent = 0;

  explicit SlotModel(std::uint32_t extent_end)
      : blocked(extent_end, false),
        occupied(extent_end, false),
        uncertain(extent_end, false),
        reserved(extent_end, false),
        extent(extent_end) {}

  void mark(std::vector<bool>& target, const SlotInterval& interval) {
    for (std::uint32_t slot = interval.begin(); slot < interval.end() && slot < extent; ++slot) {
      target[slot] = true;
    }
  }

  [[nodiscard]] std::uint32_t count(const std::vector<bool>& target) const {
    std::uint32_t total = 0;
    for (std::uint32_t slot = 1; slot < extent; ++slot) {
      if (target[slot]) {
        ++total;
      }
    }
    return total;
  }

  [[nodiscard]] std::vector<bool> conservative_free() const {
    std::vector<bool> free(extent, false);
    for (std::uint32_t slot = 1; slot < extent; ++slot) {
      free[slot] = !blocked[slot] && !occupied[slot] && !uncertain[slot] && !reserved[slot];
    }
    return free;
  }

  [[nodiscard]] std::vector<bool> optimistic_free() const {
    std::vector<bool> free(extent, false);
    for (std::uint32_t slot = 1; slot < extent; ++slot) {
      free[slot] = !blocked[slot] && !occupied[slot] && !reserved[slot];
    }
    return free;
  }

  // Policy headroom is withheld from the conservative free set, taking the
  // highest numbered free slots, and exactly those slots are then withheld from
  // the optimistic free set. The rule is documented; this model restates it
  // independently so that a divergence between the two is visible.
  void withhold(std::uint32_t headroom, std::vector<bool>& lower,
                std::vector<bool>& upper) const {
    std::uint32_t remaining = headroom;
    for (std::uint32_t slot = extent - 1; slot >= 1 && remaining > 0; --slot) {
      if (lower[slot]) {
        lower[slot] = false;
        upper[slot] = false;
        --remaining;
      }
    }
  }

  [[nodiscard]] std::vector<bool> free_lower(std::uint32_t headroom) const {
    std::vector<bool> free = conservative_free();
    std::vector<bool> upper = optimistic_free();
    withhold(headroom, free, upper);
    return free;
  }

  [[nodiscard]] std::vector<bool> free_upper(std::uint32_t headroom) const {
    std::vector<bool> free = conservative_free();
    std::vector<bool> upper = optimistic_free();
    withhold(headroom, free, upper);
    return upper;
  }

  [[nodiscard]] std::uint32_t anchors(const std::vector<bool>& free, std::uint32_t span) const {
    std::uint32_t total = 0;
    for (std::uint32_t start = 1; start % 2u == 1u && start + span < extent; start += 2u) {
      bool fits = true;
      for (std::uint32_t offset = 0; offset < span; ++offset) {
        if (!free[start + offset]) {
          fits = false;
          break;
        }
      }
      if (fits) {
        ++total;
      }
    }
    return total;
  }
};

struct Generated {
  RackCapacityInputs inputs{};
  SlotModel model{0};
  // Known committed load, count of unknown occupants, and the same for heat and
  // mass, all computed directly from the generated facts.
  std::int64_t committed_draw = 0;
  std::uint32_t unknown_draw = 0;
  std::int64_t declared_heat = 0;
  std::int64_t derived_heat = 0;
  std::uint32_t unknown_heat = 0;
  std::int64_t occupied_mass = 0;
  std::uint32_t unknown_mass = 0;
  std::int64_t reserved_draw = 0;
  std::int64_t reserved_heat = 0;
  std::int64_t reserved_mass = 0;
  std::uint32_t span_slots_for_probe = 0;
};

Generated generate(std::uint64_t seed) {
  rctest::Rng rng(seed);
  const std::uint32_t units = 2u + static_cast<std::uint32_t>(rng.below(24));
  rctest::Bundle bundle("rack-gen", units);
  Generated generated;
  generated.model = SlotModel(slot_extent_of(units));

  // Structural reservations, always inside the rack extent.
  const std::uint32_t extent = slot_extent_of(units);
  const std::uint32_t structural_count = static_cast<std::uint32_t>(rng.below(3));
  std::vector<std::string> structural_text;
  for (std::uint32_t index = 0; index < structural_count; ++index) {
    const std::uint32_t begin = 1u + static_cast<std::uint32_t>(rng.below(units * 2u));
    const std::uint32_t end = std::min(begin + 1u + static_cast<std::uint32_t>(rng.below(4)), extent);
    if (end <= begin) {
      continue;
    }
    const Result<SlotInterval> interval = SlotInterval::create(begin, end);
    if (!interval.has_value()) {
      continue;
    }
    structural_text.push_back(interval.value().to_text());
  }
  if (!structural_text.empty()) {
    std::string joined;
    for (std::size_t index = 0; index < structural_text.size(); ++index) {
      if (index != 0) {
        joined.push_back(',');
      }
      joined += structural_text[index];
    }
    const Result<SlotSet> set = SlotSet::parse(joined);
    if (set.has_value()) {
      bundle.structural_reserved(joined);
      for (const SlotInterval& interval : set.value().intervals()) {
        generated.model.mark(generated.model.blocked, interval);
      }
    }
  }

  // Occupants, placed on unit boundaries in the slots the structure leaves.
  bool shared_placed = false;
  const std::uint32_t attempts = static_cast<std::uint32_t>(rng.below(14));
  for (std::uint32_t index = 0; index < attempts; ++index) {
    const std::uint32_t first_unit = 1u + static_cast<std::uint32_t>(rng.below(units));
    const std::uint32_t height = 1u + static_cast<std::uint32_t>(rng.below(3));
    if (first_unit + height - 1u > units) {
      continue;
    }
    const Result<SlotInterval> interval = SlotInterval::for_units(first_unit, height);
    if (!interval.has_value()) {
      continue;
    }
    bool overlaps = false;
    for (std::uint32_t slot = interval.value().begin(); slot < interval.value().end(); ++slot) {
      if (generated.model.blocked[slot] || generated.model.occupied[slot] ||
          generated.model.uncertain[slot]) {
        overlaps = true;
        break;
      }
    }
    if (overlaps) {
      continue;
    }
    std::string id = "a-" + std::to_string(1000u + index);
    const bool shared = !shared_placed && rng.chance(20);
    if (shared) {
      // A shared mount with exactly one co-occupant slot pair.
      const std::string klass = "bay-" + std::to_string(index);
      bundle.shared_asset(id, interval.value().to_text(), klass, 2);
      shared_placed = true;
    } else {
      bundle.asset(id, interval.value().to_text());
    }
    const PresenceState presence =
        rng.chance(12) ? PresenceState::Indeterminate
                       : (rng.chance(8) ? PresenceState::Absent : PresenceState::Present);
    bundle.asset_presence(id, presence);
    const bool known_draw = rng.chance(70);
    const bool known_heat = rng.chance(50);
    const bool known_mass = rng.chance(70);
    const std::int64_t draw = static_cast<std::int64_t>(rng.below(3000));
    const std::int64_t heat = static_cast<std::int64_t>(rng.below(3000));
    const std::int64_t mass = static_cast<std::int64_t>(rng.below(40000));
    if (presence == PresenceState::Present) {
      generated.model.mark(generated.model.occupied, interval.value());
    } else if (presence == PresenceState::Indeterminate) {
      generated.model.mark(generated.model.uncertain, interval.value());
    }
    if (presence != PresenceState::Absent) {
      if (known_draw) {
        bundle.asset_draw(id, draw);
      }
      if (known_heat) {
        bundle.asset_heat(id, heat);
      }
      if (known_mass) {
        bundle.asset_mass(id, mass);
      }
    }
    if (presence == PresenceState::Present) {
      if (known_draw) {
        generated.committed_draw += draw;
      } else {
        ++generated.unknown_draw;
      }
      if (known_heat) {
        generated.declared_heat += heat;
      } else if (known_draw) {
        generated.derived_heat += draw;
      } else {
        ++generated.unknown_heat;
      }
      if (known_mass) {
        generated.occupied_mass += mass;
      } else {
        ++generated.unknown_mass;
      }
    } else if (presence == PresenceState::Indeterminate) {
      ++generated.unknown_draw;
      ++generated.unknown_heat;
      ++generated.unknown_mass;
    }
  }

  // Reservations.
  const std::uint32_t reservation_count = static_cast<std::uint32_t>(rng.below(4));
  for (std::uint32_t index = 0; index < reservation_count; ++index) {
    const std::string id = "r-" + std::to_string(2000u + index);
    const ReservationState state = rng.chance(50)   ? ReservationState::Committed
                                   : rng.chance(50) ? ReservationState::Submitted
                                                    : ReservationState::Released;
    bundle.reservation(id, state);
    const std::uint32_t begin = 1u + static_cast<std::uint32_t>(rng.below(units * 2u));
    const std::uint32_t end = std::min(begin + 1u + static_cast<std::uint32_t>(rng.below(3)), extent);
    if (end <= begin) {
      continue;
    }
    const Result<SlotInterval> interval = SlotInterval::create(begin, end);
    if (!interval.has_value()) {
      continue;
    }
    std::string span_text = interval.value().to_text();
    bundle.reservation_span(id, span_text);
    const std::int64_t draw = static_cast<std::int64_t>(rng.below(800));
    const std::int64_t heat = static_cast<std::int64_t>(rng.below(800));
    const std::int64_t mass = static_cast<std::int64_t>(rng.below(20000));
    if (rng.chance(70)) {
      bundle.reservation_draw(id, draw);
      if (state == ReservationState::Committed) {
        generated.reserved_draw += draw;
      }
    }
    if (rng.chance(60)) {
      bundle.reservation_heat(id, heat);
      if (state == ReservationState::Committed) {
        generated.reserved_heat += heat;
      }
    }
    if (rng.chance(60)) {
      bundle.reservation_mass(id, mass);
      if (state == ReservationState::Committed) {
        generated.reserved_mass += mass;
      }
    }
    if (state == ReservationState::Committed) {
      generated.model.mark(generated.model.reserved, interval.value());
    }
  }

  // Envelopes and policy.
  const bool has_power = rng.chance(85);
  const bool has_cooling = rng.chance(85);
  const bool has_weight = rng.chance(85);
  if (!has_power) {
    bundle.no_power();
  } else {
    const std::uint32_t feeds = 1u + static_cast<std::uint32_t>(rng.below(4));
    const RedundancyMode mode =
        feeds == 1 ? RedundancyMode::None
                   : (rng.chance(50) ? RedundancyMode::N1 : RedundancyMode::N2);
    bundle.power(feeds, 2000 + static_cast<std::int64_t>(rng.below(9000)), mode,
                 static_cast<std::int64_t>(rng.below(3000)));
  }
  if (!has_cooling) {
    bundle.no_cooling();
  } else {
    bundle.cooling(5000 + static_cast<std::int64_t>(rng.below(30000)),
                   static_cast<std::int64_t>(rng.below(2000)));
  }
  if (!has_weight) {
    bundle.no_weight();
  } else {
    bundle.weight(100000 + static_cast<std::int64_t>(rng.below(900000)),
                  static_cast<std::int64_t>(rng.below(1000)));
  }
  bundle.heat_equivalence(1'000'000);
  bundle.power_headroom(static_cast<std::int64_t>(rng.below(500)));
  bundle.cooling_headroom(static_cast<std::int64_t>(rng.below(500)));
  bundle.weight_headroom(static_cast<std::int64_t>(rng.below(5000)));
  bundle.slot_headroom(static_cast<std::uint32_t>(rng.below(4)));
  bundle.power_policy_derate(static_cast<std::int64_t>(rng.below(500)));
  bundle.cooling_policy_derate(static_cast<std::int64_t>(rng.below(500)));
  bundle.weight_policy_derate(static_cast<std::int64_t>(rng.below(500)));
  bundle.epoch(1);
  bundle.request("req-gen");

  generated.inputs = bundle.build();
  generated.span_slots_for_probe = 2u * (1u + static_cast<std::uint32_t>(rng.below(2)));
  return generated;
}

// Reference power envelope arithmetic, written directly from the evidence.
std::int64_t reference_power_usable(const RackCapacityInputs& inputs) {
  if (!inputs.power.has_value()) {
    return -1;
  }
  const PowerCapacityEvidence& power = inputs.power.value();
  std::int64_t multiplier = power.feed_count;
  if (power.redundancy == RedundancyMode::N1) {
    multiplier = power.feed_count - 1;
  } else if (power.redundancy == RedundancyMode::N2) {
    multiplier = 1;
  }
  const std::int64_t nominal = power.watts_per_feed.value() * multiplier;
  const std::int64_t physical = nominal * (10000 - power.derate.value()) / 10000;
  const std::int64_t policy = physical * (10000 - inputs.policy.headroom.power_derate.value()) / 10000;
  const std::int64_t derived = policy - inputs.policy.headroom.power_headroom.value();
  return derived > 0 ? derived : 0;
}

}  // namespace

RC_TEST(property_slot_accounting_matches_an_independent_model) {
  for (std::uint64_t seed = 1; seed <= 250; ++seed) {
    const Generated generated = generate(seed);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        generated.inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    if (!snapshot.has_value()) {
      std::cout << "    seed " << seed << " failed to evaluate: "
                << describe(snapshot.error()) << "\n";
      RC_CHECK(false);
      continue;
    }
    const std::uint32_t headroom = generated.inputs.policy.headroom.slot_headroom;
    const std::vector<bool> lower = generated.model.free_lower(headroom);
    const std::vector<bool> upper = generated.model.free_upper(headroom);
    std::uint32_t lower_count = 0;
    std::uint32_t upper_count = 0;
    for (std::uint32_t slot = 1; slot < generated.model.extent; ++slot) {
      lower_count += lower[slot] ? 1u : 0u;
      upper_count += upper[slot] ? 1u : 0u;
      if (snapshot.value().slots.free_lower_set.contains_slot(slot) != lower[slot]) {
        std::cout << "    seed " << seed << " slot " << slot << " disagrees on the free set\n";
        RC_CHECK(false);
        break;
      }
      if (snapshot.value().slots.free_upper_set.contains_slot(slot) != upper[slot]) {
        std::cout << "    seed " << seed << " slot " << slot << " disagrees on the free upper set\n";
        RC_CHECK(false);
        break;
      }
    }
    RC_CHECK_EQ(snapshot.value().slots.free.lower(), lower_count);
    RC_CHECK_EQ(snapshot.value().slots.free.upper(), upper_count);
    RC_CHECK_EQ(snapshot.value().slots.occupied_slots,
                generated.model.count(generated.model.occupied));

    // Fragmentation, measured against the same model.
    std::uint32_t runs = 0;
    std::uint32_t largest = 0;
    std::uint32_t current = 0;
    for (std::uint32_t slot = 1; slot < generated.model.extent; ++slot) {
      if (lower[slot]) {
        if (current == 0) {
          ++runs;
        }
        ++current;
        largest = std::max(largest, current);
      } else {
        current = 0;
      }
    }
    RC_CHECK_EQ(snapshot.value().slots.fragmentation.free_run_count, runs);
    RC_CHECK_EQ(snapshot.value().slots.fragmentation.largest_free_run_slots, largest);

    const ClosureReport closure = verify_closure(snapshot.value());
    if (!closure.holds) {
      std::cout << "    seed " << seed << " closure:\n" << closure.to_text();
      RC_CHECK(false);
    }

    const Result<RackCapacitySnapshot> again = evaluate_capacity(
        generated.inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    RC_REQUIRE_OK(again);
    RC_CHECK_EQ(again.value().digest, snapshot.value().digest);
  }
}

RC_TEST(property_envelope_accounting_matches_direct_arithmetic) {
  for (std::uint64_t seed = 251; seed <= 500; ++seed) {
    const Generated generated = generate(seed);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        generated.inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    RC_REQUIRE_OK(snapshot);
    const RackCapacitySnapshot& value = snapshot.value();

    if (!generated.inputs.power.has_value()) {
      RC_CHECK(!value.power.free.is_known());
    } else {
      const std::int64_t usable = reference_power_usable(generated.inputs);
      RC_CHECK_EQ(value.power.usable->value(), usable);
      RC_CHECK_EQ(value.power.committed_known.value(), generated.committed_draw);
      RC_CHECK_EQ(value.power.reserved_known.value(), generated.reserved_draw);
      RC_CHECK_EQ(value.power.unknown_draw_assets, generated.unknown_draw);
      const std::int64_t claims = generated.committed_draw + generated.reserved_draw;
      const std::int64_t expected_upper = usable > claims ? usable - claims : 0;
      RC_CHECK_EQ(value.power.free.upper().value(), expected_upper);
      RC_CHECK_EQ(value.power.free.lower().value(),
                  generated.unknown_draw > 0 ? 0 : expected_upper);
      RC_CHECK_EQ(value.power.overcommit.value(),
                  claims > usable ? claims - usable : 0);
    }

    if (generated.inputs.cooling.has_value()) {
      RC_CHECK_EQ(value.cooling.declared_heat_known.value(), generated.declared_heat);
      RC_CHECK_EQ(value.cooling.derived_heat_known.value(), generated.derived_heat);
      RC_CHECK_EQ(value.cooling.reserved_heat_known.value(), generated.reserved_heat);
      RC_CHECK_EQ(value.cooling.unknown_heat_assets, generated.unknown_heat);
    }

    if (generated.inputs.weight.has_value() &&
        generated.inputs.weight->static_load_limit.has_value()) {
      RC_CHECK_EQ(value.weight.occupied_known.value(), generated.occupied_mass);
      RC_CHECK_EQ(value.weight.reserved_known.value(), generated.reserved_mass);
      RC_CHECK_EQ(value.weight.unknown_mass_assets, generated.unknown_mass);
    }

    // An unknown never becomes free capacity.
    if (value.unknown.unknown_power_consumption || value.unknown.unknown_heat ||
        value.unknown.unknown_mass) {
      if (value.power.free.is_known() && generated.unknown_draw > 0) {
        RC_CHECK_EQ(value.power.free.lower().value(), 0);
      }
    }
  }
}

RC_TEST(property_adding_load_never_increases_free_capacity) {
  for (std::uint64_t seed = 501; seed <= 700; ++seed) {
    const Generated generated = generate(seed);
    const Result<RackCapacitySnapshot> before = evaluate_capacity(
        generated.inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    RC_REQUIRE_OK(before);

    // Find a free, unit-aligned position for one more occupant.
    const SlotInterval* candidate = nullptr;
    for (const SlotInterval& interval : before.value().slots.free_lower_set.intervals()) {
      if (interval.slot_count() >= 2 && interval.starts_at_unit_boundary()) {
        candidate = &interval;
        break;
      }
    }
    if (candidate == nullptr) {
      continue;
    }
    const Result<SlotInterval> chosen =
        SlotInterval::create(candidate->begin(), candidate->begin() + 2u);
    RC_REQUIRE_OK(chosen);

    // The bundle is the same bundle with one more occupant. Only the epoch and
    // the request identity move, so any change in free capacity is caused by
    // the occupant and nothing else.
    RackCapacityInputs extended = generated.inputs;
    extended.epoch = EvidenceEpoch::trusted(generated.inputs.epoch.value() + 1);
    extended.request = RequestId::create("req-gen-2").value();
    AssetOccupancyEvidence extra;
    extra.asset = AssetId::create("a-extra").value();
    extra.span = chosen.value();
    extra.kind = MountSpanKind::FullSpan;
    extra.presence = PresenceState::Present;
    extra.nameplate_draw = Watts::trusted(100);
    extra.declared_heat_rejection = Watts::trusted(100);
    extra.mass = Grams::trusted(1000);
    extra.reference.source = EvidenceSource::AssetRegistry;
    extra.reference.evidence = EvidenceId::create("ev-asset-extra").value();
    extra.reference.version = 1;
    extra.reference.observed_at = TimestampNs::trusted(kNow);
    extended.assets.push_back(extra);

    const Status canonical = canonicalize_inputs(extended);
    RC_REQUIRE(canonical.has_value());
    const Status valid = validate_inputs(extended);
    if (!valid.has_value()) {
      std::cout << "    seed " << seed << " extended bundle invalid: "
                << describe(valid.error()) << "\n";
      RC_CHECK(false);
      continue;
    }
    const Result<RackCapacitySnapshot> after = evaluate_capacity(
        extended, CapacityGeneration::trusted(2), SnapshotRevision::trusted(2),
        TimestampNs::trusted(kNow + 1), EvidenceFreshness::Fresh);
    RC_REQUIRE_OK(after);

    if (after.value().slots.free.upper() > before.value().slots.free.upper()) {
      std::cout << "    seed " << seed << " slot free upper grew from "
                << before.value().slots.free.upper() << " to "
                << after.value().slots.free.upper() << "\n";
    }
    RC_CHECK(after.value().slots.free.upper() <= before.value().slots.free.upper());
    RC_CHECK(after.value().slots.free.lower() <= before.value().slots.free.lower());
    RC_CHECK(after.value().slots.occupied_slots >= before.value().slots.occupied_slots);

    if (before.value().power.free.is_known() && after.value().power.free.is_known()) {
      RC_CHECK(after.value().power.free.upper().value() <=
               before.value().power.free.upper().value());
    }
    // When both readings are exact and the envelope was not already exhausted,
    // the added nameplate draw must reduce free power by exactly that amount.
    if (before.value().power.free.is_known() && after.value().power.free.is_known() &&
        before.value().power.unknown_draw_assets == 0 &&
        after.value().power.unknown_draw_assets == 0 &&
        before.value().power.overcommit.value() == 0) {
      RC_CHECK_EQ(after.value().power.free.upper().value(),
                  before.value().power.free.upper().value() - 100);
    }
    if (before.value().weight.free.is_known() && after.value().weight.free.is_known()) {
      RC_CHECK(after.value().weight.free.upper().value() <=
               before.value().weight.free.upper().value());
    }
    RC_CHECK(verify_closure(after.value()).holds);
  }
}
RC_TEST(property_fit_agrees_with_the_free_sets_it_reports) {
  for (std::uint64_t seed = 701; seed <= 900; ++seed) {
    const Generated generated = generate(seed);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        generated.inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    RC_REQUIRE_OK(snapshot);

    const std::uint32_t span = generated.span_slots_for_probe;
    const std::uint32_t first_unit = 1u + static_cast<std::uint32_t>(seed % 2u);
    if (first_unit * 2u + span > snapshot.value().slot_extent) {
      continue;
    }
    const Result<SlotInterval> interval = SlotInterval::create(first_unit * 2u - 1u,
                                                               first_unit * 2u - 1u + span);
    RC_REQUIRE_OK(interval);

    FitRequest request;
    request.expected = CapacityExpectation::of(snapshot.value());
    request.span = interval.value();
    const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
    RC_REQUIRE_OK(evaluation);

    const bool in_lower = snapshot.value().slots.free_lower_set.contains(interval.value());
    const bool in_upper = snapshot.value().slots.free_upper_set.contains(interval.value());
    const FitVerdict expected = in_lower ? FitVerdict::Fits
                                         : (in_upper ? FitVerdict::Indeterminate
                                                     : FitVerdict::DoesNotFit);
    if (evaluation.value().constraints[0].verdict != expected) {
      std::cout << "    seed " << seed << " span " << interval.value().to_text()
                << " slot verdict disagreed\n";
    }
    RC_CHECK(evaluation.value().constraints[0].verdict == expected);

    // The verdict is always the worst of its constraints.
    FitVerdict combined = FitVerdict::Fits;
    for (const ConstraintOutcome& outcome : evaluation.value().constraints) {
      combined = combine_verdicts(combined, outcome.verdict);
    }
    RC_CHECK(evaluation.value().verdict == combined);
  }
}

RC_TEST(property_catalog_survives_a_random_mutation_sequence) {
  for (std::uint64_t seed = 901; seed <= 930; ++seed) {
    rctest::Rng rng(seed);
    rctest::CatalogFixture fixture("property-catalog");
    std::uint64_t epoch = 1;
    std::uint64_t request_counter = 0;
    std::uint64_t composition = 1;
    std::vector<std::string> racks;
    for (std::uint32_t step = 0; step < 24; ++step) {
      const std::uint32_t choice = static_cast<std::uint32_t>(rng.below(100));
      const std::string rack = "rack-" + std::to_string(rng.below(3));
      const bool exists = fixture.catalog->contains_rack(RackId::create(rack).value());
      if (choice < 40 && !exists) {
        ++epoch;
        ++request_counter;
        const Result<MutationReceipt> receipt = fixture.catalog->register_rack(
            RegisterRackRequest{rctest::Bundle(rack, 24)
                                    .composition_generation(composition)
                                    .epoch(epoch)
                                    .request("req-" + std::to_string(request_counter))
                                    .asset_units("a-0001", 1, 1)
                                    .asset_draw("a-0001", 100)
                                    .build(),
                                TimestampNs::trusted(kNow + static_cast<std::int64_t>(step))});
        if (receipt.has_value()) {
          racks.push_back(rack);
        }
      } else if (exists) {
        const Result<RackCapacityRecord> record = fixture.catalog->record(RackId::create(rack).value());
        RC_REQUIRE_OK(record);
        if (choice < 80) {
          ++epoch;
          ++request_counter;
          if (rng.chance(30)) {
            ++composition;
          }
          const Result<MutationReceipt> receipt = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
              CapacityExpectation::of(record.value()),
              rctest::Bundle(rack, 24)
                  .composition_generation(composition)
                  .epoch(epoch)
                  .request("req-" + std::to_string(request_counter))
                  .asset_units("a-0001", 1, 1)
                  .asset_draw("a-0001", 100 + static_cast<std::int64_t>(rng.below(500)))
                  .build(),
              TimestampNs::trusted(kNow + static_cast<std::int64_t>(step) + 1)});
          // A random sequence may legitimately be refused: the rack may have
          // been retired, or the evidence may have been planned against an
          // older composition. Anything else would be a defect.
          RC_CHECK(receipt.has_value() ||
                   receipt.error().code == ErrorCode::StaleCompositionGeneration ||
                   receipt.error().code == ErrorCode::StaleEvidenceEpoch ||
                   receipt.error().code == ErrorCode::StaleCapacityGeneration ||
                   receipt.error().code == ErrorCode::StaleSnapshotRevision ||
                   receipt.error().code == ErrorCode::RackRetired ||
                   receipt.error().code == ErrorCode::LifecycleMutationForbidden);
        } else {
          ++request_counter;
          RetireRackRequest retire;
          retire.expected = CapacityExpectation::of(record.value());
          retire.request = RequestId::create("req-" + std::to_string(request_counter)).value();
          retire.actor = ActorId::create("operator-1").value();
          retire.requested_at = TimestampNs::trusted(kNow + static_cast<std::int64_t>(step) + 2);
          const Result<MutationReceipt> receipt = fixture.catalog->retire_rack(retire);
          RC_CHECK(receipt.has_value() ||
                   receipt.error().code == ErrorCode::LifecycleTransitionNotAllowed);
        }
      }

      // Whatever happened, every record the catalog holds must still close
      // exactly and be quotable or explicitly barred from quotation.
      for (const RackCapacityRecord& record : fixture.catalog->records()) {
        RC_CHECK(verify_closure(record.snapshot).holds);
        RC_CHECK_EQ(record.snapshot.digest, compute_snapshot_digest(record.snapshot));
        if (record.quotes_capacity()) {
          const Result<RackCapacitySnapshot> quoted =
              fixture.catalog->snapshot(CapacityExpectation::of(record));
          RC_CHECK(quoted.has_value());
        }
      }
    }
    RC_CHECK_EQ(fixture.catalog->stats().rack_count, racks.size());
  }
}
