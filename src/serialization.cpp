// Rack Capacity - canonical encoding and strict decoding of domain values.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "codec.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace rackcapacity::codec {

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------

void ByteWriter::raw(const void* data, std::size_t size) {
  if (!ok_) {
    return;
  }
  if (static_cast<std::uint64_t>(data_.size()) + size > limit_) {
    ok_ = false;
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  data_.insert(data_.end(), bytes, bytes + size);
}

void ByteWriter::u16(std::uint16_t value) {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 8u) & 0xFFu)};
  raw(bytes, 2);
}

void ByteWriter::u32(std::uint32_t value) {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 16u) & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 24u) & 0xFFu)};
  raw(bytes, 4);
}

void ByteWriter::u64(std::uint64_t value) {
  std::uint8_t bytes[8];
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu);
  }
  raw(bytes, 8);
}

void ByteWriter::i64(std::int64_t value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "64-bit conversion must be exact");
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void ByteWriter::string(std::string_view text) {
  u32(static_cast<std::uint32_t>(text.size()));
  raw(text.data(), text.size());
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------

Status ByteReader::need(std::size_t bytes) const {
  if (bytes > size_ - position_) {
    return Status(make_error(ErrorCode::TruncatedState,
                             "stored state ends before the declared structure",
                             ErrorDetail{"decode", {}, {}, bytes, size_ - position_, {}}));
  }
  return Status{};
}

Result<std::uint8_t> ByteReader::u8() {
  const Status available = need(1);
  if (!available.has_value()) {
    return available.error();
  }
  return data_[position_++];
}

Result<std::uint16_t> ByteReader::u16() {
  const Status available = need(2);
  if (!available.has_value()) {
    return available.error();
  }
  const std::uint16_t value = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data_[position_]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[position_ + 1]) << 8u));
  position_ += 2;
  return value;
}

Result<std::uint32_t> ByteReader::u32() {
  const Status available = need(4);
  if (!available.has_value()) {
    return available.error();
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(index)])
             << (index * 8);
  }
  position_ += 4;
  return value;
}

Result<std::uint64_t> ByteReader::u64() {
  const Status available = need(8);
  if (!available.has_value()) {
    return available.error();
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(index)])
             << (index * 8);
  }
  position_ += 8;
  return value;
}

Result<std::int64_t> ByteReader::i64() {
  const Result<std::uint64_t> bits = u64();
  if (!bits.has_value()) {
    return bits.error();
  }
  std::int64_t value = 0;
  const std::uint64_t raw = bits.value();
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

Result<bool> ByteReader::boolean() {
  const Result<std::uint8_t> value = u8();
  if (!value.has_value()) {
    return value.error();
  }
  if (value.value() > 1u) {
    return make_error(ErrorCode::InvalidStateEncoding,
                      "a boolean field holds neither zero nor one",
                      ErrorDetail{"decode.boolean", {}, {}, 1, value.value(), {}});
  }
  return value.value() == 1u;
}

Result<std::string> ByteReader::string(std::size_t max_bytes) {
  const Result<std::uint32_t> length = u32();
  if (!length.has_value()) {
    return length.error();
  }
  if (length.value() > max_bytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "a stored string exceeds the documented byte length",
                      ErrorDetail{"decode.string", {}, {}, max_bytes, length.value(), {}});
  }
  const Status available = need(length.value());
  if (!available.has_value()) {
    return available.error();
  }
  std::string text(reinterpret_cast<const char*>(data_ + position_), length.value());
  position_ += length.value();
  return text;
}

Result<StateDigest> ByteReader::digest() {
  const Status available = need(Sha256::kDigestBytes);
  if (!available.has_value()) {
    return available.error();
  }
  std::array<std::uint8_t, Sha256::kDigestBytes> bytes{};
  std::memcpy(bytes.data(), data_ + position_, bytes.size());
  position_ += bytes.size();
  return StateDigest(bytes);
}

Result<std::vector<std::uint8_t>> ByteReader::raw(std::size_t size) {
  const Status available = need(size);
  if (!available.has_value()) {
    return available.error();
  }
  std::vector<std::uint8_t> bytes(data_ + position_, data_ + position_ + size);
  position_ += size;
  return bytes;
}

Result<std::uint32_t> ByteReader::count(std::size_t max_count, std::string_view field) {
  const Result<std::uint32_t> value = u32();
  if (!value.has_value()) {
    return value.error();
  }
  if (value.value() > max_count) {
    return make_error(ErrorCode::LimitExceeded,
                      "a stored collection declares more elements than the documented bound",
                      ErrorDetail{"decode.count", std::string(field), {}, max_count,
                                  value.value(), {}});
  }
  return value;
}

Status ByteReader::require_exhausted(std::string_view subject) const {
  if (position_ != size_) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "stored state carries trailing bytes after the declared structure",
                             ErrorDetail{"decode.exhausted", std::string(subject), {}, size_,
                                         position_, {}}));
  }
  return Status{};
}

namespace {

// --- small helpers ----------------------------------------------------------

template <typename Enum>
void put_enum(ByteWriter& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
Result<Enum> get_enum(ByteReader& reader, std::uint8_t max_value, std::string_view field) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.has_value()) {
    return raw.error();
  }
  if (raw.value() > max_value) {
    return make_error(ErrorCode::InvalidEnumValue,
                      "a stored enumeration member is not defined by this version",
                      ErrorDetail{"decode.enum", std::string(field), {}, max_value, raw.value(),
                                  {}});
  }
  return static_cast<Enum>(raw.value());
}

void put_bound_u32(ByteWriter& writer, const Bound<std::uint32_t>& bound) {
  writer.boolean(bound.is_known());
  if (bound.is_known()) {
    writer.u32(bound.lower());
    writer.u32(bound.upper());
  }
}

Result<Bound<std::uint32_t>> get_bound_u32(ByteReader& reader, std::string_view field) {
  const Result<bool> known = reader.boolean();
  if (!known.has_value()) {
    return known.error();
  }
  if (!known.value()) {
    return Bound<std::uint32_t>::unknown();
  }
  const Result<std::uint32_t> lower = reader.u32();
  if (!lower.has_value()) {
    return lower.error();
  }
  const Result<std::uint32_t> upper = reader.u32();
  if (!upper.has_value()) {
    return upper.error();
  }
  if (upper.value() < lower.value()) {
    return make_error(ErrorCode::InvalidStateEncoding,
                      "a stored bound has an upper limit below its lower limit",
                      ErrorDetail{"decode.bound", std::string(field), {}, lower.value(),
                                  upper.value(), {}});
  }
  return Bound<std::uint32_t>::between(lower.value(), upper.value());
}

template <typename Measure>
void put_bound_measure(ByteWriter& writer, const Bound<Measure>& bound) {
  writer.boolean(bound.is_known());
  if (bound.is_known()) {
    writer.i64(bound.lower().value());
    writer.i64(bound.upper().value());
  }
}

template <typename Measure>
Result<Bound<Measure>> get_bound_measure(ByteReader& reader, std::string_view field) {
  const Result<bool> known = reader.boolean();
  if (!known.has_value()) {
    return known.error();
  }
  if (!known.value()) {
    return Bound<Measure>::unknown();
  }
  const Result<std::int64_t> lower = reader.i64();
  if (!lower.has_value()) {
    return lower.error();
  }
  const Result<std::int64_t> upper = reader.i64();
  if (!upper.has_value()) {
    return upper.error();
  }
  const Result<Measure> lower_value = Measure::create(lower.value());
  if (!lower_value.has_value()) {
    return lower_value.error();
  }
  const Result<Measure> upper_value = Measure::create(upper.value());
  if (!upper_value.has_value()) {
    return upper_value.error();
  }
  if (upper_value.value() < lower_value.value()) {
    return make_error(ErrorCode::InvalidStateEncoding,
                      "a stored bound has an upper limit below its lower limit",
                      ErrorDetail{"decode.bound", std::string(field), {},
                                  static_cast<std::uint64_t>(lower.value()),
                                  static_cast<std::uint64_t>(upper.value()), {}});
  }
  return Bound<Measure>::between(lower_value.value(), upper_value.value());
}

template <typename T, typename Encode>
void put_optional(ByteWriter& writer, const std::optional<T>& value, Encode encode) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    encode(writer, *value);
  }
}

template <typename T, typename Decode>
Result<std::optional<T>> get_optional(ByteReader& reader, Decode decode) {
  const Result<bool> present = reader.boolean();
  if (!present.has_value()) {
    return present.error();
  }
  if (!present.value()) {
    return std::optional<T>{};
  }
  Result<T> value = decode(reader);
  if (!value.has_value()) {
    return value.error();
  }
  return std::optional<T>(std::move(value).value());
}

template <typename Measure>
void put_optional_measure(ByteWriter& writer, const std::optional<Measure>& value) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    writer.i64(value->value());
  }
}

template <typename Measure>
Result<std::optional<Measure>> get_optional_measure(ByteReader& reader, std::string_view field) {
  const Result<bool> present = reader.boolean();
  if (!present.has_value()) {
    return present.error();
  }
  if (!present.value()) {
    return std::optional<Measure>{};
  }
  const Result<std::int64_t> raw = reader.i64();
  if (!raw.has_value()) {
    return raw.error();
  }
  const Result<Measure> value = Measure::create(raw.value());
  if (!value.has_value()) {
    return make_error(value.error().code, value.error().message + " (field " +
                                              std::string(field) + ")",
                      value.error().detail);
  }
  return std::optional<Measure>(value.value());
}

