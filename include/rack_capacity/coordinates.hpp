// Rack Capacity - mount slot coordinates, exact intervals and interval sets.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/result.hpp"
#include "rack_capacity/version.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Coordinate model
// ---------------------------------------------------------------------------
//
// Vertical position inside a rack is expressed in mount slots. One rack unit
// spans exactly kMountSlotsPerRackUnit (2) mount slots, so the coordinate space
// resolves half-unit mountings without resorting to floating point:
//
//   rack unit U  <->  mount slots { 2U-1, 2U }
//
// Slot numbers are 1-based; the lower half of unit U is slot 2U-1 and the upper
// half is slot 2U.
//
// This is deliberately the same coordinate space the Rack Registry runtime
// publishes, so a composition fact expressed there is carried into capacity
// accounting without reinterpretation. Rack Registry remains authoritative for
// composition; this library only accounts for capacity over those coordinates.
//
// EVERY interval in this library is half open: [begin, end) contains begin and
// excludes end. Two intervals are disjoint when one begins at or after the
// other's end, so adjacent intervals such as [1,3) and [3,5) never overlap.

// One 1-based rack unit index, valid from 1 to kMaxRackUnits.
class RackUnitIndex {
 public:
  constexpr RackUnitIndex() noexcept = default;
  constexpr explicit RackUnitIndex(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<RackUnitIndex> create(std::uint64_t value);
  [[nodiscard]] static Result<RackUnitIndex> parse(std::string_view text);

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return value_ >= 1 && value_ <= kMaxRackUnits;
  }
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const RackUnitIndex& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const RackUnitIndex& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const RackUnitIndex& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const RackUnitIndex& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const RackUnitIndex& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const RackUnitIndex& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  std::uint32_t value_ = 0;
};

// A non-empty half-open interval of mount slots: [begin, end).
class SlotInterval {
 public:
  constexpr SlotInterval() noexcept = default;

  [[nodiscard]] static Result<SlotInterval> create(std::uint64_t begin, std::uint64_t end);
  [[nodiscard]] static Result<SlotInterval> single(std::uint64_t slot);
  // A device `unit_count` rack units tall starting at rack unit `first_unit`.
  [[nodiscard]] static Result<SlotInterval> for_units(std::uint64_t first_unit,
                                                      std::uint64_t unit_count);

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return begin_ >= 1 && end_ > begin_ && end_ <= kMaxMountSlotExclusive;
  }
  [[nodiscard]] constexpr std::uint32_t begin() const noexcept { return begin_; }
  [[nodiscard]] constexpr std::uint32_t end() const noexcept { return end_; }
  [[nodiscard]] constexpr std::uint32_t slot_count() const noexcept {
    return end_ > begin_ ? end_ - begin_ : 0u;
  }
  [[nodiscard]] constexpr std::uint32_t first_unit() const noexcept {
    return (begin_ + 1u) / kMountSlotsPerRackUnit;
  }
  [[nodiscard]] constexpr std::uint32_t last_unit() const noexcept {
    return end_ > 0 ? (end_ - 1u + 1u) / kMountSlotsPerRackUnit : 0u;
  }
  // True when the interval starts at the lower half of a rack unit.
  [[nodiscard]] constexpr bool starts_at_unit_boundary() const noexcept {
    return begin_ % kMountSlotsPerRackUnit == 1u;
  }

  [[nodiscard]] constexpr bool contains_slot(std::uint32_t slot) const noexcept {
    return slot >= begin_ && slot < end_;
  }
  [[nodiscard]] constexpr bool contains(const SlotInterval& other) const noexcept {
    return begin_ <= other.begin_ && other.end_ <= end_;
  }
  [[nodiscard]] constexpr bool overlaps(const SlotInterval& other) const noexcept {
    return begin_ < other.end_ && other.begin_ < end_;
  }

  // Human-facing inclusive form, for example "[1,3)".
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const SlotInterval& other) const noexcept {
    return begin_ == other.begin_ && end_ == other.end_;
  }
  [[nodiscard]] constexpr bool operator!=(const SlotInterval& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] constexpr bool operator<(const SlotInterval& other) const noexcept {
    return begin_ != other.begin_ ? begin_ < other.begin_ : end_ < other.end_;
  }

 private:
  std::uint32_t begin_ = 0;
  std::uint32_t end_ = 0;
};

// A normalized set of disjoint, non-adjacent mount slot intervals in ascending
// order. Normalization is total: any construction path produces exactly one
// representation of a given set of slots, which is what makes interval
// accounting, fragmentation and diffs reproducible.
class SlotSet {
 public:
  SlotSet() = default;

  [[nodiscard]] static SlotSet empty() noexcept { return SlotSet{}; }
  [[nodiscard]] static Result<SlotSet> single(SlotInterval interval);
  [[nodiscard]] static Result<SlotSet> from_intervals(std::vector<SlotInterval> intervals);

