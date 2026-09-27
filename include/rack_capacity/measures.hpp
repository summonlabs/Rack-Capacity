// Rack Capacity - exact integer measures and checked arithmetic.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "rack_capacity/export.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Strong integer measures
// ---------------------------------------------------------------------------
//
// Every authoritative quantity in this library is an exact integer with a
// named unit. There is no floating point anywhere in capacity accounting:
// derating, headroom, utilisation and fragmentation are all computed with
// checked integer operations whose rounding rule is documented and tested.
//
// Conversions between measures are not implicit. A `Watts` is not a `Grams`
// and neither is an `int64_t`; constructing one from a raw integer goes
// through a validating factory that reports the exact violated bound.

namespace detail {

// Largest signed 64-bit value, spelled out so the arithmetic helpers below do
// not depend on any library formatting behavior.
inline constexpr std::int64_t kInt64Max = 0x7fffffffffffffffLL;
inline constexpr std::int64_t kInt64Min = -0x7fffffffffffffffLL - 1;

[[nodiscard]] RACK_CAPACITY_API Status checked_add_i64(std::int64_t a, std::int64_t b,
                                                       std::int64_t& out) noexcept;
[[nodiscard]] RACK_CAPACITY_API Status checked_sub_i64(std::int64_t a, std::int64_t b,
                                                       std::int64_t& out) noexcept;
[[nodiscard]] RACK_CAPACITY_API Status checked_mul_i64(std::int64_t a, std::int64_t b,
                                                       std::int64_t& out) noexcept;

// Renders an exact integer quantity together with its unit, applying the
// documented scaling for units that are spoken about in larger denominations
// (grams as kilograms, watts as kilowatts above ten kilowatts).
[[nodiscard]] RACK_CAPACITY_API std::string format_measure(std::int64_t value,
                                                           std::string_view unit);

}  // namespace detail

// One signed 64-bit quantity with a named unit, a documented inclusive range,
// and no implicit conversion to or from any other measure.
template <typename Tag>
class Measure {
 public:
  using rep = std::int64_t;

  constexpr Measure() noexcept = default;

  // Validating factory for externally supplied values. Out-of-range values are
  // reported, never clamped and never wrapped.
  [[nodiscard]] static Result<Measure> create(std::int64_t value) {
    if (!is_valid(value)) {
      return make_error(ErrorCode::InvalidRange,
                        "value is outside the documented range of this measure",
                        ErrorDetail{"measure.create", std::string(Tag::kName), {}, 
                                    static_cast<std::uint64_t>(Tag::kMax),
                                    value < 0 ? 0u : static_cast<std::uint64_t>(value), {}});
    }
    return Measure(value);
  }

  // Factory for values the caller has already validated. A value outside the
  // documented range is clamped into it, so a Measure never holds an invalid
  // quantity in any build; the contract is that callers pass in-range values,
  // and create() is the validating entry point for untrusted input.
  [[nodiscard]] static constexpr Measure trusted(std::int64_t value) noexcept {
    return Measure(clamp_to_range(value));
  }

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

  [[nodiscard]] static constexpr std::int64_t min_value() noexcept { return Tag::kMin; }
  [[nodiscard]] static constexpr std::int64_t max_value() noexcept { return Tag::kMax; }
  [[nodiscard]] static constexpr bool is_valid(std::int64_t value) noexcept {
    return value >= Tag::kMin && value <= Tag::kMax;
  }
  [[nodiscard]] static constexpr std::string_view unit_name() noexcept { return Tag::kName; }

  [[nodiscard]] constexpr bool operator==(const Measure& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const Measure& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Measure& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const Measure& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const Measure& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const Measure& other) const noexcept {
    return value_ >= other.value_;
  }

  // Exact addition and subtraction inside the documented range. An operation
  // that would leave the range is refused with ArithmeticOverflow rather than
  // wrapping or saturating silently.
  [[nodiscard]] Result<Measure> add(Measure other) const {
    std::int64_t sum = 0;
    const Status status = detail::checked_add_i64(value_, other.value_, sum);
    if (!status.has_value()) {
      return status.error();
    }
    return create(sum);
  }

  [[nodiscard]] Result<Measure> subtract(Measure other) const {
    std::int64_t difference = 0;
    const Status status = detail::checked_sub_i64(value_, other.value_, difference);
    if (!status.has_value()) {
      return status.error();
    }
    return create(difference);
  }

