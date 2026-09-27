// Rack Capacity - capacity accounting results and rack capacity snapshots.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/bound.hpp"
#include "rack_capacity/coordinates.hpp"
#include "rack_capacity/digest.hpp"
#include "rack_capacity/evidence.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/measures.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Reason codes
// ---------------------------------------------------------------------------
//
// Every conclusion this library reaches is accompanied by a stable, machine
// readable reason code. Codes are additive: their names and numeric values are
// part of the contract, and a reader may branch on them without parsing text.

enum class ReasonCode : std::uint16_t {
  Ok = 0,

  // Slot accounting.
  SlotAccountingExact = 100,
  SlotBlockedByStructure = 101,
  SlotOccupiedByPresentAsset = 102,
  SlotIndeterminatePresence = 103,
  SlotReservedCommitted = 104,
  SlotPendingSubmitted = 105,
  SlotPolicyHeadroomHeld = 106,
  SlotExhausted = 107,
  SlotFragmentedNoContiguousRun = 108,
  SlotOvercommitted = 109,

  // Power accounting.
  PowerEnvelopeUnknown = 200,
  PowerRedundancyReductionApplied = 201,
  PowerPhysicalDerateApplied = 202,
  PowerPolicyDerateApplied = 203,
  PowerPolicyHeadroomApplied = 204,
  PowerHeadroomExceedsEnvelope = 205,
  PowerUnknownConsumption = 206,
  PowerOvercommitted = 207,
  PowerMeasurementMissing = 208,
  PowerMeasurementStale = 209,
  PowerMeasurementExceedsCommitted = 210,

  // Cooling accounting.
  CoolingEnvelopeUnknown = 300,
  CoolingPhysicalDerateApplied = 301,
  CoolingPolicyDerateApplied = 302,
  CoolingPolicyHeadroomApplied = 303,
  CoolingHeadroomExceedsEnvelope = 304,
  CoolingUnknownHeat = 305,
  CoolingHeatDerivedFromPolicy = 306,
  CoolingOvercommitted = 307,
  CoolingMeasurementMissing = 308,
  CoolingMeasurementStale = 309,
  CoolingMeasurementExceedsCommitted = 310,

  // Weight accounting.
  WeightEnvelopeUnknown = 400,
  WeightPhysicalDerateApplied = 401,
  WeightPolicyDerateApplied = 402,
  WeightPolicyHeadroomApplied = 403,
  WeightHeadroomExceedsEnvelope = 404,
  WeightUnknownMass = 405,
  WeightOvercommitted = 406,
  WeightPointLoadExceeded = 407,
  WeightPointLoadUnknown = 408,
  WeightMeasurementStale = 409,

  // Serviceability.
  ServiceClearanceRecorded = 500,
  ServiceHeightLimitDeclared = 501,
  ServiceOccupancyAboveServiceHeight = 502,

  // Evidence standing.
  EvidenceFresh = 600,
  EvidenceStale = 601,
  EvidenceUnrevalidated = 602,
  EvidenceReferenceInvalid = 603,
  EvidenceEnvelopeMissing = 604,

  // Shared mount classes.
  SharedClassCapacityAvailable = 700,
  SharedClassCapacityExhausted = 701,
  SharedClassOvercommitted = 702,

  // Fit outcomes.
  FitAccepted = 800,
  FitSlotOccupied = 801,
  FitSlotBlockedByStructure = 802,
  FitSlotIndeterminate = 803,
  FitSlotNoAnchor = 804,
  FitSlotExhausted = 805,
  FitPowerInsufficient = 806,
  FitPowerIndeterminate = 807,
  FitCoolingInsufficient = 808,
  FitCoolingIndeterminate = 809,
  FitWeightInsufficient = 810,
  FitWeightIndeterminate = 811,
  FitPointLoadExceeded = 812,
  FitPointLoadIndeterminate = 813,
  FitServiceHeightExceeded = 814,
  FitServiceClearanceInsufficient = 815,
  FitRequestIncomplete = 816,

