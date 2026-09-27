// Rack Capacity - mount slot coordinates and interval set algebra.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/coordinates.hpp"

#include <algorithm>
#include <string>

namespace rackcapacity {
namespace {

[[nodiscard]] Result<std::uint32_t> parse_positive_integer(std::string_view text,
                                                           std::string_view field) {
  if (text.empty()) {
    return make_error(ErrorCode::EmptyValue, "expected a decimal integer",
                      ErrorDetail{"parse.integer", std::string(field), {}, 0, 0, {}});
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return make_error(ErrorCode::InvalidCharacter,
                        "expected a decimal integer without sign or separator",
                        ErrorDetail{"parse.integer", std::string(field), {}, 0, 0, {}});
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (0xFFFFFFFFull - digit) / 10ull) {
      return make_error(ErrorCode::InvalidRange, "integer is too large to represent",
                        ErrorDetail{"parse.integer", std::string(field), {}, 0, 0, {}});
    }
    value = value * 10ull + digit;
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// RackUnitIndex
// ---------------------------------------------------------------------------

Result<RackUnitIndex> RackUnitIndex::create(std::uint64_t value) {
  if (value < 1 || value > kMaxRackUnits) {
    return make_error(ErrorCode::InvalidRange, "rack unit index is outside the rack extent",
                      ErrorDetail{"unit.create", {}, {}, kMaxRackUnits, value, {}});
  }
  return RackUnitIndex(static_cast<std::uint32_t>(value));
}

Result<RackUnitIndex> RackUnitIndex::parse(std::string_view text) {
  std::string_view digits = text;
  if (digits.size() >= 2 && (digits.front() == 'U' || digits.front() == 'u')) {
    digits.remove_prefix(1);
  }
  const Result<std::uint32_t> parsed = parse_positive_integer(digits, "unit");
  if (!parsed.has_value()) {
    return parsed.error();
  }
  return create(parsed.value());
}

std::string RackUnitIndex::to_text() const {
  std::string text = "U";
  text += std::to_string(value_);
  return text;
}

// ---------------------------------------------------------------------------
// SlotInterval
// ---------------------------------------------------------------------------

Result<SlotInterval> SlotInterval::create(std::uint64_t begin, std::uint64_t end) {
  if (begin < 1) {
    return make_error(ErrorCode::InvalidSlotInterval,
                      "a slot interval must begin at slot 1 or above",
                      ErrorDetail{"interval.create", {}, {}, 1, begin, {}});
  }
  if (end <= begin) {
    return make_error(ErrorCode::InvalidSlotInterval,
                      "a slot interval must be non-empty; end must exceed begin",
                      ErrorDetail{"interval.create", {}, {}, begin + 1, end, {}});
  }
  if (end > kMaxMountSlotExclusive) {
    return make_error(ErrorCode::SlotOutOfBounds,
                      "a slot interval extends beyond the largest representable rack",
                      ErrorDetail{"interval.create", {}, {}, kMaxMountSlotExclusive, end, {}});
  }
  SlotInterval interval;
  interval.begin_ = static_cast<std::uint32_t>(begin);
  interval.end_ = static_cast<std::uint32_t>(end);
  return interval;
}

Result<SlotInterval> SlotInterval::single(std::uint64_t slot) {
  return create(slot, slot + 1);
}

Result<SlotInterval> SlotInterval::for_units(std::uint64_t first_unit, std::uint64_t unit_count) {
  if (unit_count < 1) {
    return make_error(ErrorCode::InvalidRange, "a unit span must cover at least one rack unit",
                      ErrorDetail{"interval.for_units", {}, {}, 1, unit_count, {}});
  }
  if (first_unit < 1 || first_unit > kMaxRackUnits) {
    return make_error(ErrorCode::InvalidRange, "first unit is outside the rack extent",
                      ErrorDetail{"interval.for_units", {}, {}, kMaxRackUnits, first_unit, {}});
  }
  const std::uint64_t begin = first_unit * kMountSlotsPerRackUnit - (kMountSlotsPerRackUnit - 1);
  const std::uint64_t end = begin + unit_count * kMountSlotsPerRackUnit;
  return create(begin, end);
}

std::string SlotInterval::to_text() const {
  std::string text = "[";
  text += std::to_string(begin_);
  text.push_back(',');
  text += std::to_string(end_);
  text.push_back(')');
  return text;
}

// ---------------------------------------------------------------------------
// SlotSet
// ---------------------------------------------------------------------------

namespace {

// Sorts and merges a raw interval list in place. Overlapping and exactly
// adjacent intervals are merged, so the result is the unique normal form of the
// covered slot set.
void normalize(std::vector<SlotInterval>& intervals) {
  if (intervals.size() < 2) {
    return;
  }
  std::sort(intervals.begin(), intervals.end());
  std::size_t write = 0;
  for (std::size_t read = 1; read < intervals.size(); ++read) {
    if (intervals[read].begin() <= intervals[write].end()) {
      if (intervals[read].end() > intervals[write].end()) {
        const Result<SlotInterval> merged =
            SlotInterval::create(intervals[write].begin(), intervals[read].end());
        if (merged.has_value()) {
          intervals[write] = merged.value();
        }
      }
    } else {
      ++write;
      intervals[write] = intervals[read];
    }
  }
  intervals.resize(write + 1);
}

}  // namespace

Result<SlotSet> SlotSet::single(SlotInterval interval) {
  if (!interval.is_valid()) {
    return make_error(ErrorCode::InvalidSlotInterval, "slot interval is not valid",
                      ErrorDetail{"slotset.single", interval.to_text(), {}, 0, 0, {}});
  }
  SlotSet set;
  set.intervals_.push_back(interval);
  return set;
}

Result<SlotSet> SlotSet::from_intervals(std::vector<SlotInterval> intervals) {
  for (const SlotInterval& interval : intervals) {
    if (!interval.is_valid()) {
      return make_error(ErrorCode::InvalidSlotInterval, "slot interval is not valid",
                        ErrorDetail{"slotset.from_intervals", interval.to_text(), {}, 0, 0, {}});
    }
  }
  normalize(intervals);
  if (intervals.size() > kMaxSlotSetIntervals) {
    return make_error(ErrorCode::SlotSetTooLarge,
                      "a slot set exceeds the documented number of intervals",
                      ErrorDetail{"slotset.from_intervals", {}, {}, kMaxSlotSetIntervals,
                                  intervals.size(), {}});
  }
  SlotSet set;
  set.intervals_ = std::move(intervals);
  return set;
}

Status SlotSet::add(SlotInterval interval) {
  if (!interval.is_valid()) {
    return Status(make_error(ErrorCode::InvalidSlotInterval, "slot interval is not valid",
                             ErrorDetail{"slotset.add", interval.to_text(), {}, 0, 0, {}}));
  }
  intervals_.push_back(interval);
  normalize(intervals_);
  if (intervals_.size() > kMaxSlotSetIntervals) {
    const CapacityError error =
        make_error(ErrorCode::SlotSetTooLarge,
                   "a slot set exceeds the documented number of intervals",
                   ErrorDetail{"slotset.add", {}, {}, kMaxSlotSetIntervals, intervals_.size(), {}});
    intervals_.clear();
    return Status(error);
  }
  return Status{};
}

Result<SlotSet> SlotSet::union_with(const SlotSet& other) const {
  std::vector<SlotInterval> combined = intervals_;
  combined.insert(combined.end(), other.intervals_.begin(), other.intervals_.end());
  return from_intervals(std::move(combined));
}

Result<SlotSet> SlotSet::subtract(const SlotSet& other) const {
  std::vector<SlotInterval> result;
  result.reserve(intervals_.size() + other.intervals_.size());
  std::size_t hint = 0;
  for (const SlotInterval& mine : intervals_) {
    std::uint32_t cursor = mine.begin();
    std::size_t index = hint;
    while (index < other.intervals_.size() && other.intervals_[index].end() <= cursor) {
      ++index;
    }
    hint = index;
    bool consumed = false;
    while (index < other.intervals_.size() && other.intervals_[index].begin() < mine.end()) {
      const SlotInterval& theirs = other.intervals_[index];
      if (theirs.begin() > cursor) {
        const Result<SlotInterval> piece = SlotInterval::create(cursor, theirs.begin());
        if (!piece.has_value()) {
          return piece.error();
        }
        result.push_back(piece.value());
      }
      if (theirs.end() > cursor) {
        cursor = theirs.end();
      }
      consumed = true;
      if (cursor >= mine.end()) {
        break;
      }
      ++index;
    }
    if (!consumed || cursor < mine.end()) {
      const Result<SlotInterval> piece = SlotInterval::create(cursor, mine.end());
      if (!piece.has_value()) {
        return piece.error();
      }
      result.push_back(piece.value());
    }
  }
  return from_intervals(std::move(result));
}

Result<SlotSet> SlotSet::intersect(const SlotSet& other) const {
  std::vector<SlotInterval> result;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < intervals_.size() && right < other.intervals_.size()) {
    const SlotInterval& mine = intervals_[left];
    const SlotInterval& theirs = other.intervals_[right];
    const std::uint32_t begin = mine.begin() > theirs.begin() ? mine.begin() : theirs.begin();
    const std::uint32_t end = mine.end() < theirs.end() ? mine.end() : theirs.end();
    if (begin < end) {
      const Result<SlotInterval> overlap = SlotInterval::create(begin, end);
      if (!overlap.has_value()) {
        return overlap.error();
      }
      result.push_back(overlap.value());
    }
    if (mine.end() <= theirs.end()) {
      ++left;
    } else {
      ++right;
    }
  }
  return from_intervals(std::move(result));
}

Result<SlotSet> SlotSet::complement(std::uint32_t extent_end) const {
  if (extent_end < 1 || extent_end > kMaxMountSlotExclusive) {
    return make_error(ErrorCode::SlotOutOfBounds, "complement extent is outside the coordinate space",
                      ErrorDetail{"slotset.complement", {}, {}, kMaxMountSlotExclusive, extent_end,
                                  {}});
  }
  std::vector<SlotInterval> result;
  std::uint32_t cursor = 1;
  for (const SlotInterval& interval : intervals_) {
    if (interval.end() > extent_end) {
      return make_error(ErrorCode::SlotOutOfBounds,
                        "a slot set member extends beyond the requested extent",
                        ErrorDetail{"slotset.complement", interval.to_text(), {}, extent_end,
                                    interval.end(), {}});
    }
    if (interval.begin() > cursor) {
      const Result<SlotInterval> gap = SlotInterval::create(cursor, interval.begin());
      if (!gap.has_value()) {
        return gap.error();
      }
      result.push_back(gap.value());
    }
    cursor = interval.end();
  }
  if (cursor < extent_end) {
    const Result<SlotInterval> tail = SlotInterval::create(cursor, extent_end);
    if (!tail.has_value()) {
      return tail.error();
    }
    result.push_back(tail.value());
  }
  return from_intervals(std::move(result));
}

bool SlotSet::contains_slot(std::uint32_t slot) const noexcept {
  for (const SlotInterval& interval : intervals_) {
    if (interval.contains_slot(slot)) {
      return true;
    }
    if (interval.begin() > slot) {
      return false;
    }
  }
  return false;
}

bool SlotSet::contains(const SlotInterval& interval) const noexcept {
  for (const SlotInterval& candidate : intervals_) {
    if (candidate.begin() <= interval.begin() && interval.end() <= candidate.end()) {
      return true;
    }
    if (candidate.begin() > interval.begin()) {
      return false;
    }
  }
  return false;
}

bool SlotSet::intersects(const SlotSet& other) const noexcept {
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < intervals_.size() && right < other.intervals_.size()) {
    if (intervals_[left].overlaps(other.intervals_[right])) {
      return true;
    }
    if (intervals_[left].end() <= other.intervals_[right].begin()) {
      ++left;
    } else {
      ++right;
    }
  }
  return false;
}

std::uint64_t SlotSet::slot_count() const noexcept {
  std::uint64_t total = 0;
  for (const SlotInterval& interval : intervals_) {
    total += interval.slot_count();
  }
  return total;
}

std::uint64_t SlotSet::run_count() const noexcept { return intervals_.size(); }

std::uint64_t SlotSet::largest_run_slots() const noexcept {
  std::uint32_t largest = 0;
  for (const SlotInterval& interval : intervals_) {
    if (interval.slot_count() > largest) {
      largest = interval.slot_count();
    }
  }
  return largest;
}

std::uint64_t SlotSet::smallest_run_slots() const noexcept {
  if (intervals_.empty()) {
    return 0;
  }
  std::uint32_t smallest = 0xFFFFFFFFu;
  for (const SlotInterval& interval : intervals_) {
    if (interval.slot_count() < smallest) {
      smallest = interval.slot_count();
    }
  }
  return smallest;
}

std::uint64_t SlotSet::isolated_slot_count() const noexcept {
  std::uint64_t total = 0;
  for (const SlotInterval& interval : intervals_) {
    if (interval.slot_count() == 1) {
      total += 1;
    }
  }
  return total;
}

std::uint64_t SlotSet::count_anchors(std::uint32_t span_slots,
                                     bool require_unit_alignment) const noexcept {
  if (span_slots == 0) {
    return 0;
  }
  std::uint64_t total = 0;
  for (const SlotInterval& interval : intervals_) {
    const std::uint32_t length = interval.slot_count();
    if (length < span_slots) {
      continue;
    }
    const std::uint32_t last_start = interval.end() - span_slots;
    if (!require_unit_alignment) {
      total += static_cast<std::uint64_t>(length - span_slots) + 1u;
      continue;
    }
    // Anchors start at the lower half of a rack unit, which is an odd slot
    // number. When the span itself is odd the last anchor may still be required
    // to be odd, which is exactly what this bracket computes.
    std::uint32_t first_odd = (interval.begin() % 2u == 1u) ? interval.begin() : interval.begin() + 1u;
    std::uint32_t last_odd = (last_start % 2u == 1u) ? last_start : last_start - 1u;
    if (last_odd >= first_odd) {
      total += static_cast<std::uint64_t>((last_odd - first_odd) / 2u) + 1u;
    }
  }
  return total;
}

std::uint64_t SlotSet::count_fitting_runs(std::uint32_t span_slots) const noexcept {
  if (span_slots == 0) {
    return 0;
  }
  std::uint64_t total = 0;
  for (const SlotInterval& interval : intervals_) {
    if (interval.slot_count() >= span_slots) {
      total += 1;
    }
  }
  return total;
}

std::string SlotSet::to_text() const {
  if (intervals_.empty()) {
    return "{}";
  }
  std::string text;
  for (std::size_t index = 0; index < intervals_.size(); ++index) {
    if (index != 0) {
      text.push_back(',');
    }
    text += intervals_[index].to_text();
  }
  return text;
}

Result<SlotSet> SlotSet::parse(std::string_view text) {
  if (text == "{}") {
    return SlotSet::empty();
  }
  std::vector<SlotInterval> parsed;
  std::size_t position = 0;
  while (position < text.size()) {
    if (text[position] != '[') {
      return make_error(ErrorCode::InvalidText, "a slot set member must begin with '['",
                        ErrorDetail{"slotset.parse", {}, {}, 0, position, {}});
    }
    const std::size_t close = text.find(')', position);
    if (close == std::string_view::npos) {
      return make_error(ErrorCode::InvalidText, "a slot set member is missing its closing ')'",
                        ErrorDetail{"slotset.parse", {}, {}, 0, position, {}});
    }
    const std::string_view body = text.substr(position + 1, close - position - 1);
    const std::size_t comma = body.find(',');
    if (comma == std::string_view::npos) {
      return make_error(ErrorCode::InvalidText, "a slot set member must separate begin and end",
                        ErrorDetail{"slotset.parse", {}, {}, 0, position, {}});
    }
    const Result<std::uint32_t> begin = parse_positive_integer(body.substr(0, comma), "slot begin");
    if (!begin.has_value()) {
      return begin.error();
    }
    const Result<std::uint32_t> end = parse_positive_integer(body.substr(comma + 1), "slot end");
    if (!end.has_value()) {
      return end.error();
    }
    const Result<SlotInterval> interval =
        SlotInterval::create(begin.value(), end.value());
    if (!interval.has_value()) {
      return interval.error();
    }
    parsed.push_back(interval.value());
    position = close + 1;
    if (position < text.size()) {
      if (text[position] != ',') {
        return make_error(ErrorCode::InvalidText, "slot set members are separated by commas",
                          ErrorDetail{"slotset.parse", {}, {}, 0, position, {}});
      }
      ++position;
      if (position >= text.size()) {
        return make_error(ErrorCode::InvalidText, "a slot set must not end with a separator",
                          ErrorDetail{"slotset.parse", {}, {}, 0, position, {}});
      }
    }
  }
  if (parsed.empty()) {
    return make_error(ErrorCode::InvalidText, "a slot set is empty; write {} for the empty set",
                      ErrorDetail{"slotset.parse", std::string(text), {}, 0, 0, {}});
  }
  return from_intervals(std::move(parsed));
}

// ---------------------------------------------------------------------------
// Rack extent
// ---------------------------------------------------------------------------

Result<std::uint32_t> validate_unit_count(std::uint64_t unit_count) {
  if (unit_count < 1) {
    return make_error(ErrorCode::InvalidRange, "a rack must have at least one rack unit",
                      ErrorDetail{"rack.unit_count", {}, {}, 1, unit_count, {}});
  }
  if (unit_count > kMaxRackUnits) {
    return make_error(ErrorCode::InvalidRange,
                      "a rack unit count exceeds the documented maximum",
                      ErrorDetail{"rack.unit_count", {}, {}, kMaxRackUnits, unit_count, {}});
  }
  return static_cast<std::uint32_t>(unit_count);
}

std::uint32_t slot_extent_of(std::uint32_t unit_count) noexcept {
  return unit_count * kMountSlotsPerRackUnit + 1u;
}

Result<SlotInterval> rack_slot_extent(std::uint32_t unit_count) {
  const Result<std::uint32_t> validated = validate_unit_count(unit_count);
  if (!validated.has_value()) {
    return validated.error();
  }
  return SlotInterval::create(1, slot_extent_of(validated.value()));
}

// ---------------------------------------------------------------------------
// Enumeration names
// ---------------------------------------------------------------------------

std::string_view mount_span_kind_name(MountSpanKind kind) noexcept {
  switch (kind) {
    case MountSpanKind::FullSpan:
      return "full";
    case MountSpanKind::SharedSpan:
      return "shared";
    case MountSpanKind::ZeroU:
      return "zero-u";
  }
  return "unknown";
}

Result<MountSpanKind> parse_mount_span_kind(std::string_view text) {
  if (text == "full" || text == "full-span") {
    return MountSpanKind::FullSpan;
  }
  if (text == "shared" || text == "shared-span") {
    return MountSpanKind::SharedSpan;
  }
  if (text == "zero-u" || text == "zerou") {
    return MountSpanKind::ZeroU;
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown mount span kind",
                    ErrorDetail{"parse.mount_span_kind", std::string(text), {}, 0, 0, {}});
}

std::string_view presence_state_name(PresenceState state) noexcept {
  switch (state) {
    case PresenceState::Present:
      return "present";
    case PresenceState::Absent:
      return "absent";
    case PresenceState::Indeterminate:
      return "indeterminate";
  }
  return "unknown";
}

Result<PresenceState> parse_presence_state(std::string_view text) {
  if (text == "present" || text == "installed") {
    return PresenceState::Present;
  }
  if (text == "absent" || text == "removed") {
    return PresenceState::Absent;
  }
  if (text == "indeterminate" || text == "unknown") {
    return PresenceState::Indeterminate;
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown presence state",
                    ErrorDetail{"parse.presence_state", std::string(text), {}, 0, 0, {}});
}

}  // namespace rackcapacity
