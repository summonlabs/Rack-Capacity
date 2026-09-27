// Rack Capacity - typed evidence consumed from adjacent authoritative runtimes.
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
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/measures.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

// Which adjacent runtime an evidence record came from. Rack Capacity consumes
// every one of these and authors none of them.
enum class EvidenceSource : std::uint8_t {
  RackRegistry = 0,
  PhysicalLocationRegistry = 1,
  AssetRegistry = 2,
  FacilityPlacementPlanner = 3,
  PowerCapacity = 4,
  CoolingCapacity = 5,
  FacilityCapacityReservation = 6,
  PolicyAuthority = 7,
  MeasurementSystem = 8,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view evidence_source_name(
    EvidenceSource source) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<EvidenceSource> parse_evidence_source(
    std::string_view text);

// One immutable reference to one exact version of one external fact. The
// version and observed-at instant travel with the value so that a snapshot can
// state precisely which evidence it was derived from and whether that evidence
// is still current.
struct EvidenceReference {
  EvidenceSource source = EvidenceSource::RackRegistry;
  EvidenceId evidence{};
  std::uint32_t version = 0;
  TimestampNs observed_at{};
  SourceReference producer{};

  [[nodiscard]] bool is_set() const noexcept { return !evidence.empty() && version > 0; }
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] bool operator==(const EvidenceReference& other) const noexcept {
    return source == other.source && evidence == other.evidence && version == other.version &&
           observed_at == other.observed_at && producer == other.producer;
  }
  [[nodiscard]] bool operator!=(const EvidenceReference& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] bool operator<(const EvidenceReference& other) const noexcept;
};

// One validated reference. A bundle may not be built from a malformed
// reference, so every snapshot can name its provenance exactly.
[[nodiscard]] RACK_CAPACITY_API Status validate_reference(const EvidenceReference& reference,
                                                          std::string_view field);

// ---------------------------------------------------------------------------
// Rack composition evidence (owned by Rack Registry)
// ---------------------------------------------------------------------------

// Serviceability limits of a rack. Clearance figures are physical facts of the
// rack installation. `service_height_limit_unit`, when present, is the highest
// rack unit that can be serviced without special equipment; when absent the
// composition evidence declares that no service-height limit applies. There is
// no third state: the composition record itself is mandatory, so an unstated
// limit is a declared absence rather than missing evidence.
struct ServiceabilityLimits {
  Millimetres front_clearance{};
  Millimetres rear_clearance{};
  std::optional<std::uint32_t> service_height_limit_unit{};

  [[nodiscard]] bool operator==(const ServiceabilityLimits& other) const noexcept {
    return front_clearance == other.front_clearance &&
           rear_clearance == other.rear_clearance &&
           service_height_limit_unit == other.service_height_limit_unit;
  }
};

// The physical shape of one exact rack generation as published by rack
// composition authority. This is mandatory evidence: a capacity record cannot
// exist without the physical extent it accounts for.
struct RackCompositionEvidence {
  RackId rack{};
  RackCompositionGeneration generation{};
  std::uint32_t unit_count = 0;
  // Slots removed from general use by the rack structure itself: PDU rails,
  // brackets, cable management, structural members. Structurally reserved slots
  // are neither occupied nor free, and they are never candidates for anything.
  SlotSet structural_reserved_slots{};
  ServiceabilityLimits serviceability{};
  LocationReference location{};
  EvidenceReference reference{};

  [[nodiscard]] std::uint32_t slot_extent() const noexcept {
    return slot_extent_of(unit_count);
  }
};

// ---------------------------------------------------------------------------
// Occupancy evidence (owned by Asset Registry / Facility Placement Planner)
// ---------------------------------------------------------------------------