  // Closure and invariants.
  ClosureIdentitiesHold = 900,
  ClosureIdentityViolated = 901,
  ClosureArithmeticRefused = 902,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view reason_code_name(ReasonCode code) noexcept;

// One capacity dimension. The enumeration order is the canonical reporting
// order used to break ties when several dimensions are equally binding.
enum class Dimension : std::uint8_t {
  Slot = 0,
  Power = 1,
  Cooling = 2,
  Weight = 3,
  Serviceability = 4,
  Evidence = 5,
  None = 6,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view dimension_name(Dimension dimension) noexcept;

enum class Severity : std::uint8_t {
  Info = 0,
  Warning = 1,
  Blocking = 2,
  Unknown = 3,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view severity_name(Severity severity) noexcept;

// Whether a measurement that was supplied is still current under policy.
enum class MeasurementStanding : std::uint8_t {
  // No measurement was supplied.
  Missing = 0,
  Fresh = 1,
  Stale = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view measurement_standing_name(
    MeasurementStanding standing) noexcept;

// Whether the evidence a snapshot was derived from is still current. Freshness
// is always decided against an instant the caller supplies.
enum class EvidenceFreshness : std::uint8_t {
  Fresh = 0,
  Stale = 1,
  // The record was recovered from durable state and has not been revalidated
  // against current evidence in this process.
  Unrevalidated = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view evidence_freshness_name(
    EvidenceFreshness freshness) noexcept;

// One machine-readable explanation of a conclusion or a rejection.
struct Explanation {
  ReasonCode code = ReasonCode::Ok;
  Dimension dimension = Dimension::None;
  Severity severity = Severity::Info;
  std::string subject{};
  std::string message{};
  std::optional<std::int64_t> observed{};
  std::optional<std::int64_t> limit{};
  std::string unit{};

  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] bool operator<(const Explanation& other) const noexcept;
  [[nodiscard]] bool operator==(const Explanation& other) const noexcept;
};

// How close one dimension is to becoming binding, expressed in basis points of
// the usable envelope that is already claimed by occupied and committed load.
struct ConstraintPressure {
  Dimension dimension = Dimension::None;
  // True when the dimension currently has no headroom at all.
  bool binding = false;
  // True when the ratio is defined; false when the envelope is unknown.
  bool known = false;
  std::int64_t utilization_bp = 0;
};

// ---------------------------------------------------------------------------
// Per-dimension accounting
// ---------------------------------------------------------------------------

struct SlotFragmentation {
  std::uint32_t free_run_count = 0;
  std::uint32_t largest_free_run_slots = 0;
  std::uint32_t smallest_free_run_slots = 0;
  std::uint32_t isolated_free_slots = 0;
};

struct SlotAccounting {
  std::uint32_t total_slots = 0;
  std::uint32_t structural_reserved_slots = 0;
  std::uint32_t occupied_slots = 0;
  std::uint32_t shared_occupied_slots = 0;
  std::uint32_t indeterminate_slots = 0;
  std::uint32_t reserved_slots = 0;
  std::uint32_t pending_slots = 0;
  std::uint32_t policy_headroom_slots = 0;
  std::uint32_t overcommitted_slots = 0;
  // Free slots, conservatively bounded. The lower bound treats every uncertain
  // occupant as consuming its span; the upper bound does not.
  Bound<std::uint32_t> free{};
  // The structurally reserved, structurally occupied, indeterminate, reserved,
  // free and withheld slot sets. They are retained explicitly so that the slot
  // partition can be verified exactly on a decoded snapshot without consulting
  // the evidence bundle it came from.
  SlotSet structural_reserved_set{};
  SlotSet occupied_set{};
  SlotSet indeterminate_set{};
  SlotSet reserved_set{};
  SlotSet free_lower_set{};
  SlotSet free_upper_set{};
  // Free slots withheld by policy headroom. Retained explicitly so that the
  // slot partition can be verified exactly rather than inferred from a count.
  SlotSet policy_headroom_set{};
  SlotFragmentation fragmentation{};

  [[nodiscard]] bool is_exact() const noexcept { return free.is_exact(); }
};

struct PowerAccounting {
  bool envelope_known = false;
  std::uint32_t feed_count = 0;
  std::uint32_t contributing_feeds = 0;
  RedundancyMode redundancy = RedundancyMode::None;
  BasisPoints physical_derate{};
  BasisPoints policy_derate{};
  std::optional<Watts> effective_nominal{};
  std::optional<Watts> physically_derated{};
  std::optional<Watts> usable{};
  Watts policy_headroom{};
  Watts committed_known{};
  Watts reserved_known{};
  Watts pending_known{};
  std::uint32_t unknown_draw_assets = 0;
  std::uint32_t unknown_draw_reservations = 0;
  std::optional<Watts> measured{};
  std::optional<TimestampNs> measured_at{};
  MeasurementStanding measurement_standing = MeasurementStanding::Missing;
  // Committed and reserved load above the usable envelope, floored at zero.
  Watts overcommit{};
  Bound<Watts> free{};

