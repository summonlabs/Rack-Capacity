// Rack Capacity - stable machine-readable error taxonomy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/export.hpp"

namespace rackcapacity {

// Error codes are a stable, machine-readable contract. Numeric values are
// grouped by fault domain and must never be reused for a different meaning.
// New codes are appended inside the matching group.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // 1xx - input rejected at a construction or validation boundary.
  InvalidArgument = 100,
  MalformedIdentity = 101,
  IdentityTooLong = 102,
  InvalidCharacter = 103,
  InvalidEnumValue = 104,
  InvalidRange = 105,
  EmptyValue = 106,
  InvalidTimestamp = 107,
  InvalidText = 108,
  ParseFailure = 109,

  // 2xx - identity uniqueness, existence and evidence well-formedness.
  DuplicateRackId = 200,
  UnknownRackId = 201,
  DuplicateAssetId = 202,
  DuplicateReservationId = 203,
  DuplicateSlotOccupancy = 204,
  SharedClassCapacityExceeded = 205,
  SharedClassMismatch = 206,
  ConflictingEvidence = 207,
  RequestIdConflict = 208,
  UnknownEntity = 209,

  // 3xx - stale authority: generations, revisions, epochs and writer fencing.
  StaleCompositionGeneration = 300,
  StaleCapacityGeneration = 301,
  StaleSnapshotRevision = 302,
  StaleEvidenceEpoch = 303,
  GenerationRegression = 304,
  StaleWriterFenced = 305,
  SequenceRegression = 306,
  NotAuthoritative = 307,
  RevalidationRequired = 308,

  // 4xx - lifecycle gates.
  LifecycleTransitionNotAllowed = 400,
  LifecycleMutationForbidden = 401,
  RackRetired = 402,
  RackQuarantined = 403,

  // 5xx - slot coordinates, intervals and structural reservation.
  SlotOutOfBounds = 500,
  InvalidSlotInterval = 501,
  SlotConflict = 502,
  StructuralReservationConflict = 503,
  SlotSetTooLarge = 504,

  // 6xx - evidence, policy and derivation.
  MissingCompositionEvidence = 600,
  EvidenceConflict = 601,
  UnknownEvidenceSource = 602,
  PolicyReferenceMismatch = 603,
  UnsupportedEvidence = 604,
  EvidenceStale = 605,

  // 7xx - persistence, encoding and integrity.
  IoFailure = 700,
  IntegrityCheckFailed = 701,
  UnsupportedFormatVersion = 702,
  TruncatedState = 703,
  CorruptState = 704,
  StateTooLarge = 705,
  NoAuthoritativeState = 706,
  UnsupportedCoordinateModel = 707,
  InvalidStateEncoding = 708,
  WrongByteOrder = 709,
  StoreIncarnationMismatch = 710,
  PermissionDenied = 711,
  NoPreviousState = 712,

  // 8xx - bounds, arithmetic and capability.
  LimitExceeded = 800,
  SnapshotInvalid = 801,
  ArithmeticOverflow = 802,
  UnsupportedOperation = 803,
  Unavailable = 804,
  Indeterminate = 805,

  // 9xx - writer ownership, OS-level locking and fencing.
  WriterLockHeld = 900,
  WriterLockInvalid = 901,
  ReadOnlyStore = 902,
  LockIoFailure = 903,
  WriterIdentityMismatch = 904,
  LockUnavailable = 905,

  // 10xx - internal invariants.
  InvariantViolation = 1000,
  ClosureFailure = 1001,
};

// Broad category of a code, so callers can branch on fault class without
// enumerating every code.
enum class ErrorCategory : std::uint8_t {
  None = 0,
  Input = 1,
  Identity = 2,
  Authority = 3,
  Lifecycle = 4,
  Coordinates = 5,
  Evidence = 6,
  Persistence = 7,
  Limits = 8,
  Writer = 9,
  Internal = 10,
};

// Machine-readable structured context for a rejection. Fields that do not
// apply are empty. `items` carries the ordered, deduplicated list relevant to
// the rejection (for example the conflicting slot intervals).
struct ErrorDetail {
  std::string operation;
  std::string subject;
  std::string related;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  std::vector<std::string> items;
};

struct CapacityError {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  ErrorDetail detail;
};

[[nodiscard]] RACK_CAPACITY_API ErrorCategory category_of(ErrorCode code) noexcept;

// Stable identifier for a code, for example "stale_capacity_generation".
[[nodiscard]] RACK_CAPACITY_API std::string_view code_name(ErrorCode code) noexcept;

// One sentence explaining what the code means, independent of any particular
// occurrence.
[[nodiscard]] RACK_CAPACITY_API std::string_view explain(ErrorCode code) noexcept;

[[nodiscard]] RACK_CAPACITY_API std::uint16_t code_value(ErrorCode code) noexcept;

// Renders "code_name(code_value): message" plus any structured context.
[[nodiscard]] RACK_CAPACITY_API std::string describe(const CapacityError& error);

[[nodiscard]] RACK_CAPACITY_API CapacityError make_error(ErrorCode code,
                                                         std::string message,
                                                         ErrorDetail detail = {});

// Stable process exit code for a rejection. The mapping is part of the command
// line contract: 0 means success, 1 is reserved for an unhandled failure, and
// every other value identifies one fault class so that scripts can branch
// without parsing text.
[[nodiscard]] RACK_CAPACITY_API std::uint8_t exit_code_for(ErrorCode code) noexcept;

}  // namespace rackcapacity
