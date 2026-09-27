// Rack Capacity - snapshot diffs and revalidation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/diff.hpp"

#include <algorithm>
#include <string>

namespace rackcapacity {
namespace {

template <typename MeasureType>
[[nodiscard]] std::string render(MeasureType value) {
  return std::to_string(value.value());
}

template <typename MeasureType>
[[nodiscard]] std::string render(const std::optional<MeasureType>& value) {
  return value.has_value() ? std::to_string(value->value()) : std::string("unknown");
}

template <typename MeasureType>
[[nodiscard]] std::string render(const Bound<MeasureType>& bound) {
  if (!bound.is_known()) {
    return "unknown";
  }
  return "[" + std::to_string(bound.lower().value()) + "," +
         std::to_string(bound.upper().value()) + "]";
}

[[nodiscard]] std::string render(bool value) { return value ? "true" : "false"; }

// Slot counts are plain integers rather than measures, so they have their own
// bound rendering.
[[nodiscard]] std::string render(const Bound<std::uint32_t>& bound) {
  if (!bound.is_known()) {
    return "unknown";
  }
  if (bound.is_exact()) {
    return std::to_string(bound.lower());
  }
  return "[" + std::to_string(bound.lower()) + "," + std::to_string(bound.upper()) + "]";
}

// Plain integer and text overloads. They are non-templates on purpose: an exact
// non-template match is preferred over the measure template below, so a plain
// count renders as a count and only a real measure renders through value().
[[nodiscard]] std::string render(std::uint32_t value) { return std::to_string(value); }
[[nodiscard]] std::string render(std::uint64_t value) { return std::to_string(value); }
[[nodiscard]] std::string render(std::int64_t value) { return std::to_string(value); }
[[nodiscard]] std::string render(const std::string& value) { return value; }

[[nodiscard]] std::string render(const SlotSet& set) { return set.to_text(); }

[[nodiscard]] std::string render_shared_classes(
    const std::vector<SharedClassUtilization>& entries) {
  std::string text;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const SharedClassUtilization& entry = entries[index];
    if (index != 0) {
      text.push_back(';');
    }
    text += entry.shared_class.text();
    text.push_back('@');
    text += entry.span.to_text();
    text += "/cap=";
    text += std::to_string(entry.share_capacity);
    text += "/used=";
    text += std::to_string(entry.used);
    text += "/reserved=";
    text += std::to_string(entry.reserved);
    text += "/pending=";
    text += std::to_string(entry.pending);
    text += "/remaining=";
    text += std::to_string(entry.remaining);
  }
  return text.empty() ? std::string("-") : text;
}

class DiffBuilder {
 public:
  explicit DiffBuilder(Dimension dimension) : dimension_(dimension) {}

  template <typename T>
  void field(std::string path, const T& before, const T& after) {
    const std::string left = render(before);
    const std::string right = render(after);
    if (left == right) {
      return;
    }
    FieldChange change;
    change.path = prefix_ + std::move(path);
    change.before = left;
    change.after = right;
    change.dimension = dimension_;
    changes_.push_back(std::move(change));
  }

  void scoped(std::string prefix) { prefix_ = std::move(prefix); }

  [[nodiscard]] std::vector<FieldChange> take() { return std::move(changes_); }

 private:
  Dimension dimension_;
  std::string prefix_;
  std::vector<FieldChange> changes_;
};

}  // namespace

