// Rack Capacity - human-authorable specification text.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/text.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rack_capacity/coordinates.hpp"

namespace rackcapacity {
namespace {

[[nodiscard]] CapacityError spec_error(ErrorCode code, std::string message, std::size_t line,
                                       std::string field) {
  return make_error(code, std::move(message),
                    ErrorDetail{"spec.parse", std::move(field), {}, line, 0, {}});
}

struct Section {
  std::string name{};
  std::string subject{};
  std::map<std::string, std::string> values{};
  std::size_t line = 0;

  [[nodiscard]] bool has(std::string_view key) const {
    return values.find(std::string(key)) != values.end();
  }
  [[nodiscard]] std::string get(std::string_view key, std::string fallback = {}) const {
    const auto found = values.find(std::string(key));
    return found == values.end() ? std::move(fallback) : found->second;
  }
};

[[nodiscard]] Result<std::uint64_t> parse_unsigned(std::string_view text, std::size_t line,
                                                   std::string field) {
  if (text.empty()) {
    return spec_error(ErrorCode::EmptyValue, "expected a decimal integer", line, std::move(field));
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return spec_error(ErrorCode::InvalidCharacter,
                        "expected a decimal integer without sign or separator", line,
                        std::move(field));
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (0x7FFFFFFFFFFFFFFFull - digit) / 10ull) {
      return spec_error(ErrorCode::InvalidRange, "integer is too large to represent", line,
                        std::move(field));
    }
    value = value * 10ull + digit;
  }
  return value;
}

[[nodiscard]] Result<std::string> parse_text(std::string_view text, std::size_t line,
                                             std::string field) {
  if (text.empty()) {
    return spec_error(ErrorCode::EmptyValue, "expected a value", line, std::move(field));
  }
  return std::string(text);
}

[[nodiscard]] Status check_known_keys(const Section& section,
                                      const std::vector<std::string>& allowed) {
  for (const auto& entry : section.values) {
    if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
      return Status(spec_error(ErrorCode::InvalidArgument,
                               "unknown key in section [" + section.name + "]", section.line,
                               entry.first));
    }
  }
  return Status{};
}

[[nodiscard]] Result<std::string> require(const Section& section, std::string_view key) {
  const auto found = section.values.find(std::string(key));
  if (found == section.values.end()) {
    return spec_error(ErrorCode::EmptyValue,
                      "section [" + section.name + "] requires the key " + std::string(key),
                      section.line, std::string(key));
  }
  return found->second;
}

[[nodiscard]] Result<EvidenceSource> read_source(const Section& section,
                                                 EvidenceSource fallback) {
  if (!section.has("source")) {
    return fallback;
  }
  return parse_evidence_source(section.get("source"));
}

// Reads the evidence reference fields that every evidence block shares.
[[nodiscard]] Result<EvidenceReference> read_reference(const Section& section,
                                                       EvidenceSource fallback) {
  EvidenceReference reference;
  const Result<EvidenceSource> source = read_source(section, fallback);
  if (!source.has_value()) {
    return source.error();
  }
  reference.source = source.value();
  const Result<std::string> evidence = require(section, "evidence");
  if (!evidence.has_value()) {
    return evidence.error();
  }
  const Result<EvidenceId> id = EvidenceId::create(evidence.value());
  if (!id.has_value()) {
    return id.error();
  }
  reference.evidence = id.value();
  const Result<std::string> version = require(section, "version");
  if (!version.has_value()) {
    return version.error();
  }
  const Result<std::uint64_t> version_value = parse_unsigned(version.value(), section.line, "version");
  if (!version_value.has_value()) {
    return version_value.error();
  }
  if (version_value.value() == 0 || version_value.value() > 0xFFFFFFFFull) {
    return spec_error(ErrorCode::InvalidRange, "an evidence version starts at one and fits in 32 bits",
                      section.line, "version");
  }
  reference.version = static_cast<std::uint32_t>(version_value.value());
  const Result<std::string> observed = require(section, "observed-at-ns");
  if (!observed.has_value()) {
    return observed.error();
  }
  const Result<std::uint64_t> observed_value =
      parse_unsigned(observed.value(), section.line, "observed-at-ns");
  if (!observed_value.has_value()) {
    return observed_value.error();
  }
  const Result<TimestampNs> timestamp =
      TimestampNs::create(static_cast<std::int64_t>(observed_value.value()));
  if (!timestamp.has_value()) {
    return timestamp.error();
  }
  reference.observed_at = timestamp.value();
  if (section.has("producer")) {
    const Result<SourceReference> producer = SourceReference::create(section.get("producer"));
    if (!producer.has_value()) {
      return producer.error();
    }
    reference.producer = producer.value();
  }
  return reference;
}

[[nodiscard]] Result<std::optional<TimestampNs>> read_optional_timestamp(const Section& section,
                                                                        std::string_view key) {
  if (!section.has(key)) {
    return std::optional<TimestampNs>{};
  }
  const Result<std::uint64_t> value = parse_unsigned(section.get(key), section.line, std::string(key));
  if (!value.has_value()) {
    return value.error();
  }
  const Result<TimestampNs> timestamp = TimestampNs::create(static_cast<std::int64_t>(value.value()));
  if (!timestamp.has_value()) {
    return timestamp.error();
  }
  return std::optional<TimestampNs>(timestamp.value());
}

