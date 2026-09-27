// Rack Capacity - human-facing rendering of reports.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every rendering here is derived only from values the library proved. Nothing
// is embellished, no derived quantity is presented as observed, and an unknown
// quantity is always rendered as "unknown" rather than as a number.

#include <algorithm>
#include <string>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/catalog.hpp"
#include "rack_capacity/diff.hpp"
#include "rack_capacity/fit.hpp"
#include "rack_capacity/persistence.hpp"
#include "rack_capacity/requests.hpp"

namespace rackcapacity {
namespace {

[[nodiscard]] std::string unsigned_text(std::uint64_t value) { return std::to_string(value); }

template <typename MeasureType>
[[nodiscard]] std::string measure_text(const std::optional<MeasureType>& value) {
  return value.has_value() ? value->to_text() : std::string("unknown");
}

template <typename MeasureType>
[[nodiscard]] std::string bound_text(const Bound<MeasureType>& bound) {
  if (!bound.is_known()) {
    return "unknown";
  }
  if (bound.is_exact()) {
    return bound.lower().to_text();
  }
  return "[" + bound.lower().to_text() + ", " + bound.upper().to_text() + "]";
}

[[nodiscard]] std::string bound_slots_text(const Bound<std::uint32_t>& bound) {
  if (!bound.is_known()) {
    return "unknown";
  }
  if (bound.is_exact()) {
    return std::to_string(bound.lower()) + " slots";
  }
  return "[" + std::to_string(bound.lower()) + ", " + std::to_string(bound.upper()) + "] slots";
}

void append_line(std::string& text, const std::string& line) {
  text += line;
  text.push_back('\n');
}

}  // namespace

std::string Explanation::to_text() const {
  std::string text = "[";
  text += severity_name(severity);
  text += "] ";
  text += reason_code_name(code);
  text += " (";
  text += dimension_name(dimension);
  text += ")";
  if (!subject.empty()) {
    text += " ";
    text += subject;
  }
  text += ": ";
  text += message;
  if (observed.has_value()) {
    text += " [observed=";
    text += std::to_string(observed.value());
    if (!unit.empty()) {
      text.push_back(' ');
      text += unit;
    }
    text.push_back(']');
  }
  if (limit.has_value()) {
    text += " [limit=";
    text += std::to_string(limit.value());
    if (!unit.empty()) {
      text.push_back(' ');
      text += unit;
    }
    text.push_back(']');
  }
  return text;
}

std::string ConstraintOutcome::to_text() const {
  std::string text(dimension_name(dimension));
  text += ": ";
  text += fit_verdict_name(verdict);
  text += " (";
  text += reason_code_name(code);
  text += ")";
  if (!message.empty()) {
    text += " - ";
    text += message;
  }
  if (required.has_value()) {
    text += " [required=";
    text += std::to_string(required.value());
    if (!unit.empty()) {
      text.push_back(' ');
      text += unit;
    }
    text.push_back(']');
  }
  if (available_lower.has_value() && available_upper.has_value()) {
    text += " [available=";
    text += std::to_string(available_lower.value());
    if (available_upper.value() != available_lower.value()) {
      text += "..";
      text += std::to_string(available_upper.value());
    }
    if (!unit.empty()) {
      text.push_back(' ');
      text += unit;
    }
    text.push_back(']');
  }
  return text;
}

std::string FitEvaluation::to_text() const {
  std::string text;
  append_line(text, "fit evaluation for rack " + rack.text());
  append_line(text, "  evaluated against: " + evaluated_against.to_text());
  append_line(text, "  evidence epoch    : " + unsigned_text(evidence_epoch.value()));
  append_line(text, "  snapshot digest   : " + snapshot_digest.to_short_hex());
  append_line(text, std::string("  verdict           : ") + std::string(fit_verdict_name(verdict)));
  if (binding_constraint != Dimension::None) {
    append_line(text, std::string("  binding dimension : ") +
                          std::string(dimension_name(binding_constraint)));
  }
  if (requested_span_slots.has_value()) {
    std::string line = "  candidate height  : " + unsigned_text(requested_span_slots.value()) +
                       " slots; feasible mounting positions ";
    line += feasible_anchors_lower.has_value() ? unsigned_text(feasible_anchors_lower.value())
                                               : std::string("unknown");
    if (feasible_anchors_upper.has_value() &&
        (!feasible_anchors_lower.has_value() ||
         feasible_anchors_upper.value() != feasible_anchors_lower.value())) {
      line += "..";
      line += unsigned_text(feasible_anchors_upper.value());
    }
    append_line(text, line);
  }
  append_line(text, "  constraints:");
  for (const ConstraintOutcome& outcome : constraints) {
    append_line(text, "    " + outcome.to_text());
  }
  append_line(text, "  placement authority: none (this runtime never places an occupant)");
  append_line(text, "  reservation authority: none (this runtime never reserves capacity)");
  return text;
}

std::string CapacityDiff::to_text() const {
  std::string text;
  append_line(text, "capacity diff for rack " + rack.text());
  append_line(text, "  from: generation " + unsigned_text(from_generation.value()) + ", revision " +
                        unsigned_text(from_revision.value()) + ", digest " +
                        from_digest.to_short_hex());
  append_line(text, "  to  : generation " + unsigned_text(to_generation.value()) + ", revision " +
                        unsigned_text(to_revision.value()) + ", digest " + to_digest.to_short_hex());
  append_line(text, std::string("  binding dimension: ") +
                        std::string(dimension_name(primary_binding_before)) + " -> " +
                        std::string(dimension_name(primary_binding_after)));
  if (identical) {
    append_line(text, "  no field changed");
  } else {
    append_line(text, "  " + unsigned_text(changes.size()) + " field(s) changed:");
    for (const FieldChange& change : changes) {
      append_line(text, "    " + change.path + ": " + change.before + " -> " + change.after);
    }
  }
  if (!new_explanations.empty()) {
    append_line(text, "  new explanations:");
    for (const Explanation& explanation : new_explanations) {
      append_line(text, "    " + explanation.to_text());
    }
  }
  if (!resolved_explanations.empty()) {
    append_line(text, "  resolved explanations:");
    for (const Explanation& explanation : resolved_explanations) {
      append_line(text, "    " + explanation.to_text());
    }
  }
  return text;
}

std::string RevalidationReport::to_text() const {
  std::string text;
  append_line(text, "revalidation for rack " + rack.text());
  append_line(text, "  state             : " + revalidated.to_text());
  append_line(text, std::string("  verdict           : ") +
                        std::string(revalidation_verdict_name(verdict)));
  append_line(text, std::string("  freshness         : ") +
                        std::string(evidence_freshness_name(freshness)));
  append_line(text, std::string("  digest matches    : ") + (digest_matches ? "yes" : "no"));
  append_line(text, std::string("  reproduces exactly: ") + (reproduces ? "yes" : "no"));
  for (const std::string& difference : differences) {
    append_line(text, "  difference: " + difference);
  }
  for (const Explanation& explanation : explanations) {
    append_line(text, "  " + explanation.to_text());
  }
  return text;
}

std::string MutationReceipt::to_text() const {
  std::string text;
  append_line(text, std::string("mutation ") + std::string(mutation_outcome_name(outcome)) +
                        " for rack " + rack.text());
  append_line(text, std::string("  lifecycle         : ") + std::string(rack_lifecycle_name(lifecycle)));
  append_line(text, "  composition gen   : " + unsigned_text(composition_generation.value()));
  append_line(text, "  capacity gen      : " + unsigned_text(capacity_generation.value()));
  append_line(text, "  revision          : " + unsigned_text(revision.value()));
  append_line(text, "  evidence epoch    : " + unsigned_text(evidence_epoch.value()));
  append_line(text, "  attempt           : " + unsigned_text(attempt.value()));
  append_line(text, "  store sequence    : " + unsigned_text(sequence.value()));
  append_line(text, "  store epoch       : " + unsigned_text(epoch.value()));
  append_line(text, "  snapshot digest   : " + snapshot_digest.to_short_hex());
  append_line(text, std::string("  reason            : ") + std::string(reason_code_name(reason)));
  if (!detail.empty()) {
    append_line(text, "  detail            : " + detail);
  }
  return text;
}

std::string RackCapacitySnapshot::to_text() const {
  std::string text;
  append_line(text, "rack capacity snapshot for " + rack.text());
  append_line(text, "  composition generation : " + unsigned_text(composition_generation.value()));
  append_line(text, "  capacity generation    : " + unsigned_text(capacity_generation.value()));
  append_line(text, "  revision               : " + unsigned_text(revision.value()));
  append_line(text, "  evidence epoch         : " + unsigned_text(evidence_epoch.value()));
  append_line(text, std::string("  evidence freshness     : ") +
                        std::string(evidence_freshness_name(freshness)));
  append_line(text, "  evidence observed at   : " + evidence_observed_at.to_text());
  append_line(text, "  evaluated at           : " + evaluated_at.to_text());
  append_line(text, "  policy                 : " + policy.to_text());
  append_line(text, "  rack height            : " + unsigned_text(unit_count) + " U (" +
                        unsigned_text(slots.total_slots) + " slots)");
  append_line(text, "  snapshot digest        : " + digest.to_hex());

  append_line(text, "slots:");
  append_line(text, "  total                  : " + unsigned_text(slots.total_slots));
  append_line(text, "  structural reserve     : " + unsigned_text(slots.structural_reserved_slots));
  append_line(text, "  occupied               : " + unsigned_text(slots.occupied_slots) + " (" +
                        unsigned_text(slots.shared_occupied_slots) + " shared)");
  append_line(text, "  committed reservations : " + unsigned_text(slots.reserved_slots));
  append_line(text, "  submitted reservations : " + unsigned_text(slots.pending_slots) +
                        " (consume nothing)");
  append_line(text, "  indeterminate          : " + unsigned_text(slots.indeterminate_slots));
  append_line(text, "  policy headroom        : " + unsigned_text(slots.policy_headroom_slots));
  append_line(text, "  overcommitted          : " + unsigned_text(slots.overcommitted_slots));
  append_line(text, "  free                   : " + bound_slots_text(slots.free));
  append_line(text, "  free runs              : " + unsigned_text(slots.fragmentation.free_run_count) +
                        ", largest " + unsigned_text(slots.fragmentation.largest_free_run_slots) +
                        " slots, isolated " +
                        unsigned_text(slots.fragmentation.isolated_free_slots));

  append_line(text, "power:");
  if (!power.envelope_known) {
    append_line(text, "  envelope               : unknown");
  } else {
    append_line(text, "  feeds                  : " + unsigned_text(power.feed_count) + " (" +
                          unsigned_text(power.contributing_feeds) + " contributing, " +
                          std::string(redundancy_mode_name(power.redundancy)) + ")");
    append_line(text, "  nominal                : " + measure_text(power.effective_nominal));
    append_line(text, "  physical derate        : " + power.physical_derate.to_text());
    append_line(text, "  physically derated     : " + measure_text(power.physically_derated));
    append_line(text, "  policy derate          : " + power.policy_derate.to_text());
    append_line(text, "  policy headroom        : " + power.policy_headroom.to_text());
    append_line(text, "  usable                 : " + measure_text(power.usable));
  }
  append_line(text, "  committed              : " + power.committed_known.to_text() + " (" +
                        unsigned_text(power.unknown_draw_assets) + " unknown)");
  append_line(text, "  reserved               : " + power.reserved_known.to_text());
  append_line(text, "  submitted              : " + power.pending_known.to_text() +
                        " (consumes nothing)");
  append_line(text, "  measured               : " + measure_text(power.measured) + " (" +
                        std::string(measurement_standing_name(power.measurement_standing)) + ")");
  append_line(text, "  overcommit             : " + power.overcommit.to_text());
  append_line(text, "  free                   : " + bound_text(power.free));

  append_line(text, "cooling:");
  if (!cooling.envelope_known) {
    append_line(text, "  envelope               : unknown");
  } else {
    append_line(text, "  nominal                : " + measure_text(cooling.nominal));
    append_line(text, "  physical derate        : " + cooling.physical_derate.to_text());
    append_line(text, "  physically derated     : " + measure_text(cooling.physically_derated));
    append_line(text, "  policy derate          : " + cooling.policy_derate.to_text());
    append_line(text, "  policy headroom        : " + cooling.policy_headroom.to_text());
    append_line(text, "  usable                 : " + measure_text(cooling.usable));
  }
  append_line(text, "  declared heat          : " + cooling.declared_heat_known.to_text());
  append_line(text, "  policy-derived heat    : " + cooling.derived_heat_known.to_text());
  append_line(text, "  reserved heat          : " + cooling.reserved_heat_known.to_text());
  append_line(text, "  submitted heat         : " + cooling.pending_heat_known.to_text() +
                        " (consumes nothing)");
  append_line(text, "  unknown heat occupants : " + unsigned_text(cooling.unknown_heat_assets));
  append_line(text, "  supply air temperature : " + measure_text(cooling.supply_air_temp));
  append_line(text, "  measured               : " + measure_text(cooling.measured) + " (" +
                        std::string(measurement_standing_name(cooling.measurement_standing)) +
                        ")");
  append_line(text, "  overcommit             : " + cooling.overcommit.to_text());
  append_line(text, "  free                   : " + bound_text(cooling.free));

  append_line(text, "weight:");
  if (!weight.envelope_known) {
    append_line(text, "  envelope               : unknown");
  } else {
    append_line(text, "  nominal limit          : " + measure_text(weight.nominal_limit));
    append_line(text, "  physical derate        : " + weight.physical_derate.to_text());
    append_line(text, "  physically derated     : " + measure_text(weight.physically_derated));
    append_line(text, "  policy derate          : " + weight.policy_derate.to_text());
    append_line(text, "  policy headroom        : " + weight.policy_headroom.to_text());
    append_line(text, "  usable                 : " + measure_text(weight.usable));
  }
  append_line(text, "  occupied               : " + weight.occupied_known.to_text() + " (" +
                        unsigned_text(weight.unknown_mass_assets) + " unknown)");
  append_line(text, "  reserved               : " + weight.reserved_known.to_text());
  append_line(text, "  submitted              : " + weight.pending_known.to_text() +
                        " (consumes nothing)");
  append_line(text, "  per-unit point limit   : " + measure_text(weight.per_unit_point_limit));
  append_line(text, "  peak unit load         : " + measure_text(weight.max_unit_load) + " (" +
                        unsigned_text(weight.units_at_or_above_point_limit) +
                        " unit(s) at or above the limit)");
  append_line(text, "  measured               : " + measure_text(weight.measured));
  append_line(text, "  overcommit             : " + weight.overcommit.to_text());
  append_line(text, "  free                   : " + bound_text(weight.free));

  append_line(text, "serviceability:");
  append_line(text, "  front clearance        : " + serviceability.front_clearance.to_text());
  append_line(text, "  rear clearance         : " + serviceability.rear_clearance.to_text());
  append_line(text, "  service height limit   : " +
                        (serviceability.service_height_limit_unit.has_value()
                             ? "U" + unsigned_text(serviceability.service_height_limit_unit.value())
                             : std::string("none declared")));
  append_line(text, "  occupied above limit   : " +
                        unsigned_text(serviceability.occupied_units_above_service_height) + " U");

  if (!shared_classes.empty()) {
    append_line(text, "shared mount classes:");
    for (const SharedClassUtilization& entry : shared_classes) {
      append_line(text, "  " + entry.shared_class.text() + " " + entry.span.to_text() +
                            ": capacity " + unsigned_text(entry.share_capacity) + ", used " +
                            unsigned_text(entry.used) + ", reserved " +
                            unsigned_text(entry.reserved) + ", submitted " +
                            unsigned_text(entry.pending) + ", remaining " +
                            unsigned_text(entry.remaining));
    }
  }

  append_line(text, "constraint pressure (highest first):");
  for (const ConstraintPressure& pressure : pressures) {
    std::string line = "  ";
    line += dimension_name(pressure.dimension);
    line += ": ";
    if (!pressure.known) {
      line += "unknown";
    } else {
      line += std::to_string(pressure.utilization_bp / 100) + "." +
              std::to_string(pressure.utilization_bp % 100) + "% of the usable envelope claimed";
      if (pressure.binding) {
        line += " (binding)";
      }
    }
    append_line(text, line);
  }
  append_line(text, std::string("  binding constraint becomes: ") +
                        std::string(dimension_name(primary_binding)));

  append_line(text, "explanations:");
  for (const Explanation& explanation : explanations) {
    append_line(text, "  " + explanation.to_text());
  }
  return text;
}

std::string InspectedSnapshot::to_text() const {
  std::string text;
  append_line(text, std::string("inspection of rack ") + record.rack.text());
  append_line(text, std::string("  record standing  : ") +
                        std::string(recovery_standing_name(record.standing)));
  append_line(text, std::string("  lifecycle        : ") +
                        std::string(rack_lifecycle_name(record.lifecycle)));
  append_line(text, std::string("  revalidation     : ") +
                        std::string(revalidation_verdict_name(verdict)));
  append_line(text, std::string("  freshness        : ") +
                        std::string(evidence_freshness_name(freshness)));
  if (recovered) {
    append_line(text, "  the record was recovered from durable state and was revalidated against "
                      "its own evidence for this inspection; it has not been promoted to "
                      "authoritative for this process");
  }
  text += snapshot.to_text();
  return text;
}
std::string ClosureReport::to_text() const {
  std::string text;
  append_line(text, std::string("closure: ") + (holds ? "holds" : "VIOLATED"));
  for (const std::string& identity : identities) {
    append_line(text, "  identity: " + identity);
  }
  for (const std::string& violation : violations) {
    append_line(text, "  violation: " + violation);
  }
  return text;
}

std::string RejectionRecord::to_text() const {
  std::string text = at.to_text();
  text += " ";
  if (!operation.empty()) {
    text += operation;
    text += ": ";
  }
  text += describe(error);
  return text;
}

std::string CatalogStats::to_text() const {
  std::string text;
  append_line(text, "racks                : " + unsigned_text(rack_count));
  append_line(text, "  active             : " + unsigned_text(active_racks));
  append_line(text, "  retired            : " + unsigned_text(retired_racks));
  append_line(text, "  quarantined        : " + unsigned_text(quarantined_racks));
  append_line(text, "  pending validation : " + unsigned_text(pending_revalidation_racks));
  append_line(text, "occupants recorded   : " + unsigned_text(asset_count));
  append_line(text, "reservations recorded: " + unsigned_text(reservation_count));
  append_line(text, "rejections retained  : " + unsigned_text(rejection_count));
  append_line(text, "idempotency receipts : " + unsigned_text(idempotency_records) + " (" +
                        unsigned_text(idempotency_evictions) + " evicted)");
  return text;
}

std::string CatalogStorage::to_text() const {
  std::string text;
  append_line(text, "incarnation          : " + incarnation);
  append_line(text, "store epoch          : " + unsigned_text(epoch.value()));
  append_line(text, "store sequence       : " + unsigned_text(sequence.value()));
  append_line(text, std::string("read only            : ") + (read_only ? "yes" : "no"));
  append_line(text, std::string("writer authority     : ") +
                        (holds_writer_authority ? "held" : "not held"));
  append_line(text, std::string("recovery             : ") +
                        std::string(recovery_action_name(recovery_action)));
  append_line(text, std::string("previous publication : ") + (has_previous ? "retained" : "none"));
  append_line(text, writer_lock.to_text());
  return text;
}

std::string WriterLockInfo::to_text() const {
  std::string text = "writer lock          : ";
  text += writer_lock_state_name(state);
  if (!writer_id.empty()) {
    text += " by ";
    text += writer_id.text();
  }
  if (pid != 0) {
    text += " pid ";
    text += unsigned_text(pid);
  }
  if (!incarnation.empty()) {
    text += " incarnation ";
    text += incarnation;
  }
  if (epoch.value() != 0) {
    text += " epoch ";
    text += unsigned_text(epoch.value());
  }
  if (!adopted_from.empty()) {
    text += " (adopted from ";
    text += adopted_from.text();
    if (adopted_from_pid != 0) {
      text += " pid ";
      text += unsigned_text(adopted_from_pid);
    }
    text += ")";
  }
  if (!detail.empty()) {
    text += ": ";
    text += detail;
  }
  return text;
}

std::string RecoveryReport::to_text() const {
  std::string text;
  append_line(text, std::string("recovery             : ") +
                        std::string(recovery_action_name(action)));
  append_line(text, "incarnation          : " + incarnation);
  append_line(text, "store epoch          : " + unsigned_text(epoch.value()));
  append_line(text, "store sequence       : " + unsigned_text(sequence.value()));
  append_line(text, "racks loaded         : " + unsigned_text(rack_count));
  if (current_rejection.has_value()) {
    append_line(text, "current file rejected: " + describe(current_rejection.value()));
  }
  for (const std::string& retired : retired_temp_files) {
    append_line(text, "retired temporary    : " + retired);
  }
  for (const std::string& note : notes) {
    append_line(text, "note                 : " + note);
  }
  return text;
}

std::string StateFileInfo::to_text() const {
  std::string text;
  append_line(text, "format version       : " + unsigned_text(format_version));
  append_line(text, "snapshot layout      : " + unsigned_text(snapshot_layout_version));
  append_line(text, "slots per rack unit  : " + unsigned_text(mount_slots_per_rack_unit));
  append_line(text, "incarnation          : " + incarnation);
  append_line(text, "store epoch          : " + unsigned_text(epoch.value()));
  append_line(text, "store sequence       : " + unsigned_text(sequence.value()));
  append_line(text, "racks                : " + unsigned_text(rack_count));
  append_line(text, "occupants            : " + unsigned_text(asset_count));
  append_line(text, "payload digest       : " + payload_digest.to_hex());
  append_line(text, "byte size            : " + unsigned_text(byte_size));
  return text;
}

}  // namespace rackcapacity
