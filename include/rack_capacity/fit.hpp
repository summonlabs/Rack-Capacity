// Rack Capacity - fit evaluation without placement authority.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/coordinates.hpp"
#include "rack_capacity/digest.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/measures.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Fit evaluation
// ---------------------------------------------------------------------------
//
// A fit evaluation answers one question: is this candidate compatible with the
// remaining capacity of this exact rack generation? It never chooses a
// position, never reserves anything and never publishes a placement. Placement
// authority belongs to the Facility Placement Planner and reservation authority
// belongs to Facility Capacity Reservation; this library only reports whether a
// candidate would fit if it were placed.
//
// The verdict is three valued because capacity accounting can be genuinely
// undecided. When an occupant's draw is unknown, the free power capacity is a
// range: a small request may still be provably satisfiable, a large one may be
// provably impossible, and a request in between is indeterminate. Reporting
// "does not fit" there would be a claim the evidence does not support, and
// reporting "fits" would be free capacity conjured out of an unknown.

enum class FitVerdict : std::uint8_t {
  // Every constraint is provably satisfied for every admissible reading of the
  // evidence.
  Fits = 0,
  // At least one constraint is provably violated.
  DoesNotFit = 1,
  // No constraint is provably violated and at least one cannot be decided from
  // the evidence. This is not a synonym for either other verdict.
  Indeterminate = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view fit_verdict_name(FitVerdict verdict) noexcept;

// Definitive failure dominates genuine uncertainty, which dominates success.
[[nodiscard]] RACK_CAPACITY_API FitVerdict combine_verdicts(FitVerdict left,
                                                            FitVerdict right) noexcept;

// The outcome of one constraint.
struct ConstraintOutcome {
  Dimension dimension = Dimension::None;
  FitVerdict verdict = FitVerdict::Fits;
  ReasonCode code = ReasonCode::FitAccepted;
  std::string message{};
  // The quantity the candidate asked for.
  std::optional<std::int64_t> required{};
  // What the rack can supply, conservatively and optimistically. When both are
  // present and differ, the difference is exactly the part that unknown
  // evidence may consume.
  std::optional<std::int64_t> available_lower{};
  std::optional<std::int64_t> available_upper{};
  // The physical or policy limit the constraint was compared against.
  std::optional<std::int64_t> limit{};
  std::string unit{};

  [[nodiscard]] std::string to_text() const;
};

struct FitRequest {
  // Explicit precondition: the exact capacity state the verdict is valid for.
  CapacityExpectation expected{};
  // The candidate position, when the caller has one. When absent, the
  // evaluation reports how many positions could host the request and
  // deliberately does not choose among them, because choosing is placement
  // authority and this library has none.
  std::optional<SlotInterval> span{};
  // Height of the candidate in whole rack units, used when no explicit span is
  // supplied. Exactly one of `span` and `unit_height` must be set.
  std::optional<std::uint32_t> unit_height{};
  MountSpanKind kind = MountSpanKind::FullSpan;
  SharedMountClassId shared_class{};
  std::uint32_t share_capacity = 0;
  std::optional<Watts> draw{};
  std::optional<Watts> heat_rejection{};
  std::optional<Grams> mass{};
  // Requires that the candidate be serviceable in place.
  bool requires_service_access = false;
  std::optional<Millimetres> required_front_clearance{};
  std::optional<Millimetres> required_rear_clearance{};
  // Informational only: the asset this evaluation is being prepared for.
  std::optional<AssetId> for_asset{};
};

struct FitEvaluation {
  RackId rack{};
  // The precondition the verdict was produced against, echoed back so a caller
  // can prove the verdict is about the state it asked about.
  CapacityExpectation evaluated_against{};
  EvidenceEpoch evidence_epoch{};
  TimestampNs evaluated_at{};
  StateDigest snapshot_digest{};
  FitVerdict verdict = FitVerdict::Indeterminate;
  std::vector<ConstraintOutcome> constraints{};
  // The first constraint in canonical dimension order that is not satisfied.
  Dimension binding_constraint = Dimension::None;
  // Slot availability when the request named no span.
  std::optional<std::uint32_t> requested_span_slots{};
  std::optional<std::uint32_t> feasible_anchors_lower{};
  std::optional<std::uint32_t> feasible_anchors_upper{};
  std::vector<Explanation> explanations{};
  // Always false. Stated explicitly so that no consumer can mistake this
  // evaluation for a placement decision.
  bool placement_authority = false;
  // Always false. Stated explicitly so that no consumer can mistake this
  // evaluation for a capacity reservation.
  bool reservation_authority = false;

  [[nodiscard]] Result<ConstraintOutcome> constraint_for(Dimension dimension) const;
  [[nodiscard]] std::string to_text() const;
};

// Evaluates a fit request against a snapshot. The snapshot's identity must
// match the request's precondition; the caller resolves the precondition
// against the catalog before calling this.
[[nodiscard]] RACK_CAPACITY_API Result<FitEvaluation> evaluate_fit(
    const FitRequest& request, const RackCapacitySnapshot& snapshot);

}  // namespace rackcapacity