template <typename MeasureType>
[[nodiscard]] Result<std::optional<MeasureType>> read_optional_measure(const Section& section,
                                                                      std::string_view key) {
  if (!section.has(key)) {
    return std::optional<MeasureType>{};
  }
  const Result<std::uint64_t> value = parse_unsigned(section.get(key), section.line, std::string(key));
  if (!value.has_value()) {
    return value.error();
  }
  const Result<MeasureType> measure = MeasureType::create(static_cast<std::int64_t>(value.value()));
  if (!measure.has_value()) {
    return measure.error();
  }
  return std::optional<MeasureType>(measure.value());
}

[[nodiscard]] Result<std::uint64_t> read_required_unsigned(const Section& section,
                                                           std::string_view key) {
  const Result<std::string> text = require(section, key);
  if (!text.has_value()) {
    return text.error();
  }
  return parse_unsigned(text.value(), section.line, std::string(key));
}

[[nodiscard]] std::string render_measure(std::int64_t value) { return std::to_string(value); }

void emit(std::string& out, std::string_view key, std::string_view value) {
  out += key;
  out += " = ";
  out += value;
  out.push_back('\n');
}

void emit_reference(std::string& out, const EvidenceReference& reference, bool include_source) {
  if (include_source) {
    emit(out, "source", evidence_source_name(reference.source));
  }
  emit(out, "evidence", reference.evidence.text());
  emit(out, "version", std::to_string(reference.version));
  emit(out, "observed-at-ns", std::to_string(reference.observed_at.value()));
  if (!reference.producer.empty()) {
    emit(out, "producer", reference.producer.text());
  }
}

}  // namespace

