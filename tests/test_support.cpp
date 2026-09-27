// Rack Capacity - shared test support implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rctest {
namespace {

std::atomic<std::uint64_t> g_counter{0};

[[nodiscard]] std::uint64_t process_id() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::string temp_root() {
  // std::filesystem::temp_directory_path is the portable, warning-free way to
  // find the temporary directory on every supported platform.
  std::error_code error;
  const std::filesystem::path path = std::filesystem::temp_directory_path(error);
  return error ? std::string(".") : path.string();
}

[[nodiscard]] rackcapacity::Result<rackcapacity::SlotInterval> single_interval(
    const std::string& text) {
  const rackcapacity::Result<rackcapacity::SlotSet> parsed = rackcapacity::SlotSet::parse(text);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  if (parsed.value().interval_count() != 1) {
    return rackcapacity::make_error(rackcapacity::ErrorCode::InvalidSlotInterval,
                                    "the test support expects exactly one interval",
                                    rackcapacity::ErrorDetail{"test.support", text, {}, 0, 0, {}});
  }
  return parsed.value().intervals().front();
}

[[nodiscard]] rackcapacity::EvidenceReference reference_for(rackcapacity::EvidenceSource source,
                                                            const std::string& evidence,
                                                            std::int64_t observed_at) {
  rackcapacity::EvidenceReference reference;
  reference.source = source;
  const rackcapacity::Result<rackcapacity::EvidenceId> id =
      rackcapacity::EvidenceId::create(evidence);
  if (id.has_value()) {
    reference.evidence = id.value();
  }
  reference.version = 1;
  reference.observed_at = rackcapacity::TimestampNs::trusted(observed_at);
  const rackcapacity::Result<rackcapacity::SourceReference> producer =
      rackcapacity::SourceReference::create("test-harness");
  if (producer.has_value()) {
    reference.producer = producer.value();
  }
  return reference;
}

}  // namespace

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

std::string make_temp_dir(const std::string& tag) {
  std::ostringstream name;
  name << "rcap-test-" << tag << "-" << process_id() << "-" << g_counter.fetch_add(1);
  const std::filesystem::path path = std::filesystem::path(temp_root()) / name.str();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

void remove_tree(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

TempDir::TempDir(const std::string& tag) : path_(make_temp_dir(tag)) {}

TempDir::~TempDir() {
  if (owned_) {
    remove_tree(path_);
  }
}

TempDir::TempDir(TempDir&& other) noexcept
    : path_(std::move(other.path_)), owned_(other.owned_) {
  other.owned_ = false;
}

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    if (owned_) {
      remove_tree(path_);
    }
    path_ = std::move(other.path_);
    owned_ = other.owned_;
    other.owned_ = false;
  }
  return *this;
}