void put_policy_reference(ByteWriter& writer, const PolicyReference& reference) {
  writer.string(reference.policy.text());
  writer.u32(reference.version);
}

Result<PolicyReference> get_policy_reference(ByteReader& reader) {
  const Result<std::string> policy = reader.string(kMaxIdentityTextBytes);
  if (!policy.has_value()) {
    return policy.error();
  }
  const Result<std::uint32_t> version = reader.u32();
  if (!version.has_value()) {
    return version.error();
  }
  const Result<PolicyId> id = PolicyId::create(policy.value());
  if (!id.has_value()) {
    return id.error();
  }
  PolicyReference reference;
  reference.policy = id.value();
  reference.version = version.value();
  return reference;
}

void put_identity(ByteWriter& writer, const std::string& text) { writer.string(text); }

template <typename IdentityType>
Result<IdentityType> get_identity(ByteReader& reader) {
  const Result<std::string> text = reader.string(kMaxIdentityTextBytes);
  if (!text.has_value()) {
    return text.error();
  }
  return IdentityType::create(text.value());
}

// Reads an identity that is allowed to be absent. An empty string decodes to a
// default-constructed identity, which every consumer treats as unset; a
// non-empty string is validated exactly as strictly as a mandatory one.
template <typename IdentityType>
Result<IdentityType> get_identity_or_unset(ByteReader& reader) {
  const Result<std::string> text = reader.string(kMaxIdentityTextBytes);
  if (!text.has_value()) {
    return text.error();
  }
  if (text.value().empty()) {
    return IdentityType{};
  }
  return IdentityType::create(text.value());
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void encode_slot_interval(ByteWriter& writer, const SlotInterval& interval) {
  writer.u32(interval.begin());
  writer.u32(interval.end());
}

Result<SlotInterval> decode_slot_interval(ByteReader& reader) {
  const Result<std::uint32_t> begin = reader.u32();
  if (!begin.has_value()) {
    return begin.error();
  }
  const Result<std::uint32_t> end = reader.u32();
  if (!end.has_value()) {
    return end.error();
  }
  return SlotInterval::create(begin.value(), end.value());
}

void encode_slot_set(ByteWriter& writer, const SlotSet& set) {
  writer.u32(static_cast<std::uint32_t>(set.intervals().size()));
  for (const SlotInterval& interval : set.intervals()) {
    encode_slot_interval(writer, interval);
  }
}

Result<SlotSet> decode_slot_set(ByteReader& reader) {
  const Result<std::uint32_t> count = reader.count(kMaxSlotSetIntervals, "slot_set");
  if (!count.has_value()) {
    return count.error();
  }
  std::vector<SlotInterval> intervals;
  intervals.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    Result<SlotInterval> interval = decode_slot_interval(reader);
    if (!interval.has_value()) {
      return interval.error();
    }
    intervals.push_back(interval.value());
  }
  Result<SlotSet> set = SlotSet::from_intervals(intervals);
  if (!set.has_value()) {
    return set.error();
  }
  // The encoding is canonical: a stored set must already be in normal form, so
  // a file whose members overlap or are out of order is refused rather than
  // silently repaired into a different set.
  if (set.value().intervals() != intervals) {
    return make_error(ErrorCode::InvalidStateEncoding,
                      "a stored slot set is not in canonical normalized form",
                      ErrorDetail{"decode.slot_set", {}, {}, intervals.size(),
                                  set.value().interval_count(), {}});
  }
  return set;
}

void encode_evidence_reference(ByteWriter& writer, const EvidenceReference& reference) {
  put_enum(writer, reference.source);
  put_identity(writer, reference.evidence.text());
  writer.u32(reference.version);
  writer.i64(reference.observed_at.value());
  put_identity(writer, reference.producer.text());
}