Result<RackCapacityInputs> parse_specification(std::string_view text,
                                               const SpecificationParseOptions& options) {
  if (text.size() > kMaxSpecificationBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "the specification document exceeds the documented size bound",
                      ErrorDetail{"spec.parse", options.source_name, {}, kMaxSpecificationBytes,
                                  text.size(), {}});
  }

  std::vector<Section> sections;
  std::size_t line_number = 0;
  std::size_t position = 0;
  bool saw_header = false;
  Section* current = nullptr;

  while (position <= text.size()) {
    const std::size_t end = text.find('\n', position);
    std::string_view raw =
        end == std::string_view::npos ? text.substr(position) : text.substr(position, end - position);
    position = end == std::string_view::npos ? text.size() + 1 : end + 1;
    ++line_number;
    if (line_number > kMaxSpecificationLines) {
      return make_error(ErrorCode::LimitExceeded,
                        "the specification document has more lines than the documented bound",
                        ErrorDetail{"spec.parse", options.source_name, {}, kMaxSpecificationLines,
                                    line_number, {}});
    }
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }
    // Leading and trailing spaces are tolerated; internal spaces are not, so a
    // value can never be silently trimmed into a different value.
    const std::size_t first = raw.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      continue;
    }
    const std::size_t last = raw.find_last_not_of(" \t");
    const std::string_view line = raw.substr(first, last - first + 1);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    if (!saw_header) {
      if (line != "rcap-spec 1") {
        return make_error(ErrorCode::UnsupportedFormatVersion,
                          "a specification document must begin with the line 'rcap-spec 1'",
                          ErrorDetail{"spec.parse", options.source_name, {}, 1, line_number, {}});
      }
      saw_header = true;
      continue;
    }

    if (line.front() == '[') {
      if (line.back() != ']') {
        return spec_error(ErrorCode::InvalidText, "a section header must end with ']'",
                          line_number, options.source_name);
      }
      const std::string_view body = line.substr(1, line.size() - 2);
      const std::size_t space = body.find(' ');
      Section section;
      section.line = line_number;
      if (space == std::string_view::npos) {
        section.name = std::string(body);
      } else {
        section.name = std::string(body.substr(0, space));
        section.subject = std::string(body.substr(space + 1));
      }
      const bool is_composition = section.name == "composition";
      const bool is_policy = section.name == "policy";
      const bool is_power = section.name == "power";
      const bool is_cooling = section.name == "cooling";
      const bool is_weight = section.name == "weight";
      const bool is_request = section.name == "request";
      const bool is_asset = section.name == "asset";
      const bool is_reservation = section.name == "reservation";
      if (!is_composition && !is_policy && !is_power && !is_cooling && !is_weight &&
          !is_request && !is_asset && !is_reservation) {
        return spec_error(ErrorCode::InvalidArgument, "unknown section", line_number, section.name);
      }
      if ((is_asset || is_reservation) && section.subject.empty()) {
        return spec_error(ErrorCode::EmptyValue,
                          "an occupant or reservation section must name its identity", line_number,
                          section.name);
      }
      if (!(is_asset || is_reservation) && !section.subject.empty()) {
        return spec_error(ErrorCode::InvalidArgument,
                          "this section does not take an identity", line_number, section.name);
      }
      const bool repeated =
          std::any_of(sections.begin(), sections.end(), [&section](const Section& existing) {
            return existing.name == section.name && existing.subject == section.subject;
          });
      if (repeated) {
        return spec_error(ErrorCode::InvalidArgument, "duplicate section", line_number, section.name);
      }
      sections.push_back(std::move(section));
      current = &sections.back();
      continue;
    }

    if (current == nullptr) {
      return spec_error(ErrorCode::InvalidText, "a key must appear inside a section", line_number,
                        options.source_name);
    }
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos) {
      return spec_error(ErrorCode::InvalidText, "a key line must be written as 'key = value'",
                        line_number, std::string(line));
    }
    std::string_view key = line.substr(0, separator);
    std::string_view value = line.substr(separator + 1);
    const std::size_t key_end = key.find_last_not_of(" \t");
    if (key_end == std::string_view::npos) {
      return spec_error(ErrorCode::EmptyValue, "a key must not be empty", line_number,
                        options.source_name);
    }
    key = key.substr(0, key_end + 1);
    const std::size_t value_start = value.find_first_not_of(" \t");
    value = value_start == std::string_view::npos ? std::string_view{} : value.substr(value_start);
    if (current->values.find(std::string(key)) != current->values.end()) {
      return spec_error(ErrorCode::InvalidArgument, "duplicate key", line_number, std::string(key));
    }
    current->values.emplace(std::string(key), std::string(value));
  }

  if (!saw_header) {
    if (options.require_content) {
      return make_error(ErrorCode::EmptyValue,
                        "the specification document is empty; it must begin with 'rcap-spec 1'",
                        ErrorDetail{"spec.parse", options.source_name, {}, 0, 0, {}});
    }
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the specification document carries no version line",
                      ErrorDetail{"spec.parse", options.source_name, {}, 0, 0, {}});
  }

  const auto find_section = [&sections](std::string_view name,
                                        std::string_view subject) -> const Section* {
    for (const Section& section : sections) {
      if (section.name == name && section.subject == subject) {
        return &section;
      }
    }
    return nullptr;
  };

  // --- request -----------------------------------------------------------
  RackCapacityInputs inputs;
  const Section* request = find_section("request", {});
  if (request == nullptr) {
    return make_error(ErrorCode::EmptyValue, "the document must carry a [request] section",
                      ErrorDetail{"spec.parse", options.source_name, {}, 0, 0, {}});
  }
  Status status = check_known_keys(*request, {"epoch", "captured-at-ns", "actor", "request"});
  if (!status.has_value()) {
    return status.error();
  }
  {
    const Result<std::uint64_t> epoch = read_required_unsigned(*request, "epoch");
    if (!epoch.has_value()) {
      return epoch.error();
    }
    const Result<EvidenceEpoch> epoch_value =
        EvidenceEpoch::create(epoch.value());
    if (!epoch_value.has_value()) {
      return epoch_value.error();
    }
    inputs.epoch = epoch_value.value();
    const Result<std::uint64_t> captured = read_required_unsigned(*request, "captured-at-ns");
    if (!captured.has_value()) {
      return captured.error();
    }
    const Result<TimestampNs> captured_value =
        TimestampNs::create(static_cast<std::int64_t>(captured.value()));
    if (!captured_value.has_value()) {
      return captured_value.error();
    }
    inputs.captured_at = captured_value.value();
    const Result<std::string> actor = require(*request, "actor");
    if (!actor.has_value()) {
      return actor.error();
    }
    const Result<ActorId> actor_value = ActorId::create(actor.value());
    if (!actor_value.has_value()) {
      return actor_value.error();
    }
    inputs.actor = actor_value.value();
    const Result<std::string> request_id = require(*request, "request");
    if (!request_id.has_value()) {
      return request_id.error();
    }
    const Result<RequestId> request_value = RequestId::create(request_id.value());
    if (!request_value.has_value()) {
      return request_value.error();
    }
    inputs.request = request_value.value();
  }

  // --- composition -------------------------------------------------------
  const Section* composition = find_section("composition", {});
  if (composition == nullptr) {
    return make_error(ErrorCode::MissingCompositionEvidence,
                      "the document must carry a [composition] section",
                      ErrorDetail{"spec.parse", options.source_name, {}, 0, 0, {}});
  }
  status = check_known_keys(*composition,
                            {"rack", "generation", "units", "structural-reserved", "front-clearance-mm",
                             "rear-clearance-mm", "service-height-limit-u", "site", "hall", "row",
                             "position", "source", "evidence", "version", "observed-at-ns",
                             "producer"});
  if (!status.has_value()) {
    return status.error();
  }
  {
    const Result<std::string> rack = require(*composition, "rack");
    if (!rack.has_value()) {
      return rack.error();
    }
    const Result<RackId> rack_value = RackId::create(rack.value());
    if (!rack_value.has_value()) {
      return rack_value.error();
    }
    inputs.composition.rack = rack_value.value();
    const Result<std::uint64_t> generation = read_required_unsigned(*composition, "generation");
    if (!generation.has_value()) {
      return generation.error();
    }
    const Result<RackCompositionGeneration> generation_value =
        RackCompositionGeneration::create(generation.value());
    if (!generation_value.has_value()) {
      return generation_value.error();
    }
    inputs.composition.generation = generation_value.value();
    const Result<std::uint64_t> units = read_required_unsigned(*composition, "units");
    if (!units.has_value()) {
      return units.error();
    }
    const Result<std::uint32_t> units_value = validate_unit_count(units.value());
    if (!units_value.has_value()) {
      return units_value.error();
    }
    inputs.composition.unit_count = units_value.value();
    if (composition->has("structural-reserved")) {
      const Result<SlotSet> reserved = SlotSet::parse(composition->get("structural-reserved"));
      if (!reserved.has_value()) {
        return reserved.error();
      }
      inputs.composition.structural_reserved_slots = reserved.value();
    }
    if (composition->has("front-clearance-mm")) {
      const Result<std::uint64_t> front = read_required_unsigned(*composition, "front-clearance-mm");
      if (!front.has_value()) {
        return front.error();
      }
      const Result<Millimetres> value = Millimetres::create(static_cast<std::int64_t>(front.value()));
      if (!value.has_value()) {
        return value.error();
      }
      inputs.composition.serviceability.front_clearance = value.value();
    }
    if (composition->has("rear-clearance-mm")) {
      const Result<std::uint64_t> rear = read_required_unsigned(*composition, "rear-clearance-mm");
      if (!rear.has_value()) {
        return rear.error();
      }
      const Result<Millimetres> value = Millimetres::create(static_cast<std::int64_t>(rear.value()));
      if (!value.has_value()) {
        return value.error();
      }
      inputs.composition.serviceability.rear_clearance = value.value();
    }
    if (composition->has("service-height-limit-u")) {
      const Result<std::uint64_t> limit =
          read_required_unsigned(*composition, "service-height-limit-u");
      if (!limit.has_value()) {
        return limit.error();
      }
      if (limit.value() > kMaxRackUnits) {
        return spec_error(ErrorCode::InvalidRange,
                          "a service height limit must be inside the rack extent",
                          composition->line, "service-height-limit-u");
      }
      inputs.composition.serviceability.service_height_limit_unit =
          static_cast<std::uint32_t>(limit.value());
    }
    const auto read_location = [&composition](std::string_view key,
                                              auto& target) -> Status {
      if (!composition->has(key)) {
        return Status{};
      }
      using IdentityType = typename std::decay_t<decltype(target)>::value_type;
      const Result<IdentityType> value = IdentityType::create(composition->get(key));
      if (!value.has_value()) {
        return Status(value.error());
      }
      target = std::optional<IdentityType>(value.value());
      return Status{};
    };
    status = read_location("site", inputs.composition.location.site);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_location("hall", inputs.composition.location.hall);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_location("row", inputs.composition.location.row);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_location("position", inputs.composition.location.position);
    if (!status.has_value()) {
      return status.error();
    }
    const Result<EvidenceReference> reference =
        read_reference(*composition, EvidenceSource::RackRegistry);
    if (!reference.has_value()) {
      return reference.error();
    }
    inputs.composition.reference = reference.value();
  }

  // --- policy ------------------------------------------------------------
  const Section* policy = find_section("policy", {});
  if (policy == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "the document must carry a [policy] section",
                      ErrorDetail{"spec.parse", options.source_name, {}, 0, 0, {}});
  }
  status = check_known_keys(*policy,
                            {"policy", "policy-version", "power-headroom-w", "cooling-headroom-w",
                             "weight-headroom-g", "slot-headroom", "power-derate-bp",
                             "cooling-derate-bp", "weight-derate-bp", "max-envelope-age-ns",
                             "max-measurement-age-ns", "heat-per-power-ppm"});
  if (!status.has_value()) {
    return status.error();
  }
  {
    const Result<std::string> id = require(*policy, "policy");
    if (!id.has_value()) {
      return id.error();
    }
    const Result<PolicyId> policy_id = PolicyId::create(id.value());
    if (!policy_id.has_value()) {
      return policy_id.error();
    }
    inputs.policy.reference.policy = policy_id.value();
    const Result<std::uint64_t> version = read_required_unsigned(*policy, "policy-version");
    if (!version.has_value()) {
      return version.error();
    }
    if (version.value() == 0 || version.value() > 0xFFFFFFFFull) {
      return spec_error(ErrorCode::InvalidRange, "a policy version starts at one", policy->line,
                        "policy-version");
    }
    inputs.policy.reference.version = static_cast<std::uint32_t>(version.value());

    const auto read_optional_watts = [&policy](std::string_view key,
                                               Watts& target) -> Status {
      if (!policy->has(key)) {
        return Status{};
      }
      const Result<std::uint64_t> value = parse_unsigned(policy->get(key), policy->line,
                                                         std::string(key));
      if (!value.has_value()) {
        return Status(value.error());
      }
      const Result<Watts> watts = Watts::create(static_cast<std::int64_t>(value.value()));
      if (!watts.has_value()) {
        return Status(watts.error());
      }
      target = watts.value();
      return Status{};
    };
    const auto read_optional_bp = [&policy](std::string_view key, BasisPoints& target) -> Status {
      if (!policy->has(key)) {
        return Status{};
      }
      const Result<std::uint64_t> value =
          parse_unsigned(policy->get(key), policy->line, std::string(key));
      if (!value.has_value()) {
        return Status(value.error());
      }
      const Result<BasisPoints> basis = BasisPoints::create(static_cast<std::int64_t>(value.value()));
      if (!basis.has_value()) {
        return Status(basis.error());
      }
      target = basis.value();
      return Status{};
    };
    status = read_optional_watts("power-headroom-w", inputs.policy.headroom.power_headroom);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_optional_watts("cooling-headroom-w", inputs.policy.headroom.cooling_headroom);
    if (!status.has_value()) {
      return status.error();
    }
    if (policy->has("weight-headroom-g")) {
      const Result<std::uint64_t> value =
          read_required_unsigned(*policy, "weight-headroom-g");
      if (!value.has_value()) {
        return value.error();
      }
      const Result<Grams> grams = Grams::create(static_cast<std::int64_t>(value.value()));
      if (!grams.has_value()) {
        return grams.error();
      }
      inputs.policy.headroom.weight_headroom = grams.value();
    }
    if (policy->has("slot-headroom")) {
      const Result<std::uint64_t> value = read_required_unsigned(*policy, "slot-headroom");
      if (!value.has_value()) {
        return value.error();
      }
      if (value.value() > kMaxMountSlotExclusive - 1u) {
        return spec_error(ErrorCode::InvalidRange,
                          "slot headroom exceeds the largest representable rack", policy->line,
                          "slot-headroom");
      }
      inputs.policy.headroom.slot_headroom = static_cast<std::uint32_t>(value.value());
    }
    status = read_optional_bp("power-derate-bp", inputs.policy.headroom.power_derate);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_optional_bp("cooling-derate-bp", inputs.policy.headroom.cooling_derate);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_optional_bp("weight-derate-bp", inputs.policy.headroom.weight_derate);
    if (!status.has_value()) {
      return status.error();
    }
    if (policy->has("max-envelope-age-ns")) {
      const Result<std::uint64_t> value = read_required_unsigned(*policy, "max-envelope-age-ns");
      if (!value.has_value()) {
        return value.error();
      }
      const Result<DurationNs> duration = DurationNs::create(static_cast<std::int64_t>(value.value()));
      if (!duration.has_value()) {
        return duration.error();
      }
      inputs.policy.freshness.max_envelope_age = duration.value();
    }
    if (policy->has("max-measurement-age-ns")) {
      const Result<std::uint64_t> value = read_required_unsigned(*policy, "max-measurement-age-ns");
      if (!value.has_value()) {
        return value.error();
      }
      const Result<DurationNs> duration = DurationNs::create(static_cast<std::int64_t>(value.value()));
      if (!duration.has_value()) {
        return duration.error();
      }
      inputs.policy.freshness.max_measurement_age = duration.value();
    }
    if (policy->has("heat-per-power-ppm")) {
      const Result<std::uint64_t> value = read_required_unsigned(*policy, "heat-per-power-ppm");
      if (!value.has_value()) {
        return value.error();
      }
      if (value.value() > 1'000'000ull) {
        return spec_error(ErrorCode::InvalidRange,
                          "a heat equivalence above 1000000 parts per million is not a physical "
                          "conversion",
                          policy->line, "heat-per-power-ppm");
      }
      inputs.policy.heat_per_power_ppm = static_cast<std::uint32_t>(value.value());
    }
  }

  // --- power -------------------------------------------------------------
  const Section* power_section = find_section("power", {});
  if (power_section != nullptr) {
    status = check_known_keys(*power_section,
                              {"feeds", "watts-per-feed", "redundancy", "derate-bp", "measured-draw-w",
                               "measured-at-ns", "source", "evidence", "version",
                               "observed-at-ns", "producer"});
    if (!status.has_value()) {
      return status.error();
    }
    PowerCapacityEvidence power;
    const Result<std::uint64_t> feeds = read_required_unsigned(*power_section, "feeds");
    if (!feeds.has_value()) {
      return feeds.error();
    }
    if (feeds.value() > kMaxFeeds) {
      return spec_error(ErrorCode::InvalidRange, "too many feeds declared", power_section->line,
                        "feeds");
    }
    power.feed_count = static_cast<std::uint32_t>(feeds.value());
    const Result<std::uint64_t> watts = read_required_unsigned(*power_section, "watts-per-feed");
    if (!watts.has_value()) {
      return watts.error();
    }
    const Result<Watts> watts_value = Watts::create(static_cast<std::int64_t>(watts.value()));
    if (!watts_value.has_value()) {
      return watts_value.error();
    }
    power.watts_per_feed = watts_value.value();
    const Result<std::string> redundancy = require(*power_section, "redundancy");
    if (!redundancy.has_value()) {
      return redundancy.error();
    }
    const Result<RedundancyMode> mode = parse_redundancy_mode(redundancy.value());
    if (!mode.has_value()) {
      return mode.error();
    }
    power.redundancy = mode.value();
    if (power_section->has("derate-bp")) {
      const Result<std::uint64_t> bp = read_required_unsigned(*power_section, "derate-bp");
      if (!bp.has_value()) {
        return bp.error();
      }
      const Result<BasisPoints> value = BasisPoints::create(static_cast<std::int64_t>(bp.value()));
      if (!value.has_value()) {
        return value.error();
      }
      power.derate = value.value();
    }
    const Result<std::optional<Watts>> measured =
        read_optional_measure<Watts>(*power_section, "measured-draw-w");
    if (!measured.has_value()) {
      return measured.error();
    }
    power.measured_draw = measured.value();
    const Result<std::optional<TimestampNs>> measured_at =
        read_optional_timestamp(*power_section, "measured-at-ns");
    if (!measured_at.has_value()) {
      return measured_at.error();
    }
    power.measured_at = measured_at.value();
    const Result<EvidenceReference> reference =
        read_reference(*power_section, EvidenceSource::PowerCapacity);
    if (!reference.has_value()) {
      return reference.error();
    }
    power.reference = reference.value();
    inputs.power = power;
  }

  // --- cooling -----------------------------------------------------------
  const Section* cooling_section = find_section("cooling", {});
  if (cooling_section != nullptr) {
    status = check_known_keys(*cooling_section,
                              {"nominal-heat-rejection-w", "derate-bp", "supply-air-temp-mc",
                               "measured-heat-load-w", "measured-at-ns", "source", "evidence",
                               "version", "observed-at-ns", "producer"});
    if (!status.has_value()) {
      return status.error();
    }
    CoolingCapacityEvidence cooling;
    const Result<std::uint64_t> nominal =
        read_required_unsigned(*cooling_section, "nominal-heat-rejection-w");
    if (!nominal.has_value()) {
      return nominal.error();
    }
    const Result<Watts> nominal_value = Watts::create(static_cast<std::int64_t>(nominal.value()));
    if (!nominal_value.has_value()) {
      return nominal_value.error();
    }
    cooling.nominal_heat_rejection = nominal_value.value();
    if (cooling_section->has("derate-bp")) {
      const Result<std::uint64_t> bp = read_required_unsigned(*cooling_section, "derate-bp");
      if (!bp.has_value()) {
        return bp.error();
      }
      const Result<BasisPoints> value = BasisPoints::create(static_cast<std::int64_t>(bp.value()));
      if (!value.has_value()) {
        return value.error();
      }
      cooling.derate = value.value();
    }
    const Result<std::optional<MilliCelsius>> temperature =
        read_optional_measure<MilliCelsius>(*cooling_section, "supply-air-temp-mc");
    if (!temperature.has_value()) {
      return temperature.error();
    }
    cooling.supply_air_temp = temperature.value();
    const Result<std::optional<Watts>> measured =
        read_optional_measure<Watts>(*cooling_section, "measured-heat-load-w");
    if (!measured.has_value()) {
      return measured.error();
    }
    cooling.measured_heat_load = measured.value();
    const Result<std::optional<TimestampNs>> measured_at =
        read_optional_timestamp(*cooling_section, "measured-at-ns");
    if (!measured_at.has_value()) {
      return measured_at.error();
    }
    cooling.measured_at = measured_at.value();
    const Result<EvidenceReference> reference =
        read_reference(*cooling_section, EvidenceSource::CoolingCapacity);
    if (!reference.has_value()) {
      return reference.error();
    }
    cooling.reference = reference.value();
    inputs.cooling = cooling;
  }

  // --- weight ------------------------------------------------------------
  const Section* weight_section = find_section("weight", {});
  if (weight_section != nullptr) {
    status = check_known_keys(*weight_section,
                              {"static-load-limit-g", "derate-bp", "per-unit-point-load-limit-g",
                               "measured-static-load-g", "measured-at-ns", "source", "evidence",
                               "version", "observed-at-ns", "producer"});
    if (!status.has_value()) {
      return status.error();
    }
    WeightCapacityEvidence weight;
    const Result<std::optional<Grams>> limit =
        read_optional_measure<Grams>(*weight_section, "static-load-limit-g");
    if (!limit.has_value()) {
      return limit.error();
    }
    weight.static_load_limit = limit.value();
    if (weight_section->has("derate-bp")) {
      const Result<std::uint64_t> bp = read_required_unsigned(*weight_section, "derate-bp");
      if (!bp.has_value()) {
        return bp.error();
      }
      const Result<BasisPoints> value = BasisPoints::create(static_cast<std::int64_t>(bp.value()));
      if (!value.has_value()) {
        return value.error();
      }
      weight.derate = value.value();
    }
    const Result<std::optional<Grams>> point =
        read_optional_measure<Grams>(*weight_section, "per-unit-point-load-limit-g");
    if (!point.has_value()) {
      return point.error();
    }
    weight.per_unit_point_load_limit = point.value();
    const Result<std::optional<Grams>> measured =
        read_optional_measure<Grams>(*weight_section, "measured-static-load-g");
    if (!measured.has_value()) {
      return measured.error();
    }
    weight.measured_static_load = measured.value();
    const Result<std::optional<TimestampNs>> measured_at =
        read_optional_timestamp(*weight_section, "measured-at-ns");
    if (!measured_at.has_value()) {
      return measured_at.error();
    }
    weight.measured_at = measured_at.value();
    const Result<EvidenceReference> reference =
        read_reference(*weight_section, EvidenceSource::CoolingCapacity);
    if (!reference.has_value()) {
      return reference.error();
    }
    weight.reference = reference.value();
    inputs.weight = weight;
  }

  // --- occupants ---------------------------------------------------------
  for (const Section& section : sections) {
    if (section.name != "asset") {
      continue;
    }
    status = check_known_keys(section,
                              {"span", "kind", "shared-class", "share-capacity", "presence",
                               "nameplate-draw-w", "declared-heat-w", "mass-g", "source", "evidence",
                               "version", "observed-at-ns", "producer"});
    if (!status.has_value()) {
      return status.error();
    }
    AssetOccupancyEvidence asset;
    const Result<AssetId> id = AssetId::create(section.subject);
    if (!id.has_value()) {
      return id.error();
    }
    asset.asset = id.value();
    if (section.has("kind")) {
      const Result<MountSpanKind> kind = parse_mount_span_kind(section.get("kind"));
      if (!kind.has_value()) {
        return kind.error();
      }
      asset.kind = kind.value();
    }
    if (section.has("span")) {
      const Result<SlotSet> parsed = SlotSet::parse(section.get("span"));
      if (!parsed.has_value()) {
        return parsed.error();
      }
      if (parsed.value().interval_count() != 1) {
        return spec_error(ErrorCode::InvalidSlotInterval,
                          "an occupant span must be exactly one interval", section.line, "span");
      }
      asset.span = parsed.value().intervals().front();
    } else if (asset.kind == MountSpanKind::ZeroU) {
      // A zero-u occupant claims no slot, so it carries the lowest slot
      // interval purely as a well-formed placeholder that accounting ignores.
      const Result<SlotInterval> unclaimed = SlotInterval::create(1, 2);
      if (!unclaimed.has_value()) {
        return unclaimed.error();
      }
      asset.span = unclaimed.value();
    } else {
      return spec_error(ErrorCode::EmptyValue, "an occupant must declare its span", section.line,
                        "span");
    }
    if (section.has("shared-class")) {
      const Result<SharedMountClassId> shared =
          SharedMountClassId::create(section.get("shared-class"));
      if (!shared.has_value()) {
        return shared.error();
      }
      asset.shared_class = shared.value();
    }
    if (section.has("share-capacity")) {
      const Result<std::uint64_t> capacity = read_required_unsigned(section, "share-capacity");
      if (!capacity.has_value()) {
        return capacity.error();
      }
      if (capacity.value() > kMaxShareCapacity) {
        return spec_error(ErrorCode::InvalidRange, "share capacity exceeds the documented bound",
                          section.line, "share-capacity");
      }
      asset.share_capacity = static_cast<std::uint32_t>(capacity.value());
    }
    if (section.has("presence")) {
      const Result<PresenceState> presence = parse_presence_state(section.get("presence"));
      if (!presence.has_value()) {
        return presence.error();
      }
      asset.presence = presence.value();
    }
    const Result<std::optional<Watts>> draw =
        read_optional_measure<Watts>(section, "nameplate-draw-w");
    if (!draw.has_value()) {
      return draw.error();
    }
    asset.nameplate_draw = draw.value();
    const Result<std::optional<Watts>> heat =
        read_optional_measure<Watts>(section, "declared-heat-w");
    if (!heat.has_value()) {
      return heat.error();
    }
    asset.declared_heat_rejection = heat.value();
    const Result<std::optional<Grams>> mass = read_optional_measure<Grams>(section, "mass-g");
    if (!mass.has_value()) {
      return mass.error();
    }
    asset.mass = mass.value();
    const Result<EvidenceReference> reference =
        read_reference(section, EvidenceSource::AssetRegistry);
    if (!reference.has_value()) {
      return reference.error();
    }
    asset.reference = reference.value();
    inputs.assets.push_back(std::move(asset));
  }

  // --- reservations ------------------------------------------------------
  for (const Section& section : sections) {
    if (section.name != "reservation") {
      continue;
    }
    status = check_known_keys(section,
                              {"state", "span", "kind", "draw-w", "heat-w", "mass-g", "source",
                               "evidence", "version", "observed-at-ns", "producer"});
    if (!status.has_value()) {
      return status.error();
    }
    ReservationEvidence reservation;
    const Result<ReservationId> id = ReservationId::create(section.subject);
    if (!id.has_value()) {
      return id.error();
    }
    reservation.reservation = id.value();
    if (section.has("state")) {
      const Result<ReservationState> state = parse_reservation_state(section.get("state"));
      if (!state.has_value()) {
        return state.error();
      }
      reservation.state = state.value();
    }
    if (section.has("kind")) {
      const Result<MountSpanKind> kind = parse_mount_span_kind(section.get("kind"));
      if (!kind.has_value()) {
        return kind.error();
      }
      reservation.kind = kind.value();
    }
    if (section.has("span")) {
      const Result<SlotSet> parsed = SlotSet::parse(section.get("span"));
      if (!parsed.has_value()) {
        return parsed.error();
      }
      if (parsed.value().interval_count() != 1) {
        return spec_error(ErrorCode::InvalidSlotInterval,
                          "a reservation span must be exactly one interval", section.line, "span");
      }
      reservation.span = parsed.value().intervals().front();
    }
    const Result<std::optional<Watts>> draw = read_optional_measure<Watts>(section, "draw-w");
    if (!draw.has_value()) {
      return draw.error();
    }
    reservation.draw = draw.value();
    const Result<std::optional<Watts>> heat = read_optional_measure<Watts>(section, "heat-w");
    if (!heat.has_value()) {
      return heat.error();
    }
    reservation.heat_rejection = heat.value();
    const Result<std::optional<Grams>> mass = read_optional_measure<Grams>(section, "mass-g");
    if (!mass.has_value()) {
      return mass.error();
    }
    reservation.mass = mass.value();
    const Result<EvidenceReference> reference =
        read_reference(section, EvidenceSource::FacilityCapacityReservation);
    if (!reference.has_value()) {
      return reference.error();
    }
    reservation.reference = reference.value();
    inputs.reservations.push_back(std::move(reservation));
  }

  const Status canonical = canonicalize_inputs(inputs);
  if (!canonical.has_value()) {
    return canonical.error();
  }
  const Status valid = validate_inputs(inputs);
  if (!valid.has_value()) {
    return valid.error();
  }
  return inputs;
}

