// Rack Capacity - deterministic capacity evaluation, closure and digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "codec.hpp"
#include "rack_capacity/capacity.hpp"

namespace rackcapacity {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void add_explanation(std::vector<Explanation>& explanations, ReasonCode code, Dimension dimension,
                     Severity severity, std::string subject, std::string message,
                     std::optional<std::int64_t> observed = std::nullopt,
                     std::optional<std::int64_t> limit = std::nullopt,
                     std::string unit = {}) {
  Explanation explanation;
  explanation.code = code;
  explanation.dimension = dimension;
  explanation.severity = severity;
  explanation.subject = std::move(subject);
  explanation.message = std::move(message);
  explanation.observed = observed;
  explanation.limit = limit;
  explanation.unit = std::move(unit);
  explanations.push_back(std::move(explanation));
}

[[nodiscard]] std::uint32_t as_u32(std::uint64_t value) {
  return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value);
}

// Unions a list of sets, propagating the cardinality bound instead of silently
// degrading to an empty set.
[[nodiscard]] Result<SlotSet> union_all(const std::vector<const SlotSet*>& sets) {
  SlotSet total = SlotSet::empty();
  for (const SlotSet* set : sets) {
    Result<SlotSet> merged = total.union_with(*set);
    if (!merged.has_value()) {
      return merged.error();
    }
    total = std::move(merged).value();
  }
  return total;
}

// Withholds `count` slots from the top of a free set. The rule is fixed and
// documented: policy slot headroom is taken from the highest numbered free
// slots, which is where rack slack is conventionally kept, and never from the
// middle of a contiguous run.
[[nodiscard]] Status hold_back_top(const SlotSet& free, std::uint32_t count, SlotSet& kept,
                                   SlotSet& held) {
  std::vector<SlotInterval> held_intervals;
  std::vector<SlotInterval> kept_intervals;
  std::uint32_t remaining = count;
  const std::vector<SlotInterval>& intervals = free.intervals();
  for (std::size_t reverse = intervals.size(); reverse > 0; --reverse) {
    const SlotInterval& interval = intervals[reverse - 1];
    if (remaining == 0) {
      kept_intervals.push_back(interval);
      continue;
    }
    const std::uint32_t length = interval.slot_count();
    if (length <= remaining) {
      held_intervals.push_back(interval);
      remaining -= length;
      continue;
    }
    const Result<SlotInterval> head =
        SlotInterval::create(interval.begin(), interval.end() - remaining);
    const Result<SlotInterval> tail =
        SlotInterval::create(interval.end() - remaining, interval.end());
    if (!head.has_value()) {
      return Status(head.error());
    }
    if (!tail.has_value()) {
      return Status(tail.error());
    }
    kept_intervals.push_back(head.value());
    held_intervals.push_back(tail.value());
    remaining = 0;
  }
  Result<SlotSet> kept_set = SlotSet::from_intervals(std::move(kept_intervals));
  if (!kept_set.has_value()) {
    return Status(kept_set.error());
  }
  Result<SlotSet> held_set = SlotSet::from_intervals(std::move(held_intervals));
  if (!held_set.has_value()) {
    return Status(held_set.error());
  }
  kept = std::move(kept_set).value();
  held = std::move(held_set).value();
  return Status{};
}

// ---------------------------------------------------------------------------
// Constraint pressure
// ---------------------------------------------------------------------------

struct PressureDraft {
  Dimension dimension = Dimension::None;
  bool binding = false;
  bool known = false;
  std::int64_t utilization_bp = 0;
};

template <typename MeasureType>
[[nodiscard]] PressureDraft pressure_of(Dimension dimension, const Bound<MeasureType>& free,
                                        const std::optional<MeasureType>& usable,
                                        std::int64_t claimed) {
  PressureDraft draft;
  draft.dimension = dimension;
  if (!free.is_known()) {
    // The envelope itself is unknown. Nothing can be concluded about how close
    // this dimension is to binding, and saying otherwise would invent a ratio.
    draft.known = false;
    return draft;
  }
  draft.known = true;
  draft.binding = free.upper().value() == 0;
  const std::int64_t whole = usable.has_value() ? usable->value() : 0;
  if (whole > 0) {
    const Result<std::int64_t> ratio = ratio_basis_points(claimed, whole);
    draft.utilization_bp = ratio.has_value() ? ratio.value() : 0;
    return draft;
  }
  // A dimension with no usable envelope is fully binding. Reporting 100 percent
  // when nothing is claimed and 200 percent when something is keeps the
  // ordering meaningful without inventing a ratio.
  draft.utilization_bp = claimed > 0 ? 20'000 : 10'000;
  return draft;
}

[[nodiscard]] PressureDraft pressure_of_slots(const SlotAccounting& slots) {
  PressureDraft draft;
  draft.dimension = Dimension::Slot;
  draft.known = slots.free.is_known();
  if (!draft.known) {
    return draft;
  }
  draft.binding = slots.free.upper() == 0;
  const std::int64_t usable =
      static_cast<std::int64_t>(slots.total_slots) - slots.structural_reserved_slots;
  const std::int64_t claimed = usable - static_cast<std::int64_t>(slots.free_upper_set.slot_count());
  if (usable > 0) {
    const Result<std::int64_t> ratio = ratio_basis_points(claimed < 0 ? 0 : claimed, usable);
    draft.utilization_bp = ratio.has_value() ? ratio.value() : 0;
    return draft;
  }
  draft.utilization_bp = claimed > 0 ? 20'000 : 10'000;
  return draft;
}

[[nodiscard]] bool pressure_less(const PressureDraft& left, const PressureDraft& right) {
  if (left.binding != right.binding) {
    return left.binding;
  }
  if (left.known != right.known) {
    return left.known;
  }
  if (left.utilization_bp != right.utilization_bp) {
    return left.utilization_bp > right.utilization_bp;
  }
  return static_cast<std::uint8_t>(left.dimension) < static_cast<std::uint8_t>(right.dimension);
}

// ---------------------------------------------------------------------------
// Measurement standing
// ---------------------------------------------------------------------------