  [[nodiscard]] bool is_exact() const noexcept { return free.is_exact(); }
};

struct CoolingAccounting {
  bool envelope_known = false;
  BasisPoints physical_derate{};
  BasisPoints policy_derate{};
  std::optional<Watts> nominal{};
  std::optional<Watts> physically_derated{};
  std::optional<Watts> usable{};
  Watts policy_headroom{};
  Watts declared_heat_known{};
  Watts derived_heat_known{};
  Watts reserved_heat_known{};
  Watts pending_heat_known{};
  std::uint32_t unknown_heat_assets = 0;
  std::uint32_t unknown_heat_reservations = 0;
  std::optional<MilliCelsius> supply_air_temp{};
  std::optional<Watts> measured{};
  std::optional<TimestampNs> measured_at{};
  MeasurementStanding measurement_standing = MeasurementStanding::Missing;
  Watts overcommit{};
  Bound<Watts> free{};

  [[nodiscard]] bool is_exact() const noexcept { return free.is_exact(); }
  // Declared plus policy-derived heat that is known to be committed. The sum is
  // guaranteed to be inside the documented measure range by evaluation-time
  // validation, so this accessor is exact.
  [[nodiscard]] Watts committed_known() const noexcept {
    return Watts::trusted(declared_heat_known.value() + derived_heat_known.value());
  }
};

struct WeightAccounting {
  bool envelope_known = false;
  BasisPoints physical_derate{};
  BasisPoints policy_derate{};
  std::optional<Grams> nominal_limit{};
  std::optional<Grams> physically_derated{};
  std::optional<Grams> usable{};
  Grams policy_headroom{};
  Grams occupied_known{};
  Grams reserved_known{};
  Grams pending_known{};
  std::uint32_t unknown_mass_assets = 0;
  std::uint32_t unknown_mass_reservations = 0;
  std::optional<Grams> per_unit_point_limit{};
  // Greatest distributed static load on any one rack unit, over present
  // occupants whose mass is known.
  std::optional<Grams> max_unit_load{};
  std::uint32_t units_at_or_above_point_limit = 0;
  std::optional<Grams> measured{};
  std::optional<TimestampNs> measured_at{};
  MeasurementStanding measurement_standing = MeasurementStanding::Missing;
  Grams overcommit{};
  Bound<Grams> free{};

  [[nodiscard]] bool is_exact() const noexcept { return free.is_exact(); }
};

struct ServiceabilityAccounting {
  Millimetres front_clearance{};
  Millimetres rear_clearance{};
  std::optional<std::uint32_t> service_height_limit_unit{};
  std::uint32_t occupied_units_above_service_height = 0;
};

struct SharedClassUtilization {
  SharedMountClassId shared_class{};
  SlotInterval span{};
  std::uint32_t share_capacity = 0;
  std::uint32_t used = 0;
  std::uint32_t reserved = 0;
  std::uint32_t pending = 0;
  std::uint32_t remaining = 0;
  std::uint32_t overcommitted = 0;

  [[nodiscard]] bool operator<(const SharedClassUtilization& other) const noexcept;
  [[nodiscard]] bool operator==(const SharedClassUtilization& other) const noexcept;
};

// Reasons why a snapshot cannot be considered complete. A snapshot with any
// unknown dimension is still valid and usable, but it is reported as partial so
// that no caller mistakes "unknown" for "free".
struct UnknownDimensions {
  bool power_envelope = false;
  bool cooling_envelope = false;
  bool weight_envelope = false;
  bool unknown_power_consumption = false;
  bool unknown_heat = false;
  bool unknown_mass = false;
  bool indeterminate_occupancy = false;

  [[nodiscard]] bool any() const noexcept {
    return power_envelope || cooling_envelope || weight_envelope || unknown_power_consumption ||
           unknown_heat || unknown_mass || indeterminate_occupancy;
  }
  [[nodiscard]] bool operator==(const UnknownDimensions& other) const noexcept {
    return power_envelope == other.power_envelope && cooling_envelope == other.cooling_envelope &&
           weight_envelope == other.weight_envelope &&
           unknown_power_consumption == other.unknown_power_consumption &&
           unknown_heat == other.unknown_heat && unknown_mass == other.unknown_mass &&
           indeterminate_occupancy == other.indeterminate_occupancy;
  }
};

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

struct RackCapacitySnapshot {
  RackId rack{};
  RackCompositionGeneration composition_generation{};
  CapacityGeneration capacity_generation{};
  SnapshotRevision revision{};
  EvidenceEpoch evidence_epoch{};
  TimestampNs evaluated_at{};
  // Oldest observed-at instant among the evidence that governs this snapshot.
  TimestampNs evidence_observed_at{};
  EvidenceFreshness freshness = EvidenceFreshness::Fresh;
  PolicyReference policy{};
  StateDigest policy_digest{};
  StateDigest inputs_digest{};
  std::uint32_t unit_count = 0;
  std::uint32_t slot_extent = 0;

