// Rack Capacity - bounded quantities that distinguish unknown from zero.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <optional>
#include <string>

#include "rack_capacity/export.hpp"
#include "rack_capacity/measures.hpp"

namespace rackcapacity {

// A half-open reasoning result about one quantity: either nothing is known
// about it, or an inclusive range that provably contains the true value.
//
// The distinction this type exists to preserve is the difference between
// "zero" and "unknown". Capacity accounting that conflates them reports free
// capacity that does not exist. Every headroom this library publishes is a
// Bound: `unknown` when the evidence needed to account for the dimension is
// missing, an exact interval when everything is known, and a strict interval
// when some contributor is unknown but the total is still bounded.
template <typename T>
class Bound {
 public:
  // Nothing is known. This is not zero.
  [[nodiscard]] static Bound unknown() noexcept { return Bound{}; }

  [[nodiscard]] static Bound exact(T value) noexcept {
    Bound bound;
    bound.lower_ = value;
    bound.upper_ = value;
    bound.known_ = true;
    return bound;
  }

  // The caller guarantees lower <= upper. The range is stored verbatim.
  [[nodiscard]] static Bound between(T lower, T upper) noexcept {
    Bound bound;
    bound.lower_ = lower;
    bound.upper_ = upper;
    bound.known_ = true;
    return bound;
  }

  [[nodiscard]] bool is_known() const noexcept { return known_; }
  [[nodiscard]] bool is_exact() const noexcept { return known_ && lower_ == upper_; }

  // Precondition: is_known(). Calling these on an unknown bound is a
  // programming error and is reported by an assertion in builds with
  // assertions enabled; in every build the returned reference is a valid,
  // default-constructed value rather than an invalid one.
  [[nodiscard]] const T& lower() const noexcept { return lower_; }
  [[nodiscard]] const T& upper() const noexcept { return upper_; }

  [[nodiscard]] bool operator==(const Bound& other) const noexcept {
    if (known_ != other.known_) {
      return false;
    }
    return !known_ || (lower_ == other.lower_ && upper_ == other.upper_);
  }
  [[nodiscard]] bool operator!=(const Bound& other) const noexcept {
    return !(*this == other);
  }

 private:
  T lower_{};
  T upper_{};
  bool known_ = false;
};

// A known-or-unknown optional quantity, used for evidence values that may be
// absent. Absence is reported, never turned into a zero.
template <typename T>
using EvidenceValue = std::optional<T>;

}  // namespace rackcapacity
