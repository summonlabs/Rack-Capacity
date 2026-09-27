// Rack Capacity - canonical codec declarations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical encoding of a value is the byte string that its digest is
// computed over and the byte string that is written to durable state. Encoding
// is total: every field of every value is written, so two values that differ in
// any field have different encodings.

#pragma once

#include "internal.hpp"
#include "rack_capacity/capacity.hpp"
#include "rack_capacity/evidence.hpp"
#include "rack_capacity/persistence.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity::codec {

// --- primitives -------------------------------------------------------------

void encode_slot_interval(ByteWriter& writer, const SlotInterval& interval);
Result<SlotInterval> decode_slot_interval(ByteReader& reader);

void encode_slot_set(ByteWriter& writer, const SlotSet& set);
Result<SlotSet> decode_slot_set(ByteReader& reader);

void encode_evidence_reference(ByteWriter& writer, const EvidenceReference& reference);
Result<EvidenceReference> decode_evidence_reference(ByteReader& reader);

// --- evidence bundles -------------------------------------------------------

void encode_inputs(ByteWriter& writer, const RackCapacityInputs& inputs);
Result<RackCapacityInputs> decode_inputs(ByteReader& reader);

// --- snapshots --------------------------------------------------------------

void encode_snapshot(ByteWriter& writer, const RackCapacitySnapshot& snapshot);
Result<RackCapacitySnapshot> decode_snapshot(ByteReader& reader);

// --- records and store state ------------------------------------------------

void encode_record(ByteWriter& writer, const RackCapacityRecord& record);
Result<RackCapacityRecord> decode_record(ByteReader& reader);

void encode_idempotency(ByteWriter& writer, const IdempotencyJournal& journal);
Result<IdempotencyJournal> decode_idempotency(ByteReader& reader);

void encode_catalog_state(ByteWriter& writer, const CatalogState& state);
Result<CatalogState> decode_catalog_state(ByteReader& reader);

// Canonical bytes of one value, for digesting. The helpers below fail only when
// the encoder ceiling is exceeded, which cannot happen for a validated value.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_inputs_bytes(
    const RackCapacityInputs& inputs);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_snapshot_bytes(
    const RackCapacitySnapshot& snapshot);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_catalog_state_bytes(
    const CatalogState& state);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_policy_bytes(
    const CapacityPolicy& policy);

}  // namespace rackcapacity::codec
