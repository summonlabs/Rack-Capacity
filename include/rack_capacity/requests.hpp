// Rack Capacity - mutation requests, preconditions and receipts.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/digest.hpp"
#include "rack_capacity/evidence.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/measures.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// Every mutation in this library follows the same shape: an explicit
// precondition naming the exact state it was planned against, a request
// identity for bounded idempotent replay, the actor that asked for it, and the
// instant it was requested. Stale authority is refused, never merged.

// What a mutation did.
enum class MutationOutcome : std::uint8_t {
  // The mutation changed the record and was durably published.
  Applied = 0,
  // The mutation was a genuine replay of an already published request and was
  // answered from the idempotency journal without changing anything.
  Replayed = 1,
  // The mutation described a state the record already has. Nothing was
  // published and nothing was journaled.
  NoChange = 2,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view mutation_outcome_name(
    MutationOutcome outcome) noexcept;

// Registers a rack that must not already exist. The composition generation in
// the evidence is adopted as the initial authoritative generation. The actor
// and request identities travel inside the evidence bundle, so a request has
// exactly one source of truth.
struct RegisterRackRequest {
  RackCapacityInputs inputs{};
  // The instant the mutation was requested. It is the instant freshness and
  // measurement age are judged against, so the caller controls it explicitly.
  TimestampNs requested_at{};
};

// Applies a complete evidence bundle to an existing rack. The bundle replaces
// the previous one; this library never merges two bundles field by field,
// because a merge would fabricate a state no authority ever published.
struct ApplyEvidenceRequest {
  CapacityExpectation expected{};
  RackCapacityInputs inputs{};
  TimestampNs requested_at{};
};

// Retires a rack. Retiring is terminal: capacity is never quoted for a retired
// rack again and no further evidence is accepted for it.
struct RetireRackRequest {
  CapacityExpectation expected{};
  std::string reason{};
  ActorId actor{};
  RequestId request{};
  TimestampNs requested_at{};
};

// Revalidates a recovered record against the evidence it carries. A record
// recovered from durable state is not authoritative until this succeeds, so
// persisted capacity never silently becomes current again.
struct RevalidateRequest {
  CapacityExpectation expected{};
  // The instant freshness is judged against.
  TimestampNs now{};
  ActorId actor{};
  RequestId request{};
  TimestampNs requested_at{};
};

struct MutationReceipt {
  MutationOutcome outcome = MutationOutcome::Applied;
  RackId rack{};
  RackLifecycle lifecycle = RackLifecycle::Active;
  CapacityGeneration capacity_generation{};
  SnapshotRevision revision{};
  RackCompositionGeneration composition_generation{};
  EvidenceEpoch evidence_epoch{};
  StateDigest snapshot_digest{};
  AttemptId attempt{};
  // Durable publication sequence the change was committed at. Zero when
  // nothing had to be published.
  StoreSequence sequence{};
  StoreEpoch epoch{};
  TimestampNs accepted_at{};
  // Stable explanation of what the mutation concluded.
  ReasonCode reason = ReasonCode::Ok;
  std::string detail{};

  [[nodiscard]] std::string to_text() const;
};

}  // namespace rackcapacity