  // Subtracts and floors at the range minimum. Used where a physical envelope
  // can be exceeded by committed load and the excess must be reported rather
  // than hidden; callers that need the excess itself use the non-saturating
  // subtract on raw values.
  [[nodiscard]] Measure saturating_subtract(Measure other) const noexcept {
    const std::int64_t raw = value_ - other.value_;
    return Measure(clamp_to_range(raw));
  }

  [[nodiscard]] Result<Measure> multiply(std::int64_t factor) const {
    std::int64_t product = 0;
    const Status status = detail::checked_mul_i64(value_, factor, product);
    if (!status.has_value()) {
      return status.error();
    }
    return create(product);
  }

  [[nodiscard]] std::string to_text() const {
    return detail::format_measure(value_, Tag::kName);
  }

 private:
  constexpr explicit Measure(std::int64_t value) noexcept : value_(value) {}
  [[nodiscard]] static constexpr std::int64_t clamp_to_range(std::int64_t value) noexcept {
    return value < Tag::kMin ? Tag::kMin : (value > Tag::kMax ? Tag::kMax : value);
  }

  std::int64_t value_ = 0;
};

// Unit tags. Each declares its own inclusive range, which fixes both the strong
// type identity and the exact arithmetic envelope.
struct WattsTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = kMaxWatts;
  static constexpr std::string_view kName = "W";
};
struct GramsTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = kMaxGrams;
  static constexpr std::string_view kName = "g";
};
struct MillimetresTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = kMaxMillimetres;
  static constexpr std::string_view kName = "mm";
};
struct MilliCelsiusTag {
  static constexpr std::int64_t kMin = kMinMilliCelsius;
  static constexpr std::int64_t kMax = kMaxMilliCelsius;
  static constexpr std::string_view kName = "mC";
};
struct BasisPointsTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = kMaxBasisPoints;
  static constexpr std::string_view kName = "bp";
};
struct TimestampTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = detail::kInt64Max;
  static constexpr std::string_view kName = "ns";
};
struct DurationTag {
  static constexpr std::int64_t kMin = 0;
  static constexpr std::int64_t kMax = detail::kInt64Max;
  static constexpr std::string_view kName = "ns";
};

using Watts = Measure<WattsTag>;
using Grams = Measure<GramsTag>;
using Millimetres = Measure<MillimetresTag>;
using MilliCelsius = Measure<MilliCelsiusTag>;
using BasisPoints = Measure<BasisPointsTag>;

// Nanoseconds since the Unix epoch. The library never reads a wall clock on its
// own: every evaluation takes the instant it should reason about as an explicit
// argument, which is what makes capacity evaluation deterministic and testable.
using TimestampNs = Measure<TimestampTag>;
// A non-negative duration in nanoseconds.
using DurationNs = Measure<DurationTag>;

// Reads the system clock. Provided as a convenience for callers such as the
// command line tool; the library itself never calls it.
[[nodiscard]] RACK_CAPACITY_API TimestampNs system_now() noexcept;

// ---------------------------------------------------------------------------
// Exact scaling rules
// ---------------------------------------------------------------------------

// Applies a derating factor: value * (10000 - factor) / 10000, floored. A
// factor above 10000 basis points is rejected, never interpreted as a negative
// capacity.
[[nodiscard]] RACK_CAPACITY_API Result<Watts> derate(Watts value, BasisPoints factor);
[[nodiscard]] RACK_CAPACITY_API Result<Grams> derate(Grams value, BasisPoints factor);

// Converts watts to heat watts using an explicitly declared policy equivalence
// in parts per million. This is the only path in the library that can produce a
// derived heat figure, it is never applied unless a policy states the factor,
// and every derived quantity is reported separately from declared heat.
[[nodiscard]] RACK_CAPACITY_API Result<Watts> heat_from_power(Watts draw,
                                                              std::uint32_t parts_per_million);

// Exact integer division with a zero-divisor check.
[[nodiscard]] RACK_CAPACITY_API Result<std::int64_t> divide_floor(std::int64_t numerator,
                                                                 std::int64_t denominator);

// Utilisation in basis points: part * 10000 / whole, floored, with the result
// allowed to exceed 10000 when part exceeds whole. `whole` must be positive.
[[nodiscard]] RACK_CAPACITY_API Result<std::int64_t> ratio_basis_points(std::int64_t part,
                                                                        std::int64_t whole);

// Renders a measure with its unit, for example "4800 W" or "12.5 kg".
[[nodiscard]] RACK_CAPACITY_API std::string to_text(Watts value);

}  // namespace rackcapacity