[[nodiscard]] MeasurementStanding standing_of(const std::optional<TimestampNs>& measured_at,
                                              const FreshnessPolicy& policy,
                                              TimestampNs now, bool& stale) {
  stale = false;
  if (policy.max_measurement_age.value() <= 0) {
    // The policy declares no measurement age bound, so any observation that
    // carries an instant is current.
    return MeasurementStanding::Fresh;
  }
  if (!measured_at.has_value() || measured_at->value() <= 0) {
    stale = true;
    return MeasurementStanding::Stale;
  }
  if (now.value() < measured_at->value()) {
    // An observation stamped in the future cannot be ordered against now, so it
    // is not accepted as current.
    stale = true;
    return MeasurementStanding::Stale;
  }
  if (now.value() - measured_at->value() > policy.max_measurement_age.value()) {
    stale = true;
    return MeasurementStanding::Stale;
  }
  return MeasurementStanding::Fresh;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration names
// ---------------------------------------------------------------------------

std::string_view reason_code_name(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::Ok:
      return "ok";
    case ReasonCode::SlotAccountingExact:
      return "slot_accounting_exact";
    case ReasonCode::SlotBlockedByStructure:
      return "slot_blocked_by_structure";
    case ReasonCode::SlotOccupiedByPresentAsset:
      return "slot_occupied_by_present_asset";
    case ReasonCode::SlotIndeterminatePresence:
      return "slot_indeterminate_presence";
    case ReasonCode::SlotReservedCommitted:
      return "slot_reserved_committed";
    case ReasonCode::SlotPendingSubmitted:
      return "slot_pending_submitted";
    case ReasonCode::SlotPolicyHeadroomHeld:
      return "slot_policy_headroom_held";
    case ReasonCode::SlotExhausted:
      return "slot_exhausted";
    case ReasonCode::SlotFragmentedNoContiguousRun:
      return "slot_fragmented_no_contiguous_run";
    case ReasonCode::SlotOvercommitted:
      return "slot_overcommitted";
    case ReasonCode::PowerEnvelopeUnknown:
      return "power_envelope_unknown";
    case ReasonCode::PowerRedundancyReductionApplied:
      return "power_redundancy_reduction_applied";
    case ReasonCode::PowerPhysicalDerateApplied:
      return "power_physical_derate_applied";
    case ReasonCode::PowerPolicyDerateApplied:
      return "power_policy_derate_applied";
    case ReasonCode::PowerPolicyHeadroomApplied:
      return "power_policy_headroom_applied";
    case ReasonCode::PowerHeadroomExceedsEnvelope:
      return "power_headroom_exceeds_envelope";
    case ReasonCode::PowerUnknownConsumption:
      return "power_unknown_consumption";
    case ReasonCode::PowerOvercommitted:
      return "power_overcommitted";
    case ReasonCode::PowerMeasurementMissing:
      return "power_measurement_missing";
    case ReasonCode::PowerMeasurementStale:
      return "power_measurement_stale";
    case ReasonCode::PowerMeasurementExceedsCommitted:
      return "power_measurement_exceeds_committed";
    case ReasonCode::CoolingEnvelopeUnknown:
      return "cooling_envelope_unknown";
    case ReasonCode::CoolingPhysicalDerateApplied:
      return "cooling_physical_derate_applied";
    case ReasonCode::CoolingPolicyDerateApplied:
      return "cooling_policy_derate_applied";
    case ReasonCode::CoolingPolicyHeadroomApplied:
      return "cooling_policy_headroom_applied";
    case ReasonCode::CoolingHeadroomExceedsEnvelope:
      return "cooling_headroom_exceeds_envelope";
    case ReasonCode::CoolingUnknownHeat:
      return "cooling_unknown_heat";
    case ReasonCode::CoolingHeatDerivedFromPolicy:
      return "cooling_heat_derived_from_policy";
    case ReasonCode::CoolingOvercommitted:
      return "cooling_overcommitted";
    case ReasonCode::CoolingMeasurementMissing:
      return "cooling_measurement_missing";
    case ReasonCode::CoolingMeasurementStale:
      return "cooling_measurement_stale";
    case ReasonCode::CoolingMeasurementExceedsCommitted:
      return "cooling_measurement_exceeds_committed";
    case ReasonCode::WeightEnvelopeUnknown:
      return "weight_envelope_unknown";
    case ReasonCode::WeightPhysicalDerateApplied:
      return "weight_physical_derate_applied";
    case ReasonCode::WeightPolicyDerateApplied:
      return "weight_policy_derate_applied";
    case ReasonCode::WeightPolicyHeadroomApplied:
      return "weight_policy_headroom_applied";
    case ReasonCode::WeightHeadroomExceedsEnvelope:
      return "weight_headroom_exceeds_envelope";
    case ReasonCode::WeightUnknownMass:
      return "weight_unknown_mass";
    case ReasonCode::WeightOvercommitted:
      return "weight_overcommitted";
    case ReasonCode::WeightPointLoadExceeded:
      return "weight_point_load_exceeded";
    case ReasonCode::WeightPointLoadUnknown:
      return "weight_point_load_unknown";
    case ReasonCode::WeightMeasurementStale:
      return "weight_measurement_stale";
    case ReasonCode::ServiceClearanceRecorded:
      return "service_clearance_recorded";
    case ReasonCode::ServiceHeightLimitDeclared:
      return "service_height_limit_declared";
    case ReasonCode::ServiceOccupancyAboveServiceHeight:
      return "service_occupancy_above_service_height";
    case ReasonCode::EvidenceFresh:
      return "evidence_fresh";
    case ReasonCode::EvidenceStale:
      return "evidence_stale";
    case ReasonCode::EvidenceUnrevalidated:
      return "evidence_unrevalidated";
    case ReasonCode::EvidenceReferenceInvalid:
      return "evidence_reference_invalid";
    case ReasonCode::EvidenceEnvelopeMissing:
      return "evidence_envelope_missing";
    case ReasonCode::SharedClassCapacityAvailable:
      return "shared_class_capacity_available";
    case ReasonCode::SharedClassCapacityExhausted:
      return "shared_class_capacity_exhausted";
    case ReasonCode::SharedClassOvercommitted:
      return "shared_class_overcommitted";
    case ReasonCode::FitAccepted:
      return "fit_accepted";
    case ReasonCode::FitSlotOccupied:
      return "fit_slot_occupied";
    case ReasonCode::FitSlotBlockedByStructure:
      return "fit_slot_blocked_by_structure";
    case ReasonCode::FitSlotIndeterminate:
      return "fit_slot_indeterminate";
    case ReasonCode::FitSlotNoAnchor:
      return "fit_slot_no_anchor";
    case ReasonCode::FitSlotExhausted:
      return "fit_slot_exhausted";
    case ReasonCode::FitPowerInsufficient:
      return "fit_power_insufficient";
    case ReasonCode::FitPowerIndeterminate:
      return "fit_power_indeterminate";
    case ReasonCode::FitCoolingInsufficient:
      return "fit_cooling_insufficient";
    case ReasonCode::FitCoolingIndeterminate:
      return "fit_cooling_indeterminate";
    case ReasonCode::FitWeightInsufficient:
      return "fit_weight_insufficient";
    case ReasonCode::FitWeightIndeterminate:
      return "fit_weight_indeterminate";
    case ReasonCode::FitPointLoadExceeded:
      return "fit_point_load_exceeded";
    case ReasonCode::FitPointLoadIndeterminate:
      return "fit_point_load_indeterminate";
    case ReasonCode::FitServiceHeightExceeded:
      return "fit_service_height_exceeded";
    case ReasonCode::FitServiceClearanceInsufficient:
      return "fit_service_clearance_insufficient";
    case ReasonCode::FitRequestIncomplete:
      return "fit_request_incomplete";
    case ReasonCode::ClosureIdentitiesHold:
      return "closure_identities_hold";
    case ReasonCode::ClosureIdentityViolated:
      return "closure_identity_violated";
    case ReasonCode::ClosureArithmeticRefused:
      return "closure_arithmetic_refused";
  }
  return "unknown_reason";
}

std::string_view dimension_name(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Slot:
      return "slot";
    case Dimension::Power:
      return "power";
    case Dimension::Cooling:
      return "cooling";
    case Dimension::Weight:
      return "weight";
    case Dimension::Serviceability:
      return "serviceability";
    case Dimension::Evidence:
      return "evidence";
    case Dimension::None:
      return "none";
  }
  return "none";
}