  SlotAccounting slots{};
  PowerAccounting power{};
  CoolingAccounting cooling{};
  WeightAccounting weight{};
  ServiceabilityAccounting serviceability{};
  std::vector<SharedClassUtilization> shared_classes{};
  UnknownDimensions unknown{};
  std::vector<Explanation> explanations{};
  std::vector<ConstraintPressure> pressures{};
  // The dimension that becomes binding first, or Dimension::None when every
  // dimension still has headroom.
  Dimension primary_binding = Dimension::None;
  // True when at least one dimension is exactly at or beyond its usable limit.
  bool binding = false;

  // Canonical digest over every field of this snapshot except the digest
  // itself. Two snapshots with the same digest are the same capacity state.
  StateDigest digest{};

  [[nodiscard]] std::vector<Explanation> explanations_for(ReasonCode code) const;
  [[nodiscard]] bool has_code(ReasonCode code) const;
  [[nodiscard]] std::string to_text() const;
};

// Recomputes the canonical digest of a snapshot. Used by validation and by the
// decoder to detect a state file whose recorded digest does not match its
// content.
[[nodiscard]] RACK_CAPACITY_API StateDigest compute_snapshot_digest(
    const RackCapacitySnapshot& snapshot);

// ---------------------------------------------------------------------------
// Exact closure
// ---------------------------------------------------------------------------

// The accounting identities that must hold exactly for a snapshot to be
// internally consistent. Closure is verified after every evaluation and again
// after every decode; a snapshot that fails closure is refused rather than
// published.
struct ClosureReport {
  bool holds = false;
  std::vector<std::string> identities{};
  std::vector<std::string> violations{};

  [[nodiscard]] std::string to_text() const;
};

[[nodiscard]] RACK_CAPACITY_API ClosureReport verify_closure(
    const RackCapacitySnapshot& snapshot);

// ---------------------------------------------------------------------------
// Capacity record and lifecycle
// ---------------------------------------------------------------------------

enum class RackLifecycle : std::uint8_t {
  // Capacity is accounted for this rack and may be quoted.
  Active = 0,
  // The rack was retired. The record is retained for audit, capacity is never
  // quoted again, and no further evidence is accepted.
  Retired = 1,
  // The record failed revalidation against its own content. Its capacity is
  // refused, and it returns to Active only when fresh evidence is applied.
  Quarantined = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view rack_lifecycle_name(
    RackLifecycle state) noexcept;

// Whether a record's capacity may be quoted. A record recovered from durable
// state is not authoritative until it has been revalidated in this process
// against the evidence it carries.
enum class RecoveryStanding : std::uint8_t {
  Authoritative = 0,
  PendingRevalidation = 1,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view recovery_standing_name(
    RecoveryStanding standing) noexcept;

// One rack's complete capacity authority: the exact evidence it was derived
// from, the derived snapshot, and the generations that fence it.
struct RackCapacityRecord {
  RackId rack{};
  RackCompositionGeneration composition_generation{};
  CapacityGeneration capacity_generation{};
  SnapshotRevision revision{};
  RackLifecycle lifecycle = RackLifecycle::Active;
  RecoveryStanding standing = RecoveryStanding::Authoritative;
  RackCapacityInputs inputs{};
  RackCapacitySnapshot snapshot{};
  TimestampNs accepted_at{};
  AttemptId last_attempt{};
  ActorId last_actor{};

  [[nodiscard]] bool quotes_capacity() const noexcept {
    return lifecycle == RackLifecycle::Active && standing == RecoveryStanding::Authoritative;
  }
};

// Identifies one exact capacity state of one rack. Every operation that depends
// on current state carries one of these as an explicit precondition, and a
// mismatch is refused rather than merged.
struct CapacityExpectation {
  RackId rack{};
  RackCompositionGeneration composition_generation{};
  CapacityGeneration capacity_generation{};
  SnapshotRevision revision{};

  [[nodiscard]] static CapacityExpectation of(const RackCapacityRecord& record) noexcept;
  [[nodiscard]] static CapacityExpectation of(const RackCapacitySnapshot& snapshot) noexcept;
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] bool operator==(const CapacityExpectation& other) const noexcept;
};

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

// Deterministically evaluates capacity from a validated evidence bundle.
// `now` is supplied by the caller; the library never reads a clock itself.
// The same inputs and the same instant always produce the same snapshot,
// including its digest.
[[nodiscard]] RACK_CAPACITY_API Result<RackCapacitySnapshot> evaluate_capacity(
    const RackCapacityInputs& inputs, CapacityGeneration capacity_generation,
    SnapshotRevision revision, TimestampNs now, EvidenceFreshness freshness);

}  // namespace rackcapacity
