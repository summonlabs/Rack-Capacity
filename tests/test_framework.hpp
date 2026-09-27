// Rack Capacity - minimal test framework.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small framework with no third-party dependency. Tests run to
// completion: there is no timeout, no watchdog and no process kill. A test that
// does not terminate is a defect in the test or in the library, and both are
// diagnosed rather than masked.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include "rack_capacity/rack_capacity.hpp"

namespace rctest {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

std::vector<TestCase>& registry();
bool register_test(std::string name, TestFunction function);

// Records a failure against the currently running test.
void report_failure(const char* file, int line, const std::string& message);

// Runs every registered test whose name contains `filter`, in registration
// order. Returns the number of failed tests.
int run_all(std::string_view filter);

// ---------------------------------------------------------------------------
// Value rendering for assertion messages
// ---------------------------------------------------------------------------

template <typename T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

inline std::string debug_text(bool value) { return value ? "true" : "false"; }
inline std::string debug_text(const std::string& value) { return "\"" + value + "\""; }
inline std::string debug_text(std::string_view value) { return "\"" + std::string(value) + "\""; }
inline std::string debug_text(const char* value) { return std::string("\"") + value + "\""; }
inline std::string debug_text(rackcapacity::Status status) {
  return status.has_value() ? std::string("ok") : rackcapacity::describe(status.error());
}
inline std::string debug_text(const rackcapacity::CapacityError& error) {
  return rackcapacity::describe(error);
}
inline std::string debug_text(rackcapacity::ErrorCode code) {
  return std::string(rackcapacity::code_name(code));
}
inline std::string debug_text(rackcapacity::SlotInterval interval) {
  return interval.to_text();
}
inline std::string debug_text(const rackcapacity::SlotSet& set) { return set.to_text(); }
inline std::string debug_text(rackcapacity::MountSpanKind kind) {
  return std::string(rackcapacity::mount_span_kind_name(kind));
}
inline std::string debug_text(rackcapacity::PresenceState state) {
  return std::string(rackcapacity::presence_state_name(state));
}
inline std::string debug_text(rackcapacity::ReservationState state) {
  return std::string(rackcapacity::reservation_state_name(state));
}
inline std::string debug_text(rackcapacity::Dimension dimension) {
  return std::string(rackcapacity::dimension_name(dimension));
}
inline std::string debug_text(rackcapacity::FitVerdict verdict) {
  return std::string(rackcapacity::fit_verdict_name(verdict));
}
inline std::string debug_text(rackcapacity::ReasonCode code) {
  return std::string(rackcapacity::reason_code_name(code));
}
inline std::string debug_text(rackcapacity::RackLifecycle lifecycle) {
  return std::string(rackcapacity::rack_lifecycle_name(lifecycle));
}
inline std::string debug_text(rackcapacity::RecoveryStanding standing) {
  return std::string(rackcapacity::recovery_standing_name(standing));
}
inline std::string debug_text(rackcapacity::MutationOutcome outcome) {
  return std::string(rackcapacity::mutation_outcome_name(outcome));
}
inline std::string debug_text(rackcapacity::EvidenceFreshness freshness) {
  return std::string(rackcapacity::evidence_freshness_name(freshness));
}
inline std::string debug_text(rackcapacity::MeasurementStanding standing) {
  return std::string(rackcapacity::measurement_standing_name(standing));
}
inline std::string debug_text(const rackcapacity::RackId& id) { return id.text(); }
inline std::string debug_text(const rackcapacity::AssetId& id) { return id.text(); }
inline std::string debug_text(const rackcapacity::ReservationId& id) { return id.text(); }
inline std::string debug_text(const rackcapacity::StateDigest& digest) { return digest.to_hex(); }
inline std::string debug_text(const rackcapacity::Bound<std::uint32_t>& bound) {
  if (!bound.is_known()) {
    return "unknown";
  }
  return "[" + std::to_string(bound.lower()) + "," + std::to_string(bound.upper()) + "]";
}

template <typename T>
std::string debug_text(const rackcapacity::Result<T>& result);

template <typename T>
std::string debug_text(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

template <typename T>
std::string debug_text(const rackcapacity::Result<T>& result) {
  if (result.has_value()) {
    return "ok(" + debug_text(result.value()) + ")";
  }
  return "err(" + rackcapacity::describe(result.error()) + ")";
}

}  // namespace rctest

#define RC_TEST(test_name)                                                          \
  static void test_name();                                                          \
  namespace {                                                                       \
  const bool rc_registered_##test_name =                                            \
      ::rctest::register_test(#test_name, &test_name);                              \
  }                                                                                 \
  static void test_name()

#define RC_CHECK(condition)                                                          \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::rctest::report_failure(__FILE__, __LINE__, "check failed: " #condition);      \
    }                                                                                \
  } while (false)

#define RC_CHECK_EQ(actual, expected)                                                \
  do {                                                                               \
    const auto& rc_actual = (actual);                                                \
    const auto& rc_expected = (expected);                                            \
    if (!(rc_actual == rc_expected)) {                                               \
      ::rctest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #actual " == " #expected) +    \
                                   "\n    actual:   " + ::rctest::debug_text(rc_actual) + \
                                   "\n    expected: " + ::rctest::debug_text(rc_expected)); \
    }                                                                                \
  } while (false)

#define RC_CHECK_NE(actual, unexpected)                                              \
  do {                                                                               \
    const auto& rc_actual = (actual);                                                \
    const auto& rc_unexpected = (unexpected);                                        \
    if (rc_actual == rc_unexpected) {                                                \
      ::rctest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #actual " != " #unexpected) +  \
                                   "\n    both: " + ::rctest::debug_text(rc_actual)); \
    }                                                                                \
  } while (false)

#define RC_REQUIRE(condition)                                                        \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::rctest::report_failure(__FILE__, __LINE__, "requirement failed: " #condition); \
      return;                                                                        \
    }                                                                                \
  } while (false)

// Requires that a Result carries a value; on failure the test ends and the
// rejection is reported.
#define RC_REQUIRE_OK(result)                                                        \
  do {                                                                               \
    const auto& rc_result = (result);                                                \
    if (!rc_result.has_value()) {                                                    \
      ::rctest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected success from " #result) +       \
                                   "\n    error: " + ::rctest::debug_text(rc_result.error())); \
      return;                                                                        \
    }                                                                                \
  } while (false)

// Requires that a Result carries the given error code.
#define RC_REQUIRE_CODE(result, expected_code)                                       \
  do {                                                                               \
    const auto& rc_result = (result);                                                \
    if (rc_result.has_value()) {                                                     \
      ::rctest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #result " to fail with " #expected_code) + \
                                   " but it succeeded");                        \
      return;                                                                        \
    }                                                                                \
    if (rc_result.error().code != (expected_code)) {                                 \
      ::rctest::report_failure(__FILE__, __LINE__,                                   \
                               std::string("expected " #result " to fail with " #expected_code) + \
                                   "\n    actual: " + ::rctest::debug_text(rc_result.error())); \
      return;                                                                        \
    }                                                                                \
  } while (false)
