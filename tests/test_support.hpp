// Rack Capacity - shared test support: evidence builders, temporary
// directories, a deterministic generator and real process control.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"

namespace rctest {

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

// Creates a uniquely named directory beneath the system temporary directory.
// The name is derived from the tag, the process identifier and a counter, so
// concurrent test binaries never collide.
[[nodiscard]] std::string make_temp_dir(const std::string& tag);

// Removes a directory tree, tolerating files that are already gone. Used by
// tests to clean up after themselves; a test never leaves residue behind.
void remove_tree(const std::string& path);

// A directory that removes itself when it goes out of scope.
class TempDir {
 public:
  explicit TempDir(const std::string& tag);
  ~TempDir();
  TempDir(TempDir&& other) noexcept;
  TempDir& operator=(TempDir&& other) noexcept;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(const std::string& name) const;
  void release() noexcept { owned_ = false; }

 private:
  std::string path_{};
  bool owned_ = true;
};

// ---------------------------------------------------------------------------
// Deterministic generator
// ---------------------------------------------------------------------------

// A fixed, seedable xorshift generator. Tests print the seed they used so any
// failure can be reproduced exactly; nothing in the library depends on it.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() noexcept;
  // Uniform value in [0, bound). Returns 0 when bound is 0.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept;
  [[nodiscard]] bool chance(std::uint32_t percent) noexcept;
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t seed_ = 0;
};

// ---------------------------------------------------------------------------
// Evidence builder
// ---------------------------------------------------------------------------

// Builds a complete, valid evidence bundle with sensible defaults so a test can
// state only what it is about. Every setter returns the builder so calls chain.
class Bundle {
 public:
  explicit Bundle(std::string rack = "rack-a1", std::uint32_t units = 42);

  Bundle& composition_generation(std::uint64_t generation);
  Bundle& structural_reserved(const std::string& slots);
  Bundle& clearance(std::int64_t front_mm, std::int64_t rear_mm);
  Bundle& service_height_limit(std::uint32_t unit);
  Bundle& location(const std::string& site, const std::string& hall, const std::string& row,
                   const std::string& position);

  Bundle& power(std::uint32_t feeds, std::int64_t watts_per_feed,
                rackcapacity::RedundancyMode redundancy, std::int64_t derate_bp);
  Bundle& power_measurement(std::int64_t watts, std::int64_t at_ns);
  Bundle& no_power();
  Bundle& cooling(std::int64_t nominal_watts, std::int64_t derate_bp);
  Bundle& cooling_measurement(std::int64_t watts, std::int64_t at_ns);
  Bundle& no_cooling();
  Bundle& weight(std::int64_t limit_grams, std::int64_t derate_bp);
  Bundle& weight_point_limit(std::int64_t limit_grams);
  Bundle& weight_measurement(std::int64_t grams, std::int64_t at_ns);
  Bundle& no_weight();

  Bundle& policy(const std::string& id, std::uint32_t version);
  Bundle& power_headroom(std::int64_t watts);
  Bundle& cooling_headroom(std::int64_t watts);
  Bundle& weight_headroom(std::int64_t grams);
  Bundle& slot_headroom(std::uint32_t slots);
  Bundle& power_policy_derate(std::int64_t basis_points);
  Bundle& cooling_policy_derate(std::int64_t basis_points);
  Bundle& weight_policy_derate(std::int64_t basis_points);
  Bundle& envelope_age_limit(std::int64_t nanoseconds);
  Bundle& measurement_age_limit(std::int64_t nanoseconds);
  Bundle& heat_equivalence(std::uint32_t parts_per_million);
  Bundle& no_freshness_bounds();

  Bundle& epoch(std::uint64_t epoch);
  Bundle& captured_at(std::int64_t nanoseconds);
  Bundle& request(const std::string& id);
  Bundle& actor(const std::string& id);
  Bundle& evidence_observed_at(std::int64_t nanoseconds);