std::string_view severity_name(Severity severity) noexcept {
  switch (severity) {
    case Severity::Info:
      return "info";
    case Severity::Warning:
      return "warning";
    case Severity::Blocking:
      return "blocking";
    case Severity::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view measurement_standing_name(MeasurementStanding standing) noexcept {
  switch (standing) {
    case MeasurementStanding::Missing:
      return "missing";
    case MeasurementStanding::Fresh:
      return "fresh";
    case MeasurementStanding::Stale:
      return "stale";
  }
  return "unknown";
}

std::string_view evidence_freshness_name(EvidenceFreshness freshness) noexcept {
  switch (freshness) {
    case EvidenceFreshness::Fresh:
      return "fresh";
    case EvidenceFreshness::Stale:
      return "stale";
    case EvidenceFreshness::Unrevalidated:
      return "unrevalidated";
  }
  return "unknown";
}

std::string_view rack_lifecycle_name(RackLifecycle state) noexcept {
  switch (state) {
    case RackLifecycle::Active:
      return "active";
    case RackLifecycle::Retired:
      return "retired";
    case RackLifecycle::Quarantined:
      return "quarantined";
  }
  return "unknown";
}

std::string_view recovery_standing_name(RecoveryStanding standing) noexcept {
  switch (standing) {
    case RecoveryStanding::Authoritative:
      return "authoritative";
    case RecoveryStanding::PendingRevalidation:
      return "pending-revalidation";
  }
  return "unknown";
}

bool Explanation::operator<(const Explanation& other) const noexcept {
  if (code != other.code) {
    return static_cast<std::uint16_t>(code) < static_cast<std::uint16_t>(other.code);
  }
  if (dimension != other.dimension) {
    return static_cast<std::uint8_t>(dimension) < static_cast<std::uint8_t>(other.dimension);
  }
  if (subject != other.subject) {
    return subject < other.subject;
  }
  if (observed != other.observed) {
    return observed < other.observed;
  }
  return message < other.message;
}

bool Explanation::operator==(const Explanation& other) const noexcept {
  return code == other.code && dimension == other.dimension && severity == other.severity &&
         subject == other.subject && message == other.message && observed == other.observed &&
         limit == other.limit && unit == other.unit;
}

bool SharedClassUtilization::operator<(const SharedClassUtilization& other) const noexcept {
  if (shared_class != other.shared_class) {
    return shared_class < other.shared_class;
  }
  return span < other.span;
}

bool SharedClassUtilization::operator==(const SharedClassUtilization& other) const noexcept {
  return shared_class == other.shared_class && span == other.span &&
         share_capacity == other.share_capacity && used == other.used &&
         reserved == other.reserved && pending == other.pending &&
         remaining == other.remaining && overcommitted == other.overcommitted;
}

std::vector<Explanation> RackCapacitySnapshot::explanations_for(ReasonCode code) const {
  std::vector<Explanation> matches;
  for (const Explanation& explanation : explanations) {
    if (explanation.code == code) {
      matches.push_back(explanation);
    }
  }
  return matches;
}

bool RackCapacitySnapshot::has_code(ReasonCode code) const {
  for (const Explanation& explanation : explanations) {
    if (explanation.code == code) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Expectation
// ---------------------------------------------------------------------------

CapacityExpectation CapacityExpectation::of(const RackCapacityRecord& record) noexcept {
  CapacityExpectation expectation;
  expectation.rack = record.rack;
  expectation.composition_generation = record.composition_generation;
  expectation.capacity_generation = record.capacity_generation;
  expectation.revision = record.revision;
  return expectation;
}

CapacityExpectation CapacityExpectation::of(const RackCapacitySnapshot& snapshot) noexcept {
  CapacityExpectation expectation;
  expectation.rack = snapshot.rack;
  expectation.composition_generation = snapshot.composition_generation;
  expectation.capacity_generation = snapshot.capacity_generation;
  expectation.revision = snapshot.revision;
  return expectation;
}

std::string CapacityExpectation::to_text() const {
  std::string text = rack.text();
  text += " composition=";
  text += std::to_string(composition_generation.value());
  text += " capacity=";
  text += std::to_string(capacity_generation.value());
  text += " revision=";
  text += std::to_string(revision.value());
  return text;
}

bool CapacityExpectation::operator==(const CapacityExpectation& other) const noexcept {
  return rack == other.rack && composition_generation == other.composition_generation &&
         capacity_generation == other.capacity_generation && revision == other.revision;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Result<RackCapacitySnapshot> evaluate_capacity(const RackCapacityInputs& inputs,
                                               CapacityGeneration capacity_generation,
                                               SnapshotRevision revision, TimestampNs now,
                                               EvidenceFreshness freshness) {
  const Status valid = validate_inputs(inputs);
  if (!valid.has_value()) {
    return valid.error();
  }

  RackCapacitySnapshot snapshot;
  snapshot.rack = inputs.composition.rack;
  snapshot.composition_generation = inputs.composition.generation;
  snapshot.capacity_generation = capacity_generation;
  snapshot.revision = revision;
  snapshot.evidence_epoch = inputs.epoch;
  snapshot.evaluated_at = now;
  snapshot.policy = inputs.policy.reference;
  snapshot.unit_count = inputs.composition.unit_count;
  snapshot.slot_extent = slot_extent_of(inputs.composition.unit_count);
  snapshot.inputs_digest = inputs_digest(inputs);

  const std::string subject = snapshot.rack.text();
  std::vector<Explanation>& notes = snapshot.explanations;

  // -----------------------------------------------------------------------
  // Evidence standing
  // -----------------------------------------------------------------------
  TimestampNs oldest = inputs.composition.reference.observed_at;
  const auto consider = [&oldest](const EvidenceReference& reference) {
    if (reference.observed_at.value() > 0 && reference.observed_at < oldest) {
      oldest = reference.observed_at;
    }
  };
  if (inputs.power.has_value()) {
    consider(inputs.power->reference);
  }
  if (inputs.cooling.has_value()) {
    consider(inputs.cooling->reference);
  }
  if (inputs.weight.has_value()) {
    consider(inputs.weight->reference);
  }
  for (const AssetOccupancyEvidence& asset : inputs.assets) {
    consider(asset.reference);
  }
  for (const ReservationEvidence& reservation : inputs.reservations) {
    consider(reservation.reference);
  }
  snapshot.evidence_observed_at = oldest;

  snapshot.freshness = freshness;
  if (freshness != EvidenceFreshness::Unrevalidated &&
      inputs.policy.freshness.max_envelope_age.value() > 0 && now.value() >= oldest.value() &&
      now.value() - oldest.value() > inputs.policy.freshness.max_envelope_age.value()) {
    snapshot.freshness = EvidenceFreshness::Stale;
  }
  if (snapshot.freshness == EvidenceFreshness::Stale) {
    add_explanation(notes, ReasonCode::EvidenceStale, Dimension::Evidence, Severity::Warning,
                    subject, "governing evidence is older than the freshness policy allows",
                    now.value() - oldest.value(), inputs.policy.freshness.max_envelope_age.value(),
                    "ns");
  } else if (snapshot.freshness == EvidenceFreshness::Unrevalidated) {
    add_explanation(notes, ReasonCode::EvidenceUnrevalidated, Dimension::Evidence,
                    Severity::Unknown, subject,
                    "the record was recovered from durable state and has not been revalidated "
                    "against current evidence");
  } else {
    add_explanation(notes, ReasonCode::EvidenceFresh, Dimension::Evidence, Severity::Info,
                    subject, "governing evidence is inside the freshness policy");
  }

  // -----------------------------------------------------------------------
  // Slot accounting
  //
  // The slot space of one rack generation is partitioned exactly once into
  // structurally reserved slots, occupied slots, committed reservation slots,
  // indeterminate slots, policy headroom and free slots. Every part is an
  // explicit set, so the partition can be verified rather than asserted.
  // -----------------------------------------------------------------------
  {
    SlotAccounting& slots = snapshot.slots;
    slots.total_slots = snapshot.slot_extent - 1u;
    slots.structural_reserved_slots =
        as_u32(inputs.composition.structural_reserved_slots.slot_count());

    std::vector<SlotInterval> exclusive;
    std::vector<SlotInterval> shared;
    std::vector<SlotInterval> indeterminate;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.kind == MountSpanKind::ZeroU) {
        continue;
      }
      if (asset.presence == PresenceState::Present) {
        if (asset.kind == MountSpanKind::SharedSpan) {
          shared.push_back(asset.span);
        } else {
          exclusive.push_back(asset.span);
        }
      } else if (asset.presence == PresenceState::Indeterminate) {
        indeterminate.push_back(asset.span);
      }
    }
    Result<SlotSet> exclusive_set = SlotSet::from_intervals(std::move(exclusive));
    if (!exclusive_set.has_value()) {
      return exclusive_set.error();
    }
    Result<SlotSet> shared_set = SlotSet::from_intervals(std::move(shared));
    if (!shared_set.has_value()) {
      return shared_set.error();
    }
    Result<SlotSet> indeterminate_raw = SlotSet::from_intervals(std::move(indeterminate));
    if (!indeterminate_raw.has_value()) {
      return indeterminate_raw.error();
    }
    Result<SlotSet> occupied_set = exclusive_set.value().union_with(shared_set.value());
    if (!occupied_set.has_value()) {
      return occupied_set.error();
    }

    std::vector<SlotInterval> reserved;
    std::vector<SlotInterval> pending;
    for (const ReservationEvidence& reservation : inputs.reservations) {
      if (!reservation.span.has_value() || reservation.kind == MountSpanKind::ZeroU) {
        continue;
      }
      if (reservation.state == ReservationState::Committed) {
        reserved.push_back(reservation.span.value());
      } else if (reservation.state == ReservationState::Submitted) {
        pending.push_back(reservation.span.value());
      }
    }
    Result<SlotSet> reserved_set = SlotSet::from_intervals(std::move(reserved));
    if (!reserved_set.has_value()) {
      return reserved_set.error();
    }
    Result<SlotSet> pending_set = SlotSet::from_intervals(std::move(pending));
    if (!pending_set.has_value()) {
      return pending_set.error();
    }

    const SlotSet& structural = inputs.composition.structural_reserved_slots;
    slots.structural_reserved_set = structural;
    // Committed claims: structure, occupants and committed reservations. Both
    // free bounds exclude all of them.
    Result<SlotSet> definite = union_all({&structural, &occupied_set.value(), &reserved_set.value()});
    if (!definite.has_value()) {
      return definite.error();
    }
    // Uncertainty is reported only for slots that no committed claim already
    // decides: an indeterminate occupant over an occupied slot adds no new
    // uncertainty.
    Result<SlotSet> uncertainty = indeterminate_raw.value().subtract(definite.value());
    if (!uncertainty.has_value()) {
      return uncertainty.error();
    }

    Result<SlotSet> available_lower = definite.value().union_with(uncertainty.value());
    if (!available_lower.has_value()) {
      return available_lower.error();
    }
    Result<SlotSet> free_lower_claimed = available_lower.value().complement(snapshot.slot_extent);
    if (!free_lower_claimed.has_value()) {
      return free_lower_claimed.error();
    }
    Result<SlotSet> free_upper_claimed = definite.value().complement(snapshot.slot_extent);
    if (!free_upper_claimed.has_value()) {
      return free_upper_claimed.error();
    }

    SlotSet kept;
    SlotSet held;
    const Status withheld =
        hold_back_top(free_lower_claimed.value(), inputs.policy.headroom.slot_headroom, kept, held);
    if (!withheld.has_value()) {
      return withheld.error();
    }
    Result<SlotSet> upper_after_headroom = free_upper_claimed.value().subtract(held);
    if (!upper_after_headroom.has_value()) {
      return upper_after_headroom.error();
    }

    slots.occupied_set = occupied_set.value();
    slots.shared_occupied_slots = as_u32(shared_set.value().slot_count());
    slots.occupied_slots = as_u32(occupied_set.value().slot_count());
    slots.indeterminate_set = uncertainty.value();
    slots.indeterminate_slots = as_u32(uncertainty.value().slot_count());
    slots.reserved_set = reserved_set.value();
    slots.reserved_slots = as_u32(reserved_set.value().slot_count());
    slots.pending_slots = as_u32(pending_set.value().slot_count());
    slots.policy_headroom_set = held;
    slots.policy_headroom_slots = as_u32(held.slot_count());
    slots.free_lower_set = kept;
    slots.free_upper_set = std::move(upper_after_headroom).value();
    slots.free =
        Bound<std::uint32_t>::between(as_u32(kept.slot_count()),
                                      as_u32(slots.free_upper_set.slot_count()));
    slots.fragmentation.free_run_count = as_u32(kept.run_count());
    slots.fragmentation.largest_free_run_slots = as_u32(kept.largest_run_slots());
    slots.fragmentation.smallest_free_run_slots = as_u32(kept.smallest_run_slots());
    slots.fragmentation.isolated_free_slots = as_u32(kept.isolated_slot_count());

    // A committed reservation is an overcommit only when it collides with slots
    // the structure or a present occupant already claims. A reservation over
    // nothing but other reservations is not double counting: the slot counts
    // come from the union of claimed slots, which counts a slot once.
    Result<SlotSet> physically_claimed = structural.union_with(occupied_set.value());
    if (!physically_claimed.has_value()) {
      return physically_claimed.error();
    }
    Result<SlotSet> collision = reserved_set.value().intersect(physically_claimed.value());
    if (!collision.has_value()) {
      return collision.error();
    }
    slots.overcommitted_slots = as_u32(collision.value().slot_count());

    if (slots.structural_reserved_slots > 0) {
      add_explanation(notes, ReasonCode::SlotBlockedByStructure, Dimension::Slot, Severity::Info,
                      subject, "slots withheld by the rack structure",
                      static_cast<std::int64_t>(slots.structural_reserved_slots), std::nullopt,
                      "slots");
    }
    if (slots.occupied_slots > 0) {
      add_explanation(notes, ReasonCode::SlotOccupiedByPresentAsset, Dimension::Slot,
                      Severity::Info, subject, "slots occupied by present occupants",
                      static_cast<std::int64_t>(slots.occupied_slots), std::nullopt, "slots");
    }
    if (slots.reserved_slots > 0) {
      add_explanation(notes, ReasonCode::SlotReservedCommitted, Dimension::Slot, Severity::Info,
                      subject, "slots claimed by committed reservations",
                      static_cast<std::int64_t>(slots.reserved_slots), std::nullopt, "slots");
    }
    if (slots.pending_slots > 0) {
      add_explanation(notes, ReasonCode::SlotPendingSubmitted, Dimension::Slot, Severity::Info,
                      subject,
                      "slots named by submitted but uncommitted reservations; these consume "
                      "nothing",
                      static_cast<std::int64_t>(slots.pending_slots), std::nullopt, "slots");
    }
    if (slots.indeterminate_slots > 0) {
      snapshot.unknown.indeterminate_occupancy = true;
      add_explanation(notes, ReasonCode::SlotIndeterminatePresence, Dimension::Slot,
                      Severity::Unknown, subject,
                      "slots whose occupancy cannot be established; they are not counted as free",
                      static_cast<std::int64_t>(slots.indeterminate_slots), std::nullopt, "slots");
    }
    if (slots.policy_headroom_slots > 0) {
      add_explanation(notes, ReasonCode::SlotPolicyHeadroomHeld, Dimension::Slot, Severity::Info,
                      subject, "free slots withheld by policy headroom",
                      static_cast<std::int64_t>(slots.policy_headroom_slots), std::nullopt, "slots");
    }
    if (slots.free.upper() == 0) {
      add_explanation(notes, ReasonCode::SlotExhausted, Dimension::Slot, Severity::Warning,
                      subject, "no free slot remains in this rack generation");
    }
    if (slots.overcommitted_slots > 0) {
      add_explanation(notes, ReasonCode::SlotOvercommitted, Dimension::Slot, Severity::Warning,
                      subject,
                      "committed reservations overlap slots that the structure or a present "
                      "occupant already claims",
                      static_cast<std::int64_t>(slots.overcommitted_slots), std::nullopt, "slots");
    }
  }

  // -----------------------------------------------------------------------
  // Shared mount classes
  // -----------------------------------------------------------------------
  {
    std::vector<SharedClassUtilization> entries;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.kind != MountSpanKind::SharedSpan) {
        continue;
      }
      const bool seen = std::any_of(
          entries.begin(), entries.end(), [&asset](const SharedClassUtilization& entry) {
            return entry.span == asset.span && entry.shared_class == asset.shared_class;
          });
      if (seen) {
        continue;
      }
      SharedClassUtilization entry;
      entry.shared_class = asset.shared_class;
      entry.span = asset.span;
      entry.share_capacity = asset.share_capacity;
      for (const AssetOccupancyEvidence& other : inputs.assets) {
        if (other.kind != MountSpanKind::SharedSpan || !(other.span == asset.span) ||
            other.shared_class != asset.shared_class) {
          continue;
        }
        if (other.presence == PresenceState::Present) {
          ++entry.used;
        } else if (other.presence == PresenceState::Indeterminate) {
          ++entry.pending;
        }
      }
      for (const ReservationEvidence& reservation : inputs.reservations) {
        if (reservation.kind != MountSpanKind::SharedSpan || !reservation.span.has_value() ||
            !(reservation.span.value() == asset.span)) {
          continue;
        }
        if (reservation.state == ReservationState::Committed) {
          ++entry.reserved;
        } else if (reservation.state == ReservationState::Submitted) {
          ++entry.pending;
        }
      }
      const std::uint32_t claimed_shares = entry.used + entry.reserved;
      entry.overcommitted =
          claimed_shares > entry.share_capacity ? claimed_shares - entry.share_capacity : 0u;
      entry.remaining = entry.overcommitted > 0 ? 0u : entry.share_capacity - claimed_shares;
      if (entry.overcommitted > 0) {
        add_explanation(notes, ReasonCode::SharedClassOvercommitted, Dimension::Slot,
                        Severity::Warning, entry.shared_class.text(),
                        "a shared mount class is claimed beyond its declared capacity",
                        static_cast<std::int64_t>(claimed_shares),
                        static_cast<std::int64_t>(entry.share_capacity), "shares");
      } else if (entry.remaining == 0) {
        add_explanation(notes, ReasonCode::SharedClassCapacityExhausted, Dimension::Slot,
                        Severity::Info, entry.shared_class.text(),
                        "a shared mount class has no remaining share capacity",
                        static_cast<std::int64_t>(entry.used),
                        static_cast<std::int64_t>(entry.share_capacity), "shares");
      }
      entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end());
    snapshot.shared_classes = std::move(entries);
  }

  // -----------------------------------------------------------------------
  // Power accounting
  // -----------------------------------------------------------------------
  {
    PowerAccounting& power = snapshot.power;
    Watts committed{};
    std::uint32_t unknown_assets = 0;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.presence == PresenceState::Present) {
        if (asset.nameplate_draw.has_value()) {
          const Result<Watts> total = committed.add(asset.nameplate_draw.value());
          if (!total.has_value()) {
            return total.error();
          }
          committed = total.value();
        } else {
          ++unknown_assets;
        }
      } else if (asset.presence == PresenceState::Indeterminate) {
        // An occupant whose presence is unknown may be drawing power, so its
        // consumption is unknown whether or not a nameplate is recorded.
        ++unknown_assets;
      }
    }
    Watts reserved{};
    Watts pending{};
    for (const ReservationEvidence& reservation : inputs.reservations) {
      if (!reservation.draw.has_value()) {
        continue;
      }
      if (reservation.state == ReservationState::Committed) {
        const Result<Watts> total = reserved.add(reservation.draw.value());
        if (!total.has_value()) {
          return total.error();
        }
        reserved = total.value();
      } else if (reservation.state == ReservationState::Submitted) {
        const Result<Watts> total = pending.add(reservation.draw.value());
        if (!total.has_value()) {
          return total.error();
        }
        pending = total.value();
      }
    }
    power.committed_known = committed;
    power.reserved_known = reserved;
    power.pending_known = pending;
    power.unknown_draw_assets = unknown_assets;

    if (!inputs.power.has_value()) {
      power.envelope_known = false;
      power.free = Bound<Watts>::unknown();
      snapshot.unknown.power_envelope = true;
      snapshot.unknown.unknown_power_consumption = true;
      add_explanation(notes, ReasonCode::PowerEnvelopeUnknown, Dimension::Power, Severity::Unknown,
                      subject,
                      "no power capacity evidence was supplied; free power is unknown, not "
                      "unlimited");
    } else {
      const PowerCapacityEvidence& evidence = inputs.power.value();
      power.envelope_known = true;
      power.feed_count = evidence.feed_count;
      power.redundancy = evidence.redundancy;
      power.physical_derate = evidence.derate;
      power.policy_derate = inputs.policy.headroom.power_derate;
      switch (evidence.redundancy) {
        case RedundancyMode::None:
          power.contributing_feeds = evidence.feed_count;
          break;
        case RedundancyMode::N1:
          power.contributing_feeds = evidence.feed_count - 1u;
          break;
        case RedundancyMode::N2:
          power.contributing_feeds = 1u;
          break;
      }
      const Result<Watts> nominal =
          evidence.watts_per_feed.multiply(static_cast<std::int64_t>(power.contributing_feeds));
      if (!nominal.has_value()) {
        return nominal.error();
      }
      power.effective_nominal = nominal.value();
      const Result<Watts> physical = derate(nominal.value(), evidence.derate);
      if (!physical.has_value()) {
        return physical.error();
      }
      power.physically_derated = physical.value();
      const Result<Watts> after_policy =
          derate(physical.value(), inputs.policy.headroom.power_derate);
      if (!after_policy.has_value()) {
        return after_policy.error();
      }
      power.policy_headroom = inputs.policy.headroom.power_headroom;
      power.usable = after_policy.value().saturating_subtract(power.policy_headroom);
      if (power.policy_headroom.value() > after_policy.value().value()) {
        add_explanation(notes, ReasonCode::PowerHeadroomExceedsEnvelope, Dimension::Power,
                        Severity::Warning, subject,
                        "policy headroom consumes the whole derated power envelope",
                        power.policy_headroom.value(), after_policy.value().value(), "W");
      }
      if (power.contributing_feeds != evidence.feed_count) {
        add_explanation(notes, ReasonCode::PowerRedundancyReductionApplied, Dimension::Power,
                        Severity::Info, subject,
                        "redundancy reduces the feeds that may be counted toward usable capacity",
                        static_cast<std::int64_t>(power.contributing_feeds),
                        static_cast<std::int64_t>(evidence.feed_count), "feeds");
      }
      if (evidence.derate.value() > 0) {
        add_explanation(notes, ReasonCode::PowerPhysicalDerateApplied, Dimension::Power,
                        Severity::Info, subject, "physical derating applied to nominal power",
                        evidence.derate.value(), nominal.value().value(), "bp");
      }
      if (inputs.policy.headroom.power_derate.value() > 0) {
        add_explanation(notes, ReasonCode::PowerPolicyDerateApplied, Dimension::Power,
                        Severity::Info, subject, "policy derating applied to physical power",
                        inputs.policy.headroom.power_derate.value(), physical.value().value(),
                        "bp");
      }
      if (power.policy_headroom.value() > 0) {
        add_explanation(notes, ReasonCode::PowerPolicyHeadroomApplied, Dimension::Power,
                        Severity::Info, subject, "policy headroom withheld from usable power",
                        power.policy_headroom.value(), after_policy.value().value(), "W");
      }

      const std::int64_t claims = committed.value() + reserved.value();
      const std::int64_t usable = power.usable.value().value();
      const std::int64_t raw = usable - claims;
      power.overcommit = Watts::trusted(raw < 0 ? -raw : 0);
      const std::int64_t upper = raw > 0 ? raw : 0;
      if (unknown_assets == 0) {
        power.free = Bound<Watts>::exact(Watts::trusted(upper));
      } else {
        power.free = Bound<Watts>::between(Watts::trusted(0), Watts::trusted(upper));
        snapshot.unknown.unknown_power_consumption = true;
        add_explanation(notes, ReasonCode::PowerUnknownConsumption, Dimension::Power,
                        Severity::Unknown, subject,
                        "one or more occupants have unknown power draw; the conservative free "
                        "power is zero until they are resolved",
                        static_cast<std::int64_t>(unknown_assets), std::nullopt, "occupants");
      }
      if (power.overcommit.value() > 0) {
        add_explanation(notes, ReasonCode::PowerOvercommitted, Dimension::Power, Severity::Warning,
                        subject, "committed and reserved load exceeds usable power",
                        power.overcommit.value(), power.usable.value().value(), "W");
      }
      if (evidence.measured_draw.has_value()) {
        power.measured = evidence.measured_draw.value();
        power.measured_at = evidence.measured_at;
        bool stale = false;
        power.measurement_standing =
            standing_of(evidence.measured_at, inputs.policy.freshness, now, stale);
        if (stale) {
          add_explanation(notes, ReasonCode::PowerMeasurementStale, Dimension::Power,
                          Severity::Warning, subject,
                          "the metered power observation is not current under the policy: it is "
                          "either older than the policy allows or carries no usable instant");
        } else {
          add_explanation(notes, ReasonCode::PowerMeasurementMissing, Dimension::Power,
                          Severity::Info, subject,
                          "metered power is recorded as an observation only; it never increases "
                          "authoritative free capacity");
        }
        if (evidence.measured_draw.value().value() > claims) {
          add_explanation(notes, ReasonCode::PowerMeasurementExceedsCommitted, Dimension::Power,
                          Severity::Warning, subject,
                          "metered draw exceeds declared committed load, which indicates load the "
                          "evidence does not describe",
                          evidence.measured_draw.value().value(), claims, "W");
        }
      } else {
        power.measurement_standing = MeasurementStanding::Missing;
        add_explanation(notes, ReasonCode::PowerMeasurementMissing, Dimension::Power,
                        Severity::Info, subject,
                        "no metered power observation was supplied; utilisation is accounted from "
                        "declared load only");
      }
    }
  }

  // -----------------------------------------------------------------------
  // Cooling accounting
  // -----------------------------------------------------------------------
  {
    CoolingAccounting& cooling = snapshot.cooling;
    Watts declared{};
    Watts derived{};
    std::uint32_t unknown_assets = 0;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.presence == PresenceState::Absent) {
        continue;
      }
      if (asset.presence == PresenceState::Indeterminate) {
        ++unknown_assets;
        continue;
      }
      if (asset.declared_heat_rejection.has_value()) {
        const Result<Watts> total = declared.add(asset.declared_heat_rejection.value());
        if (!total.has_value()) {
          return total.error();
        }
        declared = total.value();
        continue;
      }
      if (asset.nameplate_draw.has_value() && inputs.policy.heat_per_power_ppm > 0) {
        const Result<Watts> converted =
            heat_from_power(asset.nameplate_draw.value(), inputs.policy.heat_per_power_ppm);
        if (!converted.has_value()) {
          return converted.error();
        }
        const Result<Watts> total = derived.add(converted.value());
        if (!total.has_value()) {
          return total.error();
        }
        derived = total.value();
        continue;
      }
      ++unknown_assets;
    }
    Watts reserved{};
    Watts pending{};
    for (const ReservationEvidence& reservation : inputs.reservations) {
      if (!reservation.heat_rejection.has_value()) {
        continue;
      }
      if (reservation.state == ReservationState::Committed) {
        const Result<Watts> total = reserved.add(reservation.heat_rejection.value());
        if (!total.has_value()) {
          return total.error();
        }
        reserved = total.value();
      } else if (reservation.state == ReservationState::Submitted) {
        const Result<Watts> total = pending.add(reservation.heat_rejection.value());
        if (!total.has_value()) {
          return total.error();
        }
        pending = total.value();
      }
    }
    cooling.declared_heat_known = declared;
    cooling.derived_heat_known = derived;
    cooling.reserved_heat_known = reserved;
    cooling.pending_heat_known = pending;
    cooling.unknown_heat_assets = unknown_assets;
    if (derived.value() > 0) {
      add_explanation(notes, ReasonCode::CoolingHeatDerivedFromPolicy, Dimension::Cooling,
                      Severity::Info, subject,
                      "heat was derived from nameplate draw using the equivalence the policy "
                      "declares",
                      derived.value(), static_cast<std::int64_t>(inputs.policy.heat_per_power_ppm),
                      "W");
    }

    if (!inputs.cooling.has_value()) {
      cooling.envelope_known = false;
      cooling.free = Bound<Watts>::unknown();
      snapshot.unknown.cooling_envelope = true;
      snapshot.unknown.unknown_heat = true;
      add_explanation(notes, ReasonCode::CoolingEnvelopeUnknown, Dimension::Cooling,
                      Severity::Unknown, subject,
                      "no cooling capacity evidence was supplied; free cooling is unknown, not "
                      "unlimited");
    } else {
      const CoolingCapacityEvidence& evidence = inputs.cooling.value();
      cooling.envelope_known = true;
      cooling.nominal = evidence.nominal_heat_rejection;
      cooling.physical_derate = evidence.derate;
      cooling.policy_derate = inputs.policy.headroom.cooling_derate;
      cooling.supply_air_temp = evidence.supply_air_temp;
      const Result<Watts> physical = derate(evidence.nominal_heat_rejection, evidence.derate);
      if (!physical.has_value()) {
        return physical.error();
      }
      cooling.physically_derated = physical.value();
      const Result<Watts> after_policy =
          derate(physical.value(), inputs.policy.headroom.cooling_derate);
      if (!after_policy.has_value()) {
        return after_policy.error();
      }
      cooling.policy_headroom = inputs.policy.headroom.cooling_headroom;
      cooling.usable = after_policy.value().saturating_subtract(cooling.policy_headroom);
      if (cooling.policy_headroom.value() > after_policy.value().value()) {
        add_explanation(notes, ReasonCode::CoolingHeadroomExceedsEnvelope, Dimension::Cooling,
                        Severity::Warning, subject,
                        "policy headroom consumes the whole derated cooling envelope",
                        cooling.policy_headroom.value(), after_policy.value().value(), "W");
      }
      if (evidence.derate.value() > 0) {
        add_explanation(notes, ReasonCode::CoolingPhysicalDerateApplied, Dimension::Cooling,
                        Severity::Info, subject, "physical derating applied to nominal cooling",
                        evidence.derate.value(), evidence.nominal_heat_rejection.value(), "bp");
      }
      if (inputs.policy.headroom.cooling_derate.value() > 0) {
        add_explanation(notes, ReasonCode::CoolingPolicyDerateApplied, Dimension::Cooling,
                        Severity::Info, subject, "policy derating applied to physical cooling",
                        inputs.policy.headroom.cooling_derate.value(), physical.value().value(),
                        "bp");
      }
      if (cooling.policy_headroom.value() > 0) {
        add_explanation(notes, ReasonCode::CoolingPolicyHeadroomApplied, Dimension::Cooling,
                        Severity::Info, subject, "policy headroom withheld from usable cooling",
                        cooling.policy_headroom.value(), after_policy.value().value(), "W");
      }
      const std::int64_t claims = declared.value() + derived.value() + reserved.value();
      const std::int64_t usable = cooling.usable.value().value();
      const std::int64_t raw = usable - claims;
      cooling.overcommit = Watts::trusted(raw < 0 ? -raw : 0);
      const std::int64_t upper = raw > 0 ? raw : 0;
      if (unknown_assets == 0) {
        cooling.free = Bound<Watts>::exact(Watts::trusted(upper));
      } else {
        cooling.free = Bound<Watts>::between(Watts::trusted(0), Watts::trusted(upper));
        snapshot.unknown.unknown_heat = true;
        add_explanation(notes, ReasonCode::CoolingUnknownHeat, Dimension::Cooling,
                        Severity::Unknown, subject,
                        "one or more occupants have unknown heat rejection; the conservative free "
                        "cooling is zero until they are resolved",
                        static_cast<std::int64_t>(unknown_assets), std::nullopt, "occupants");
      }
      if (cooling.overcommit.value() > 0) {
        add_explanation(notes, ReasonCode::CoolingOvercommitted, Dimension::Cooling,
                        Severity::Warning, subject,
                        "committed and reserved heat exceeds usable cooling",
                        cooling.overcommit.value(), cooling.usable.value().value(), "W");
      }
      if (evidence.measured_heat_load.has_value()) {
        cooling.measured = evidence.measured_heat_load.value();
        cooling.measured_at = evidence.measured_at;
        bool stale = false;
        cooling.measurement_standing =
            standing_of(evidence.measured_at, inputs.policy.freshness, now, stale);
        if (stale) {
          add_explanation(notes, ReasonCode::CoolingMeasurementStale, Dimension::Cooling,
                          Severity::Warning, subject,
                          "the metered heat observation is not current under the policy: it is "
                          "either older than the policy allows or carries no usable instant");
        }
        if (evidence.measured_heat_load.value().value() > claims) {
          add_explanation(notes, ReasonCode::CoolingMeasurementExceedsCommitted,
                          Dimension::Cooling, Severity::Warning, subject,
                          "metered heat exceeds declared committed heat, which indicates load the "
                          "evidence does not describe",
                          evidence.measured_heat_load.value().value(), claims, "W");
        }
      } else {
        cooling.measurement_standing = MeasurementStanding::Missing;
        add_explanation(notes, ReasonCode::CoolingMeasurementMissing, Dimension::Cooling,
                        Severity::Info, subject,
                        "no metered heat observation was supplied; utilisation is accounted from "
                        "declared load only");
      }
    }
  }

  // -----------------------------------------------------------------------
  // Weight accounting
  // -----------------------------------------------------------------------
  {
    WeightAccounting& weight = snapshot.weight;
    Grams occupied{};
    std::uint32_t unknown_assets = 0;
    std::vector<std::uint64_t> unit_load(static_cast<std::size_t>(snapshot.unit_count) + 1u, 0u);
    bool point_load_uncertain = false;
    for (const AssetOccupancyEvidence& asset : inputs.assets) {
      if (asset.presence == PresenceState::Absent) {
        continue;
      }
      if (asset.presence == PresenceState::Indeterminate) {
        ++unknown_assets;
        point_load_uncertain = true;
        continue;
      }
      if (!asset.mass.has_value()) {
        ++unknown_assets;
        point_load_uncertain = true;
        continue;
      }
      const Result<Grams> total = occupied.add(asset.mass.value());
      if (!total.has_value()) {
        return total.error();
      }
      occupied = total.value();
      const std::uint32_t first_unit = asset.span.first_unit();
      const std::uint32_t last_unit = asset.span.last_unit();
      const std::uint32_t touched = last_unit >= first_unit ? last_unit - first_unit + 1u : 1u;
      // Mass is distributed evenly across the rack units the occupant spans,
      // with the remainder charged to its lowest unit. The rule is exact,
      // deterministic and documented; charging the whole mass to every touched
      // unit would over-report a multi-unit load by its height.
      const std::uint64_t per_unit = static_cast<std::uint64_t>(asset.mass->value()) / touched;
      const std::uint64_t remainder =
          static_cast<std::uint64_t>(asset.mass->value()) - per_unit * touched;
      for (std::uint32_t unit = first_unit; unit <= last_unit && unit <= snapshot.unit_count;
           ++unit) {
        unit_load[unit] += per_unit;
      }
      if (first_unit <= snapshot.unit_count) {
        unit_load[first_unit] += remainder;
      }
    }
    Grams reserved{};
    Grams pending{};
    for (const ReservationEvidence& reservation : inputs.reservations) {
      if (!reservation.mass.has_value()) {
        continue;
      }
      if (reservation.state == ReservationState::Committed) {
        const Result<Grams> total = reserved.add(reservation.mass.value());
        if (!total.has_value()) {
          return total.error();
        }
        reserved = total.value();
      } else if (reservation.state == ReservationState::Submitted) {
        const Result<Grams> total = pending.add(reservation.mass.value());
        if (!total.has_value()) {
          return total.error();
        }
        pending = total.value();
      }
    }
    weight.occupied_known = occupied;
    weight.reserved_known = reserved;
    weight.pending_known = pending;
    weight.unknown_mass_assets = unknown_assets;

    std::uint64_t peak = 0;
    for (std::uint32_t unit = 1; unit <= snapshot.unit_count; ++unit) {
      if (unit_load[unit] > peak) {
        peak = unit_load[unit];
      }
    }
    weight.max_unit_load = Grams::trusted(peak);
    if (inputs.weight.has_value()) {
      weight.per_unit_point_limit = inputs.weight.value().per_unit_point_load_limit;
      weight.physical_derate = inputs.weight.value().derate;
    }
    weight.policy_derate = inputs.policy.headroom.weight_derate;
    if (weight.per_unit_point_limit.has_value()) {
      const std::int64_t limit = weight.per_unit_point_limit->value();
      for (std::uint32_t unit = 1; unit <= snapshot.unit_count; ++unit) {
        const std::int64_t load = static_cast<std::int64_t>(unit_load[unit]);
        if (load > 0 && load >= limit) {
          ++weight.units_at_or_above_point_limit;
          if (load > limit) {
            add_explanation(notes, ReasonCode::WeightPointLoadExceeded, Dimension::Weight,
                            Severity::Warning, subject,
                            "the static load on one rack unit exceeds its point load limit", load,
                            limit, "g");
          }
        }
      }
    }

    if (!inputs.weight.has_value() || !inputs.weight.value().static_load_limit.has_value()) {
      weight.envelope_known = false;
      weight.free = Bound<Grams>::unknown();
      snapshot.unknown.weight_envelope = true;
      snapshot.unknown.unknown_mass = true;
      add_explanation(notes, ReasonCode::WeightEnvelopeUnknown, Dimension::Weight,
                      Severity::Unknown, subject,
                      "no static load limit was supplied; free weight capacity is unknown, not "
                      "unlimited");
    } else {
      const WeightCapacityEvidence& evidence = inputs.weight.value();
      weight.envelope_known = true;
      weight.nominal_limit = evidence.static_load_limit.value();
      const Result<Grams> physical = derate(evidence.static_load_limit.value(), evidence.derate);
      if (!physical.has_value()) {
        return physical.error();
      }
      weight.physically_derated = physical.value();
      const Result<Grams> after_policy =
          derate(physical.value(), inputs.policy.headroom.weight_derate);
      if (!after_policy.has_value()) {
        return after_policy.error();
      }
      weight.policy_headroom = inputs.policy.headroom.weight_headroom;
      weight.usable = after_policy.value().saturating_subtract(weight.policy_headroom);
      if (weight.policy_headroom.value() > after_policy.value().value()) {
        add_explanation(notes, ReasonCode::WeightHeadroomExceedsEnvelope, Dimension::Weight,
                        Severity::Warning, subject,
                        "policy headroom consumes the whole derated static load envelope",
                        weight.policy_headroom.value(), after_policy.value().value(), "g");
      }
      if (evidence.derate.value() > 0) {
        add_explanation(notes, ReasonCode::WeightPhysicalDerateApplied, Dimension::Weight,
                        Severity::Info, subject,
                        "physical derating applied to the static load limit",
                        evidence.derate.value(), evidence.static_load_limit->value(), "bp");
      }
      if (inputs.policy.headroom.weight_derate.value() > 0) {
        add_explanation(notes, ReasonCode::WeightPolicyDerateApplied, Dimension::Weight,
                        Severity::Info, subject, "policy derating applied to physical weight limit",
                        inputs.policy.headroom.weight_derate.value(), physical.value().value(),
                        "bp");
      }
      if (weight.policy_headroom.value() > 0) {
        add_explanation(notes, ReasonCode::WeightPolicyHeadroomApplied, Dimension::Weight,
                        Severity::Info, subject, "policy headroom withheld from usable weight",
                        weight.policy_headroom.value(), after_policy.value().value(), "g");
      }
      if (evidence.measured_static_load.has_value()) {
        weight.measured = evidence.measured_static_load.value();
        weight.measured_at = evidence.measured_at;
        bool stale = false;
        weight.measurement_standing =
            standing_of(evidence.measured_at, inputs.policy.freshness, now, stale);
        if (stale) {
          add_explanation(notes, ReasonCode::WeightMeasurementStale, Dimension::Weight,
                          Severity::Warning, subject,
                          "the measured static load is not current under the policy: it is either "
                          "older than the policy allows or carries no usable instant");
        }
      } else {
        weight.measurement_standing = MeasurementStanding::Missing;
      }

      const std::int64_t claims = occupied.value() + reserved.value();
      const std::int64_t usable = weight.usable.value().value();
      const std::int64_t raw = usable - claims;
      weight.overcommit = Grams::trusted(raw < 0 ? -raw : 0);
      const std::int64_t upper = raw > 0 ? raw : 0;
      if (unknown_assets == 0) {
        weight.free = Bound<Grams>::exact(Grams::trusted(upper));
      } else {
        weight.free = Bound<Grams>::between(Grams::trusted(0), Grams::trusted(upper));
        snapshot.unknown.unknown_mass = true;
        add_explanation(notes, ReasonCode::WeightUnknownMass, Dimension::Weight, Severity::Unknown,
                        subject,
                        "one or more occupants have unknown mass; the conservative free weight is "
                        "zero until they are resolved",
                        static_cast<std::int64_t>(unknown_assets), std::nullopt, "occupants");
      }
      if (weight.overcommit.value() > 0) {
        add_explanation(notes, ReasonCode::WeightOvercommitted, Dimension::Weight,
                        Severity::Warning, subject,
                        "committed and reserved static load exceeds usable weight",
                        weight.overcommit.value(), weight.usable->value(), "g");
      }
    }
    if (point_load_uncertain && weight.per_unit_point_limit.has_value()) {
      add_explanation(notes, ReasonCode::WeightPointLoadUnknown, Dimension::Weight,
                      Severity::Unknown, subject,
                      "point load on at least one rack unit cannot be decided because an occupant "
                      "mass is unknown");
    }
  }

  // -----------------------------------------------------------------------
  // Serviceability
  // -----------------------------------------------------------------------
  {
    ServiceabilityAccounting& service = snapshot.serviceability;
    service.front_clearance = inputs.composition.serviceability.front_clearance;
    service.rear_clearance = inputs.composition.serviceability.rear_clearance;
    service.service_height_limit_unit = inputs.composition.serviceability.service_height_limit_unit;
    if (service.service_height_limit_unit.has_value()) {
      const std::uint32_t limit = service.service_height_limit_unit.value();
      add_explanation(notes, ReasonCode::ServiceHeightLimitDeclared, Dimension::Serviceability,
                      Severity::Info, subject,
                      "the composition declares a service height limit",
                      static_cast<std::int64_t>(limit), std::nullopt, "U");
      std::vector<bool> occupied_unit(static_cast<std::size_t>(snapshot.unit_count) + 1u, false);
      for (const AssetOccupancyEvidence& asset : inputs.assets) {
        if (asset.presence != PresenceState::Present || asset.kind == MountSpanKind::ZeroU) {
          continue;
        }
        for (std::uint32_t unit = asset.span.first_unit();
             unit <= asset.span.last_unit() && unit <= snapshot.unit_count; ++unit) {
          occupied_unit[unit] = true;
        }
      }
      for (std::uint32_t unit = limit + 1u; unit <= snapshot.unit_count; ++unit) {
        if (occupied_unit[unit]) {
          ++service.occupied_units_above_service_height;
        }
      }
      if (service.occupied_units_above_service_height > 0) {
        add_explanation(notes, ReasonCode::ServiceOccupancyAboveServiceHeight,
                        Dimension::Serviceability, Severity::Warning, subject,
                        "occupants sit above the service height limit and cannot be serviced in "
                        "place without additional equipment",
                        static_cast<std::int64_t>(service.occupied_units_above_service_height),
                        static_cast<std::int64_t>(limit), "U");
      }
    }
    add_explanation(notes, ReasonCode::ServiceClearanceRecorded, Dimension::Serviceability,
                    Severity::Info, subject, "service clearance recorded for this rack generation",
                    service.front_clearance.value(), service.rear_clearance.value(), "mm");
  }

  // -----------------------------------------------------------------------
  // Explanations and pressures
  // -----------------------------------------------------------------------
  std::sort(notes.begin(), notes.end());
  if (notes.size() > kMaxExplanationsPerSnapshot) {
    return make_error(ErrorCode::LimitExceeded,
                      "a snapshot produced more explanations than the documented bound",
                      ErrorDetail{"capacity.evaluate", subject, {}, kMaxExplanationsPerSnapshot,
                                  notes.size(), {}});
  }

  std::vector<PressureDraft> drafts;
  drafts.push_back(pressure_of_slots(snapshot.slots));
  drafts.push_back(pressure_of(Dimension::Power, snapshot.power.free, snapshot.power.usable,
                               snapshot.power.committed_known.value() +
                                   snapshot.power.reserved_known.value()));
  drafts.push_back(pressure_of(Dimension::Cooling, snapshot.cooling.free, snapshot.cooling.usable,
                               snapshot.cooling.committed_known().value() +
                                   snapshot.cooling.reserved_heat_known.value()));
  drafts.push_back(pressure_of(Dimension::Weight, snapshot.weight.free, snapshot.weight.usable,
                               snapshot.weight.occupied_known.value() +
                                   snapshot.weight.reserved_known.value()));
  std::sort(drafts.begin(), drafts.end(), pressure_less);
  for (const PressureDraft& draft : drafts) {
    ConstraintPressure pressure;
    pressure.dimension = draft.dimension;
    pressure.binding = draft.binding;
    pressure.known = draft.known;
    pressure.utilization_bp = draft.utilization_bp;
    snapshot.pressures.push_back(pressure);
    if (draft.binding) {
      snapshot.binding = true;
    }
  }
  snapshot.primary_binding = drafts.empty() ? Dimension::None : drafts.front().dimension;

  snapshot.digest = compute_snapshot_digest(snapshot);

  const ClosureReport closure = verify_closure(snapshot);
  if (!closure.holds) {
    std::vector<std::string> violations = closure.violations;
    return make_error(ErrorCode::ClosureFailure, "the evaluated snapshot does not close exactly",
                      ErrorDetail{"capacity.evaluate", subject, {}, 0, 0, std::move(violations)});
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Digest and closure
// ---------------------------------------------------------------------------

StateDigest compute_snapshot_digest(const RackCapacitySnapshot& snapshot) {
  RackCapacitySnapshot copy = snapshot;
  copy.digest = StateDigest{};
  const Result<std::vector<std::uint8_t>> bytes = codec::encode_snapshot_bytes(copy);
  if (!bytes.has_value()) {
    return StateDigest{};
  }
  return StateDigest::domain("rcap.snapshot.v1", bytes.value());
}

ClosureReport verify_closure(const RackCapacitySnapshot& snapshot) {
  // A set operation that cannot be performed is reported as a violated
  // identity rather than as a passing one. Every failure path here therefore
  // fails closed: the snapshot is refused instead of accepted.
  ClosureReport report;
  const auto identity = [&report](bool holds, std::string description) {
    report.identities.push_back(description);
    if (!holds) {
      report.violations.push_back(std::move(description));
    }
  };

  // --- slots -------------------------------------------------------------
  {
    const SlotAccounting& slots = snapshot.slots;
    const Result<SlotInterval> extent = rack_slot_extent(snapshot.unit_count);
    if (!extent.has_value()) {
      report.violations.push_back("slots: the declared rack height is not representable");
      report.holds = false;
      return report;
    }
    const SlotSet whole = SlotSet::single(extent.value()).value_or(SlotSet::empty());

    identity(whole.slot_count() == slots.total_slots,
             "slots: the declared extent covers exactly total_slots slots");

    const Result<SlotSet> parts = union_all({&slots.structural_reserved_set, &slots.occupied_set,
                                             &slots.indeterminate_set, &slots.reserved_set,
                                             &slots.free_lower_set, &slots.policy_headroom_set});
    if (!parts.has_value()) {
      report.violations.push_back("slots: the partition could not be recomputed");
      report.holds = false;
      return report;
    }
    identity(parts.value() == whole,
             "slots: structure + occupied + reserved + indeterminate + free + policy headroom "
             "covers the rack extent");
    if (parts.value() == whole) {
      const std::uint64_t sum = slots.structural_reserved_slots + slots.occupied_slots +
                                slots.reserved_slots + slots.indeterminate_slots +
                                slots.free_lower_set.slot_count() + slots.policy_headroom_slots;
      identity(sum == static_cast<std::uint64_t>(slots.total_slots) + slots.overcommitted_slots,
               "slots: the parts sum to total_slots plus the slots claimed twice");
    }
    identity(slots.structural_reserved_set.slot_count() == slots.structural_reserved_slots,
             "slots: the structurally reserved set matches its recorded size");
    identity(slots.occupied_set.slot_count() == slots.occupied_slots,
             "slots: the occupied set matches its recorded size");
    identity(slots.indeterminate_set.slot_count() == slots.indeterminate_slots,
             "slots: the indeterminate set matches its recorded size");
    identity(slots.reserved_set.slot_count() == slots.reserved_slots,
             "slots: the reserved set matches its recorded size");
    identity(slots.policy_headroom_set.slot_count() == slots.policy_headroom_slots,
             "slots: the withheld set matches its recorded size");
    identity(slots.free_lower_set.subtract(slots.free_upper_set).value_or(SlotSet::empty())
                 .is_empty(),
             "slots: the conservative free set is contained in the optimistic free set");
    identity(slots.policy_headroom_set.intersect(slots.free_lower_set)
                 .value_or(SlotSet::empty())
                 .is_empty(),
             "slots: policy headroom is disjoint from the free set it was withheld from");
    {
      const Result<SlotSet> claims =
          union_all({&slots.structural_reserved_set, &slots.occupied_set, &slots.reserved_set,
                     &slots.indeterminate_set});
      identity(claims.has_value() &&
                   slots.policy_headroom_set.subtract(claims.value())
                           .value_or(SlotSet::empty()) == slots.policy_headroom_set,
               "slots: policy headroom is drawn from slots no committed claim already holds");
    }
    identity(slots.free.is_known() && slots.free.lower() == slots.free_lower_set.slot_count() &&
                 slots.free.upper() == slots.free_upper_set.slot_count(),
             "slots: the free bound equals the cardinality of the free sets");
    identity(slots.shared_occupied_slots <= slots.occupied_slots,
             "slots: shared occupancy is a subset of total occupancy");
  }

  // --- power -------------------------------------------------------------
  {
    const PowerAccounting& power = snapshot.power;
    if (power.envelope_known) {
      identity(power.usable.has_value(), "power: a known envelope has a usable figure");
      if (power.usable.has_value()) {
        const std::int64_t claims = power.committed_known.value() + power.reserved_known.value();
        const std::int64_t upper = power.free.is_known() ? power.free.upper().value() : -1;
        const std::int64_t lower = power.free.is_known() ? power.free.lower().value() : -1;
        identity(upper >= 0 && lower >= 0, "power: a known envelope reports a known free bound");
        identity(claims + upper == power.usable.value().value() + power.overcommit.value(),
                 "power: committed + reserved + free == usable + overcommit");
        identity(lower <= upper, "power: the free bound is ordered");
        identity(power.unknown_draw_assets > 0 ? lower == 0 : lower == upper,
                 "power: an unknown draw forces the conservative free figure to zero");
      }
    } else {
      identity(!power.free.is_known(),
               "power: an unknown envelope reports unknown free capacity, never a number");
    }
  }

  // --- cooling -----------------------------------------------------------
  {
    const CoolingAccounting& cooling = snapshot.cooling;
    if (cooling.envelope_known) {
      identity(cooling.usable.has_value(), "cooling: a known envelope has a usable figure");
      if (cooling.usable.has_value()) {
        const std::int64_t claims = cooling.committed_known().value() +
                                    cooling.reserved_heat_known.value();
        const std::int64_t upper = cooling.free.is_known() ? cooling.free.upper().value() : -1;
        const std::int64_t lower = cooling.free.is_known() ? cooling.free.lower().value() : -1;
        identity(upper >= 0 && lower >= 0, "cooling: a known envelope reports a known free bound");
        identity(claims + upper == cooling.usable->value() + cooling.overcommit.value(),
                 "cooling: committed + reserved + free == usable + overcommit");
        identity(lower <= upper, "cooling: the free bound is ordered");
        identity(cooling.unknown_heat_assets > 0 ? lower == 0 : lower == upper,
                 "cooling: unknown heat forces the conservative free figure to zero");
      }
    } else {
      identity(!cooling.free.is_known(),
               "cooling: an unknown envelope reports unknown free capacity, never a number");
    }
  }

  // --- weight ------------------------------------------------------------
  {
    const WeightAccounting& weight = snapshot.weight;
    if (weight.envelope_known) {
      identity(weight.usable.has_value(), "weight: a known envelope has a usable figure");
      if (weight.usable.has_value()) {
        const std::int64_t claims =
            weight.occupied_known.value() + weight.reserved_known.value();
        const std::int64_t upper = weight.free.is_known() ? weight.free.upper().value() : -1;
        const std::int64_t lower = weight.free.is_known() ? weight.free.lower().value() : -1;
        identity(upper >= 0 && lower >= 0, "weight: a known envelope reports a known free bound");
        identity(claims + upper == weight.usable->value() + weight.overcommit.value(),
                 "weight: occupied + reserved + free == usable + overcommit");
        identity(lower <= upper, "weight: the free bound is ordered");
        identity(weight.unknown_mass_assets > 0 ? lower == 0 : lower == upper,
                 "weight: unknown mass forces the conservative free figure to zero");
      }
      if (weight.per_unit_point_limit.has_value() && weight.max_unit_load.has_value()) {
        identity(weight.max_unit_load->value() >= 0,
                 "weight: the peak distributed unit load is a non-negative quantity");
      }
    } else {
      identity(!weight.free.is_known(),
               "weight: an unknown envelope reports unknown free capacity, never a number");
    }
  }

  // --- snapshot identity --------------------------------------------------
  identity(snapshot.capacity_generation.value() >= 1,
           "state: a published snapshot carries a capacity generation of at least one");
  identity(snapshot.revision.value() >= 1,
           "state: a published snapshot carries a revision of at least one");
  identity(snapshot.slot_extent == slot_extent_of(snapshot.unit_count),
           "state: the declared slot extent matches the declared rack height");
  identity(snapshot.pressures.size() <= kMaxConstraintPressures,
           "state: the pressure table is inside its documented bound");
  identity(snapshot.explanations.size() <= kMaxExplanationsPerSnapshot,
           "state: the explanation table is inside its documented bound");

  report.holds = report.violations.empty();
  return report;
}

}  // namespace rackcapacity