std::string TempDir::child(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

// ---------------------------------------------------------------------------
// Deterministic generator
// ---------------------------------------------------------------------------

std::uint64_t Rng::next() noexcept {
  std::uint64_t value = state_;
  value ^= value << 13;
  value ^= value >> 7;
  value ^= value << 17;
  state_ = value;
  return value;
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return next() % bound;
}

bool Rng::chance(std::uint32_t percent) noexcept {
  return below(100) < percent;
}

// ---------------------------------------------------------------------------
// Bundle builder
// ---------------------------------------------------------------------------

Bundle::Bundle(std::string rack, std::uint32_t units) {
  rack_ = std::move(rack);
  units_ = units;
  inputs_.composition.rack = rackcapacity::RackId::create(rack_).value();
  inputs_.composition.generation = rackcapacity::RackCompositionGeneration::trusted(1);
  inputs_.composition.unit_count = units_;
  inputs_.composition.serviceability.front_clearance = rackcapacity::Millimetres::trusted(900);
  inputs_.composition.serviceability.rear_clearance = rackcapacity::Millimetres::trusted(700);
  inputs_.composition.reference = reference_for(rackcapacity::EvidenceSource::RackRegistry,
                                                "ev-composition", observed_at_);
  inputs_.policy.reference.policy = rackcapacity::PolicyId::create("standard-2026").value();
  inputs_.policy.reference.version = 1;
  inputs_.policy.freshness.max_envelope_age = rackcapacity::DurationNs::trusted(3'600'000'000'000ll);
  inputs_.policy.freshness.max_measurement_age =
      rackcapacity::DurationNs::trusted(300'000'000'000ll);
  inputs_.power = rackcapacity::PowerCapacityEvidence{};
  inputs_.power->feed_count = 2;
  inputs_.power->watts_per_feed = rackcapacity::Watts::trusted(8000);
  inputs_.power->redundancy = rackcapacity::RedundancyMode::N2;
  inputs_.power->derate = rackcapacity::BasisPoints::trusted(2000);
  inputs_.power->reference =
      reference_for(rackcapacity::EvidenceSource::PowerCapacity, "ev-power", observed_at_);
  inputs_.cooling = rackcapacity::CoolingCapacityEvidence{};
  inputs_.cooling->nominal_heat_rejection = rackcapacity::Watts::trusted(20000);
  inputs_.cooling->derate = rackcapacity::BasisPoints::trusted(1000);
  inputs_.cooling->reference =
      reference_for(rackcapacity::EvidenceSource::CoolingCapacity, "ev-cooling", observed_at_);
  inputs_.weight = rackcapacity::WeightCapacityEvidence{};
  inputs_.weight->static_load_limit = rackcapacity::Grams::trusted(900'000);
  inputs_.weight->derate = rackcapacity::BasisPoints::trusted(0);
  inputs_.weight->reference =
      reference_for(rackcapacity::EvidenceSource::CoolingCapacity, "ev-weight", observed_at_);
  inputs_.epoch = rackcapacity::EvidenceEpoch::trusted(1);
  inputs_.captured_at = rackcapacity::TimestampNs::trusted(observed_at_);
  inputs_.actor = rackcapacity::ActorId::create("operator-1").value();
  inputs_.request = rackcapacity::RequestId::create("req-0001").value();
}

Bundle& Bundle::composition_generation(std::uint64_t generation) {
  inputs_.composition.generation = rackcapacity::RackCompositionGeneration::trusted(generation);
  return *this;
}

Bundle& Bundle::structural_reserved(const std::string& slots) {
  const rackcapacity::Result<rackcapacity::SlotSet> parsed = rackcapacity::SlotSet::parse(slots);
  if (parsed.has_value()) {
    inputs_.composition.structural_reserved_slots = parsed.value();
  }
  return *this;
}

Bundle& Bundle::clearance(std::int64_t front_mm, std::int64_t rear_mm) {
  inputs_.composition.serviceability.front_clearance = rackcapacity::Millimetres::trusted(front_mm);
  inputs_.composition.serviceability.rear_clearance = rackcapacity::Millimetres::trusted(rear_mm);
  return *this;
}

Bundle& Bundle::service_height_limit(std::uint32_t unit) {
  inputs_.composition.serviceability.service_height_limit_unit = unit;
  return *this;
}

Bundle& Bundle::location(const std::string& site, const std::string& hall, const std::string& row,
                         const std::string& position) {
  inputs_.composition.location.site = rackcapacity::SiteId::create(site).value();
  inputs_.composition.location.hall = rackcapacity::HallId::create(hall).value();
  inputs_.composition.location.row = rackcapacity::RowId::create(row).value();
  inputs_.composition.location.position = rackcapacity::RackPositionId::create(position).value();
  return *this;
}

Bundle& Bundle::power(std::uint32_t feeds, std::int64_t watts_per_feed,
                      rackcapacity::RedundancyMode redundancy, std::int64_t derate_bp) {
  inputs_.power = rackcapacity::PowerCapacityEvidence{};
  inputs_.power->feed_count = feeds;
  inputs_.power->watts_per_feed = rackcapacity::Watts::trusted(watts_per_feed);
  inputs_.power->redundancy = redundancy;
  inputs_.power->derate = rackcapacity::BasisPoints::trusted(derate_bp);
  inputs_.power->reference =
      reference_for(rackcapacity::EvidenceSource::PowerCapacity, "ev-power", observed_at_);
  return *this;
}

Bundle& Bundle::power_measurement(std::int64_t watts, std::int64_t at_ns) {
  if (inputs_.power.has_value()) {
    inputs_.power->measured_draw = rackcapacity::Watts::trusted(watts);
    inputs_.power->measured_at = rackcapacity::TimestampNs::trusted(at_ns);
  }
  return *this;
}

Bundle& Bundle::no_power() {
  inputs_.power.reset();
  return *this;
}

Bundle& Bundle::cooling(std::int64_t nominal_watts, std::int64_t derate_bp) {
  inputs_.cooling = rackcapacity::CoolingCapacityEvidence{};
  inputs_.cooling->nominal_heat_rejection = rackcapacity::Watts::trusted(nominal_watts);
  inputs_.cooling->derate = rackcapacity::BasisPoints::trusted(derate_bp);
  inputs_.cooling->reference =
      reference_for(rackcapacity::EvidenceSource::CoolingCapacity, "ev-cooling", observed_at_);
  return *this;
}

Bundle& Bundle::cooling_measurement(std::int64_t watts, std::int64_t at_ns) {
  if (inputs_.cooling.has_value()) {
    inputs_.cooling->measured_heat_load = rackcapacity::Watts::trusted(watts);
    inputs_.cooling->measured_at = rackcapacity::TimestampNs::trusted(at_ns);
  }
  return *this;
}

Bundle& Bundle::no_cooling() {
  inputs_.cooling.reset();
  return *this;
}

Bundle& Bundle::weight(std::int64_t limit_grams, std::int64_t derate_bp) {
  inputs_.weight = rackcapacity::WeightCapacityEvidence{};
  inputs_.weight->static_load_limit = rackcapacity::Grams::trusted(limit_grams);
  inputs_.weight->derate = rackcapacity::BasisPoints::trusted(derate_bp);
  inputs_.weight->reference =
      reference_for(rackcapacity::EvidenceSource::CoolingCapacity, "ev-weight", observed_at_);
  return *this;
}

Bundle& Bundle::weight_point_limit(std::int64_t limit_grams) {
  if (inputs_.weight.has_value()) {
    inputs_.weight->per_unit_point_load_limit = rackcapacity::Grams::trusted(limit_grams);
  }
  return *this;
}

Bundle& Bundle::weight_measurement(std::int64_t grams, std::int64_t at_ns) {
  if (inputs_.weight.has_value()) {
    inputs_.weight->measured_static_load = rackcapacity::Grams::trusted(grams);
    inputs_.weight->measured_at = rackcapacity::TimestampNs::trusted(at_ns);
  }
  return *this;
}

Bundle& Bundle::no_weight() {
  inputs_.weight.reset();
  return *this;
}

Bundle& Bundle::policy(const std::string& id, std::uint32_t version) {
  inputs_.policy.reference.policy = rackcapacity::PolicyId::create(id).value();
  inputs_.policy.reference.version = version;
  return *this;
}

Bundle& Bundle::power_headroom(std::int64_t watts) {
  inputs_.policy.headroom.power_headroom = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::cooling_headroom(std::int64_t watts) {
  inputs_.policy.headroom.cooling_headroom = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::weight_headroom(std::int64_t grams) {
  inputs_.policy.headroom.weight_headroom = rackcapacity::Grams::trusted(grams);
  return *this;
}

Bundle& Bundle::slot_headroom(std::uint32_t slots) {
  inputs_.policy.headroom.slot_headroom = slots;
  return *this;
}

Bundle& Bundle::power_policy_derate(std::int64_t basis_points) {
  inputs_.policy.headroom.power_derate = rackcapacity::BasisPoints::trusted(basis_points);
  return *this;
}

Bundle& Bundle::cooling_policy_derate(std::int64_t basis_points) {
  inputs_.policy.headroom.cooling_derate = rackcapacity::BasisPoints::trusted(basis_points);
  return *this;
}

Bundle& Bundle::weight_policy_derate(std::int64_t basis_points) {
  inputs_.policy.headroom.weight_derate = rackcapacity::BasisPoints::trusted(basis_points);
  return *this;
}

Bundle& Bundle::envelope_age_limit(std::int64_t nanoseconds) {
  inputs_.policy.freshness.max_envelope_age = rackcapacity::DurationNs::trusted(nanoseconds);
  return *this;
}

Bundle& Bundle::measurement_age_limit(std::int64_t nanoseconds) {
  inputs_.policy.freshness.max_measurement_age = rackcapacity::DurationNs::trusted(nanoseconds);
  return *this;
}

Bundle& Bundle::heat_equivalence(std::uint32_t parts_per_million) {
  inputs_.policy.heat_per_power_ppm = parts_per_million;
  return *this;
}

Bundle& Bundle::no_freshness_bounds() {
  inputs_.policy.freshness.max_envelope_age = rackcapacity::DurationNs::trusted(0);
  inputs_.policy.freshness.max_measurement_age = rackcapacity::DurationNs::trusted(0);
  return *this;
}

Bundle& Bundle::epoch(std::uint64_t epoch) {
  inputs_.epoch = rackcapacity::EvidenceEpoch::trusted(epoch);
  return *this;
}

Bundle& Bundle::captured_at(std::int64_t nanoseconds) {
  inputs_.captured_at = rackcapacity::TimestampNs::trusted(nanoseconds);
  return *this;
}

Bundle& Bundle::request(const std::string& id) {
  const rackcapacity::Result<rackcapacity::RequestId> value = rackcapacity::RequestId::create(id);
  if (value.has_value()) {
    inputs_.request = value.value();
  }
  return *this;
}

Bundle& Bundle::actor(const std::string& id) {
  const rackcapacity::Result<rackcapacity::ActorId> value = rackcapacity::ActorId::create(id);
  if (value.has_value()) {
    inputs_.actor = value.value();
  }
  return *this;
}

Bundle& Bundle::evidence_observed_at(std::int64_t nanoseconds) {
  observed_at_ = nanoseconds;
  inputs_.composition.reference = reference_for(rackcapacity::EvidenceSource::RackRegistry,
                                                "ev-composition", observed_at_);
  if (inputs_.power.has_value()) {
    inputs_.power->reference = reference_for(rackcapacity::EvidenceSource::PowerCapacity, "ev-power",
                                             observed_at_);
  }
  if (inputs_.cooling.has_value()) {
    inputs_.cooling->reference = reference_for(rackcapacity::EvidenceSource::CoolingCapacity,
                                               "ev-cooling", observed_at_);
  }
  if (inputs_.weight.has_value()) {
    inputs_.weight->reference = reference_for(rackcapacity::EvidenceSource::CoolingCapacity,
                                              "ev-weight", observed_at_);
  }
  for (rackcapacity::AssetOccupancyEvidence& asset : inputs_.assets) {
    asset.reference = reference_for(rackcapacity::EvidenceSource::AssetRegistry,
                                    asset.reference.evidence.text(), observed_at_);
  }
  for (rackcapacity::ReservationEvidence& reservation : inputs_.reservations) {
    reservation.reference = reference_for(rackcapacity::EvidenceSource::FacilityCapacityReservation,
                                          reservation.reference.evidence.text(), observed_at_);
  }
  return *this;
}

rackcapacity::AssetOccupancyEvidence* Bundle::find_asset(const std::string& id) {
  for (rackcapacity::AssetOccupancyEvidence& asset : inputs_.assets) {
    if (asset.asset.text() == id) {
      return &asset;
    }
  }
  // A setter that names an occupant which was never declared creates it with an
  // invalid span, so validation reports the test's mistake instead of the
  // fixture silently inventing a mounting position.
  rackcapacity::AssetOccupancyEvidence undeclared;
  const rackcapacity::Result<rackcapacity::AssetId> value = rackcapacity::AssetId::create(id);
  if (value.has_value()) {
    undeclared.asset = value.value();
  }
  inputs_.assets.push_back(undeclared);
  return &inputs_.assets.back();
}

rackcapacity::ReservationEvidence* Bundle::find_reservation(const std::string& id) {
  for (rackcapacity::ReservationEvidence& reservation : inputs_.reservations) {
    if (reservation.reservation.text() == id) {
      return &reservation;
    }
  }
  rackcapacity::ReservationEvidence undeclared;
  const rackcapacity::Result<rackcapacity::ReservationId> value =
      rackcapacity::ReservationId::create(id);
  if (value.has_value()) {
    undeclared.reservation = value.value();
  }
  inputs_.reservations.push_back(undeclared);
  return &inputs_.reservations.back();
}

Bundle& Bundle::asset(const std::string& id, const std::string& span) {
  rackcapacity::AssetOccupancyEvidence* entry = find_asset(id);
  const rackcapacity::Result<rackcapacity::SlotInterval> interval = single_interval(span);
  if (interval.has_value()) {
    entry->span = interval.value();
  }
  entry->kind = rackcapacity::MountSpanKind::FullSpan;
  entry->presence = rackcapacity::PresenceState::Present;
  entry->reference = reference_for(rackcapacity::EvidenceSource::AssetRegistry, "ev-asset-" + id,
                                   observed_at_);
  return *this;
}

Bundle& Bundle::asset_units(const std::string& id, std::uint32_t first_unit, std::uint32_t units) {
  const rackcapacity::Result<rackcapacity::SlotInterval> interval =
      rackcapacity::SlotInterval::for_units(first_unit, units);
  if (interval.has_value()) {
    asset(id, interval.value().to_text());
  }
  return *this;
}

Bundle& Bundle::asset_draw(const std::string& id, std::int64_t watts) {
  find_asset(id)->nameplate_draw = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::asset_heat(const std::string& id, std::int64_t watts) {
  find_asset(id)->declared_heat_rejection = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::asset_mass(const std::string& id, std::int64_t grams) {
  find_asset(id)->mass = rackcapacity::Grams::trusted(grams);
  return *this;
}

Bundle& Bundle::asset_presence(const std::string& id, rackcapacity::PresenceState presence) {
  find_asset(id)->presence = presence;
  return *this;
}

Bundle& Bundle::asset_absent(const std::string& id) {
  return asset_presence(id, rackcapacity::PresenceState::Absent);
}

Bundle& Bundle::shared_asset(const std::string& id, const std::string& span,
                             const std::string& shared_class, std::uint32_t share_capacity) {
  rackcapacity::AssetOccupancyEvidence* entry = find_asset(id);
  const rackcapacity::Result<rackcapacity::SlotInterval> interval = single_interval(span);
  if (interval.has_value()) {
    entry->span = interval.value();
  }
  entry->kind = rackcapacity::MountSpanKind::SharedSpan;
  entry->presence = rackcapacity::PresenceState::Present;
  const rackcapacity::Result<rackcapacity::SharedMountClassId> klass =
      rackcapacity::SharedMountClassId::create(shared_class);
  if (klass.has_value()) {
    entry->shared_class = klass.value();
  }
  entry->share_capacity = share_capacity;
  entry->reference = reference_for(rackcapacity::EvidenceSource::AssetRegistry, "ev-asset-" + id,
                                   observed_at_);
  return *this;
}

Bundle& Bundle::zero_u_asset(const std::string& id) {
  rackcapacity::AssetOccupancyEvidence* entry = find_asset(id);
  const rackcapacity::Result<rackcapacity::SlotInterval> interval =
      rackcapacity::SlotInterval::create(1, 2);
  if (interval.has_value()) {
    entry->span = interval.value();
  }
  entry->kind = rackcapacity::MountSpanKind::ZeroU;
  entry->presence = rackcapacity::PresenceState::Present;
  entry->reference = reference_for(rackcapacity::EvidenceSource::AssetRegistry, "ev-asset-" + id,
                                   observed_at_);
  return *this;
}

Bundle& Bundle::reservation(const std::string& id, rackcapacity::ReservationState state) {
  rackcapacity::ReservationEvidence* entry = find_reservation(id);
  entry->state = state;
  entry->reference = reference_for(rackcapacity::EvidenceSource::FacilityCapacityReservation,
                                   "ev-reservation-" + id, observed_at_);
  return *this;
}

Bundle& Bundle::reservation_span(const std::string& id, const std::string& span) {
  rackcapacity::ReservationEvidence* entry = find_reservation(id);
  const rackcapacity::Result<rackcapacity::SlotInterval> interval = single_interval(span);
  if (interval.has_value()) {
    entry->span = interval.value();
  }
  entry->kind = rackcapacity::MountSpanKind::FullSpan;
  return *this;
}

Bundle& Bundle::reservation_draw(const std::string& id, std::int64_t watts) {
  find_reservation(id)->draw = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::reservation_heat(const std::string& id, std::int64_t watts) {
  find_reservation(id)->heat_rejection = rackcapacity::Watts::trusted(watts);
  return *this;
}

Bundle& Bundle::reservation_mass(const std::string& id, std::int64_t grams) {
  find_reservation(id)->mass = rackcapacity::Grams::trusted(grams);
  return *this;
}

rackcapacity::RackCapacityInputs Bundle::build_unchecked() const { return inputs_; }

rackcapacity::RackCapacityInputs Bundle::build() const {
  rackcapacity::RackCapacityInputs inputs = inputs_;
  const rackcapacity::Status canonical = rackcapacity::canonicalize_inputs(inputs);
  if (!canonical.has_value()) {
    std::cout << "test fixture could not be canonicalized: "
              << rackcapacity::describe(canonical.error()) << "\n";
    std::abort();
  }
  const rackcapacity::Status valid = rackcapacity::validate_inputs(inputs);
  if (!valid.has_value()) {
    std::cout << "test fixture produced an invalid bundle: "
              << rackcapacity::describe(valid.error()) << "\n";
    std::abort();
  }
  return inputs;
}

// ---------------------------------------------------------------------------
// Catalog fixture
// ---------------------------------------------------------------------------

CatalogFixture::CatalogFixture(const std::string& tag, bool read_only) : dir(tag) {
  store_path_ = dir.child("capacity.rcstate");
  rackcapacity::CatalogOptions options;
  options.path = store_path_;
  options.writer_id = rackcapacity::WriterId::create("test-writer").value();
  options.read_only = read_only;
  options.create_if_missing = true;
  rackcapacity::Result<std::unique_ptr<rackcapacity::CapacityCatalog>> opened =
      rackcapacity::CapacityCatalog::open(options);
  if (!opened.has_value()) {
    std::cout << "catalog fixture could not open a store: "
              << rackcapacity::describe(opened.error()) << "\n";
    std::abort();
  }
  catalog = std::move(opened).value();
}

CatalogFixture::~CatalogFixture() {
  if (catalog) {
    (void)catalog->close();
    catalog.reset();
  }
}

// ---------------------------------------------------------------------------
// Real process control
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

// Standard Windows command line quoting: an argument is wrapped in double
// quotes and every backslash that precedes a quote or the closing quote is
// doubled.
[[nodiscard]] std::wstring quote_argument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted = L"\"";
  for (std::size_t index = 0; index < argument.size(); ++index) {
    std::size_t backslashes = 0;
    while (index < argument.size() && argument[index] == L'\\') {
      ++backslashes;
      ++index;
    }
    if (index == argument.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[index] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(argument[index]);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

[[nodiscard]] HANDLE open_output_file(const std::string& output_path) {
  if (output_path.empty()) {
    return INVALID_HANDLE_VALUE;
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(widen(output_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

[[nodiscard]] std::string read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

ChildProcess ChildProcess::start(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& output_path) {
  ChildProcess child;
  child.output_path_ = output_path;
  std::wstring command_line = quote_argument(widen(executable));
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_argument(widen(argument));
  }
  std::vector<wchar_t> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back(L'\0');

  const HANDLE output = open_output_file(output_path);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output == INVALID_HANDLE_VALUE ? ::GetStdHandle(STD_OUTPUT_HANDLE) : output;
  startup.hStdError = startup.hStdOutput;

  PROCESS_INFORMATION info{};
  const BOOL started =
      ::CreateProcessW(widen(executable).c_str(), mutable_line.data(), nullptr, nullptr,
                       output != INVALID_HANDLE_VALUE ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &info);
  if (output != INVALID_HANDLE_VALUE) {
    ::CloseHandle(output);
  }
  if (started == 0) {
    std::cout << "test harness could not start " << executable << "\n";
    return child;
  }
  child.handle_ = info.hProcess;
  child.pid_ = static_cast<std::uint64_t>(info.dwProcessId);
  ::CloseHandle(info.hThread);
  return child;
}

void ChildProcess::close() noexcept {
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

ChildProcess::~ChildProcess() { close(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), output_path_(std::move(other.output_path_)) {
  other.handle_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    pid_ = other.pid_;
    output_path_ = std::move(other.output_path_);
    other.handle_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ProcessResult ChildProcess::wait() {
  ProcessResult result;
  if (handle_ == nullptr) {
    return result;
  }
  ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD exit_code = 1;
  ::GetExitCodeProcess(static_cast<HANDLE>(handle_), &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  close();
  result.output = read_text_file(output_path_);
  return result;
}

void ChildProcess::kill() {
  if (handle_ == nullptr) {
    return;
  }
  ::TerminateProcess(static_cast<HANDLE>(handle_), 3);
  ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  close();
}

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
  // The child's output is captured to a temporary file so that a test can
  // assert on what the child reported. The file is removed before returning.
  const std::filesystem::path output =
      std::filesystem::path(temp_root()) /
      ("rcap-child-" + std::to_string(process_id()) + "-" +
       std::to_string(g_counter.fetch_add(1)) + ".out");
  ChildProcess child = ChildProcess::start(executable, arguments, output.string());
  if (!child.running()) {
    ProcessResult failed;
    failed.exit_code = -1;
    return failed;
  }
  ProcessResult result = child.wait();
  result.output = read_text_file(output.string());
  std::error_code error;
  std::filesystem::remove(output, error);
  return result;
}

#else  // POSIX

namespace {

[[nodiscard]] std::string read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

ChildProcess ChildProcess::start(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& output_path) {
  ChildProcess child;
  child.output_path_ = output_path;
  std::vector<std::string> storage;
  storage.push_back(executable);
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& entry : storage) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::cout << "test harness could not fork\n";
    return child;
  }
  if (pid == 0) {
    if (!output_path.empty()) {
      FILE* stream = std::freopen(output_path.c_str(), "w", stdout);
      if (stream != nullptr) {
        (void)std::freopen(output_path.c_str(), "a", stderr);
      }
    }
    ::execv(executable.c_str(), argv.data());
    std::_Exit(127);
  }
  child.pid_ = static_cast<std::uint64_t>(pid);
  child.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  return child;
}

void ChildProcess::close() noexcept { handle_ = nullptr; }

ChildProcess::~ChildProcess() { close(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), output_path_(std::move(other.output_path_)) {
  other.handle_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    pid_ = other.pid_;
    output_path_ = std::move(other.output_path_);
    other.handle_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ProcessResult ChildProcess::wait() {
  ProcessResult result;
  if (pid_ == 0) {
    return result;
  }
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else {
    result.exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  }
  pid_ = 0;
  close();
  result.output = read_text_file(output_path_);
  return result;
}

void ChildProcess::kill() {
  if (pid_ == 0) {
    return;
  }
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  pid_ = 0;
  close();
}

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
  const std::filesystem::path output =
      std::filesystem::path(temp_root()) /
      ("rcap-child-" + std::to_string(process_id()) + "-" +
       std::to_string(g_counter.fetch_add(1)) + ".out");
  ChildProcess child = ChildProcess::start(executable, arguments, output.string());
  ProcessResult result = child.wait();
  result.output = read_text_file(output.string());
  std::error_code error;
  std::filesystem::remove(output, error);
  return result;
}

#endif

bool wait_for_file(const std::string& path, std::uint32_t max_iterations) {
  for (std::uint32_t iteration = 0; iteration < max_iterations; ++iteration) {
    if (std::filesystem::exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::filesystem::exists(path);
}

std::string helper_executable(const char* compile_time_path) {
  std::string path(compile_time_path);
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

}  // namespace rctest