// One occupant reported against a rack. Rack Capacity consumes these as
// observations of what the placement authority has committed; it never creates,
// moves or removes an occupant.
struct AssetOccupancyEvidence {
  AssetId asset{};
  SlotInterval span{};
  MountSpanKind kind = MountSpanKind::FullSpan;
  SharedMountClassId shared_class{};
  std::uint32_t share_capacity = 0;
  PresenceState presence = PresenceState::Present;
  // Nameplate draw. Absent means the draw is unknown, which is never treated as
  // zero consumption.
  std::optional<Watts> nameplate_draw{};
  // Heat rejection declared by the asset record. Absent means heat is unknown
  // unless a policy supplies an explicit power-to-heat equivalence.
  std::optional<Watts> declared_heat_rejection{};
  // Mass. Absent means the mass is unknown, which is never treated as zero
  // load.
  std::optional<Grams> mass{};
  EvidenceReference reference{};

  [[nodiscard]] bool occupies_slots() const noexcept {
    return kind != MountSpanKind::ZeroU && presence != PresenceState::Absent;
  }
  [[nodiscard]] bool conflicts_are_allowed() const noexcept {
    return kind == MountSpanKind::SharedSpan;
  }
};

// ---------------------------------------------------------------------------
// Reservation evidence (owned by Facility Capacity Reservation)
// ---------------------------------------------------------------------------

// Lifecycle of a reservation as reported by reservation authority. Only a
// committed reservation consumes capacity. A submitted reservation is reported
// as planned work in progress and consumes nothing, because planned is not
// committed.
enum class ReservationState : std::uint8_t {
  Submitted = 0,
  Committed = 1,
  Released = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view reservation_state_name(
    ReservationState state) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<ReservationState> parse_reservation_state(
    std::string_view text);

struct ReservationEvidence {
  ReservationId reservation{};
  ReservationState state = ReservationState::Submitted;
  // A reservation may claim only power, only slots, or both. Every field is
  // optional and an absent field claims nothing.
  std::optional<SlotInterval> span{};
  MountSpanKind kind = MountSpanKind::FullSpan;
  std::optional<Watts> draw{};
  std::optional<Watts> heat_rejection{};
  std::optional<Grams> mass{};
  EvidenceReference reference{};

