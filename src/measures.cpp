// Rack Capacity - exact integer measures and checked arithmetic.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/measures.hpp"

#include <chrono>
#include <string>

namespace rackcapacity {

namespace detail {

Status checked_add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b > 0 && a > kInt64Max - b) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit addition overflowed",
                      ErrorDetail{"arithmetic.add", {}, {}, 0, 0, {}});
  }
  if (b < 0 && a < kInt64Min - b) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit addition underflowed",
                      ErrorDetail{"arithmetic.add", {}, {}, 0, 0, {}});
  }
  out = a + b;
  return Status{};
}

Status checked_sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b < 0 && a > kInt64Max + b) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit subtraction overflowed",
                      ErrorDetail{"arithmetic.subtract", {}, {}, 0, 0, {}});
  }
  if (b > 0 && a < kInt64Min + b) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit subtraction underflowed",
                      ErrorDetail{"arithmetic.subtract", {}, {}, 0, 0, {}});
  }
  out = a - b;
  return Status{};
}

Status checked_mul_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return Status{};
  }
  // Division-based pre-check: it is exact for the ranges this library uses and
  // never itself overflows, because the divisor is non-zero and the minimum
  // value divided by -1 is excluded by the explicit guard below.
  if (a == kInt64Min || b == kInt64Min) {
    if (a == -1 || b == -1) {
      return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit multiplication overflowed",
                        ErrorDetail{"arithmetic.multiply", {}, {}, 0, 0, {}});
    }
  }
  const std::int64_t limit = kInt64Max / (b < 0 ? -b : b);
  const std::int64_t magnitude = a < 0 ? -a : a;
  if (magnitude > limit) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit multiplication overflowed",
                      ErrorDetail{"arithmetic.multiply", {}, {}, 0, 0, {}});
  }
  out = a * b;
  return Status{};
}

std::string format_measure(std::int64_t value, std::string_view unit) {
  std::string text = std::to_string(value);
  if (!unit.empty()) {
    text.push_back(' ');
    text.append(unit);
  }
  return text;
}

}  // namespace detail

TimestampNs system_now() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return TimestampNs::trusted(nanoseconds < 0 ? 0 : nanoseconds);
}

Result<Watts> derate(Watts value, BasisPoints factor) {
  const std::int64_t remaining = kMaxBasisPoints - factor.value();
  std::int64_t product = 0;
  const Status multiplied = detail::checked_mul_i64(value.value(), remaining, product);
  if (!multiplied.has_value()) {
    return multiplied.error();
  }
  return Watts::create(product / kMaxBasisPoints);
}

Result<Grams> derate(Grams value, BasisPoints factor) {
  const std::int64_t remaining = kMaxBasisPoints - factor.value();
  std::int64_t product = 0;
  const Status multiplied = detail::checked_mul_i64(value.value(), remaining, product);
  if (!multiplied.has_value()) {
    return multiplied.error();
  }
  return Grams::create(product / kMaxBasisPoints);
}

Result<Watts> heat_from_power(Watts draw, std::uint32_t parts_per_million) {
  constexpr std::uint32_t kMaxPartsPerMillion = 1'000'000u;
  if (parts_per_million > kMaxPartsPerMillion) {
    return make_error(ErrorCode::InvalidRange,
                      "a power to heat equivalence above 1000000 parts per million is not a "
                      "physical conversion",
                      ErrorDetail{"measures.heat_from_power", {}, {}, kMaxPartsPerMillion,
                                  parts_per_million, {}});
  }
  if (parts_per_million == 0) {
    return Watts::create(0);
  }
  std::int64_t product = 0;
  const Status multiplied =
      detail::checked_mul_i64(draw.value(), static_cast<std::int64_t>(parts_per_million), product);
  if (!multiplied.has_value()) {
    return multiplied.error();
  }
  return Watts::create(product / static_cast<std::int64_t>(kMaxPartsPerMillion));
}

Result<std::int64_t> divide_floor(std::int64_t numerator, std::int64_t denominator) {
  if (denominator == 0) {
    return make_error(ErrorCode::InvalidArgument, "division by zero",
                      ErrorDetail{"arithmetic.divide", {}, {}, 0, 0, {}});
  }
  if (numerator == detail::kInt64Min && denominator == -1) {
    return make_error(ErrorCode::ArithmeticOverflow, "signed 64-bit division overflowed",
                      ErrorDetail{"arithmetic.divide", {}, {}, 0, 0, {}});
  }
  std::int64_t quotient = numerator / denominator;
  const std::int64_t remainder = numerator % denominator;
  if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
    quotient -= 1;
  }
  return quotient;
}

Result<std::int64_t> ratio_basis_points(std::int64_t part, std::int64_t whole) {
  if (part < 0) {
    return make_error(ErrorCode::InvalidArgument,
                      "a utilisation ratio needs a non-negative part",
                      ErrorDetail{"arithmetic.ratio", {}, {}, 0,
                                  static_cast<std::uint64_t>(part < 0 ? 0 : part), {}});
  }
  if (whole <= 0) {
    return make_error(ErrorCode::InvalidArgument,
                      "a utilisation ratio needs a positive whole quantity",
                      ErrorDetail{"arithmetic.ratio", {}, {}, 1,
                                  static_cast<std::uint64_t>(whole < 0 ? 0 : whole), {}});
  }
  std::int64_t scaled = 0;
  const Status multiplied = detail::checked_mul_i64(part, kMaxBasisPoints, scaled);
  if (!multiplied.has_value()) {
    return multiplied.error();
  }
  return divide_floor(scaled, whole);
}

std::string to_text(Watts value) { return value.to_text(); }

}  // namespace rackcapacity