  // Union with one more interval. The result replaces *this on success and is
  // unchanged on failure.
  [[nodiscard]] Status add(SlotInterval interval);

  [[nodiscard]] Result<SlotSet> union_with(const SlotSet& other) const;
  [[nodiscard]] Result<SlotSet> subtract(const SlotSet& other) const;
  [[nodiscard]] Result<SlotSet> intersect(const SlotSet& other) const;
  // Every slot in [1, extent_end) that is not in this set.
  [[nodiscard]] Result<SlotSet> complement(std::uint32_t extent_end) const;

  [[nodiscard]] bool is_empty() const noexcept { return intervals_.empty(); }
  [[nodiscard]] bool contains_slot(std::uint32_t slot) const noexcept;
  [[nodiscard]] bool contains(const SlotInterval& interval) const noexcept;
  [[nodiscard]] bool intersects(const SlotSet& other) const noexcept;

  [[nodiscard]] std::uint64_t slot_count() const noexcept;
  [[nodiscard]] std::size_t interval_count() const noexcept { return intervals_.size(); }
  // Number of maximal contiguous runs. Identical to interval_count for a
  // normalized set; provided under the name the fragmentation report uses.
  [[nodiscard]] std::uint64_t run_count() const noexcept;
  [[nodiscard]] std::uint64_t largest_run_slots() const noexcept;
  [[nodiscard]] std::uint64_t smallest_run_slots() const noexcept;
  // Slots that belong to a run of exactly one slot.
  [[nodiscard]] std::uint64_t isolated_slot_count() const noexcept;

  // Number of positions at which an interval of `span_slots` slots fits inside
  // this set. When `require_unit_alignment` is set, only positions that start
  // at the lower half of a rack unit are counted. This is the fragmentation
  // measure used by fit evaluation; it is a count, never a choice, because this
  // library has no placement authority.
  [[nodiscard]] std::uint64_t count_anchors(std::uint32_t span_slots,
                                            bool require_unit_alignment) const noexcept;
  // Number of maximal runs of at least `span_slots` slots.
  [[nodiscard]] std::uint64_t count_fitting_runs(std::uint32_t span_slots) const noexcept;

  [[nodiscard]] const std::vector<SlotInterval>& intervals() const noexcept {
    return intervals_;
  }

  [[nodiscard]] bool operator==(const SlotSet& other) const noexcept {
    return intervals_ == other.intervals_;
  }
  [[nodiscard]] bool operator!=(const SlotSet& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] bool operator<(const SlotSet& other) const noexcept {
    return intervals_ < other.intervals_;
  }

  // Canonical text form: "{}" for the empty set, otherwise comma separated
  // intervals such as "[1,3),[5,7)".
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] static Result<SlotSet> parse(std::string_view text);

 private:
  std::vector<SlotInterval> intervals_{};
};

// Validates a rack unit count, which is the physical height of a rack.
[[nodiscard]] RACK_CAPACITY_API Result<std::uint32_t> validate_unit_count(std::uint64_t unit_count);

// The exclusive upper slot bound of a rack with `unit_count` rack units.
[[nodiscard]] RACK_CAPACITY_API std::uint32_t slot_extent_of(std::uint32_t unit_count) noexcept;

// The slot interval covering the whole rack with `unit_count` rack units.
[[nodiscard]] RACK_CAPACITY_API Result<SlotInterval> rack_slot_extent(
    std::uint32_t unit_count);

// ---------------------------------------------------------------------------
// Mount classification
// ---------------------------------------------------------------------------

// How an occupant is mounted. The names and meanings match the published Rack
// Registry coordinate model so that composition evidence maps onto capacity
// accounting without translation.
enum class MountSpanKind : std::uint8_t {
  // Exclusive occupation of the slot interval.
  FullSpan = 0,
  // Occupation that may legally be shared with other occupants declaring the
  // same shared class over exactly the same slot interval, up to the declared
  // share capacity.
  SharedSpan = 1,
  // Mounted outside the unit space (side or rear rail). Occupies no slot.
  ZeroU = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view mount_span_kind_name(
    MountSpanKind kind) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<MountSpanKind> parse_mount_span_kind(
    std::string_view text);

// Identity of one shared mount class, for example a PSU bay or a shelf with two
// half-width positions. The identity is opaque text owned by rack composition
// authority; this library only groups by it.
struct SharedMountClassIdTag {
  static constexpr std::string_view kKind = "shared_class";
};
using SharedMountClassId = Identity<SharedMountClassIdTag>;

// Whether an occupant is physically present. `Indeterminate` is not a synonym
// for absent: an occupant whose presence cannot be established is never treated
// as free capacity.
enum class PresenceState : std::uint8_t {
  Present = 0,
  Absent = 1,
  Indeterminate = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view presence_state_name(
    PresenceState state) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<PresenceState> parse_presence_state(
    std::string_view text);

}  // namespace rackcapacity