std::string render_specification(const RackCapacityInputs& inputs) {
  std::string out;
  out += "rcap-spec 1\n";

  out += "[composition]\n";
  emit(out, "rack", inputs.composition.rack.text());
  emit(out, "generation", std::to_string(inputs.composition.generation.value()));
  emit(out, "units", std::to_string(inputs.composition.unit_count));
  emit(out, "structural-reserved", inputs.composition.structural_reserved_slots.to_text());
  emit(out, "front-clearance-mm",
       std::to_string(inputs.composition.serviceability.front_clearance.value()));
  emit(out, "rear-clearance-mm",
       std::to_string(inputs.composition.serviceability.rear_clearance.value()));
  if (inputs.composition.serviceability.service_height_limit_unit.has_value()) {
    emit(out, "service-height-limit-u",
         std::to_string(inputs.composition.serviceability.service_height_limit_unit.value()));
  }
  if (inputs.composition.location.site.has_value()) {
    emit(out, "site", inputs.composition.location.site->text());
  }
  if (inputs.composition.location.hall.has_value()) {
    emit(out, "hall", inputs.composition.location.hall->text());
  }
  if (inputs.composition.location.row.has_value()) {
    emit(out, "row", inputs.composition.location.row->text());
  }
  if (inputs.composition.location.position.has_value()) {
    emit(out, "position", inputs.composition.location.position->text());
  }
  emit_reference(out, inputs.composition.reference, true);

  out += "[policy]\n";
  emit(out, "policy", inputs.policy.reference.policy.text());
  emit(out, "policy-version", std::to_string(inputs.policy.reference.version));
  emit(out, "power-headroom-w", render_measure(inputs.policy.headroom.power_headroom.value()));
  emit(out, "cooling-headroom-w",
       render_measure(inputs.policy.headroom.cooling_headroom.value()));
  emit(out, "weight-headroom-g",
       render_measure(inputs.policy.headroom.weight_headroom.value()));
  emit(out, "slot-headroom", std::to_string(inputs.policy.headroom.slot_headroom));
  emit(out, "power-derate-bp", std::to_string(inputs.policy.headroom.power_derate.value()));
  emit(out, "cooling-derate-bp", std::to_string(inputs.policy.headroom.cooling_derate.value()));
  emit(out, "weight-derate-bp", std::to_string(inputs.policy.headroom.weight_derate.value()));
  emit(out, "max-envelope-age-ns", std::to_string(inputs.policy.freshness.max_envelope_age.value()));
  emit(out, "max-measurement-age-ns",
       std::to_string(inputs.policy.freshness.max_measurement_age.value()));
  emit(out, "heat-per-power-ppm", std::to_string(inputs.policy.heat_per_power_ppm));

  if (inputs.power.has_value()) {
    const PowerCapacityEvidence& power = inputs.power.value();
    out += "[power]\n";
    emit(out, "feeds", std::to_string(power.feed_count));
    emit(out, "watts-per-feed", std::to_string(power.watts_per_feed.value()));
    emit(out, "redundancy", redundancy_mode_name(power.redundancy));
    emit(out, "derate-bp", std::to_string(power.derate.value()));
    if (power.measured_draw.has_value()) {
      emit(out, "measured-draw-w", std::to_string(power.measured_draw->value()));
    }
    if (power.measured_at.has_value()) {
      emit(out, "measured-at-ns", std::to_string(power.measured_at->value()));
    }
    emit_reference(out, power.reference, true);
  }

  if (inputs.cooling.has_value()) {
    const CoolingCapacityEvidence& cooling = inputs.cooling.value();
    out += "[cooling]\n";
    emit(out, "nominal-heat-rejection-w", std::to_string(cooling.nominal_heat_rejection.value()));
    emit(out, "derate-bp", std::to_string(cooling.derate.value()));
    if (cooling.supply_air_temp.has_value()) {
      emit(out, "supply-air-temp-mc", std::to_string(cooling.supply_air_temp->value()));
    }
    if (cooling.measured_heat_load.has_value()) {
      emit(out, "measured-heat-load-w", std::to_string(cooling.measured_heat_load->value()));
    }
    if (cooling.measured_at.has_value()) {
      emit(out, "measured-at-ns", std::to_string(cooling.measured_at->value()));
    }
    emit_reference(out, cooling.reference, true);
  }

  if (inputs.weight.has_value()) {
    const WeightCapacityEvidence& weight = inputs.weight.value();
    out += "[weight]\n";
    if (weight.static_load_limit.has_value()) {
      emit(out, "static-load-limit-g", std::to_string(weight.static_load_limit->value()));
    }
    emit(out, "derate-bp", std::to_string(weight.derate.value()));
    if (weight.per_unit_point_load_limit.has_value()) {
      emit(out, "per-unit-point-load-limit-g",
           std::to_string(weight.per_unit_point_load_limit->value()));
    }
    if (weight.measured_static_load.has_value()) {
      emit(out, "measured-static-load-g", std::to_string(weight.measured_static_load->value()));
    }
    if (weight.measured_at.has_value()) {
      emit(out, "measured-at-ns", std::to_string(weight.measured_at->value()));
    }
    emit_reference(out, weight.reference, true);
  }

  for (const AssetOccupancyEvidence& asset : inputs.assets) {
    out += "[asset ";
    out += asset.asset.text();
    out += "]\n";
    emit(out, "span", asset.span.to_text());
    emit(out, "kind", mount_span_kind_name(asset.kind));
    if (!asset.shared_class.empty()) {
      emit(out, "shared-class", asset.shared_class.text());
    }
    if (asset.share_capacity != 0) {
      emit(out, "share-capacity", std::to_string(asset.share_capacity));
    }
    emit(out, "presence", presence_state_name(asset.presence));
    if (asset.nameplate_draw.has_value()) {
      emit(out, "nameplate-draw-w", std::to_string(asset.nameplate_draw->value()));
    }
    if (asset.declared_heat_rejection.has_value()) {
      emit(out, "declared-heat-w", std::to_string(asset.declared_heat_rejection->value()));
    }
    if (asset.mass.has_value()) {
      emit(out, "mass-g", std::to_string(asset.mass->value()));
    }
    emit_reference(out, asset.reference, true);
  }

  for (const ReservationEvidence& reservation : inputs.reservations) {
    out += "[reservation ";
    out += reservation.reservation.text();
    out += "]\n";
    emit(out, "state", reservation_state_name(reservation.state));
    if (reservation.span.has_value()) {
      emit(out, "span", reservation.span->to_text());
    }
    emit(out, "kind", mount_span_kind_name(reservation.kind));
    if (reservation.draw.has_value()) {
      emit(out, "draw-w", std::to_string(reservation.draw->value()));
    }
    if (reservation.heat_rejection.has_value()) {
      emit(out, "heat-w", std::to_string(reservation.heat_rejection->value()));
    }
    if (reservation.mass.has_value()) {
      emit(out, "mass-g", std::to_string(reservation.mass->value()));
    }
    emit_reference(out, reservation.reference, true);
  }

  out += "[request]\n";
  emit(out, "epoch", std::to_string(inputs.epoch.value()));
  emit(out, "captured-at-ns", std::to_string(inputs.captured_at.value()));
  emit(out, "actor", inputs.actor.text());
  emit(out, "request", inputs.request.text());
  return out;
}

}  // namespace rackcapacity
