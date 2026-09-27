// Rack Capacity - fit evaluation without placement authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/fit.hpp"

#include <algorithm>
#include <string>

namespace rackcapacity {
namespace {

[[nodiscard]] Status check_precondition(const CapacityExpectation& expected,
                                        const RackCapacitySnapshot& snapshot) {
  if (expected.rack != snapshot.rack) {
    return Status(make_error(ErrorCode::InvalidArgument,
                             "the expectation names a different rack than the snapshot",
                             ErrorDetail{"fit.precondition", "rack", expected.rack.text(), 0, 0,
                                         {snapshot.rack.text()}}));
  }
  if (expected.composition_generation != snapshot.composition_generation) {
    return Status(make_error(
        ErrorCode::StaleCompositionGeneration,
        "the expectation was planned against a different rack composition generation",
        ErrorDetail{"fit.precondition", "composition_generation", {},
                    snapshot.composition_generation.value(),
                    expected.composition_generation.value(), {}}));
  }
  if (expected.capacity_generation != snapshot.capacity_generation) {
    return Status(make_error(
        ErrorCode::StaleCapacityGeneration,
        "the expectation was planned against a different capacity generation",
        ErrorDetail{"fit.precondition", "capacity_generation", {},
                    snapshot.capacity_generation.value(), expected.capacity_generation.value(),
                    {}}));
  }
  if (expected.revision != snapshot.revision) {
    return Status(make_error(ErrorCode::StaleSnapshotRevision,
                             "the expectation was planned against a different snapshot revision",
                             ErrorDetail{"fit.precondition", "revision", {},
                                         snapshot.revision.value(), expected.revision.value(),
                                         {}}));
  }
  return Status{};
}

[[nodiscard]] ConstraintOutcome make_outcome(Dimension dimension, FitVerdict verdict,
                                             ReasonCode code, std::string message) {
  ConstraintOutcome outcome;
  outcome.dimension = dimension;
  outcome.verdict = verdict;
  outcome.code = code;
  outcome.message = std::move(message);
  return outcome;
}

[[nodiscard]] bool ordered_before(Dimension left, Dimension right) {
  return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
}

[[nodiscard]] std::uint32_t slots_for_height(std::uint32_t unit_height) {
  return unit_height * kMountSlotsPerRackUnit;
}

}  // namespace

std::string_view fit_verdict_name(FitVerdict verdict) noexcept {
  switch (verdict) {
    case FitVerdict::Fits:
      return "fits";
    case FitVerdict::DoesNotFit:
      return "does-not-fit";
    case FitVerdict::Indeterminate:
      return "indeterminate";
  }
  return "unknown";
}

FitVerdict combine_verdicts(FitVerdict left, FitVerdict right) noexcept {
  const auto rank = [](FitVerdict verdict) {
    switch (verdict) {
      case FitVerdict::Fits:
        return 0;
      case FitVerdict::Indeterminate:
        return 1;
      case FitVerdict::DoesNotFit:
        return 2;
    }
    return 2;
  };
  return rank(left) >= rank(right) ? left : right;
}

Result<FitEvaluation> evaluate_fit(const FitRequest& request, const RackCapacitySnapshot& snapshot) {
  const Status precondition = check_precondition(request.expected, snapshot);
  if (!precondition.has_value()) {
    return precondition.error();
  }
  if (!request.span.has_value() && !request.unit_height.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a fit request must state either a candidate span or a candidate height",
                      ErrorDetail{"fit.request", "span", {}, 0, 0, {}});
  }
  if (request.span.has_value() && request.unit_height.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a fit request must state a span or a height, not both",
                      ErrorDetail{"fit.request", "unit_height", {}, 0,
                                  request.unit_height.value(), {}});
  }

  FitEvaluation evaluation;
  evaluation.rack = snapshot.rack;
  evaluation.evaluated_against = CapacityExpectation::of(snapshot);
  evaluation.evidence_epoch = snapshot.evidence_epoch;
  evaluation.evaluated_at = snapshot.evaluated_at;
  evaluation.snapshot_digest = snapshot.digest;

  std::uint32_t span_slots = 0;
  if (request.span.has_value()) {
    if (!request.span->is_valid()) {
      return make_error(ErrorCode::InvalidSlotInterval, "the candidate span is not a valid interval",
                        ErrorDetail{"fit.request", "span", request.span->to_text(), 0, 0, {}});
    }
    if (request.span->end() > snapshot.slot_extent) {
      return make_error(ErrorCode::SlotOutOfBounds,
                        "the candidate span lies outside the rack extent",
                        ErrorDetail{"fit.request", "span", request.span->to_text(),
                                    snapshot.slot_extent, request.span->end(), {}});
    }
    span_slots = request.span->slot_count();
  } else {
    const Result<std::uint32_t> height = validate_unit_count(request.unit_height.value());
    if (!height.has_value()) {
      return height.error();
    }
    span_slots = slots_for_height(height.value());
    if (span_slots > snapshot.slot_extent - 1u) {
      return make_error(ErrorCode::SlotOutOfBounds,
                        "the candidate height exceeds the rack extent",
                        ErrorDetail{"fit.request", "unit_height", {}, snapshot.unit_count,
                                    height.value(), {}});
    }
    evaluation.requested_span_slots = span_slots;
  }

  // -----------------------------------------------------------------------
  // Slot constraint
  // -----------------------------------------------------------------------
  {
    const SlotAccounting& slots = snapshot.slots;
    // A serviceable position, when the request requires service access and the
    // caller has not named one, must lie at or below the declared service
    // height limit. The free sets are restricted to that band before anchors
    // are counted, so the count reported is exact for the requirement rather
    // than for the whole rack. When the caller names a span, the position is
    // already decided and the serviceability constraints below judge it
    // directly; restricting the slot sets as well would report a free span as
    // occupied.
    const bool service_band_needed =
        request.requires_service_access && !request.span.has_value() &&
        snapshot.serviceability.service_height_limit_unit.has_value();
    std::optional<SlotSet> band;
    if (service_band_needed) {
      const std::uint32_t limit = snapshot.serviceability.service_height_limit_unit.value();
      const std::uint32_t band_end = std::min(snapshot.slot_extent,
                                              limit * kMountSlotsPerRackUnit + 1u);
      const Result<SlotInterval> extent = SlotInterval::create(1, band_end);
      if (!extent.has_value()) {
        return extent.error();
      }
      const Result<SlotSet> single = SlotSet::single(extent.value());
      if (!single.has_value()) {
        return single.error();
      }
      band = single.value();
    }

    SlotSet free_lower = slots.free_lower_set;
    SlotSet free_upper = slots.free_upper_set;
    if (band.has_value()) {
      Result<SlotSet> lower = free_lower.intersect(band.value());
      if (!lower.has_value()) {
        return lower.error();
      }
      Result<SlotSet> upper = free_upper.intersect(band.value());
      if (!upper.has_value()) {
        return upper.error();
      }
      free_lower = std::move(lower).value();
      free_upper = std::move(upper).value();
    }

    ConstraintOutcome outcome = make_outcome(Dimension::Slot, FitVerdict::Fits,
                                             ReasonCode::FitAccepted, "slots are available");
    if (request.span.has_value()) {
      const SlotInterval& span = request.span.value();
      if (free_lower.contains(span)) {
        outcome.message = "the requested span is free";
      } else if (free_upper.contains(span)) {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitSlotIndeterminate;
        outcome.message =
            "the requested span is free only if occupants whose presence cannot be established are "
            "absent";
      } else if (slots.structural_reserved_set.intersects(SlotSet::single(span).value_or(
                     SlotSet::empty()))) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotBlockedByStructure;
        outcome.message = "the requested span overlaps slots the rack structure reserves";
      } else if (slots.occupied_set.intersects(
                     SlotSet::single(span).value_or(SlotSet::empty()))) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotOccupied;
        outcome.message = "the requested span overlaps a present occupant";
      } else if (slots.reserved_set.intersects(
                     SlotSet::single(span).value_or(SlotSet::empty()))) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotOccupied;
        outcome.message = "the requested span overlaps a committed reservation";
      } else if (slots.policy_headroom_set.intersects(
                     SlotSet::single(span).value_or(SlotSet::empty()))) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotOccupied;
        outcome.message = "the requested span lies inside the slots policy headroom withholds";
      } else if (slots.free.upper() == 0) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotExhausted;
        outcome.message = "no free slot remains in this rack generation";
      } else {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotOccupied;
        outcome.message = "the requested span is not free";
      }
    } else {
      const std::uint64_t anchors_lower = free_lower.count_anchors(span_slots, true);
      const std::uint64_t anchors_upper = free_upper.count_anchors(span_slots, true);
      outcome.required = static_cast<std::int64_t>(span_slots);
      outcome.available_lower = static_cast<std::int64_t>(anchors_lower);
      outcome.available_upper = static_cast<std::int64_t>(anchors_upper);
      outcome.unit = "anchors";
      evaluation.feasible_anchors_lower =
          anchors_lower > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(anchors_lower);
      evaluation.feasible_anchors_upper =
          anchors_upper > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(anchors_upper);
      if (anchors_lower > 0) {
        outcome.message = "at least one position can host the candidate";
      } else if (anchors_upper > 0) {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitSlotIndeterminate;
        outcome.message =
            "a position can host the candidate only if occupants whose presence cannot be "
            "established are absent";
      } else if (slots.free.upper() == 0) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotExhausted;
        outcome.message = "no free slot remains in this rack generation";
      } else {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitSlotNoAnchor;
        outcome.message =
            "free slots remain but none form a run long enough at a legal mounting boundary";
      }
    }
    evaluation.constraints.push_back(std::move(outcome));
  }

  // -----------------------------------------------------------------------
  // Power constraint
  // -----------------------------------------------------------------------
  if (request.draw.has_value()) {
    ConstraintOutcome outcome =
        make_outcome(Dimension::Power, FitVerdict::Fits, ReasonCode::FitAccepted, "");
    outcome.required = request.draw->value();
    outcome.unit = "W";
    if (!snapshot.power.free.is_known()) {
      outcome.verdict = FitVerdict::Indeterminate;
      outcome.code = ReasonCode::FitPowerIndeterminate;
      outcome.message =
          "free power is unknown because no power capacity evidence is available for this rack "
          "generation";
    } else {
      outcome.available_lower = snapshot.power.free.lower().value();
      outcome.available_upper = snapshot.power.free.upper().value();
      if (snapshot.power.free.lower() >= request.draw.value()) {
        outcome.message = "the conservative free power covers the candidate";
      } else if (snapshot.power.free.upper() < request.draw.value()) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitPowerInsufficient;
        outcome.message = "even the optimistic free power does not cover the candidate";
      } else {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitPowerIndeterminate;
        outcome.message =
            "the candidate fits only within the part of free power that unknown load may consume";
      }
    }
    evaluation.constraints.push_back(std::move(outcome));
  }

  // -----------------------------------------------------------------------
  // Cooling constraint
  // -----------------------------------------------------------------------
  if (request.heat_rejection.has_value()) {
    ConstraintOutcome outcome =
        make_outcome(Dimension::Cooling, FitVerdict::Fits, ReasonCode::FitAccepted, "");
    outcome.required = request.heat_rejection->value();
    outcome.unit = "W";
    if (!snapshot.cooling.free.is_known()) {
      outcome.verdict = FitVerdict::Indeterminate;
      outcome.code = ReasonCode::FitCoolingIndeterminate;
      outcome.message =
          "free cooling is unknown because no cooling capacity evidence is available for this "
          "rack generation";
    } else {
      outcome.available_lower = snapshot.cooling.free.lower().value();
      outcome.available_upper = snapshot.cooling.free.upper().value();
      if (snapshot.cooling.free.lower() >= request.heat_rejection.value()) {
        outcome.message = "the conservative free cooling covers the candidate";
      } else if (snapshot.cooling.free.upper() < request.heat_rejection.value()) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitCoolingInsufficient;
        outcome.message = "even the optimistic free cooling does not cover the candidate";
      } else {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitCoolingIndeterminate;
        outcome.message =
            "the candidate fits only within the part of free cooling that unknown heat may "
            "consume";
      }
    }
    evaluation.constraints.push_back(std::move(outcome));
  }

  // -----------------------------------------------------------------------
  // Weight constraint
  // -----------------------------------------------------------------------
  if (request.mass.has_value()) {
    {
      ConstraintOutcome outcome =
          make_outcome(Dimension::Weight, FitVerdict::Fits, ReasonCode::FitAccepted, "");
      outcome.required = request.mass->value();
      outcome.unit = "g";
      if (!snapshot.weight.free.is_known()) {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitWeightIndeterminate;
        outcome.message =
            "free weight capacity is unknown because no static load limit is available for this "
            "rack generation";
      } else {
        outcome.available_lower = snapshot.weight.free.lower().value();
        outcome.available_upper = snapshot.weight.free.upper().value();
        if (snapshot.weight.free.lower() >= request.mass.value()) {
          outcome.message = "the conservative free weight capacity covers the candidate";
        } else if (snapshot.weight.free.upper() < request.mass.value()) {
          outcome.verdict = FitVerdict::DoesNotFit;
          outcome.code = ReasonCode::FitWeightInsufficient;
          outcome.message = "even the optimistic free weight capacity does not cover the candidate";
        } else {
          outcome.verdict = FitVerdict::Indeterminate;
          outcome.code = ReasonCode::FitWeightIndeterminate;
          outcome.message =
              "the candidate fits only within the part of free weight that unknown mass may "
              "consume";
        }
      }
      evaluation.constraints.push_back(std::move(outcome));
    }
    // Point load is decided only when a limit is declared and the rack is not
    // already over it. The comparison uses the worst case that the whole
    // candidate mass lands on the single most loaded unit the candidate
    // touches, so a positive verdict is guaranteed and an undecided one is
    // reported as undecided rather than guessed.
    if (snapshot.weight.per_unit_point_limit.has_value() &&
        snapshot.weight.max_unit_load.has_value() && request.kind != MountSpanKind::ZeroU) {
      ConstraintOutcome outcome =
          make_outcome(Dimension::Weight, FitVerdict::Fits, ReasonCode::FitAccepted, "");
      const std::int64_t limit = snapshot.weight.per_unit_point_limit->value();
      const std::int64_t existing = snapshot.weight.max_unit_load->value();
      const std::int64_t added = request.mass->value();
      outcome.required = added;
      outcome.limit = limit;
      outcome.available_upper = limit - existing;
      outcome.available_lower = outcome.available_upper;
      outcome.unit = "g";
      if (existing > limit) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitPointLoadExceeded;
        outcome.message = "a rack unit already carries more static load than its point load limit";
      } else if (added > limit) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitPointLoadExceeded;
        outcome.message =
            "the candidate alone is heavier than the point load limit of a single rack unit";
      } else if (existing + added <= limit) {
        outcome.message =
            "the candidate fits under the point load limit even if its whole mass lands on one "
            "unit";
      } else {
        outcome.verdict = FitVerdict::Indeterminate;
        outcome.code = ReasonCode::FitPointLoadIndeterminate;
        outcome.message =
            "the point load limit is satisfied only if the candidate mass is distributed over "
            "more than one rack unit";
      }
      evaluation.constraints.push_back(std::move(outcome));
    }
  }

  // -----------------------------------------------------------------------
  // Serviceability constraints
  // -----------------------------------------------------------------------
  {
    if (request.requires_service_access &&
        snapshot.serviceability.service_height_limit_unit.has_value() &&
        request.span.has_value()) {
      const std::uint32_t limit = snapshot.serviceability.service_height_limit_unit.value();
      ConstraintOutcome outcome =
          make_outcome(Dimension::Serviceability, FitVerdict::Fits, ReasonCode::FitAccepted,
                       "the candidate is inside the service height limit");
      outcome.required = static_cast<std::int64_t>(request.span->last_unit());
      outcome.limit = static_cast<std::int64_t>(limit);
      outcome.unit = "U";
      if (request.span->last_unit() > limit) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitServiceHeightExceeded;
        outcome.message =
            "the candidate would sit above the highest rack unit that can be serviced in place";
      }
      evaluation.constraints.push_back(std::move(outcome));
    }
    if (request.required_front_clearance.has_value()) {
      ConstraintOutcome outcome =
          make_outcome(Dimension::Serviceability, FitVerdict::Fits, ReasonCode::FitAccepted,
                       "the rack provides the required front clearance");
      outcome.required = request.required_front_clearance->value();
      outcome.limit = snapshot.serviceability.front_clearance.value();
      outcome.available_lower = snapshot.serviceability.front_clearance.value();
      outcome.available_upper = snapshot.serviceability.front_clearance.value();
      outcome.unit = "mm";
      if (request.required_front_clearance.value() > snapshot.serviceability.front_clearance) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitServiceClearanceInsufficient;
        outcome.message = "the rack provides less front clearance than the candidate requires";
      }
      evaluation.constraints.push_back(std::move(outcome));
    }
    if (request.required_rear_clearance.has_value()) {
      ConstraintOutcome outcome =
          make_outcome(Dimension::Serviceability, FitVerdict::Fits, ReasonCode::FitAccepted,
                       "the rack provides the required rear clearance");
      outcome.required = request.required_rear_clearance->value();
      outcome.limit = snapshot.serviceability.rear_clearance.value();
      outcome.available_lower = snapshot.serviceability.rear_clearance.value();
      outcome.available_upper = snapshot.serviceability.rear_clearance.value();
      outcome.unit = "mm";
      if (request.required_rear_clearance.value() > snapshot.serviceability.rear_clearance) {
        outcome.verdict = FitVerdict::DoesNotFit;
        outcome.code = ReasonCode::FitServiceClearanceInsufficient;
        outcome.message = "the rack provides less rear clearance than the candidate requires";
      }
      evaluation.constraints.push_back(std::move(outcome));
    }
  }

  // -----------------------------------------------------------------------
  // Verdict
  // -----------------------------------------------------------------------
  std::stable_sort(evaluation.constraints.begin(), evaluation.constraints.end(),
                   [](const ConstraintOutcome& left, const ConstraintOutcome& right) {
                     return ordered_before(left.dimension, right.dimension);
                   });
  FitVerdict verdict = FitVerdict::Fits;
  for (const ConstraintOutcome& outcome : evaluation.constraints) {
    verdict = combine_verdicts(verdict, outcome.verdict);
  }
  evaluation.verdict = verdict;

  for (const ConstraintOutcome& outcome : evaluation.constraints) {
    if (outcome.verdict != FitVerdict::Fits) {
      evaluation.binding_constraint = outcome.dimension;
      break;
    }
  }

  if (verdict == FitVerdict::Fits) {
    Explanation note;
    note.code = ReasonCode::FitAccepted;
    note.dimension = Dimension::None;
    note.severity = Severity::Info;
    note.subject = snapshot.rack.text();
    note.message = evaluation.constraints.empty()
                       ? "the request stated no measurable constraint and is accepted vacuously"
                       : "every stated constraint is satisfied";
    evaluation.explanations.push_back(std::move(note));
  } else {
    for (const ConstraintOutcome& outcome : evaluation.constraints) {
      if (outcome.verdict == FitVerdict::Fits) {
        continue;
      }
      Explanation note;
      note.code = outcome.code;
      note.dimension = outcome.dimension;
      note.severity = outcome.verdict == FitVerdict::DoesNotFit ? Severity::Blocking
                                                                : Severity::Unknown;
      note.subject = snapshot.rack.text();
      note.message = outcome.message;
      note.observed = outcome.required;
      note.limit = outcome.available_upper;
      note.unit = outcome.unit;
      evaluation.explanations.push_back(std::move(note));
    }
  }
  return evaluation;
}

}  // namespace rackcapacity