Result<EvidenceReference> decode_evidence_reference(ByteReader& reader) {
  EvidenceReference reference;
  const Result<EvidenceSource> source =
      get_enum<EvidenceSource>(reader, 8, "evidence_source");
  if (!source.has_value()) {
    return source.error();
  }
  reference.source = source.value();
  Result<EvidenceId> evidence = get_identity<EvidenceId>(reader);
  if (!evidence.has_value()) {
    return evidence.error();
  }
  reference.evidence = evidence.value();
  const Result<std::uint32_t> version = reader.u32();
  if (!version.has_value()) {
    return version.error();
  }
  reference.version = version.value();
  const Result<std::int64_t> observed = reader.i64();
  if (!observed.has_value()) {
    return observed.error();
  }
  const Result<TimestampNs> timestamp = TimestampNs::create(observed.value());
  if (!timestamp.has_value()) {
    return timestamp.error();
  }
  reference.observed_at = timestamp.value();
  Result<SourceReference> producer = get_identity_or_unset<SourceReference>(reader);
  if (!producer.has_value()) {
    return producer.error();
  }
  reference.producer = producer.value();
  return reference;
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

namespace {

void put_location(ByteWriter& writer, const LocationReference& location) {
  const auto put = [&writer](const auto& value) {
    writer.boolean(value.has_value());
    if (value.has_value()) {
      writer.string(value->text());
    }
  };
  put(location.site);
  put(location.hall);
  put(location.row);
  put(location.position);
}

Result<LocationReference> get_location(ByteReader& reader) {
  LocationReference location;
  const auto get = [&reader](auto& slot) -> Status {
    using IdentityType = typename std::decay_t<decltype(slot)>::value_type;
    const Result<bool> present = reader.boolean();
    if (!present.has_value()) {
      return Status(present.error());
    }
    if (!present.value()) {
      return Status{};
    }
    Result<IdentityType> value = get_identity<IdentityType>(reader);
    if (!value.has_value()) {
      return Status(value.error());
    }
    slot = std::optional<IdentityType>(std::move(value).value());
    return Status{};
  };
  Status status = get(location.site);
  if (!status.has_value()) {
    return status.error();
  }
  status = get(location.hall);
  if (!status.has_value()) {
    return status.error();
  }
  status = get(location.row);
  if (!status.has_value()) {
    return status.error();
  }
  status = get(location.position);
  if (!status.has_value()) {
    return status.error();
  }
  return location;
}

void put_serviceability(ByteWriter& writer, const ServiceabilityLimits& limits) {
  writer.i64(limits.front_clearance.value());
  writer.i64(limits.rear_clearance.value());
  put_optional<std::uint32_t>(
      writer, limits.service_height_limit_unit,
      [](ByteWriter& target, std::uint32_t value) { target.u32(value); });
}

Result<ServiceabilityLimits> get_serviceability(ByteReader& reader) {
  ServiceabilityLimits limits;
  const Result<std::int64_t> front = reader.i64();
  if (!front.has_value()) {
    return front.error();
  }
  const Result<Millimetres> front_value = Millimetres::create(front.value());
  if (!front_value.has_value()) {
    return front_value.error();
  }
  limits.front_clearance = front_value.value();
  const Result<std::int64_t> rear = reader.i64();
  if (!rear.has_value()) {
    return rear.error();
  }
  const Result<Millimetres> rear_value = Millimetres::create(rear.value());
  if (!rear_value.has_value()) {
    return rear_value.error();
  }
  limits.rear_clearance = rear_value.value();
  Result<std::optional<std::uint32_t>> height = get_optional<std::uint32_t>(
      reader, [](ByteReader& source) -> Result<std::uint32_t> { return source.u32(); });
  if (!height.has_value()) {
    return height.error();
  }
  limits.service_height_limit_unit = height.value();
  return limits;
}

void put_headroom(ByteWriter& writer, const HeadroomPolicy& headroom) {
  writer.i64(headroom.power_headroom.value());
  writer.i64(headroom.cooling_headroom.value());
  writer.i64(headroom.weight_headroom.value());
  writer.u32(headroom.slot_headroom);
  writer.i64(headroom.power_derate.value());
  writer.i64(headroom.cooling_derate.value());
  writer.i64(headroom.weight_derate.value());
}

Result<HeadroomPolicy> get_headroom(ByteReader& reader) {
  HeadroomPolicy headroom;
  const Result<std::int64_t> power = reader.i64();
  if (!power.has_value()) {
    return power.error();
  }
  const Result<Watts> power_value = Watts::create(power.value());
  if (!power_value.has_value()) {
    return power_value.error();
  }
  headroom.power_headroom = power_value.value();
  const Result<std::int64_t> cooling = reader.i64();
  if (!cooling.has_value()) {
    return cooling.error();
  }
  const Result<Watts> cooling_value = Watts::create(cooling.value());
  if (!cooling_value.has_value()) {
    return cooling_value.error();
  }
  headroom.cooling_headroom = cooling_value.value();
  const Result<std::int64_t> weight = reader.i64();
  if (!weight.has_value()) {
    return weight.error();
  }
  const Result<Grams> weight_value = Grams::create(weight.value());
  if (!weight_value.has_value()) {
    return weight_value.error();
  }
  headroom.weight_headroom = weight_value.value();
  const Result<std::uint32_t> slots = reader.u32();
  if (!slots.has_value()) {
    return slots.error();
  }
  headroom.slot_headroom = slots.value();
  const auto read_bp = [&reader](BasisPoints& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<BasisPoints> value = BasisPoints::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_bp(headroom.power_derate);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bp(headroom.cooling_derate);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bp(headroom.weight_derate);
  if (!status.has_value()) {
    return status.error();
  }
  return headroom;
}

void put_policy(ByteWriter& writer, const CapacityPolicy& policy) {
  put_policy_reference(writer, policy.reference);
  put_headroom(writer, policy.headroom);
  writer.i64(policy.freshness.max_envelope_age.value());
  writer.i64(policy.freshness.max_measurement_age.value());
  writer.u32(policy.heat_per_power_ppm);
}

Result<CapacityPolicy> get_policy(ByteReader& reader) {
  CapacityPolicy policy;
  Result<PolicyReference> reference = get_policy_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  policy.reference = reference.value();
  Result<HeadroomPolicy> headroom = get_headroom(reader);
  if (!headroom.has_value()) {
    return headroom.error();
  }
  policy.headroom = headroom.value();
  const Result<std::int64_t> envelope_age = reader.i64();
  if (!envelope_age.has_value()) {
    return envelope_age.error();
  }
  const Result<DurationNs> envelope_value = DurationNs::create(envelope_age.value());
  if (!envelope_value.has_value()) {
    return envelope_value.error();
  }
  policy.freshness.max_envelope_age = envelope_value.value();
  const Result<std::int64_t> measurement_age = reader.i64();
  if (!measurement_age.has_value()) {
    return measurement_age.error();
  }
  const Result<DurationNs> measurement_value = DurationNs::create(measurement_age.value());
  if (!measurement_value.has_value()) {
    return measurement_value.error();
  }
  policy.freshness.max_measurement_age = measurement_value.value();
  const Result<std::uint32_t> ppm = reader.u32();
  if (!ppm.has_value()) {
    return ppm.error();
  }
  policy.heat_per_power_ppm = ppm.value();
  return policy;
}

void put_composition(ByteWriter& writer, const RackCompositionEvidence& composition) {
  put_identity(writer, composition.rack.text());
  writer.u64(composition.generation.value());
  writer.u32(composition.unit_count);
  encode_slot_set(writer, composition.structural_reserved_slots);
  put_serviceability(writer, composition.serviceability);
  put_location(writer, composition.location);
  encode_evidence_reference(writer, composition.reference);
}

Result<RackCompositionEvidence> get_composition(ByteReader& reader) {
  RackCompositionEvidence composition;
  Result<RackId> rack = get_identity<RackId>(reader);
  if (!rack.has_value()) {
    return rack.error();
  }
  composition.rack = rack.value();
  const Result<std::uint64_t> generation = reader.u64();
  if (!generation.has_value()) {
    return generation.error();
  }
  const Result<RackCompositionGeneration> generation_value =
      RackCompositionGeneration::create(generation.value());
  if (!generation_value.has_value()) {
    return generation_value.error();
  }
  composition.generation = generation_value.value();
  const Result<std::uint32_t> units = reader.u32();
  if (!units.has_value()) {
    return units.error();
  }
  composition.unit_count = units.value();
  Result<SlotSet> reserved = decode_slot_set(reader);
  if (!reserved.has_value()) {
    return reserved.error();
  }
  composition.structural_reserved_slots = reserved.value();
  Result<ServiceabilityLimits> serviceability = get_serviceability(reader);
  if (!serviceability.has_value()) {
    return serviceability.error();
  }
  composition.serviceability = serviceability.value();
  Result<LocationReference> location = get_location(reader);
  if (!location.has_value()) {
    return location.error();
  }
  composition.location = location.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  composition.reference = reference.value();
  return composition;
}

void put_asset(ByteWriter& writer, const AssetOccupancyEvidence& asset) {
  put_identity(writer, asset.asset.text());
  encode_slot_interval(writer, asset.span);
  put_enum(writer, asset.kind);
  put_identity(writer, asset.shared_class.text());
  writer.u32(asset.share_capacity);
  put_enum(writer, asset.presence);
  put_optional_measure(writer, asset.nameplate_draw);
  put_optional_measure(writer, asset.declared_heat_rejection);
  put_optional_measure(writer, asset.mass);
  encode_evidence_reference(writer, asset.reference);
}

Result<AssetOccupancyEvidence> get_asset(ByteReader& reader) {
  AssetOccupancyEvidence asset;
  Result<AssetId> id = get_identity<AssetId>(reader);
  if (!id.has_value()) {
    return id.error();
  }
  asset.asset = id.value();
  Result<SlotInterval> span = decode_slot_interval(reader);
  if (!span.has_value()) {
    return span.error();
  }
  asset.span = span.value();
  Result<MountSpanKind> kind = get_enum<MountSpanKind>(reader, 2, "mount_span_kind");
  if (!kind.has_value()) {
    return kind.error();
  }
  asset.kind = kind.value();
  Result<SharedMountClassId> shared_class = get_identity_or_unset<SharedMountClassId>(reader);
  if (!shared_class.has_value()) {
    return shared_class.error();
  }
  asset.shared_class = shared_class.value();
  const Result<std::uint32_t> capacity = reader.u32();
  if (!capacity.has_value()) {
    return capacity.error();
  }
  asset.share_capacity = capacity.value();
  Result<PresenceState> presence = get_enum<PresenceState>(reader, 2, "presence_state");
  if (!presence.has_value()) {
    return presence.error();
  }
  asset.presence = presence.value();
  Result<std::optional<Watts>> draw = get_optional_measure<Watts>(reader, "nameplate_draw");
  if (!draw.has_value()) {
    return draw.error();
  }
  asset.nameplate_draw = draw.value();
  Result<std::optional<Watts>> heat = get_optional_measure<Watts>(reader, "declared_heat");
  if (!heat.has_value()) {
    return heat.error();
  }
  asset.declared_heat_rejection = heat.value();
  Result<std::optional<Grams>> mass = get_optional_measure<Grams>(reader, "mass");
  if (!mass.has_value()) {
    return mass.error();
  }
  asset.mass = mass.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  asset.reference = reference.value();
  return asset;
}

void put_reservation(ByteWriter& writer, const ReservationEvidence& reservation) {
  put_identity(writer, reservation.reservation.text());
  put_enum(writer, reservation.state);
  put_optional<SlotInterval>(writer, reservation.span,
                             [](ByteWriter& target, const SlotInterval& value) {
                               encode_slot_interval(target, value);
                             });
  put_enum(writer, reservation.kind);
  put_optional_measure(writer, reservation.draw);
  put_optional_measure(writer, reservation.heat_rejection);
  put_optional_measure(writer, reservation.mass);
  encode_evidence_reference(writer, reservation.reference);
}

Result<ReservationEvidence> get_reservation(ByteReader& reader) {
  ReservationEvidence reservation;
  Result<ReservationId> id = get_identity<ReservationId>(reader);
  if (!id.has_value()) {
    return id.error();
  }
  reservation.reservation = id.value();
  Result<ReservationState> state = get_enum<ReservationState>(reader, 2, "reservation_state");
  if (!state.has_value()) {
    return state.error();
  }
  reservation.state = state.value();
  Result<std::optional<SlotInterval>> span =
      get_optional<SlotInterval>(reader, [](ByteReader& source) {
        return decode_slot_interval(source);
      });
  if (!span.has_value()) {
    return span.error();
  }
  reservation.span = span.value();
  Result<MountSpanKind> kind = get_enum<MountSpanKind>(reader, 2, "mount_span_kind");
  if (!kind.has_value()) {
    return kind.error();
  }
  reservation.kind = kind.value();
  Result<std::optional<Watts>> draw = get_optional_measure<Watts>(reader, "reservation_draw");
  if (!draw.has_value()) {
    return draw.error();
  }
  reservation.draw = draw.value();
  Result<std::optional<Watts>> heat =
      get_optional_measure<Watts>(reader, "reservation_heat");
  if (!heat.has_value()) {
    return heat.error();
  }
  reservation.heat_rejection = heat.value();
  Result<std::optional<Grams>> mass = get_optional_measure<Grams>(reader, "reservation_mass");
  if (!mass.has_value()) {
    return mass.error();
  }
  reservation.mass = mass.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  reservation.reference = reference.value();
  return reservation;
}

void put_power_evidence(ByteWriter& writer, const PowerCapacityEvidence& power) {
  writer.u32(power.feed_count);
  writer.i64(power.watts_per_feed.value());
  put_enum(writer, power.redundancy);
  writer.i64(power.derate.value());
  put_optional_measure(writer, power.measured_draw);
  put_optional_measure(writer, power.measured_at);
  encode_evidence_reference(writer, power.reference);
}

Result<PowerCapacityEvidence> get_power_evidence(ByteReader& reader) {
  PowerCapacityEvidence power;
  const Result<std::uint32_t> feeds = reader.u32();
  if (!feeds.has_value()) {
    return feeds.error();
  }
  power.feed_count = feeds.value();
  const Result<std::int64_t> watts = reader.i64();
  if (!watts.has_value()) {
    return watts.error();
  }
  const Result<Watts> watts_value = Watts::create(watts.value());
  if (!watts_value.has_value()) {
    return watts_value.error();
  }
  power.watts_per_feed = watts_value.value();
  Result<RedundancyMode> redundancy = get_enum<RedundancyMode>(reader, 2, "redundancy_mode");
  if (!redundancy.has_value()) {
    return redundancy.error();
  }
  power.redundancy = redundancy.value();
  const Result<std::int64_t> derate_raw = reader.i64();
  if (!derate_raw.has_value()) {
    return derate_raw.error();
  }
  const Result<BasisPoints> derate_value = BasisPoints::create(derate_raw.value());
  if (!derate_value.has_value()) {
    return derate_value.error();
  }
  power.derate = derate_value.value();
  Result<std::optional<Watts>> measured = get_optional_measure<Watts>(reader, "measured_draw");
  if (!measured.has_value()) {
    return measured.error();
  }
  power.measured_draw = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  power.measured_at = measured_at.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  power.reference = reference.value();
  return power;
}

void put_cooling_evidence(ByteWriter& writer, const CoolingCapacityEvidence& cooling) {
  writer.i64(cooling.nominal_heat_rejection.value());
  writer.i64(cooling.derate.value());
  put_optional_measure(writer, cooling.supply_air_temp);
  put_optional_measure(writer, cooling.measured_heat_load);
  put_optional_measure(writer, cooling.measured_at);
  encode_evidence_reference(writer, cooling.reference);
}

Result<CoolingCapacityEvidence> get_cooling_evidence(ByteReader& reader) {
  CoolingCapacityEvidence cooling;
  const Result<std::int64_t> nominal = reader.i64();
  if (!nominal.has_value()) {
    return nominal.error();
  }
  const Result<Watts> nominal_value = Watts::create(nominal.value());
  if (!nominal_value.has_value()) {
    return nominal_value.error();
  }
  cooling.nominal_heat_rejection = nominal_value.value();
  const Result<std::int64_t> derate_raw = reader.i64();
  if (!derate_raw.has_value()) {
    return derate_raw.error();
  }
  const Result<BasisPoints> derate_value = BasisPoints::create(derate_raw.value());
  if (!derate_value.has_value()) {
    return derate_value.error();
  }
  cooling.derate = derate_value.value();
  Result<std::optional<MilliCelsius>> temperature =
      get_optional_measure<MilliCelsius>(reader, "supply_air_temp");
  if (!temperature.has_value()) {
    return temperature.error();
  }
  cooling.supply_air_temp = temperature.value();
  Result<std::optional<Watts>> measured =
      get_optional_measure<Watts>(reader, "measured_heat_load");
  if (!measured.has_value()) {
    return measured.error();
  }
  cooling.measured_heat_load = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  cooling.measured_at = measured_at.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  cooling.reference = reference.value();
  return cooling;
}

void put_weight_evidence(ByteWriter& writer, const WeightCapacityEvidence& weight) {
  put_optional_measure(writer, weight.static_load_limit);
  writer.i64(weight.derate.value());
  put_optional_measure(writer, weight.per_unit_point_load_limit);
  put_optional_measure(writer, weight.measured_static_load);
  put_optional_measure(writer, weight.measured_at);
  encode_evidence_reference(writer, weight.reference);
}

Result<WeightCapacityEvidence> get_weight_evidence(ByteReader& reader) {
  WeightCapacityEvidence weight;
  Result<std::optional<Grams>> limit =
      get_optional_measure<Grams>(reader, "static_load_limit");
  if (!limit.has_value()) {
    return limit.error();
  }
  weight.static_load_limit = limit.value();
  const Result<std::int64_t> derate_raw = reader.i64();
  if (!derate_raw.has_value()) {
    return derate_raw.error();
  }
  const Result<BasisPoints> derate_value = BasisPoints::create(derate_raw.value());
  if (!derate_value.has_value()) {
    return derate_value.error();
  }
  weight.derate = derate_value.value();
  Result<std::optional<Grams>> point =
      get_optional_measure<Grams>(reader, "per_unit_point_load_limit");
  if (!point.has_value()) {
    return point.error();
  }
  weight.per_unit_point_load_limit = point.value();
  Result<std::optional<Grams>> measured =
      get_optional_measure<Grams>(reader, "measured_static_load");
  if (!measured.has_value()) {
    return measured.error();
  }
  weight.measured_static_load = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  weight.measured_at = measured_at.value();
  Result<EvidenceReference> reference = decode_evidence_reference(reader);
  if (!reference.has_value()) {
    return reference.error();
  }
  weight.reference = reference.value();
  return weight;
}

}  // namespace

void encode_inputs(ByteWriter& writer, const RackCapacityInputs& inputs) {
  put_composition(writer, inputs.composition);
  put_policy(writer, inputs.policy);
  put_optional<PowerCapacityEvidence>(writer, inputs.power, put_power_evidence);
  put_optional<CoolingCapacityEvidence>(writer, inputs.cooling, put_cooling_evidence);
  put_optional<WeightCapacityEvidence>(writer, inputs.weight, put_weight_evidence);
  writer.u32(static_cast<std::uint32_t>(inputs.assets.size()));
  for (const AssetOccupancyEvidence& asset : inputs.assets) {
    put_asset(writer, asset);
  }
  writer.u32(static_cast<std::uint32_t>(inputs.reservations.size()));
  for (const ReservationEvidence& reservation : inputs.reservations) {
    put_reservation(writer, reservation);
  }
  writer.u64(inputs.epoch.value());
  writer.i64(inputs.captured_at.value());
  put_identity(writer, inputs.actor.text());
  put_identity(writer, inputs.request.text());
}

Result<RackCapacityInputs> decode_inputs(ByteReader& reader) {
  RackCapacityInputs inputs;
  Result<RackCompositionEvidence> composition = get_composition(reader);
  if (!composition.has_value()) {
    return composition.error();
  }
  inputs.composition = composition.value();
  Result<CapacityPolicy> policy = get_policy(reader);
  if (!policy.has_value()) {
    return policy.error();
  }
  inputs.policy = policy.value();
  Result<std::optional<PowerCapacityEvidence>> power =
      get_optional<PowerCapacityEvidence>(reader, get_power_evidence);
  if (!power.has_value()) {
    return power.error();
  }
  inputs.power = power.value();
  Result<std::optional<CoolingCapacityEvidence>> cooling =
      get_optional<CoolingCapacityEvidence>(reader, get_cooling_evidence);
  if (!cooling.has_value()) {
    return cooling.error();
  }
  inputs.cooling = cooling.value();
  Result<std::optional<WeightCapacityEvidence>> weight =
      get_optional<WeightCapacityEvidence>(reader, get_weight_evidence);
  if (!weight.has_value()) {
    return weight.error();
  }
  inputs.weight = weight.value();

  const Result<std::uint32_t> asset_count = reader.count(kMaxAssetsPerRack, "assets");
  if (!asset_count.has_value()) {
    return asset_count.error();
  }
  inputs.assets.reserve(asset_count.value());
  for (std::uint32_t index = 0; index < asset_count.value(); ++index) {
    Result<AssetOccupancyEvidence> asset = get_asset(reader);
    if (!asset.has_value()) {
      return asset.error();
    }
    inputs.assets.push_back(std::move(asset).value());
  }

  const Result<std::uint32_t> reservation_count =
      reader.count(kMaxReservationsPerRack, "reservations");
  if (!reservation_count.has_value()) {
    return reservation_count.error();
  }
  inputs.reservations.reserve(reservation_count.value());
  for (std::uint32_t index = 0; index < reservation_count.value(); ++index) {
    Result<ReservationEvidence> reservation = get_reservation(reader);
    if (!reservation.has_value()) {
      return reservation.error();
    }
    inputs.reservations.push_back(std::move(reservation).value());
  }

  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.has_value()) {
    return epoch.error();
  }
  const Result<EvidenceEpoch> epoch_value = EvidenceEpoch::create(epoch.value());
  if (!epoch_value.has_value()) {
    return epoch_value.error();
  }
  inputs.epoch = epoch_value.value();
  const Result<std::int64_t> captured = reader.i64();
  if (!captured.has_value()) {
    return captured.error();
  }
  const Result<TimestampNs> captured_value = TimestampNs::create(captured.value());
  if (!captured_value.has_value()) {
    return captured_value.error();
  }
  inputs.captured_at = captured_value.value();
  Result<ActorId> actor = get_identity<ActorId>(reader);
  if (!actor.has_value()) {
    return actor.error();
  }
  inputs.actor = actor.value();
  Result<RequestId> request = get_identity<RequestId>(reader);
  if (!request.has_value()) {
    return request.error();
  }
  inputs.request = request.value();
  return inputs;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

namespace {

void put_slot_accounting(ByteWriter& writer, const SlotAccounting& slots) {
  writer.u32(slots.total_slots);
  writer.u32(slots.structural_reserved_slots);
  writer.u32(slots.occupied_slots);
  writer.u32(slots.shared_occupied_slots);
  writer.u32(slots.indeterminate_slots);
  writer.u32(slots.reserved_slots);
  writer.u32(slots.pending_slots);
  writer.u32(slots.policy_headroom_slots);
  writer.u32(slots.overcommitted_slots);
  put_bound_u32(writer, slots.free);
  encode_slot_set(writer, slots.structural_reserved_set);
  encode_slot_set(writer, slots.occupied_set);
  encode_slot_set(writer, slots.indeterminate_set);
  encode_slot_set(writer, slots.reserved_set);
  encode_slot_set(writer, slots.free_lower_set);
  encode_slot_set(writer, slots.free_upper_set);
  encode_slot_set(writer, slots.policy_headroom_set);
  writer.u32(slots.fragmentation.free_run_count);
  writer.u32(slots.fragmentation.largest_free_run_slots);
  writer.u32(slots.fragmentation.smallest_free_run_slots);
  writer.u32(slots.fragmentation.isolated_free_slots);
}

Result<SlotAccounting> get_slot_accounting(ByteReader& reader) {
  SlotAccounting slots;
  const auto read_u32 = [&reader](std::uint32_t& target) -> Status {
    const Result<std::uint32_t> value = reader.u32();
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_u32(slots.total_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.structural_reserved_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.occupied_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.shared_occupied_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.indeterminate_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.reserved_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.pending_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.policy_headroom_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.overcommitted_slots);
  if (!status.has_value()) {
    return status.error();
  }
  Result<Bound<std::uint32_t>> free = get_bound_u32(reader, "slots.free");
  if (!free.has_value()) {
    return free.error();
  }
  slots.free = free.value();
  const auto read_set = [&reader](SlotSet& target, const char* field) -> Status {
    Result<SlotSet> value = decode_slot_set(reader);
    if (!value.has_value()) {
      return Status(make_error(value.error().code, value.error().message,
                               ErrorDetail{"decode", field, {}, 0, 0, {}}));
    }
    target = std::move(value).value();
    return Status{};
  };
  status = read_set(slots.structural_reserved_set, "structural_reserved_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.occupied_set, "occupied_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.indeterminate_set, "indeterminate_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.reserved_set, "reserved_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.free_lower_set, "free_lower_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.free_upper_set, "free_upper_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_set(slots.policy_headroom_set, "policy_headroom_set");
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.fragmentation.free_run_count);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.fragmentation.largest_free_run_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.fragmentation.smallest_free_run_slots);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_u32(slots.fragmentation.isolated_free_slots);
  if (!status.has_value()) {
    return status.error();
  }
  return slots;
}

void put_power_accounting(ByteWriter& writer, const PowerAccounting& power) {
  writer.boolean(power.envelope_known);
  writer.u32(power.feed_count);
  writer.u32(power.contributing_feeds);
  put_enum(writer, power.redundancy);
  writer.i64(power.physical_derate.value());
  writer.i64(power.policy_derate.value());
  put_optional_measure(writer, power.effective_nominal);
  put_optional_measure(writer, power.physically_derated);
  put_optional_measure(writer, power.usable);
  writer.i64(power.policy_headroom.value());
  writer.i64(power.committed_known.value());
  writer.i64(power.reserved_known.value());
  writer.i64(power.pending_known.value());
  writer.u32(power.unknown_draw_assets);
  writer.u32(power.unknown_draw_reservations);
  put_optional_measure(writer, power.measured);
  put_optional_measure(writer, power.measured_at);
  put_enum(writer, power.measurement_standing);
  writer.i64(power.overcommit.value());
  put_bound_measure(writer, power.free);
}

Result<PowerAccounting> get_power_accounting(ByteReader& reader) {
  PowerAccounting power;
  const Result<bool> known = reader.boolean();
  if (!known.has_value()) {
    return known.error();
  }
  power.envelope_known = known.value();
  const Result<std::uint32_t> feeds = reader.u32();
  if (!feeds.has_value()) {
    return feeds.error();
  }
  power.feed_count = feeds.value();
  const Result<std::uint32_t> contributing = reader.u32();
  if (!contributing.has_value()) {
    return contributing.error();
  }
  power.contributing_feeds = contributing.value();
  Result<RedundancyMode> redundancy = get_enum<RedundancyMode>(reader, 2, "redundancy_mode");
  if (!redundancy.has_value()) {
    return redundancy.error();
  }
  power.redundancy = redundancy.value();
  const auto read_bp = [&reader](BasisPoints& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<BasisPoints> value = BasisPoints::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  const auto read_watts = [&reader](Watts& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<Watts> value = Watts::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_bp(power.physical_derate);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bp(power.policy_derate);
  if (!status.has_value()) {
    return status.error();
  }
  Result<std::optional<Watts>> nominal = get_optional_measure<Watts>(reader, "effective_nominal");
  if (!nominal.has_value()) {
    return nominal.error();
  }
  power.effective_nominal = nominal.value();
  Result<std::optional<Watts>> derated = get_optional_measure<Watts>(reader, "physically_derated");
  if (!derated.has_value()) {
    return derated.error();
  }
  power.physically_derated = derated.value();
  Result<std::optional<Watts>> usable = get_optional_measure<Watts>(reader, "usable");
  if (!usable.has_value()) {
    return usable.error();
  }
  power.usable = usable.value();
  status = read_watts(power.policy_headroom);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(power.committed_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(power.reserved_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(power.pending_known);
  if (!status.has_value()) {
    return status.error();
  }
  const Result<std::uint32_t> unknown_assets = reader.u32();
  if (!unknown_assets.has_value()) {
    return unknown_assets.error();
  }
  power.unknown_draw_assets = unknown_assets.value();
  const Result<std::uint32_t> unknown_reservations = reader.u32();
  if (!unknown_reservations.has_value()) {
    return unknown_reservations.error();
  }
  power.unknown_draw_reservations = unknown_reservations.value();
  Result<std::optional<Watts>> measured = get_optional_measure<Watts>(reader, "measured");
  if (!measured.has_value()) {
    return measured.error();
  }
  power.measured = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  power.measured_at = measured_at.value();
  Result<MeasurementStanding> standing =
      get_enum<MeasurementStanding>(reader, 2, "measurement_standing");
  if (!standing.has_value()) {
    return standing.error();
  }
  power.measurement_standing = standing.value();
  status = read_watts(power.overcommit);
  if (!status.has_value()) {
    return status.error();
  }
  Result<Bound<Watts>> free = get_bound_measure<Watts>(reader, "power.free");
  if (!free.has_value()) {
    return free.error();
  }
  power.free = free.value();
  return power;
}

void put_cooling_accounting(ByteWriter& writer, const CoolingAccounting& cooling) {
  writer.boolean(cooling.envelope_known);
  writer.i64(cooling.physical_derate.value());
  writer.i64(cooling.policy_derate.value());
  put_optional_measure(writer, cooling.nominal);
  put_optional_measure(writer, cooling.physically_derated);
  put_optional_measure(writer, cooling.usable);
  writer.i64(cooling.policy_headroom.value());
  writer.i64(cooling.declared_heat_known.value());
  writer.i64(cooling.derived_heat_known.value());
  writer.i64(cooling.reserved_heat_known.value());
  writer.i64(cooling.pending_heat_known.value());
  writer.u32(cooling.unknown_heat_assets);
  writer.u32(cooling.unknown_heat_reservations);
  put_optional_measure(writer, cooling.supply_air_temp);
  put_optional_measure(writer, cooling.measured);
  put_optional_measure(writer, cooling.measured_at);
  put_enum(writer, cooling.measurement_standing);
  writer.i64(cooling.overcommit.value());
  put_bound_measure(writer, cooling.free);
}

Result<CoolingAccounting> get_cooling_accounting(ByteReader& reader) {
  CoolingAccounting cooling;
  const Result<bool> known = reader.boolean();
  if (!known.has_value()) {
    return known.error();
  }
  cooling.envelope_known = known.value();
  const auto read_bp = [&reader](BasisPoints& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<BasisPoints> value = BasisPoints::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  const auto read_watts = [&reader](Watts& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<Watts> value = Watts::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_bp(cooling.physical_derate);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bp(cooling.policy_derate);
  if (!status.has_value()) {
    return status.error();
  }
  Result<std::optional<Watts>> nominal = get_optional_measure<Watts>(reader, "nominal");
  if (!nominal.has_value()) {
    return nominal.error();
  }
  cooling.nominal = nominal.value();
  Result<std::optional<Watts>> derated = get_optional_measure<Watts>(reader, "physically_derated");
  if (!derated.has_value()) {
    return derated.error();
  }
  cooling.physically_derated = derated.value();
  Result<std::optional<Watts>> usable = get_optional_measure<Watts>(reader, "usable");
  if (!usable.has_value()) {
    return usable.error();
  }
  cooling.usable = usable.value();
  status = read_watts(cooling.policy_headroom);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(cooling.declared_heat_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(cooling.derived_heat_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(cooling.reserved_heat_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_watts(cooling.pending_heat_known);
  if (!status.has_value()) {
    return status.error();
  }
  const Result<std::uint32_t> unknown_assets = reader.u32();
  if (!unknown_assets.has_value()) {
    return unknown_assets.error();
  }
  cooling.unknown_heat_assets = unknown_assets.value();
  const Result<std::uint32_t> unknown_reservations = reader.u32();
  if (!unknown_reservations.has_value()) {
    return unknown_reservations.error();
  }
  cooling.unknown_heat_reservations = unknown_reservations.value();
  Result<std::optional<MilliCelsius>> temperature =
      get_optional_measure<MilliCelsius>(reader, "supply_air_temp");
  if (!temperature.has_value()) {
    return temperature.error();
  }
  cooling.supply_air_temp = temperature.value();
  Result<std::optional<Watts>> measured = get_optional_measure<Watts>(reader, "measured");
  if (!measured.has_value()) {
    return measured.error();
  }
  cooling.measured = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  cooling.measured_at = measured_at.value();
  Result<MeasurementStanding> standing =
      get_enum<MeasurementStanding>(reader, 2, "measurement_standing");
  if (!standing.has_value()) {
    return standing.error();
  }
  cooling.measurement_standing = standing.value();
  status = read_watts(cooling.overcommit);
  if (!status.has_value()) {
    return status.error();
  }
  Result<Bound<Watts>> free = get_bound_measure<Watts>(reader, "cooling.free");
  if (!free.has_value()) {
    return free.error();
  }
  cooling.free = free.value();
  return cooling;
}

void put_weight_accounting(ByteWriter& writer, const WeightAccounting& weight) {
  writer.boolean(weight.envelope_known);
  writer.i64(weight.physical_derate.value());
  writer.i64(weight.policy_derate.value());
  put_optional_measure(writer, weight.nominal_limit);
  put_optional_measure(writer, weight.physically_derated);
  put_optional_measure(writer, weight.usable);
  writer.i64(weight.policy_headroom.value());
  writer.i64(weight.occupied_known.value());
  writer.i64(weight.reserved_known.value());
  writer.i64(weight.pending_known.value());
  writer.u32(weight.unknown_mass_assets);
  writer.u32(weight.unknown_mass_reservations);
  put_optional_measure(writer, weight.per_unit_point_limit);
  put_optional_measure(writer, weight.max_unit_load);
  writer.u32(weight.units_at_or_above_point_limit);
  put_optional_measure(writer, weight.measured);
  put_optional_measure(writer, weight.measured_at);
  put_enum(writer, weight.measurement_standing);
  writer.i64(weight.overcommit.value());
  put_bound_measure(writer, weight.free);
}

Result<WeightAccounting> get_weight_accounting(ByteReader& reader) {
  WeightAccounting weight;
  const Result<bool> known = reader.boolean();
  if (!known.has_value()) {
    return known.error();
  }
  weight.envelope_known = known.value();
  const auto read_bp = [&reader](BasisPoints& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<BasisPoints> value = BasisPoints::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  const auto read_grams = [&reader](Grams& target) -> Status {
    const Result<std::int64_t> raw = reader.i64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<Grams> value = Grams::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_bp(weight.physical_derate);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bp(weight.policy_derate);
  if (!status.has_value()) {
    return status.error();
  }
  Result<std::optional<Grams>> nominal = get_optional_measure<Grams>(reader, "nominal_limit");
  if (!nominal.has_value()) {
    return nominal.error();
  }
  weight.nominal_limit = nominal.value();
  Result<std::optional<Grams>> derated = get_optional_measure<Grams>(reader, "physically_derated");
  if (!derated.has_value()) {
    return derated.error();
  }
  weight.physically_derated = derated.value();
  Result<std::optional<Grams>> usable = get_optional_measure<Grams>(reader, "usable");
  if (!usable.has_value()) {
    return usable.error();
  }
  weight.usable = usable.value();
  status = read_grams(weight.policy_headroom);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_grams(weight.occupied_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_grams(weight.reserved_known);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_grams(weight.pending_known);
  if (!status.has_value()) {
    return status.error();
  }
  const Result<std::uint32_t> unknown_assets = reader.u32();
  if (!unknown_assets.has_value()) {
    return unknown_assets.error();
  }
  weight.unknown_mass_assets = unknown_assets.value();
  const Result<std::uint32_t> unknown_reservations = reader.u32();
  if (!unknown_reservations.has_value()) {
    return unknown_reservations.error();
  }
  weight.unknown_mass_reservations = unknown_reservations.value();
  Result<std::optional<Grams>> point =
      get_optional_measure<Grams>(reader, "per_unit_point_limit");
  if (!point.has_value()) {
    return point.error();
  }
  weight.per_unit_point_limit = point.value();
  Result<std::optional<Grams>> max_unit = get_optional_measure<Grams>(reader, "max_unit_load");
  if (!max_unit.has_value()) {
    return max_unit.error();
  }
  weight.max_unit_load = max_unit.value();
  const Result<std::uint32_t> units_at_limit = reader.u32();
  if (!units_at_limit.has_value()) {
    return units_at_limit.error();
  }
  weight.units_at_or_above_point_limit = units_at_limit.value();
  Result<std::optional<Grams>> measured = get_optional_measure<Grams>(reader, "measured");
  if (!measured.has_value()) {
    return measured.error();
  }
  weight.measured = measured.value();
  Result<std::optional<TimestampNs>> measured_at =
      get_optional_measure<TimestampNs>(reader, "measured_at");
  if (!measured_at.has_value()) {
    return measured_at.error();
  }
  weight.measured_at = measured_at.value();
  Result<MeasurementStanding> standing =
      get_enum<MeasurementStanding>(reader, 2, "measurement_standing");
  if (!standing.has_value()) {
    return standing.error();
  }
  weight.measurement_standing = standing.value();
  status = read_grams(weight.overcommit);
  if (!status.has_value()) {
    return status.error();
  }
  Result<Bound<Grams>> free = get_bound_measure<Grams>(reader, "weight.free");
  if (!free.has_value()) {
    return free.error();
  }
  weight.free = free.value();
  return weight;
}

void put_explanation(ByteWriter& writer, const Explanation& explanation) {
  writer.u16(static_cast<std::uint16_t>(explanation.code));
  put_enum(writer, explanation.dimension);
  put_enum(writer, explanation.severity);
  writer.string(explanation.subject);
  writer.string(explanation.message);
  put_optional<std::int64_t>(writer, explanation.observed,
                             [](ByteWriter& target, std::int64_t value) { target.i64(value); });
  put_optional<std::int64_t>(writer, explanation.limit,
                             [](ByteWriter& target, std::int64_t value) { target.i64(value); });
  writer.string(explanation.unit);
}

Result<Explanation> get_explanation(ByteReader& reader) {
  Explanation explanation;
  const Result<std::uint16_t> code = reader.u16();
  if (!code.has_value()) {
    return code.error();
  }
  explanation.code = static_cast<ReasonCode>(code.value());
  Result<Dimension> dimension = get_enum<Dimension>(reader, 6, "dimension");
  if (!dimension.has_value()) {
    return dimension.error();
  }
  explanation.dimension = dimension.value();
  Result<Severity> severity = get_enum<Severity>(reader, 3, "severity");
  if (!severity.has_value()) {
    return severity.error();
  }
  explanation.severity = severity.value();
  Result<std::string> subject = reader.string(kMaxIdentityTextBytes);
  if (!subject.has_value()) {
    return subject.error();
  }
  explanation.subject = subject.value();
  Result<std::string> message = reader.string(kMaxNoteBytes);
  if (!message.has_value()) {
    return message.error();
  }
  explanation.message = message.value();
  Result<std::optional<std::int64_t>> observed = get_optional<std::int64_t>(
      reader, [](ByteReader& source) -> Result<std::int64_t> { return source.i64(); });
  if (!observed.has_value()) {
    return observed.error();
  }
  explanation.observed = observed.value();
  Result<std::optional<std::int64_t>> limit = get_optional<std::int64_t>(
      reader, [](ByteReader& source) -> Result<std::int64_t> { return source.i64(); });
  if (!limit.has_value()) {
    return limit.error();
  }
  explanation.limit = limit.value();
  Result<std::string> unit = reader.string(16);
  if (!unit.has_value()) {
    return unit.error();
  }
  explanation.unit = unit.value();
  return explanation;
}

}  // namespace

void encode_snapshot(ByteWriter& writer, const RackCapacitySnapshot& snapshot) {
  put_identity(writer, snapshot.rack.text());
  writer.u64(snapshot.composition_generation.value());
  writer.u64(snapshot.capacity_generation.value());
  writer.u64(snapshot.revision.value());
  writer.u64(snapshot.evidence_epoch.value());
  writer.i64(snapshot.evaluated_at.value());
  writer.i64(snapshot.evidence_observed_at.value());
  put_enum(writer, snapshot.freshness);
  put_policy_reference(writer, snapshot.policy);
  writer.digest(snapshot.policy_digest);
  writer.digest(snapshot.inputs_digest);
  writer.u32(snapshot.unit_count);
  writer.u32(snapshot.slot_extent);
  put_slot_accounting(writer, snapshot.slots);
  put_power_accounting(writer, snapshot.power);
  put_cooling_accounting(writer, snapshot.cooling);
  put_weight_accounting(writer, snapshot.weight);
  writer.i64(snapshot.serviceability.front_clearance.value());
  writer.i64(snapshot.serviceability.rear_clearance.value());
  put_optional<std::uint32_t>(
      writer, snapshot.serviceability.service_height_limit_unit,
      [](ByteWriter& target, std::uint32_t value) { target.u32(value); });
  writer.u32(snapshot.serviceability.occupied_units_above_service_height);
  writer.u32(static_cast<std::uint32_t>(snapshot.shared_classes.size()));
  for (const SharedClassUtilization& entry : snapshot.shared_classes) {
    put_identity(writer, entry.shared_class.text());
    encode_slot_interval(writer, entry.span);
    writer.u32(entry.share_capacity);
    writer.u32(entry.used);
    writer.u32(entry.reserved);
    writer.u32(entry.pending);
    writer.u32(entry.remaining);
    writer.u32(entry.overcommitted);
  }
  writer.boolean(snapshot.unknown.power_envelope);
  writer.boolean(snapshot.unknown.cooling_envelope);
  writer.boolean(snapshot.unknown.weight_envelope);
  writer.boolean(snapshot.unknown.unknown_power_consumption);
  writer.boolean(snapshot.unknown.unknown_heat);
  writer.boolean(snapshot.unknown.unknown_mass);
  writer.boolean(snapshot.unknown.indeterminate_occupancy);
  writer.u32(static_cast<std::uint32_t>(snapshot.explanations.size()));
  for (const Explanation& explanation : snapshot.explanations) {
    put_explanation(writer, explanation);
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.pressures.size()));
  for (const ConstraintPressure& pressure : snapshot.pressures) {
    put_enum(writer, pressure.dimension);
    writer.boolean(pressure.binding);
    writer.boolean(pressure.known);
    writer.i64(pressure.utilization_bp);
  }
  put_enum(writer, snapshot.primary_binding);
  writer.boolean(snapshot.binding);
  writer.digest(snapshot.digest);
}

Result<RackCapacitySnapshot> decode_snapshot(ByteReader& reader) {
  RackCapacitySnapshot snapshot;
  Result<RackId> rack = get_identity<RackId>(reader);
  if (!rack.has_value()) {
    return rack.error();
  }
  snapshot.rack = rack.value();
  const auto read_counter = [&reader](auto& target) -> Status {
    using CounterType = std::decay_t<decltype(target)>;
    const Result<std::uint64_t> raw = reader.u64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<CounterType> value = CounterType::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_counter(snapshot.composition_generation);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_counter(snapshot.capacity_generation);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_counter(snapshot.revision);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_counter(snapshot.evidence_epoch);
  if (!status.has_value()) {
    return status.error();
  }
  const Result<std::int64_t> evaluated = reader.i64();
  if (!evaluated.has_value()) {
    return evaluated.error();
  }
  const Result<TimestampNs> evaluated_value = TimestampNs::create(evaluated.value());
  if (!evaluated_value.has_value()) {
    return evaluated_value.error();
  }
  snapshot.evaluated_at = evaluated_value.value();
  const Result<std::int64_t> observed = reader.i64();
  if (!observed.has_value()) {
    return observed.error();
  }
  const Result<TimestampNs> observed_value = TimestampNs::create(observed.value());
  if (!observed_value.has_value()) {
    return observed_value.error();
  }
  snapshot.evidence_observed_at = observed_value.value();
  Result<EvidenceFreshness> freshness = get_enum<EvidenceFreshness>(reader, 2, "freshness");
  if (!freshness.has_value()) {
    return freshness.error();
  }
  snapshot.freshness = freshness.value();
  Result<PolicyReference> policy = get_policy_reference(reader);
  if (!policy.has_value()) {
    return policy.error();
  }
  snapshot.policy = policy.value();
  Result<StateDigest> policy_digest = reader.digest();
  if (!policy_digest.has_value()) {
    return policy_digest.error();
  }
  snapshot.policy_digest = policy_digest.value();
  Result<StateDigest> inputs_digest = reader.digest();
  if (!inputs_digest.has_value()) {
    return inputs_digest.error();
  }
  snapshot.inputs_digest = inputs_digest.value();
  const Result<std::uint32_t> units = reader.u32();
  if (!units.has_value()) {
    return units.error();
  }
  snapshot.unit_count = units.value();
  const Result<std::uint32_t> extent = reader.u32();
  if (!extent.has_value()) {
    return extent.error();
  }
  snapshot.slot_extent = extent.value();
  Result<SlotAccounting> slots = get_slot_accounting(reader);
  if (!slots.has_value()) {
    return slots.error();
  }
  snapshot.slots = std::move(slots).value();
  Result<PowerAccounting> power = get_power_accounting(reader);
  if (!power.has_value()) {
    return power.error();
  }
  snapshot.power = power.value();
  Result<CoolingAccounting> cooling = get_cooling_accounting(reader);
  if (!cooling.has_value()) {
    return cooling.error();
  }
  snapshot.cooling = cooling.value();
  Result<WeightAccounting> weight = get_weight_accounting(reader);
  if (!weight.has_value()) {
    return weight.error();
  }
  snapshot.weight = weight.value();
  const Result<std::int64_t> front = reader.i64();
  if (!front.has_value()) {
    return front.error();
  }
  const Result<Millimetres> front_value = Millimetres::create(front.value());
  if (!front_value.has_value()) {
    return front_value.error();
  }
  snapshot.serviceability.front_clearance = front_value.value();
  const Result<std::int64_t> rear = reader.i64();
  if (!rear.has_value()) {
    return rear.error();
  }
  const Result<Millimetres> rear_value = Millimetres::create(rear.value());
  if (!rear_value.has_value()) {
    return rear_value.error();
  }
  snapshot.serviceability.rear_clearance = rear_value.value();
  Result<std::optional<std::uint32_t>> height = get_optional<std::uint32_t>(
      reader, [](ByteReader& source) -> Result<std::uint32_t> { return source.u32(); });
  if (!height.has_value()) {
    return height.error();
  }
  snapshot.serviceability.service_height_limit_unit = height.value();
  const Result<std::uint32_t> above = reader.u32();
  if (!above.has_value()) {
    return above.error();
  }
  snapshot.serviceability.occupied_units_above_service_height = above.value();

  const Result<std::uint32_t> shared_count =
      reader.count(kMaxSharedClassesPerRack, "shared_classes");
  if (!shared_count.has_value()) {
    return shared_count.error();
  }
  snapshot.shared_classes.reserve(shared_count.value());
  for (std::uint32_t index = 0; index < shared_count.value(); ++index) {
    SharedClassUtilization entry;
    Result<SharedMountClassId> id = get_identity<SharedMountClassId>(reader);
    if (!id.has_value()) {
      return id.error();
    }
    entry.shared_class = id.value();
    Result<SlotInterval> span = decode_slot_interval(reader);
    if (!span.has_value()) {
      return span.error();
    }
    entry.span = span.value();
    const auto read_u32 = [&reader](std::uint32_t& target) -> Status {
      const Result<std::uint32_t> value = reader.u32();
      if (!value.has_value()) {
        return Status(value.error());
      }
      target = value.value();
      return Status{};
    };
    status = read_u32(entry.share_capacity);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_u32(entry.used);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_u32(entry.reserved);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_u32(entry.pending);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_u32(entry.remaining);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_u32(entry.overcommitted);
    if (!status.has_value()) {
      return status.error();
    }
    snapshot.shared_classes.push_back(entry);
  }

  const auto read_bool = [&reader](bool& target) -> Status {
    const Result<bool> value = reader.boolean();
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  status = read_bool(snapshot.unknown.power_envelope);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.cooling_envelope);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.weight_envelope);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.unknown_power_consumption);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.unknown_heat);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.unknown_mass);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_bool(snapshot.unknown.indeterminate_occupancy);
  if (!status.has_value()) {
    return status.error();
  }

  const Result<std::uint32_t> explanation_count =
      reader.count(kMaxExplanationsPerSnapshot, "explanations");
  if (!explanation_count.has_value()) {
    return explanation_count.error();
  }
  snapshot.explanations.reserve(explanation_count.value());
  for (std::uint32_t index = 0; index < explanation_count.value(); ++index) {
    Result<Explanation> explanation = get_explanation(reader);
    if (!explanation.has_value()) {
      return explanation.error();
    }
    snapshot.explanations.push_back(std::move(explanation).value());
  }

  const Result<std::uint32_t> pressure_count =
      reader.count(kMaxConstraintPressures, "pressures");
  if (!pressure_count.has_value()) {
    return pressure_count.error();
  }
  snapshot.pressures.reserve(pressure_count.value());
  for (std::uint32_t index = 0; index < pressure_count.value(); ++index) {
    ConstraintPressure pressure;
    Result<Dimension> dimension = get_enum<Dimension>(reader, 6, "dimension");
    if (!dimension.has_value()) {
      return dimension.error();
    }
    pressure.dimension = dimension.value();
    status = read_bool(pressure.binding);
    if (!status.has_value()) {
      return status.error();
    }
    status = read_bool(pressure.known);
    if (!status.has_value()) {
      return status.error();
    }
    const Result<std::int64_t> utilization = reader.i64();
    if (!utilization.has_value()) {
      return utilization.error();
    }
    pressure.utilization_bp = utilization.value();
    snapshot.pressures.push_back(pressure);
  }
  Result<Dimension> primary = get_enum<Dimension>(reader, 6, "primary_binding");
  if (!primary.has_value()) {
    return primary.error();
  }
  snapshot.primary_binding = primary.value();
  status = read_bool(snapshot.binding);
  if (!status.has_value()) {
    return status.error();
  }
  Result<StateDigest> digest = reader.digest();
  if (!digest.has_value()) {
    return digest.error();
  }
  snapshot.digest = digest.value();
  return snapshot;
}

// ---------------------------------------------------------------------------
// Records and store state
// ---------------------------------------------------------------------------

void encode_record(ByteWriter& writer, const RackCapacityRecord& record) {
  put_identity(writer, record.rack.text());
  writer.u64(record.composition_generation.value());
  writer.u64(record.capacity_generation.value());
  writer.u64(record.revision.value());
  put_enum(writer, record.lifecycle);
  put_enum(writer, record.standing);
  encode_inputs(writer, record.inputs);
  encode_snapshot(writer, record.snapshot);
  writer.i64(record.accepted_at.value());
  writer.u64(record.last_attempt.value());
  put_identity(writer, record.last_actor.text());
}

Result<RackCapacityRecord> decode_record(ByteReader& reader) {
  RackCapacityRecord record;
  Result<RackId> rack = get_identity<RackId>(reader);
  if (!rack.has_value()) {
    return rack.error();
  }
  record.rack = rack.value();
  const auto read_counter = [&reader](auto& target) -> Status {
    using CounterType = std::decay_t<decltype(target)>;
    const Result<std::uint64_t> raw = reader.u64();
    if (!raw.has_value()) {
      return Status(raw.error());
    }
    const Result<CounterType> value = CounterType::create(raw.value());
    if (!value.has_value()) {
      return Status(value.error());
    }
    target = value.value();
    return Status{};
  };
  Status status = read_counter(record.composition_generation);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_counter(record.capacity_generation);
  if (!status.has_value()) {
    return status.error();
  }
  status = read_counter(record.revision);
  if (!status.has_value()) {
    return status.error();
  }
  Result<RackLifecycle> lifecycle = get_enum<RackLifecycle>(reader, 2, "rack_lifecycle");
  if (!lifecycle.has_value()) {
    return lifecycle.error();
  }
  record.lifecycle = lifecycle.value();
  Result<RecoveryStanding> standing =
      get_enum<RecoveryStanding>(reader, 1, "recovery_standing");
  if (!standing.has_value()) {
    return standing.error();
  }
  record.standing = standing.value();
  Result<RackCapacityInputs> inputs = decode_inputs(reader);
  if (!inputs.has_value()) {
    return inputs.error();
  }
  record.inputs = std::move(inputs).value();
  Result<RackCapacitySnapshot> snapshot = decode_snapshot(reader);
  if (!snapshot.has_value()) {
    return snapshot.error();
  }
  record.snapshot = std::move(snapshot).value();
  const Result<std::int64_t> accepted = reader.i64();
  if (!accepted.has_value()) {
    return accepted.error();
  }
  const Result<TimestampNs> accepted_value = TimestampNs::create(accepted.value());
  if (!accepted_value.has_value()) {
    return accepted_value.error();
  }
  record.accepted_at = accepted_value.value();
  const Result<std::uint64_t> attempt = reader.u64();
  if (!attempt.has_value()) {
    return attempt.error();
  }
  const Result<AttemptId> attempt_value = AttemptId::create(attempt.value());
  if (!attempt_value.has_value()) {
    return attempt_value.error();
  }
  record.last_attempt = attempt_value.value();
  Result<ActorId> actor = get_identity<ActorId>(reader);
  if (!actor.has_value()) {
    return actor.error();
  }
  record.last_actor = actor.value();
  return record;
}

void encode_idempotency(ByteWriter& writer, const IdempotencyJournal& journal) {
  writer.u32(static_cast<std::uint32_t>(journal.records.size()));
  for (const IdempotencyRecord& entry : journal.records) {
    put_identity(writer, entry.request.text());
    put_identity(writer, entry.rack.text());
    writer.u64(entry.attempt.value());
    writer.u64(entry.sequence.value());
    put_enum(writer, entry.outcome);
    writer.u64(entry.capacity_generation.value());
    writer.u64(entry.revision.value());
    writer.digest(entry.inputs_digest);
    writer.digest(entry.snapshot_digest);
    writer.i64(entry.accepted_at.value());
  }
  writer.u64(journal.evicted);
}

Result<IdempotencyJournal> decode_idempotency(ByteReader& reader) {
  IdempotencyJournal journal;
  const Result<std::uint32_t> count = reader.count(kMaxIdempotencyRecords, "idempotency");
  if (!count.has_value()) {
    return count.error();
  }
  journal.records.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    IdempotencyRecord entry;
    Result<RequestId> request = get_identity<RequestId>(reader);
    if (!request.has_value()) {
      return request.error();
    }
    entry.request = request.value();
    Result<RackId> rack = get_identity<RackId>(reader);
    if (!rack.has_value()) {
      return rack.error();
    }
    entry.rack = rack.value();
    const Result<std::uint64_t> attempt = reader.u64();
    if (!attempt.has_value()) {
      return attempt.error();
    }
    const Result<AttemptId> attempt_value = AttemptId::create(attempt.value());
    if (!attempt_value.has_value()) {
      return attempt_value.error();
    }
    entry.attempt = attempt_value.value();
    const Result<std::uint64_t> sequence = reader.u64();
    if (!sequence.has_value()) {
      return sequence.error();
    }
    const Result<StoreSequence> sequence_value = StoreSequence::create(sequence.value());
    if (!sequence_value.has_value()) {
      return sequence_value.error();
    }
    entry.sequence = sequence_value.value();
    Result<MutationOutcome> outcome = get_enum<MutationOutcome>(reader, 2, "mutation_outcome");
    if (!outcome.has_value()) {
      return outcome.error();
    }
    entry.outcome = outcome.value();
    const Result<std::uint64_t> generation = reader.u64();
    if (!generation.has_value()) {
      return generation.error();
    }
    const Result<CapacityGeneration> generation_value =
        CapacityGeneration::create(generation.value());
    if (!generation_value.has_value()) {
      return generation_value.error();
    }
    entry.capacity_generation = generation_value.value();
    const Result<std::uint64_t> revision = reader.u64();
    if (!revision.has_value()) {
      return revision.error();
    }
    const Result<SnapshotRevision> revision_value = SnapshotRevision::create(revision.value());
    if (!revision_value.has_value()) {
      return revision_value.error();
    }
    entry.revision = revision_value.value();
    Result<StateDigest> inputs_digest = reader.digest();
    if (!inputs_digest.has_value()) {
      return inputs_digest.error();
    }
    entry.inputs_digest = inputs_digest.value();
    Result<StateDigest> digest = reader.digest();
    if (!digest.has_value()) {
      return digest.error();
    }
    entry.snapshot_digest = digest.value();
    const Result<std::int64_t> accepted = reader.i64();
    if (!accepted.has_value()) {
      return accepted.error();
    }
    const Result<TimestampNs> accepted_value = TimestampNs::create(accepted.value());
    if (!accepted_value.has_value()) {
      return accepted_value.error();
    }
    entry.accepted_at = accepted_value.value();
    journal.records.push_back(entry);
  }
  const Result<std::uint64_t> evicted = reader.u64();
  if (!evicted.has_value()) {
    return evicted.error();
  }
  journal.evicted = evicted.value();
  return journal;
}

void encode_catalog_state(ByteWriter& writer, const CatalogState& state) {
  writer.string(state.incarnation);
  writer.u64(state.epoch.value());
  writer.u64(state.sequence.value());
  writer.i64(state.written_at.value());
  writer.u32(static_cast<std::uint32_t>(state.records.size()));
  for (const RackCapacityRecord& record : state.records) {
    encode_record(writer, record);
  }
  encode_idempotency(writer, state.idempotency);
}

Result<CatalogState> decode_catalog_state(ByteReader& reader) {
  CatalogState state;
  Result<std::string> incarnation = reader.string(64);
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  state.incarnation = incarnation.value();
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.has_value()) {
    return epoch.error();
  }
  const Result<StoreEpoch> epoch_value = StoreEpoch::create(epoch.value());
  if (!epoch_value.has_value()) {
    return epoch_value.error();
  }
  state.epoch = epoch_value.value();
  const Result<std::uint64_t> sequence = reader.u64();
  if (!sequence.has_value()) {
    return sequence.error();
  }
  const Result<StoreSequence> sequence_value = StoreSequence::create(sequence.value());
  if (!sequence_value.has_value()) {
    return sequence_value.error();
  }
  state.sequence = sequence_value.value();
  const Result<std::int64_t> written = reader.i64();
  if (!written.has_value()) {
    return written.error();
  }
  const Result<TimestampNs> written_value = TimestampNs::create(written.value());
  if (!written_value.has_value()) {
    return written_value.error();
  }
  state.written_at = written_value.value();
  const Result<std::uint32_t> count = reader.count(kMaxRacks, "records");
  if (!count.has_value()) {
    return count.error();
  }
  state.records.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    Result<RackCapacityRecord> record = decode_record(reader);
    if (!record.has_value()) {
      return record.error();
    }
    state.records.push_back(std::move(record).value());
  }
  Result<IdempotencyJournal> journal = decode_idempotency(reader);
  if (!journal.has_value()) {
    return journal.error();
  }
  state.idempotency = std::move(journal).value();
  return state;
}

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

namespace {

Result<std::vector<std::uint8_t>> finish(ByteWriter& writer, std::string_view subject) {
  if (!writer.ok()) {
    return make_error(ErrorCode::LimitExceeded,
                      "the canonical encoding exceeds the documented state size",
                      ErrorDetail{"encode", std::string(subject), {}, kMaxStateFileBytes,
                                  writer.size(), {}});
  }
  return writer.take();
}

}  // namespace

Result<std::vector<std::uint8_t>> encode_inputs_bytes(const RackCapacityInputs& inputs) {
  ByteWriter writer;
  encode_inputs(writer, inputs);
  return finish(writer, "inputs");
}

Result<std::vector<std::uint8_t>> encode_snapshot_bytes(const RackCapacitySnapshot& snapshot) {
  ByteWriter writer;
  encode_snapshot(writer, snapshot);
  return finish(writer, "snapshot");
}

Result<std::vector<std::uint8_t>> encode_catalog_state_bytes(const CatalogState& state) {
  ByteWriter writer;
  encode_catalog_state(writer, state);
  return finish(writer, "catalog_state");
}

Result<std::vector<std::uint8_t>> encode_policy_bytes(const CapacityPolicy& policy) {
  ByteWriter writer;
  put_policy(writer, policy);
  return finish(writer, "policy");
}

}  // namespace rackcapacity::codec