Result<CapacityDiff> diff_snapshots(const RackCapacitySnapshot& from,
                                    const RackCapacitySnapshot& to) {
  if (from.rack != to.rack) {
    return make_error(ErrorCode::InvalidArgument,
                      "snapshots of two different racks cannot be diffed",
                      ErrorDetail{"diff.snapshots", "rack", from.rack.text(), 0, 0,
                                  {to.rack.text()}});
  }
  CapacityDiff diff;
  diff.rack = from.rack;
  diff.from_generation = from.capacity_generation;
  diff.to_generation = to.capacity_generation;
  diff.from_revision = from.revision;
  diff.to_revision = to.revision;
  diff.from_digest = from.digest;
  diff.to_digest = to.digest;
  diff.primary_binding_before = from.primary_binding;
  diff.primary_binding_after = to.primary_binding;

  std::vector<FieldChange> changes;

  {
    DiffBuilder builder(Dimension::Slot);
    builder.scoped("slots.");
    builder.field("total_slots", from.slots.total_slots, to.slots.total_slots);
    builder.field("structural_reserved_slots", from.slots.structural_reserved_slots,
                  to.slots.structural_reserved_slots);
    builder.field("occupied_slots", from.slots.occupied_slots, to.slots.occupied_slots);
    builder.field("shared_occupied_slots", from.slots.shared_occupied_slots,
                  to.slots.shared_occupied_slots);
    builder.field("indeterminate_slots", from.slots.indeterminate_slots,
                  to.slots.indeterminate_slots);
    builder.field("reserved_slots", from.slots.reserved_slots, to.slots.reserved_slots);
    builder.field("pending_slots", from.slots.pending_slots, to.slots.pending_slots);
    builder.field("policy_headroom_slots", from.slots.policy_headroom_slots,
                  to.slots.policy_headroom_slots);
    builder.field("overcommitted_slots", from.slots.overcommitted_slots,
                  to.slots.overcommitted_slots);
    builder.field("free", from.slots.free, to.slots.free);
    builder.field("structural_reserved_set", from.slots.structural_reserved_set,
                  to.slots.structural_reserved_set);
    builder.field("occupied_set", from.slots.occupied_set, to.slots.occupied_set);
    builder.field("indeterminate_set", from.slots.indeterminate_set,
                  to.slots.indeterminate_set);
    builder.field("reserved_set", from.slots.reserved_set, to.slots.reserved_set);
    builder.field("free_lower_set", from.slots.free_lower_set, to.slots.free_lower_set);
    builder.field("free_upper_set", from.slots.free_upper_set, to.slots.free_upper_set);
    builder.field("policy_headroom_set", from.slots.policy_headroom_set,
                  to.slots.policy_headroom_set);
    builder.field("fragmentation.free_run_count", from.slots.fragmentation.free_run_count,
                  to.slots.fragmentation.free_run_count);
    builder.field("fragmentation.largest_free_run_slots",
                  from.slots.fragmentation.largest_free_run_slots,
                  to.slots.fragmentation.largest_free_run_slots);
    builder.field("fragmentation.smallest_free_run_slots",
                  from.slots.fragmentation.smallest_free_run_slots,
                  to.slots.fragmentation.smallest_free_run_slots);
    builder.field("fragmentation.isolated_free_slots",
                  from.slots.fragmentation.isolated_free_slots,
                  to.slots.fragmentation.isolated_free_slots);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Power);
    builder.scoped("power.");
    builder.field("envelope_known", from.power.envelope_known, to.power.envelope_known);
    builder.field("feed_count", from.power.feed_count, to.power.feed_count);
    builder.field("contributing_feeds", from.power.contributing_feeds,
                  to.power.contributing_feeds);
    builder.field("redundancy", std::string(redundancy_mode_name(from.power.redundancy)),
                  std::string(redundancy_mode_name(to.power.redundancy)));
    builder.field("physical_derate", from.power.physical_derate, to.power.physical_derate);
    builder.field("policy_derate", from.power.policy_derate, to.power.policy_derate);
    builder.field("effective_nominal", from.power.effective_nominal, to.power.effective_nominal);
    builder.field("physically_derated", from.power.physically_derated,
                  to.power.physically_derated);
    builder.field("usable", from.power.usable, to.power.usable);
    builder.field("policy_headroom", from.power.policy_headroom, to.power.policy_headroom);
    builder.field("committed_known", from.power.committed_known, to.power.committed_known);
    builder.field("reserved_known", from.power.reserved_known, to.power.reserved_known);
    builder.field("pending_known", from.power.pending_known, to.power.pending_known);
    builder.field("unknown_draw_assets", from.power.unknown_draw_assets,
                  to.power.unknown_draw_assets);
    builder.field("measured", from.power.measured, to.power.measured);
    builder.field("measurement_standing",
                  std::string(measurement_standing_name(from.power.measurement_standing)),
                  std::string(measurement_standing_name(to.power.measurement_standing)));
    builder.field("overcommit", from.power.overcommit, to.power.overcommit);
    builder.field("free", from.power.free, to.power.free);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Cooling);
    builder.scoped("cooling.");
    builder.field("envelope_known", from.cooling.envelope_known, to.cooling.envelope_known);
    builder.field("physical_derate", from.cooling.physical_derate, to.cooling.physical_derate);
    builder.field("policy_derate", from.cooling.policy_derate, to.cooling.policy_derate);
    builder.field("nominal", from.cooling.nominal, to.cooling.nominal);
    builder.field("physically_derated", from.cooling.physically_derated,
                  to.cooling.physically_derated);
    builder.field("usable", from.cooling.usable, to.cooling.usable);
    builder.field("policy_headroom", from.cooling.policy_headroom, to.cooling.policy_headroom);
    builder.field("declared_heat_known", from.cooling.declared_heat_known,
                  to.cooling.declared_heat_known);
    builder.field("derived_heat_known", from.cooling.derived_heat_known,
                  to.cooling.derived_heat_known);
    builder.field("reserved_heat_known", from.cooling.reserved_heat_known,
                  to.cooling.reserved_heat_known);
    builder.field("pending_heat_known", from.cooling.pending_heat_known,
                  to.cooling.pending_heat_known);
    builder.field("unknown_heat_assets", from.cooling.unknown_heat_assets,
                  to.cooling.unknown_heat_assets);
    builder.field("measured", from.cooling.measured, to.cooling.measured);
    builder.field("measurement_standing",
                  std::string(measurement_standing_name(from.cooling.measurement_standing)),
                  std::string(measurement_standing_name(to.cooling.measurement_standing)));
    builder.field("overcommit", from.cooling.overcommit, to.cooling.overcommit);
    builder.field("free", from.cooling.free, to.cooling.free);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Weight);
    builder.scoped("weight.");
    builder.field("envelope_known", from.weight.envelope_known, to.weight.envelope_known);
    builder.field("physical_derate", from.weight.physical_derate, to.weight.physical_derate);
    builder.field("policy_derate", from.weight.policy_derate, to.weight.policy_derate);
    builder.field("nominal_limit", from.weight.nominal_limit, to.weight.nominal_limit);
    builder.field("physically_derated", from.weight.physically_derated,
                  to.weight.physically_derated);
    builder.field("usable", from.weight.usable, to.weight.usable);
    builder.field("policy_headroom", from.weight.policy_headroom, to.weight.policy_headroom);
    builder.field("occupied_known", from.weight.occupied_known, to.weight.occupied_known);
    builder.field("reserved_known", from.weight.reserved_known, to.weight.reserved_known);
    builder.field("pending_known", from.weight.pending_known, to.weight.pending_known);
    builder.field("unknown_mass_assets", from.weight.unknown_mass_assets,
                  to.weight.unknown_mass_assets);
    builder.field("per_unit_point_limit", from.weight.per_unit_point_limit,
                  to.weight.per_unit_point_limit);
    builder.field("max_unit_load", from.weight.max_unit_load, to.weight.max_unit_load);
    builder.field("units_at_or_above_point_limit", from.weight.units_at_or_above_point_limit,
                  to.weight.units_at_or_above_point_limit);
    builder.field("overcommit", from.weight.overcommit, to.weight.overcommit);
    builder.field("free", from.weight.free, to.weight.free);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Serviceability);
    builder.scoped("serviceability.");
    builder.field("front_clearance", from.serviceability.front_clearance,
                  to.serviceability.front_clearance);
    builder.field("rear_clearance", from.serviceability.rear_clearance,
                  to.serviceability.rear_clearance);
    const std::string before_limit = from.serviceability.service_height_limit_unit.has_value()
                                         ? std::to_string(
                                               from.serviceability.service_height_limit_unit.value())
                                         : std::string("none");
    const std::string after_limit = to.serviceability.service_height_limit_unit.has_value()
                                        ? std::to_string(
                                              to.serviceability.service_height_limit_unit.value())
                                        : std::string("none");
    builder.field("service_height_limit_unit", before_limit, after_limit);
    builder.field("occupied_units_above_service_height",
                  from.serviceability.occupied_units_above_service_height,
                  to.serviceability.occupied_units_above_service_height);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Slot);
    builder.scoped("shared_classes");
    builder.field("", render_shared_classes(from.shared_classes),
                  render_shared_classes(to.shared_classes));
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  {
    DiffBuilder builder(Dimension::Evidence);
    builder.scoped("state.");
    builder.field("composition_generation", from.composition_generation.value(),
                  to.composition_generation.value());
    builder.field("capacity_generation", from.capacity_generation.value(),
                  to.capacity_generation.value());
    builder.field("revision", from.revision.value(), to.revision.value());
    builder.field("evidence_epoch", from.evidence_epoch.value(), to.evidence_epoch.value());
    builder.field("evidence_observed_at", from.evidence_observed_at.value(),
                  to.evidence_observed_at.value());
    builder.field("freshness", std::string(evidence_freshness_name(from.freshness)),
                  std::string(evidence_freshness_name(to.freshness)));
    builder.field("policy", from.policy.to_text(), to.policy.to_text());
    builder.field("policy_digest", from.policy_digest.to_hex(), to.policy_digest.to_hex());
    builder.field("inputs_digest", from.inputs_digest.to_hex(), to.inputs_digest.to_hex());
    builder.field("unknown.power_envelope", from.unknown.power_envelope,
                  to.unknown.power_envelope);
    builder.field("unknown.cooling_envelope", from.unknown.cooling_envelope,
                  to.unknown.cooling_envelope);
    builder.field("unknown.weight_envelope", from.unknown.weight_envelope,
                  to.unknown.weight_envelope);
    builder.field("unknown.unknown_power_consumption", from.unknown.unknown_power_consumption,
                  to.unknown.unknown_power_consumption);
    builder.field("unknown.unknown_heat", from.unknown.unknown_heat, to.unknown.unknown_heat);
    builder.field("unknown.unknown_mass", from.unknown.unknown_mass, to.unknown.unknown_mass);
    builder.field("unknown.indeterminate_occupancy", from.unknown.indeterminate_occupancy,
                  to.unknown.indeterminate_occupancy);
    builder.field("primary_binding", std::string(dimension_name(from.primary_binding)),
                  std::string(dimension_name(to.primary_binding)));
    builder.field("binding", from.binding, to.binding);
    std::vector<FieldChange> produced = builder.take();
    changes.insert(changes.end(), produced.begin(), produced.end());
  }

  std::sort(changes.begin(), changes.end());
  diff.changes = std::move(changes);

  for (const Explanation& explanation : to.explanations) {
    if (std::find(from.explanations.begin(), from.explanations.end(), explanation) ==
        from.explanations.end()) {
      diff.new_explanations.push_back(explanation);
    }
  }
  for (const Explanation& explanation : from.explanations) {
    if (std::find(to.explanations.begin(), to.explanations.end(), explanation) ==
        to.explanations.end()) {
      diff.resolved_explanations.push_back(explanation);
    }
  }
  diff.identical = diff.changes.empty();
  return diff;
}

