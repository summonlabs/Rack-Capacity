// Rack Capacity - evidence validation, canonicalization and digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/evidence.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "codec.hpp"

namespace rackcapacity {
namespace {

[[nodiscard]] CapacityError evidence_error(ErrorCode code, std::string message,
                                           std::string subject, std::string related = {}) {
  return make_error(code, std::move(message),
                    ErrorDetail{"evidence.validate", std::move(subject), std::move(related), 0, 0,
                                {}});
}

// Collects the span of every occupant of a given kind into a slot set so that
// conflicts can be decided with exact interval algebra instead of pairwise
// comparison.
[[nodiscard]] Result<SlotSet> span_set_of(const std::vector<AssetOccupancyEvidence>& assets,
                                          bool shared) {
  std::vector<SlotInterval> spans;
  for (const AssetOccupancyEvidence& asset : assets) {
    if (asset.kind == MountSpanKind::ZeroU || asset.presence == PresenceState::Absent) {
      continue;
    }
    const bool is_shared = asset.kind == MountSpanKind::SharedSpan;
    if (is_shared != shared) {
      continue;
    }
    spans.push_back(asset.span);
  }
  return SlotSet::from_intervals(std::move(spans));
}

[[nodiscard]] bool set_touches_interval(const SlotSet& set, const SlotInterval& interval) {
  for (const SlotInterval& member : set.intervals()) {
    if (member.overlaps(interval)) {
      return true;
    }
    if (member.begin() > interval.begin()) {
      return false;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration names
// ---------------------------------------------------------------------------

std::string_view evidence_source_name(EvidenceSource source) noexcept {
  switch (source) {
    case EvidenceSource::RackRegistry:
      return "rack-registry";
    case EvidenceSource::PhysicalLocationRegistry:
      return "physical-location-registry";
    case EvidenceSource::AssetRegistry:
      return "asset-registry";
    case EvidenceSource::FacilityPlacementPlanner:
      return "facility-placement-planner";
    case EvidenceSource::PowerCapacity:
      return "power-capacity";
    case EvidenceSource::CoolingCapacity:
      return "cooling-capacity";
    case EvidenceSource::FacilityCapacityReservation:
      return "facility-capacity-reservation";
    case EvidenceSource::PolicyAuthority:
      return "policy-authority";
    case EvidenceSource::MeasurementSystem:
      return "measurement-system";
  }
  return "unknown";
}

Result<EvidenceSource> parse_evidence_source(std::string_view text) {
  if (text == "rack-registry") {
    return EvidenceSource::RackRegistry;
  }
  if (text == "physical-location-registry") {
    return EvidenceSource::PhysicalLocationRegistry;
  }
  if (text == "asset-registry") {
    return EvidenceSource::AssetRegistry;
  }
  if (text == "facility-placement-planner") {
    return EvidenceSource::FacilityPlacementPlanner;
  }
  if (text == "power-capacity") {
    return EvidenceSource::PowerCapacity;
  }
  if (text == "cooling-capacity") {
    return EvidenceSource::CoolingCapacity;
  }
  if (text == "facility-capacity-reservation") {
    return EvidenceSource::FacilityCapacityReservation;
  }
  if (text == "policy-authority") {
    return EvidenceSource::PolicyAuthority;
  }
  if (text == "measurement-system") {
    return EvidenceSource::MeasurementSystem;
  }
  return make_error(ErrorCode::UnknownEvidenceSource, "unknown evidence source",
                    ErrorDetail{"parse.evidence_source", std::string(text), {}, 0, 0, {}});
}

std::string_view reservation_state_name(ReservationState state) noexcept {
  switch (state) {
    case ReservationState::Submitted:
      return "submitted";
    case ReservationState::Committed:
      return "committed";
    case ReservationState::Released:
      return "released";
  }
  return "unknown";
}

Result<ReservationState> parse_reservation_state(std::string_view text) {
  if (text == "submitted" || text == "pending") {
    return ReservationState::Submitted;
  }
  if (text == "committed") {
    return ReservationState::Committed;
  }
  if (text == "released") {
    return ReservationState::Released;
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown reservation state",
                    ErrorDetail{"parse.reservation_state", std::string(text), {}, 0, 0, {}});
}

std::string_view redundancy_mode_name(RedundancyMode mode) noexcept {
  switch (mode) {
    case RedundancyMode::None:
      return "none";
    case RedundancyMode::N1:
      return "n+1";
    case RedundancyMode::N2:
      return "2n";
  }
  return "unknown";
}

Result<RedundancyMode> parse_redundancy_mode(std::string_view text) {
  if (text == "none" || text == "single") {
    return RedundancyMode::None;
  }
  if (text == "n+1" || text == "n1") {
    return RedundancyMode::N1;
  }
  if (text == "2n" || text == "n2") {
    return RedundancyMode::N2;
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown redundancy mode",
                    ErrorDetail{"parse.redundancy_mode", std::string(text), {}, 0, 0, {}});
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

std::string EvidenceReference::to_text() const {
  std::string text = std::string(evidence_source_name(source));
  text.push_back(':');
  text += evidence.text();
  text.push_back('@');
  text += std::to_string(version);
  return text;
}

bool EvidenceReference::operator<(const EvidenceReference& other) const noexcept {
  if (source != other.source) {
    return static_cast<std::uint8_t>(source) < static_cast<std::uint8_t>(other.source);
  }
  if (evidence != other.evidence) {
    return evidence < other.evidence;
  }
  if (version != other.version) {
    return version < other.version;
  }
  return observed_at < other.observed_at;
}

Status validate_reference(const EvidenceReference& reference, std::string_view field) {
  const std::string subject(field);
  if (reference.evidence.empty()) {
    return Status(evidence_error(ErrorCode::InvalidArgument,
                                 "evidence must name the record it was taken from", subject));
  }
  if (reference.version == 0) {
    return Status(evidence_error(ErrorCode::InvalidArgument,
                                 "evidence must name a version; version zero means unversioned",
                                 subject));
  }
  if (reference.observed_at.value() <= 0) {
    return Status(evidence_error(ErrorCode::InvalidTimestamp,
                                 "evidence must carry the instant it was observed at", subject));
  }
  return Status{};
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

Status validate_policy(const CapacityPolicy& policy) {
  if (!policy.reference.is_set()) {
    return Status(evidence_error(ErrorCode::InvalidArgument,
                                 "a capacity policy reference with a version is mandatory",
                                 "policy.reference"));
  }
  if (policy.heat_per_power_ppm > 1'000'000u) {
    return Status(make_error(
        ErrorCode::InvalidRange,
        "a heat equivalence above 1000000 parts per million is not a physical conversion",
        ErrorDetail{"policy.validate", "heat_per_power_ppm", {}, 1'000'000u,
                    policy.heat_per_power_ppm, {}}));
  }
  if (policy.headroom.slot_headroom > kMaxMountSlotExclusive - 1u) {
    return Status(make_error(
        ErrorCode::InvalidRange,
        "a policy slot headroom exceeds the largest representable rack",
        ErrorDetail{"policy.validate", "headroom.slot_headroom", {},
                    kMaxMountSlotExclusive - 1u, policy.headroom.slot_headroom, {}}));
  }
  return Status{};
}

// ---------------------------------------------------------------------------
// Canonical ordering
// ---------------------------------------------------------------------------

Status canonicalize_inputs(RackCapacityInputs& inputs) {
  std::stable_sort(inputs.assets.begin(), inputs.assets.end(),
                   [](const AssetOccupancyEvidence& left, const AssetOccupancyEvidence& right) {
                     if (left.asset != right.asset) {
                       return left.asset < right.asset;
                     }
                     return left.span < right.span;
                   });
  std::stable_sort(inputs.reservations.begin(), inputs.reservations.end(),
                   [](const ReservationEvidence& left, const ReservationEvidence& right) {
                     return left.reservation < right.reservation;
                   });
  return Status{};
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Status validate_inputs(const RackCapacityInputs& inputs) {
  const RackCompositionEvidence& composition = inputs.composition;

  if (composition.rack.empty()) {
    return Status(evidence_error(ErrorCode::MissingCompositionEvidence,
                                 "rack composition evidence must name the rack it describes",
                                 "composition.rack"));
  }
  if (composition.generation.value() < 1) {
    return Status(evidence_error(ErrorCode::InvalidRange,
                                 "a rack composition generation starts at one",
                                 "composition.generation"));
  }
  const Result<std::uint32_t> units = validate_unit_count(composition.unit_count);
  if (!units.has_value()) {
    return Status(units.error());
  }
  const std::uint32_t extent = slot_extent_of(units.value());

  for (const SlotInterval& interval : composition.structural_reserved_slots.intervals()) {
    if (interval.end() > extent) {
      return Status(make_error(
          ErrorCode::SlotOutOfBounds,
          "a structurally reserved slot lies outside the declared rack extent",
          ErrorDetail{"evidence.validate", "composition.structural_reserved_slots",
                      interval.to_text(), extent, interval.end(), {}}));
    }
  }
  Status status = validate_reference(composition.reference, "composition.reference");
  if (!status.has_value()) {
    return status;
  }
  status = validate_policy(inputs.policy);
  if (!status.has_value()) {
    return status;
  }
  if (inputs.epoch.value() < 1) {
    return Status(evidence_error(ErrorCode::InvalidRange,
                                 "an evidence epoch starts at one; an unversioned bundle cannot "
                                 "be ordered against a published one",
                                 "inputs.epoch"));
  }
  if (inputs.captured_at.value() <= 0) {
    return Status(evidence_error(ErrorCode::InvalidTimestamp,
                                 "an evidence bundle must state when it was captured",
                                 "inputs.captured_at"));
  }
  if (inputs.actor.empty()) {
    return Status(evidence_error(ErrorCode::EmptyValue,
                                 "an evidence bundle must name the actor that supplied it",
                                 "inputs.actor"));
  }
  if (inputs.request.empty()) {
    return Status(evidence_error(ErrorCode::EmptyValue,
                                 "an evidence bundle must carry a request identity for bounded "
                                 "idempotent replay",
                                 "inputs.request"));
  }
  if (inputs.assets.size() > kMaxAssetsPerRack) {
    return Status(make_error(ErrorCode::LimitExceeded,
                             "an evidence bundle carries more occupants than the documented bound",
                             ErrorDetail{"evidence.validate", "assets", {}, kMaxAssetsPerRack,
                                         inputs.assets.size(), {}}));
  }
  if (inputs.reservations.size() > kMaxReservationsPerRack) {
    return Status(make_error(
        ErrorCode::LimitExceeded,
        "an evidence bundle carries more reservations than the documented bound",
        ErrorDetail{"evidence.validate", "reservations", {}, kMaxReservationsPerRack,
                    inputs.reservations.size(), {}}));
  }

  // --- optional envelopes ------------------------------------------------
  if (inputs.power.has_value()) {
    const PowerCapacityEvidence& power = inputs.power.value();
    if (power.feed_count < 1 || power.feed_count > kMaxFeeds) {
      return Status(make_error(ErrorCode::InvalidRange,
                               "a power envelope must declare between one and the documented "
                               "maximum number of feeds",
                               ErrorDetail{"evidence.validate", "power.feed_count", {},
                                           kMaxFeeds, power.feed_count, {}}));
    }
    if (power.watts_per_feed.value() == 0) {
      return Status(evidence_error(ErrorCode::InvalidRange,
                                   "a power envelope with no watts per feed declares no capacity",
                                   "power.watts_per_feed"));
    }
    if (power.redundancy != RedundancyMode::None && power.feed_count < 2) {
      return Status(evidence_error(ErrorCode::InvalidArgument,
                                   "redundancy needs at least two feeds; one feed cannot be "
                                   "redundant",
                                   "power.feed_count"));
    }
    status = validate_reference(power.reference, "power.reference");
    if (!status.has_value()) {
      return status;
    }
  }
  if (inputs.cooling.has_value()) {
    status = validate_reference(inputs.cooling.value().reference, "cooling.reference");
    if (!status.has_value()) {
      return status;
    }
  }
  if (inputs.weight.has_value()) {
    status = validate_reference(inputs.weight.value().reference, "weight.reference");
    if (!status.has_value()) {
      return status;
    }
  }

  // --- occupancy ---------------------------------------------------------
  for (std::size_t index = 0; index < inputs.assets.size(); ++index) {
    const AssetOccupancyEvidence& asset = inputs.assets[index];
    if (asset.asset.empty()) {
      return Status(evidence_error(ErrorCode::EmptyValue, "an occupant must have an identity",
                                   "assets"));
    }
    if (index > 0 && inputs.assets[index - 1].asset == asset.asset) {
      return Status(make_error(ErrorCode::DuplicateAssetId,
                               "the same asset identity appears twice in one evidence bundle",
                               ErrorDetail{"evidence.validate", "assets", asset.asset.text(), 0, 0,
                                           {}}));
    }
    if (index > 0 && asset.asset < inputs.assets[index - 1].asset) {
      return Status(evidence_error(
          ErrorCode::InvalidArgument,
          "occupants must be listed in ascending identity order so that the bundle has exactly "
          "one canonical encoding",
          "assets", asset.asset.text()));
    }
    if (!asset.span.is_valid()) {
      return Status(evidence_error(ErrorCode::InvalidSlotInterval,
                                   "an occupant declares an invalid slot span", "assets",
                                   asset.asset.text()));
    }
    if (asset.kind != MountSpanKind::ZeroU && asset.span.end() > extent) {
      return Status(make_error(ErrorCode::SlotOutOfBounds,
                               "an occupant is mounted outside the declared rack extent",
                               ErrorDetail{"evidence.validate", "assets", asset.asset.text(),
                                           extent, asset.span.end(), {}}));
    }
    if (asset.kind == MountSpanKind::SharedSpan) {
      if (asset.shared_class.empty()) {
        return Status(evidence_error(ErrorCode::SharedClassMismatch,
                                     "a shared mount must name its shared class", "assets",
                                     asset.asset.text()));
      }
      if (asset.share_capacity < 1 || asset.share_capacity > kMaxShareCapacity) {
        return Status(make_error(ErrorCode::InvalidRange,
                                 "a shared mount capacity is outside the documented range",
                                 ErrorDetail{"evidence.validate", "assets", asset.asset.text(),
                                             kMaxShareCapacity, asset.share_capacity, {}}));
      }
    } else if (!asset.shared_class.empty() || asset.share_capacity != 0) {
      return Status(evidence_error(
          ErrorCode::SharedClassMismatch,
          "an exclusive mount must not declare a shared class or a share capacity", "assets",
          asset.asset.text()));
    }
    status = validate_reference(asset.reference, "assets.reference");
    if (!status.has_value()) {
      return status;
    }
    if (asset.presence != PresenceState::Absent && asset.kind != MountSpanKind::ZeroU &&
        set_touches_interval(composition.structural_reserved_slots, asset.span)) {
      return Status(make_error(
          ErrorCode::StructuralReservationConflict,
          "an occupant claims a slot the rack structure reserves",
          ErrorDetail{"evidence.validate", "assets", asset.asset.text(), 0, 0,
                      {composition.structural_reserved_slots.to_text(), asset.span.to_text()}}));
    }
  }

  // Exclusive occupants may never overlap each other.
  {
    Result<SlotSet> exclusive = span_set_of(inputs.assets, false);
    if (!exclusive.has_value()) {
      return Status(exclusive.error());
    }
    const std::uint64_t exclusive_slots = exclusive.value().slot_count();
    std::uint64_t declared = 0;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.kind == MountSpanKind::FullSpan && asset.presence != PresenceState::Absent) {
        declared += asset.span.slot_count();
      }
    }
    if (declared != exclusive_slots) {
      return Status(make_error(ErrorCode::DuplicateSlotOccupancy,
                               "two exclusive occupants claim overlapping slots",
                               ErrorDetail{"evidence.validate", "assets", {}, exclusive_slots,
                                           declared, {}}));
    }
  }

  // Shared occupants may only share an identical span within one class, and
  // never more of them than the declared share capacity.
  {
    Result<SlotSet> exclusive = span_set_of(inputs.assets, false);
    if (!exclusive.has_value()) {
      return Status(exclusive.error());
    }
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.kind != MountSpanKind::SharedSpan || asset.presence == PresenceState::Absent) {
        continue;
      }
      if (set_touches_interval(exclusive.value(), asset.span)) {
        return Status(make_error(ErrorCode::DuplicateSlotOccupancy,
                                 "a shared mount overlaps an exclusively mounted occupant",
                                 ErrorDetail{"evidence.validate", "assets", asset.asset.text(), 0,
                                             0, {}}));
      }
    }
    for (std::size_t index = 0; index < inputs.assets.size(); ++index) {
      const AssetOccupancyEvidence& asset = inputs.assets[index];
      if (asset.kind != MountSpanKind::SharedSpan || asset.presence == PresenceState::Absent) {
        continue;
      }
      std::uint32_t occupants = 0;
      for (const AssetOccupancyEvidence& other : inputs.assets) {
        if (other.kind != MountSpanKind::SharedSpan || other.presence == PresenceState::Absent) {
          continue;
        }
        if (!(other.span == asset.span)) {
          continue;
        }
        if (other.shared_class != asset.shared_class) {
          return Status(make_error(ErrorCode::SharedClassMismatch,
                                   "occupants share a slot span under different shared classes",
                                   ErrorDetail{"evidence.validate", "assets", other.asset.text(), 0,
                                               0, {asset.shared_class.text(),
                                                   other.shared_class.text()}}));
        }
        if (other.share_capacity != asset.share_capacity) {
          return Status(make_error(
              ErrorCode::SharedClassMismatch,
              "occupants share a slot span but declare different share capacities",
              ErrorDetail{"evidence.validate", "assets", other.asset.text(),
                          asset.share_capacity, other.share_capacity, {}}));
        }
        ++occupants;
      }
      if (occupants > asset.share_capacity) {
        return Status(make_error(ErrorCode::SharedClassCapacityExceeded,
                                 "more occupants share a mount span than its capacity allows",
                                 ErrorDetail{"evidence.validate", "assets", asset.asset.text(),
                                             asset.share_capacity, occupants, {}}));
      }
    }
  }

  // --- reservations ------------------------------------------------------
  for (std::size_t index = 0; index < inputs.reservations.size(); ++index) {
    const ReservationEvidence& reservation = inputs.reservations[index];
    if (reservation.reservation.empty()) {
      return Status(evidence_error(ErrorCode::EmptyValue, "a reservation must have an identity",
                                   "reservations"));
    }
    if (index > 0 && inputs.reservations[index - 1].reservation == reservation.reservation) {
      return Status(make_error(
          ErrorCode::DuplicateReservationId,
          "the same reservation identity appears twice in one evidence bundle",
          ErrorDetail{"evidence.validate", "reservations", reservation.reservation.text(), 0, 0,
                      {}}));
    }
    if (index > 0 && reservation.reservation < inputs.reservations[index - 1].reservation) {
      return Status(evidence_error(
          ErrorCode::InvalidArgument,
          "reservations must be listed in ascending identity order so that the bundle has exactly "
          "one canonical encoding",
          "reservations", reservation.reservation.text()));
    }
    if (reservation.span.has_value()) {
      if (!reservation.span->is_valid()) {
        return Status(evidence_error(ErrorCode::InvalidSlotInterval,
                                     "a reservation declares an invalid slot span",
                                     "reservations", reservation.reservation.text()));
      }
      if (reservation.kind != MountSpanKind::ZeroU && reservation.span->end() > extent) {
        return Status(make_error(ErrorCode::SlotOutOfBounds,
                                 "a reservation claims slots outside the declared rack extent",
                                 ErrorDetail{"evidence.validate", "reservations",
                                             reservation.reservation.text(), extent,
                                             reservation.span->end(), {}}));
      }
    }
    if (reservation.kind == MountSpanKind::ZeroU && reservation.span.has_value()) {
      return Status(evidence_error(ErrorCode::InvalidArgument,
                                   "a zero-u reservation claims no slot; remove its span",
                                   "reservations", reservation.reservation.text()));
    }
    status = validate_reference(reservation.reference, "reservations.reference");
    if (!status.has_value()) {
      return status;
    }
  }

  return Status{};
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

StateDigest inputs_digest(const RackCapacityInputs& inputs) {
  // The request identity is cleared before digesting so that a genuine replay
  // of the same facts under a new request identity is recognizable as the same
  // facts, while still being distinguishable by the full digest.
  RackCapacityInputs copy = inputs;
  copy.request = RequestId{};
  const Result<std::vector<std::uint8_t>> bytes = codec::encode_inputs_bytes(copy);
  if (!bytes.has_value()) {
    return StateDigest{};
  }
  return StateDigest::domain("rcap.inputs.v1", bytes.value());
}

StateDigest inputs_full_digest(const RackCapacityInputs& inputs) {
  const Result<std::vector<std::uint8_t>> bytes = codec::encode_inputs_bytes(inputs);
  if (!bytes.has_value()) {
    return StateDigest{};
  }
  return StateDigest::domain("rcap.inputs.full.v1", bytes.value());
}

}  // namespace rackcapacity
