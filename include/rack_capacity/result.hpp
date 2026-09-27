// Rack Capacity - explicit result type.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <utility>
#include <variant>

#include "rack_capacity/errors.hpp"
#include "rack_capacity/export.hpp"

namespace rackcapacity {

// Result carries either a value or a CapacityError. It is deliberately
// explicit: there is no implicit conversion to bool and no sentinel value.
// Accessing the value of a failed Result is a programming error and throws
// std::bad_variant_access.
//
// The rvalue overload returns the value *by value* rather than by reference.
// That is deliberate: handing back a reference into a temporary Result - as in
// "for (const auto& entry : catalog.records().value())" - would leave the
// reference dangling as soon as the full expression ended, because the language
// does not extend a temporary's lifetime through a function call that returns a
// reference. Returning by value moves the result out, which is cheap for the
// containers this library returns and impossible to get wrong.
template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(CapacityError error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T value() && { return std::move(std::get<0>(storage_)); }

  [[nodiscard]] const CapacityError& error() const& { return std::get<1>(storage_); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, CapacityError> storage_;
};

// Result<void> reports success without a payload.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() noexcept = default;
  Result(CapacityError error) : error_(std::move(error)), ok_(false) {}

  [[nodiscard]] bool has_value() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  void value() const noexcept {}
  [[nodiscard]] const CapacityError& error() const noexcept { return error_; }

 private:
  CapacityError error_{};
  bool ok_ = true;
};

using Status = Result<void>;

}  // namespace rackcapacity