  [[nodiscard]] bool occupies_slots() const noexcept {
    return span.has_value() && kind != MountSpanKind::ZeroU;
  }
};

// ---------------------------------------------------------------------------
// Envelope evidence (owned by Power Capacity and Cooling Capacity)
// ---------------------------------------------------------------------------

// Redundancy topology of the power feeds serving one rack.
enum class RedundancyMode : std::uint8_t {
  // Every feed contributes; losing one feed loses capacity.
  None = 0,
  // N+1: one feed may be lost and the remaining feeds still carry the load.
  N1 = 1,
  // 2N: any one feed alone carries the whole load.
  N2 = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view redundancy_mode_name(
    RedundancyMode mode) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<RedundancyMode> parse_redundancy_mode(
    std::string_view text);

// The electrical envelope of one exact rack generation. `derate` is the
// physical or nameplate derating declared by power capacity authority, for
// example an 80 percent breaker derate.
struct PowerCapacityEvidence {
  std::uint32_t feed_count = 0;
  Watts watts_per_feed{};
  RedundancyMode redundancy = RedundancyMode::None;
  BasisPoints derate{};
  std::optional<Watts> measured_draw{};
  std::optional<TimestampNs> measured_at{};
  EvidenceReference reference{};
};

// The thermal envelope of one exact rack generation, expressed as the heat load
// the cooling path can remove. `supply_air_temp` is an observation carried for
// reporting; it never influences authoritative accounting.
struct CoolingCapacityEvidence {
  Watts nominal_heat_rejection{};
  BasisPoints derate{};
  std::optional<MilliCelsius> supply_air_temp{};
  std::optional<Watts> measured_heat_load{};
  std::optional<TimestampNs> measured_at{};
  EvidenceReference reference{};
};

// The static load envelope of one exact rack generation.
struct WeightCapacityEvidence {
  std::optional<Grams> static_load_limit{};
  BasisPoints derate{};
  std::optional<Grams> per_unit_point_load_limit{};
  std::optional<Grams> measured_static_load{};
  std::optional<TimestampNs> measured_at{};
  EvidenceReference reference{};
};

// ---------------------------------------------------------------------------
// Policy evidence (owned by policy authority)
// ---------------------------------------------------------------------------

// Headroom held back from physical limits by policy, and policy derating
// applied on top of physical derating. Policy headroom is subtracted after
// every physical limit and derate has been applied, so a snapshot can always
// state how much of the gap between nominal and usable is physical and how much
// is policy.
struct HeadroomPolicy {
  Watts power_headroom{};
  Watts cooling_headroom{};
  Grams weight_headroom{};
  std::uint32_t slot_headroom = 0;
  // Policy derating, applied after the physical derate from envelope evidence.
  BasisPoints power_derate{};
  BasisPoints cooling_derate{};
  BasisPoints weight_derate{};
};

// How stale evidence may be before a snapshot is reported as stale. Freshness
// is always evaluated against an instant supplied by the caller; the library
// never reads a clock on its own.
struct FreshnessPolicy {
  DurationNs max_envelope_age{};
  DurationNs max_measurement_age{};
};

// The resolved policy a rack's capacity is accounted under. The caller supplies
// it together with the reference that identifies it; this library never
// resolves, merges or authors policy.
struct CapacityPolicy {
  PolicyReference reference{};
  HeadroomPolicy headroom{};
  FreshnessPolicy freshness{};
  // Parts per million used to derive heat from nameplate draw when an asset
  // record does not declare heat rejection. Zero disables derivation entirely,
  // which is the default: heat is never inferred without an explicit policy.
  std::uint32_t heat_per_power_ppm = 0;
};

[[nodiscard]] RACK_CAPACITY_API Status validate_policy(const CapacityPolicy& policy);

// ---------------------------------------------------------------------------
// The evidence bundle for one rack
// ---------------------------------------------------------------------------

struct RackCapacityInputs {
  // Mandatory: the physical shape being accounted for.
  RackCompositionEvidence composition{};
  // Mandatory: the policy the accounting runs under.
  CapacityPolicy policy{};
  // Optional: absence means the dimension is unknown and is reported as
  // unknown, never as unconstrained.
  std::optional<PowerCapacityEvidence> power{};
  std::optional<CoolingCapacityEvidence> cooling{};
  std::optional<WeightCapacityEvidence> weight{};
  // Occupancy and reservation observations, in ascending identity order after
  // validation.
  std::vector<AssetOccupancyEvidence> assets{};
  std::vector<ReservationEvidence> reservations{};
  // Monotonic epoch of this bundle. A bundle whose epoch does not advance is
  // reported as a replay rather than applied again.
  EvidenceEpoch epoch{};
  TimestampNs captured_at{};
  ActorId actor{};
  RequestId request{};
};

// Validates a bundle against every structural rule this library enforces:
// identity well-formedness, slot bounds, duplicate identities, occupancy
// conflicts, shared class consistency and resource bounds. It does not evaluate
// capacity; evaluation is a separate, deterministic step.
//
// Occupants and reservations must be listed in ascending identity order. A
// bundle therefore has exactly one encoding and one digest, and two bundles
// carrying the same facts always compare equal. canonicalize_inputs() puts a
// caller-constructed bundle into that order.
[[nodiscard]] RACK_CAPACITY_API Status validate_inputs(const RackCapacityInputs& inputs);

// Sorts occupants and reservations into the canonical ascending identity order
// validation requires. The sort is stable and total: identity duplicates remain
// adjacent, so validation still reports them.
[[nodiscard]] RACK_CAPACITY_API Status canonicalize_inputs(RackCapacityInputs& inputs);

// Canonical digest of the facts in a bundle, excluding the request identity so
// that a genuine replay of the same facts is recognizable.
[[nodiscard]] RACK_CAPACITY_API StateDigest inputs_digest(const RackCapacityInputs& inputs);

// Canonical digest of every field of a bundle, including provenance and the
// request identity.
[[nodiscard]] RACK_CAPACITY_API StateDigest inputs_full_digest(const RackCapacityInputs& inputs);

}  // namespace rackcapacity