  // Adds a full-span occupant. `span` uses the canonical "[begin,end)" form.
  Bundle& asset(const std::string& id, const std::string& span);
  Bundle& asset_units(const std::string& id, std::uint32_t first_unit, std::uint32_t units);
  Bundle& asset_draw(const std::string& id, std::int64_t watts);
  Bundle& asset_heat(const std::string& id, std::int64_t watts);
  Bundle& asset_mass(const std::string& id, std::int64_t grams);
  Bundle& asset_presence(const std::string& id, rackcapacity::PresenceState presence);
  Bundle& asset_absent(const std::string& id);
  Bundle& shared_asset(const std::string& id, const std::string& span,
                       const std::string& shared_class, std::uint32_t share_capacity);
  Bundle& zero_u_asset(const std::string& id);

  Bundle& reservation(const std::string& id, rackcapacity::ReservationState state);
  Bundle& reservation_span(const std::string& id, const std::string& span);
  Bundle& reservation_draw(const std::string& id, std::int64_t watts);
  Bundle& reservation_heat(const std::string& id, std::int64_t watts);
  Bundle& reservation_mass(const std::string& id, std::int64_t grams);

  // Canonicalizes and validates. The test fails loudly if the bundle it built
  // is not valid, because a test fixture that cannot be built is a test defect.
  [[nodiscard]] rackcapacity::RackCapacityInputs build() const;
  // Returns the bundle without validation, for tests that assert on rejection.
  [[nodiscard]] rackcapacity::RackCapacityInputs build_unchecked() const;

 private:
  [[nodiscard]] rackcapacity::AssetOccupancyEvidence* find_asset(const std::string& id);
  [[nodiscard]] rackcapacity::ReservationEvidence* find_reservation(const std::string& id);

  rackcapacity::RackCapacityInputs inputs_{};
  std::string rack_{};
  std::uint32_t units_ = 42;
  std::int64_t observed_at_ = 1'700'000'000'000'000'000ll;
};

// A registered catalog plus its temporary directory, wired together.
struct CatalogFixture {
  explicit CatalogFixture(const std::string& tag, bool read_only = false);
  ~CatalogFixture();

  [[nodiscard]] const std::string& store_path() const { return store_path_; }

  TempDir dir;
  std::string store_path_{};
  std::unique_ptr<rackcapacity::CapacityCatalog> catalog{};
};

// ---------------------------------------------------------------------------
// Real process control
// ---------------------------------------------------------------------------

struct ProcessResult {
  int exit_code = -1;
  std::string output{};
};

// Starts a real operating-system process and waits for it to finish. The child
// is a separate process, not a thread: process-level authority, lock release on
// death and crash recovery are only ever proved this way.
[[nodiscard]] ProcessResult run_process(const std::string& executable,
                                        const std::vector<std::string>& arguments);

// Starts a process without waiting for it, so a test can compete with it for a
// resource. The returned handle must be finished with finish_process.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] static ChildProcess start(const std::string& executable,
                                          const std::vector<std::string>& arguments,
                                          const std::string& output_path);
  // Waits for the child and returns its result.
  [[nodiscard]] ProcessResult wait();
  // Terminates the child as the operating system would on a crash: the process
  // dies without running any cleanup.
  void kill();
  [[nodiscard]] bool running() const noexcept { return handle_ != nullptr; }

 private:
  void close() noexcept;
  void* handle_ = nullptr;
  std::uint64_t pid_ = 0;
  std::string output_path_{};
};

// Polls for a file to appear. The bound is a synchronisation bound, not a test
// timeout: it fails the wait rather than masking it, and it never terminates the
// process under test.
[[nodiscard]] bool wait_for_file(const std::string& path, std::uint32_t max_iterations = 6000);

// Absolute path of the helper executable a suite needs, supplied by the build.
[[nodiscard]] std::string helper_executable(const char* compile_time_path);

}  // namespace rctest