std::string_view revalidation_verdict_name(RevalidationVerdict verdict) noexcept {
  switch (verdict) {
    case RevalidationVerdict::Current:
      return "current";
    case RevalidationVerdict::Stale:
      return "stale";
    case RevalidationVerdict::Diverged:
      return "diverged";
    case RevalidationVerdict::Unrevalidated:
      return "unrevalidated";
  }
  return "unknown";
}

RevalidationReport revalidate_record(const RackCapacityRecord& record, TimestampNs now) {
  RevalidationReport report;
  report.rack = record.rack;
  report.revalidated = CapacityExpectation::of(record);
  report.digest_matches = compute_snapshot_digest(record.snapshot) == record.snapshot.digest;
  if (!report.digest_matches) {
    report.verdict = RevalidationVerdict::Diverged;
    report.differences.push_back(
        "the stored snapshot digest does not match its own content: the record was altered or "
        "damaged after it was published");
    report.freshness = record.snapshot.freshness;
    return report;
  }

  const Result<RackCapacitySnapshot> recomputed =
      evaluate_capacity(record.inputs, record.capacity_generation, record.revision,
                        record.snapshot.evaluated_at, record.snapshot.freshness);
  if (!recomputed.has_value()) {
    report.verdict = RevalidationVerdict::Diverged;
    report.differences.push_back("the stored evidence could not be re-evaluated: " +
                                 describe(recomputed.error()));
    report.freshness = record.snapshot.freshness;
    return report;
  }
  report.reproduces = recomputed.value().digest == record.snapshot.digest;
  if (!report.reproduces) {
    const Result<CapacityDiff> delta =
        diff_snapshots(record.snapshot, recomputed.value());
    if (delta.has_value()) {
      for (const FieldChange& change : delta.value().changes) {
        report.differences.push_back(change.path + ": stored " + change.before + ", recomputed " +
                                     change.after);
      }
    }
    if (report.differences.empty()) {
      report.differences.push_back(
          "the recomputed snapshot differs from the stored one without a named field difference");
    }
    report.verdict = RevalidationVerdict::Diverged;
    report.freshness = record.snapshot.freshness;
    return report;
  }

  // The record reproduces exactly. Freshness is then decided against the
  // instant the caller supplied, which is what stops persisted evidence from
  // silently becoming current again after a restart.
  EvidenceFreshness freshness = EvidenceFreshness::Fresh;
  const std::int64_t age = now.value() - record.snapshot.evidence_observed_at.value();
  if (record.inputs.policy.freshness.max_envelope_age.value() > 0 && now.value() >= 0 &&
      (age < 0 || age > record.inputs.policy.freshness.max_envelope_age.value())) {
    freshness = EvidenceFreshness::Stale;
  }
  report.freshness = freshness;
  report.verdict = freshness == EvidenceFreshness::Fresh ? RevalidationVerdict::Current
                                                         : RevalidationVerdict::Stale;
  Explanation note;
  note.dimension = Dimension::Evidence;
  note.code = ReasonCode::EvidenceFresh;
  note.severity = Severity::Info;
  note.subject = record.rack.text();
  if (report.verdict == RevalidationVerdict::Current) {
    note.message = "the record reproduces exactly from the evidence it carries and that evidence "
                   "is inside the freshness policy";
  } else {
    note.code = ReasonCode::EvidenceStale;
    note.severity = Severity::Warning;
    note.message = "the record reproduces exactly from the evidence it carries but that evidence "
                   "is older than the freshness policy allows";
    note.observed = age;
    note.limit = record.inputs.policy.freshness.max_envelope_age.value();
    note.unit = "ns";
  }
  report.explanations.push_back(std::move(note));
  return report;
}

}  // namespace rackcapacity
