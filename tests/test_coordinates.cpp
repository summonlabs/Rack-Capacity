// Rack Capacity - slot coordinate and interval set tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>
#include <iostream>
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

// Independent model of a slot set: one boolean per slot, with the same
// operations expressed as direct per-slot marking. It shares no code with
// SlotSet, so agreement between the two is evidence rather than tautology.
struct SlotBitmap {
  std::vector<bool> marked;

  explicit SlotBitmap(std::uint32_t extent_end) : marked(extent_end + 1u, false) {}

  void add(std::uint32_t begin, std::uint32_t end) {
    for (std::uint32_t slot = begin; slot < end && slot < marked.size(); ++slot) {
      marked[slot] = true;
    }
  }
  void remove(std::uint32_t begin, std::uint32_t end) {
    for (std::uint32_t slot = begin; slot < end && slot < marked.size(); ++slot) {
      marked[slot] = false;
    }
  }
  [[nodiscard]] std::uint64_t count() const {
    std::uint64_t total = 0;
    for (std::size_t slot = 1; slot < marked.size(); ++slot) {
      if (marked[slot]) {
        ++total;
      }
    }
    return total;
  }
  [[nodiscard]] std::uint64_t runs() const {
    std::uint64_t total = 0;
    for (std::size_t slot = 1; slot < marked.size(); ++slot) {
      if (marked[slot] && !marked[slot - 1]) {
        ++total;
      }
    }
    return total;
  }
  [[nodiscard]] std::uint64_t largest_run() const {
    std::uint64_t best = 0;
    std::uint64_t current = 0;
    for (std::size_t slot = 1; slot < marked.size(); ++slot) {
      current = marked[slot] ? current + 1 : 0;
      best = std::max(best, current);
    }
    return best;
  }
  [[nodiscard]] std::uint64_t anchors(std::uint32_t span, bool aligned) const {
    std::uint64_t total = 0;
    for (std::size_t start = 1; start + span < marked.size(); ++start) {
      if (aligned && (start % 2u) != 1u) {
        continue;
      }
      bool fits = true;
      for (std::uint32_t offset = 0; offset < span; ++offset) {
        if (!marked[start + offset]) {
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

void compare_with_model(const SlotSet& set, const SlotBitmap& model, std::uint32_t extent,
                        const char* context) {
  for (std::uint32_t slot = 1; slot < extent; ++slot) {
    const bool in_set = set.contains_slot(slot);
    RC_CHECK_EQ(in_set, model.marked[slot]);
    if (in_set != model.marked[slot]) {
      std::cout << "    diverged at slot " << slot << " in " << context << "\n";
      return;
    }
  }
  RC_CHECK_EQ(set.slot_count(), model.count());
  RC_CHECK_EQ(set.run_count(), model.runs());
  RC_CHECK_EQ(set.largest_run_slots(), model.largest_run());
}

}  // namespace

RC_TEST(interval_construction_rejects_every_invalid_shape) {
  RC_CHECK(!SlotInterval::create(0, 1).has_value());
  RC_CHECK(!SlotInterval::create(1, 1).has_value());
  RC_CHECK(!SlotInterval::create(5, 4).has_value());
  RC_CHECK(!SlotInterval::create(1, kMaxMountSlotExclusive + 1u).has_value());
  RC_CHECK(SlotInterval::create(1, 2).has_value());
  RC_CHECK(SlotInterval::create(1, kMaxMountSlotExclusive).has_value());

  const Result<SlotInterval> whole = SlotInterval::for_units(3, 2);
  RC_REQUIRE_OK(whole);
  RC_CHECK_EQ(whole.value().begin(), 5u);
  RC_CHECK_EQ(whole.value().end(), 9u);
  RC_CHECK_EQ(whole.value().first_unit(), 3u);
  RC_CHECK_EQ(whole.value().last_unit(), 4u);
  RC_CHECK(whole.value().starts_at_unit_boundary());

  const Result<SlotInterval> half = SlotInterval::single(4);
  RC_REQUIRE_OK(half);
  RC_CHECK_EQ(half.value().first_unit(), 2u);
  RC_CHECK_EQ(half.value().last_unit(), 2u);
  RC_CHECK(!half.value().starts_at_unit_boundary());
}

RC_TEST(rack_extent_matches_declared_height) {
  const Result<SlotInterval> extent = rack_slot_extent(48);
  RC_REQUIRE_OK(extent);
  RC_CHECK_EQ(extent.value().begin(), 1u);
  RC_CHECK_EQ(extent.value().end(), 97u);
  RC_CHECK_EQ(extent.value().slot_count(), 96u);
  RC_CHECK_EQ(slot_extent_of(1), 3u);
  RC_CHECK(!rack_slot_extent(0).has_value());
  RC_CHECK(!validate_unit_count(kMaxRackUnits + 1u).has_value());
  RC_CHECK(validate_unit_count(kMaxRackUnits).has_value());
}

RC_TEST(slot_set_normalizes_and_reports_exact_geometry) {
  const Result<SlotSet> set = SlotSet::parse("[5,9),[1,3),[3,5)");
  RC_REQUIRE_OK(set);
  // Adjacent intervals merge, so the three members collapse into one run, and
  // the members are reordered into ascending order.
  RC_CHECK_EQ(set.value().interval_count(), 1u);
  RC_CHECK_EQ(set.value().to_text(), std::string("[1,9)"));
  RC_CHECK_EQ(set.value().slot_count(), 8u);
  RC_CHECK_EQ(set.value().run_count(), 1u);
  RC_CHECK_EQ(set.value().largest_run_slots(), 8u);

  const Result<SlotSet> empty = SlotSet::parse("{}");
  RC_REQUIRE_OK(empty);
  RC_CHECK(empty.value().is_empty());
  RC_CHECK(!SlotSet::parse("").has_value());
  RC_CHECK(!SlotSet::parse("[1,1)").has_value());
  RC_CHECK(SlotSet::parse("[1,3)").has_value());
  RC_CHECK(!SlotSet::parse("[1,3],").has_value());
  RC_CHECK(!SlotSet::parse("1,3").has_value());
  RC_CHECK(!SlotSet::parse("[a,3)").has_value());
}

RC_TEST(slot_set_algebra_matches_set_semantics) {
  const SlotSet a = SlotSet::parse("[1,5),[9,13)").value();
  const SlotSet b = SlotSet::parse("[3,10)").value();

  const SlotSet intersection = a.intersect(b).value();
  RC_CHECK_EQ(intersection.to_text(), std::string("[3,5),[9,10)"));

  const SlotSet difference = a.subtract(b).value();
  RC_CHECK_EQ(difference.to_text(), std::string("[1,3),[10,13)"));

  const SlotSet merged = a.union_with(b).value();
  RC_CHECK_EQ(merged.to_text(), std::string("[1,13)"));

  const SlotSet complement = merged.complement(15).value();
  RC_CHECK_EQ(complement.to_text(), std::string("[13,15)"));

  RC_CHECK(!a.complement(5).has_value());
  RC_CHECK(!a.complement(0).has_value());
}

RC_TEST(anchor_counting_obeys_unit_alignment) {
  // Two free runs: slots 1..8 and slots 11..14.
  const SlotSet free = SlotSet::parse("[1,9),[11,15)").value();
  RC_CHECK_EQ(free.count_anchors(4, false), 5u + 1u);
  // Unit-aligned anchors start at an odd slot number: slot 11 is the lower half
  // of rack unit 6, so the second run does offer one aligned four-slot anchor.
  RC_CHECK_EQ(free.count_anchors(4, true), 3u + 1u);
  RC_CHECK_EQ(free.count_anchors(2, true), 4u + 2u);
  RC_CHECK_EQ(free.count_anchors(8, true), 1u);
  RC_CHECK_EQ(free.count_anchors(10, false), 0u);
  RC_CHECK_EQ(free.count_fitting_runs(4), 2u);
  RC_CHECK_EQ(free.count_fitting_runs(9), 0u);
}

RC_TEST(slot_set_property_agrees_with_independent_bitmap_model) {
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    rctest::Rng rng(seed);
    const std::uint32_t extent = 3u + static_cast<std::uint32_t>(rng.below(40));
    SlotBitmap model(extent);
    std::vector<SlotInterval> members;
    const std::uint64_t count = rng.below(10);
    for (std::uint64_t index = 0; index < count; ++index) {
      const std::uint32_t begin = 1u + static_cast<std::uint32_t>(rng.below(extent - 1u));
      const std::uint32_t end = begin + 1u + static_cast<std::uint32_t>(rng.below(extent - begin));
      const Result<SlotInterval> interval = SlotInterval::create(begin, end);
      RC_REQUIRE_OK(interval);
      members.push_back(interval.value());
      model.add(begin, end);
    }
    const Result<SlotSet> set = SlotSet::from_intervals(members);
    RC_REQUIRE_OK(set);
    compare_with_model(set.value(), model, extent, "union of random intervals");

    for (std::uint32_t span = 1; span <= 6; ++span) {
      RC_CHECK_EQ(set.value().count_anchors(span, false), model.anchors(span, false));
      RC_CHECK_EQ(set.value().count_anchors(span, true), model.anchors(span, true));
    }

    // Subtract a second random set and compare again.
    std::vector<SlotInterval> holes;
    const std::uint64_t holes_count = rng.below(6);
    for (std::uint64_t index = 0; index < holes_count; ++index) {
      const std::uint32_t begin = 1u + static_cast<std::uint32_t>(rng.below(extent - 1u));
      const std::uint32_t end = begin + 1u + static_cast<std::uint32_t>(rng.below(extent - begin));
      const Result<SlotInterval> interval = SlotInterval::create(begin, end);
      RC_REQUIRE_OK(interval);
      holes.push_back(interval.value());
      model.remove(begin, end);
    }
    const Result<SlotSet> hole_set = SlotSet::from_intervals(holes);
    RC_REQUIRE_OK(hole_set);
    const Result<SlotSet> difference = set.value().subtract(hole_set.value());
    RC_REQUIRE_OK(difference);
    compare_with_model(difference.value(), model, extent, "difference of random intervals");

    // The complement of the difference over the extent is the union of the
    // original and the holes, which the model can also state directly.
    const Result<SlotSet> complement = difference.value().complement(extent);
    RC_REQUIRE_OK(complement);
    RC_CHECK_EQ(complement.value().slot_count(),
                static_cast<std::uint64_t>(extent - 1u) - difference.value().slot_count());
  }
}

RC_TEST(slot_set_handles_the_largest_shape_a_rack_can_present) {
  // One free slot between every pair of occupied slots is the most fragmented
  // shape the coordinate space allows.
  std::vector<SlotInterval> alternating;
  for (std::uint32_t index = 0; index < kMaxRackUnits; ++index) {
    const Result<SlotInterval> interval = SlotInterval::create(1u + index * 2u, 2u + index * 2u);
    RC_REQUIRE_OK(interval);
    alternating.push_back(interval.value());
  }
  const Result<SlotSet> set = SlotSet::from_intervals(alternating);
  RC_REQUIRE_OK(set);
  RC_CHECK_EQ(set.value().interval_count(), static_cast<std::size_t>(kMaxRackUnits));
  RC_CHECK_EQ(set.value().slot_count(), static_cast<std::uint64_t>(kMaxRackUnits));
  RC_CHECK_EQ(set.value().isolated_slot_count(), static_cast<std::uint64_t>(kMaxRackUnits));
  RC_CHECK_EQ(set.value().largest_run_slots(), 1u);
}

RC_TEST(identity_text_is_validated_not_normalized) {
  RC_CHECK(RackId::create("rack-a1").has_value());
  RC_CHECK(RackId::create("rack_a1.2@site").has_value());
  RC_CHECK(!RackId::create("").has_value());
  RC_CHECK(!RackId::create("..").has_value());
  RC_CHECK(!RackId::create("a..b").has_value());
  RC_CHECK(!RackId::create(".hidden").has_value());
  RC_CHECK(!RackId::create("trailing.").has_value());
  RC_CHECK(!RackId::create("has space").has_value());
  RC_CHECK(!RackId::create("path/sep").has_value());
  RC_CHECK(!RackId::create("back\\slash").has_value());
  RC_CHECK(!RackId::create("colon:sep").has_value());
  RC_CHECK(!RackId::create(std::string(kMaxIdentityTextBytes + 1, 'a')).has_value());
  RC_CHECK(RackId::create(std::string(kMaxIdentityTextBytes, 'a')).has_value());
  RC_CHECK(!RackId::create("newline\nhere").has_value());
  RC_CHECK(!RackId::create("\xc3\xa9").has_value());

  // Identity families never convert into one another.
  const RackId rack = RackId::create("rack-a1").value();
  static_assert(!std::is_convertible_v<AssetId, RackId>);
  static_assert(!std::is_convertible_v<RackId, AssetId>);
  RC_CHECK_EQ(rack.text(), std::string("rack-a1"));
}
