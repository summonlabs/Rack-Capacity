// Rack Capacity - error taxonomy rendering and classification.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/errors.hpp"

#include <string>

namespace rackcapacity {
namespace {

struct CodeEntry {
  ErrorCode code;
  std::string_view name;
  std::string_view explanation;
};

// The table is the single source of truth for names and explanations. A code
// with no entry is reported by its numeric value rather than by an invented
// name, so a mismatch between the enumeration and the table is visible instead
// of silent; a test asserts that every enumerator has an entry.
constexpr CodeEntry kCodes[] = {
    {ErrorCode::Ok, "ok", "The operation succeeded."},

    {ErrorCode::InvalidArgument, "invalid_argument",
     "A supplied value is not acceptable for the field it was given to."},
    {ErrorCode::MalformedIdentity, "malformed_identity",
     "An identity is empty or does not follow the documented identity form."},
    {ErrorCode::IdentityTooLong, "identity_too_long",
     "An identity exceeds the documented byte length."},
    {ErrorCode::InvalidCharacter, "invalid_character",
     "An identity contains a character outside the accepted set."},
    {ErrorCode::InvalidEnumValue, "invalid_enum_value",
     "A text value does not name one of the documented enumeration members."},
    {ErrorCode::InvalidRange, "invalid_range",
     "A numeric value is outside the documented range of its type."},
    {ErrorCode::EmptyValue, "empty_value", "A mandatory value was left empty."},
    {ErrorCode::InvalidTimestamp, "invalid_timestamp",
     "A timestamp is negative or otherwise not a valid instant."},
    {ErrorCode::InvalidText, "invalid_text",
     "Text input is not well formed for the format it claims to be."},
    {ErrorCode::ParseFailure, "parse_failure",
     "A structured document could not be parsed."},

    {ErrorCode::DuplicateRackId, "duplicate_rack_id",
     "A rack with this identity already exists in the catalog."},
    {ErrorCode::UnknownRackId, "unknown_rack_id", "No rack with this identity is registered."},
    {ErrorCode::DuplicateAssetId, "duplicate_asset_id",
     "The same asset identity appears more than once in one evidence bundle."},
    {ErrorCode::DuplicateReservationId, "duplicate_reservation_id",
     "The same reservation identity appears more than once in one evidence bundle."},
    {ErrorCode::DuplicateSlotOccupancy, "duplicate_slot_occupancy",
     "Two occupants claim the same slot interval without sharing it legally."},
    {ErrorCode::SharedClassCapacityExceeded, "shared_class_capacity_exceeded",
     "More occupants share a shared mount span than its declared capacity allows."},
    {ErrorCode::SharedClassMismatch, "shared_class_mismatch",
     "Occupants sharing a slot interval declare different shared mount classes."},
    {ErrorCode::ConflictingEvidence, "conflicting_evidence",
     "Two pieces of evidence describe mutually exclusive facts about one rack."},
    {ErrorCode::RequestIdConflict, "request_id_conflict",
     "A request identity was reused for a different request."},
    {ErrorCode::UnknownEntity, "unknown_entity", "A referenced entity does not exist."},

    {ErrorCode::StaleCompositionGeneration, "stale_composition_generation",
     "The rack composition generation supplied as a precondition is not current."},
    {ErrorCode::StaleCapacityGeneration, "stale_capacity_generation",
     "The capacity generation supplied as a precondition is not current."},
    {ErrorCode::StaleSnapshotRevision, "stale_snapshot_revision",
     "The snapshot revision supplied as a precondition is not current."},
    {ErrorCode::StaleEvidenceEpoch, "stale_evidence_epoch",
     "The evidence epoch is older than the one already applied."},
    {ErrorCode::GenerationRegression, "generation_regression",
     "A generation would move backwards, which is never allowed."},
    {ErrorCode::StaleWriterFenced, "stale_writer_fenced",
     "Writer authority was superseded; the publication is refused."},
    {ErrorCode::SequenceRegression, "sequence_regression",
     "A publication sequence would move backwards, which is never allowed."},
    {ErrorCode::NotAuthoritative, "not_authoritative",
     "The record exists but is not authoritative for capacity."},
    {ErrorCode::RevalidationRequired, "revalidation_required",
     "The record was recovered from durable state and must be revalidated first."},

    {ErrorCode::LifecycleTransitionNotAllowed, "lifecycle_transition_not_allowed",
     "The requested lifecycle transition is not permitted from the current state."},
    {ErrorCode::LifecycleMutationForbidden, "lifecycle_mutation_forbidden",
     "The rack lifecycle state forbids this mutation."},
    {ErrorCode::RackRetired, "rack_retired",
     "The rack is retired; no capacity is quoted and no evidence is accepted."},
    {ErrorCode::RackQuarantined, "rack_quarantined",
     "The rack record is quarantined after a failed revalidation."},

    {ErrorCode::SlotOutOfBounds, "slot_out_of_bounds",
     "A slot coordinate lies outside the rack extent."},
    {ErrorCode::InvalidSlotInterval, "invalid_slot_interval",
     "A slot interval is empty, inverted or malformed."},
    {ErrorCode::SlotConflict, "slot_conflict",
     "A slot interval overlaps an occupant that does not share."},
    {ErrorCode::StructuralReservationConflict, "structural_reservation_conflict",
     "A slot interval overlaps a slot the rack structure reserves."},
    {ErrorCode::SlotSetTooLarge, "slot_set_too_large",
     "A slot set exceeds the documented number of intervals."},

    {ErrorCode::MissingCompositionEvidence, "missing_composition_evidence",
     "Rack composition evidence is mandatory and was not supplied."},
    {ErrorCode::EvidenceConflict, "evidence_conflict",
     "Evidence contradicts itself and cannot be accounted for."},
    {ErrorCode::UnknownEvidenceSource, "unknown_evidence_source",
     "An evidence source does not name a documented adjacent runtime."},
    {ErrorCode::PolicyReferenceMismatch, "policy_reference_mismatch",
     "The policy reference does not match the policy the record was built under."},
    {ErrorCode::UnsupportedEvidence, "unsupported_evidence",
     "The evidence is well formed but describes something this version does not model."},
    {ErrorCode::EvidenceStale, "evidence_stale",
     "The evidence is older than the freshness policy allows."},

    {ErrorCode::IoFailure, "io_failure", "A file operation failed."},
    {ErrorCode::IntegrityCheckFailed, "integrity_check_failed",
     "A stored frame failed its integrity check."},
    {ErrorCode::UnsupportedFormatVersion, "unsupported_format_version",
     "The stored format version is not implemented by this build."},
    {ErrorCode::TruncatedState, "truncated_state",
     "Stored state ends before its declared length."},
    {ErrorCode::CorruptState, "corrupt_state",
     "Stored state is structurally invalid and was not accepted."},
    {ErrorCode::StateTooLarge, "state_too_large",
     "Stored state declares a size above the documented bound."},
    {ErrorCode::NoAuthoritativeState, "no_authoritative_state",
     "No stored generation could be verified."},
    {ErrorCode::UnsupportedCoordinateModel, "unsupported_coordinate_model",
     "Stored state declares a coordinate model this build does not implement."},
    {ErrorCode::InvalidStateEncoding, "invalid_state_encoding",
     "Stored state violates the canonical encoding rules."},
    {ErrorCode::WrongByteOrder, "wrong_byte_order",
     "Stored state was written with a different byte order."},
    {ErrorCode::StoreIncarnationMismatch, "store_incarnation_mismatch",
     "The store at this path is not the store this caller expects."},
    {ErrorCode::PermissionDenied, "permission_denied",
     "The operating system refused access to a required path."},
    {ErrorCode::NoPreviousState, "no_previous_state",
     "No retained previous publication is available."},

    {ErrorCode::LimitExceeded, "limit_exceeded",
     "A documented resource bound would be exceeded."},
    {ErrorCode::SnapshotInvalid, "snapshot_invalid",
     "A capacity snapshot failed its internal consistency check."},
    {ErrorCode::ArithmeticOverflow, "arithmetic_overflow",
     "An arithmetic operation would leave the documented range."},
    {ErrorCode::UnsupportedOperation, "unsupported_operation",
     "The operation is not supported by this version."},
    {ErrorCode::Unavailable, "unavailable",
     "The information exists but is not currently available."},
    {ErrorCode::Indeterminate, "indeterminate",
     "The question cannot be decided from the available evidence."},

    {ErrorCode::WriterLockHeld, "writer_lock_held",
     "Another live process holds writer authority over this store."},
    {ErrorCode::WriterLockInvalid, "writer_lock_invalid",
     "The writer lock file is unreadable or fails its own integrity check."},
    {ErrorCode::ReadOnlyStore, "read_only_store",
     "The store was opened read only and refuses every mutation."},
    {ErrorCode::LockIoFailure, "lock_io_failure",
     "The exclusive operating-system lock could not be taken or released."},
    {ErrorCode::WriterIdentityMismatch, "writer_identity_mismatch",
     "The writer identity is not the one that holds authority."},
    {ErrorCode::LockUnavailable, "lock_unavailable",
     "No exclusive operating-system lock facility is available for this path."},

    {ErrorCode::InvariantViolation, "invariant_violation",
     "An internal invariant was violated; the operation was abandoned."},
    {ErrorCode::ClosureFailure, "closure_failure",
     "The capacity accounting identities do not close exactly."},
};

constexpr std::size_t kCodeCount = sizeof(kCodes) / sizeof(kCodes[0]);

[[nodiscard]] const CodeEntry* find_entry(ErrorCode code) noexcept {
  for (const CodeEntry& entry : kCodes) {
    if (entry.code == code) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

ErrorCategory category_of(ErrorCode code) noexcept {
  const std::uint16_t value = static_cast<std::uint16_t>(code);
  if (value == 0) {
    return ErrorCategory::None;
  }
  if (value >= 1000) {
    return ErrorCategory::Internal;
  }
  switch (value / 100) {
    case 1:
      return ErrorCategory::Input;
    case 2:
      return ErrorCategory::Identity;
    case 3:
      return ErrorCategory::Authority;
    case 4:
      return ErrorCategory::Lifecycle;
    case 5:
      return ErrorCategory::Coordinates;
    case 6:
      return ErrorCategory::Evidence;
    case 7:
      return ErrorCategory::Persistence;
    case 8:
      return ErrorCategory::Limits;
    case 9:
      return ErrorCategory::Writer;
    default:
      return ErrorCategory::Internal;
  }
}

std::string_view code_name(ErrorCode code) noexcept {
  const CodeEntry* entry = find_entry(code);
  return entry != nullptr ? entry->name : std::string_view{"unnamed_code"};
}

std::string_view explain(ErrorCode code) noexcept {
  const CodeEntry* entry = find_entry(code);
  return entry != nullptr ? entry->explanation
                          : std::string_view{"An error code with no documented meaning."};
}

std::uint16_t code_value(ErrorCode code) noexcept { return static_cast<std::uint16_t>(code); }

std::string describe(const CapacityError& error) {
  std::string text;
  text += code_name(error.code);
  text += "(";
  text += std::to_string(code_value(error.code));
  text += ")";
  if (!error.message.empty()) {
    text += ": ";
    text += error.message;
  }
  const ErrorDetail& detail = error.detail;
  bool started = false;
  const auto append_field = [&text, &started](std::string_view key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    text += started ? ", " : " [";
    started = true;
    text += key;
    text += "=";
    text += value;
  };
  append_field("operation", detail.operation);
  append_field("subject", detail.subject);
  append_field("related", detail.related);
  if (detail.expected != 0 || detail.actual != 0) {
    append_field("expected", std::to_string(detail.expected));
    append_field("actual", std::to_string(detail.actual));
  }
  if (!detail.items.empty()) {
    text += started ? ", " : " [";
    started = true;
    text += "items=";
    for (std::size_t index = 0; index < detail.items.size(); ++index) {
      if (index != 0) {
        text += "|";
      }
      text += detail.items[index];
    }
  }
  if (started) {
    text += "]";
  }
  return text;
}

CapacityError make_error(ErrorCode code, std::string message, ErrorDetail detail) {
  CapacityError error;
  error.code = code;
  error.message = std::move(message);
  error.detail = std::move(detail);
  return error;
}

std::uint8_t exit_code_for(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return 0;
    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentity:
    case ErrorCode::IdentityTooLong:
    case ErrorCode::InvalidCharacter:
    case ErrorCode::InvalidEnumValue:
    case ErrorCode::InvalidRange:
    case ErrorCode::EmptyValue:
    case ErrorCode::InvalidTimestamp:
    case ErrorCode::InvalidText:
    case ErrorCode::ParseFailure:
    case ErrorCode::UnsupportedOperation:
      return 2;
    case ErrorCode::UnknownRackId:
    case ErrorCode::UnknownEntity:
    case ErrorCode::NoAuthoritativeState:
    case ErrorCode::NoPreviousState:
      return 3;
    case ErrorCode::DuplicateRackId:
    case ErrorCode::DuplicateAssetId:
    case ErrorCode::DuplicateReservationId:
    case ErrorCode::DuplicateSlotOccupancy:
    case ErrorCode::RequestIdConflict:
      return 4;
    case ErrorCode::StaleCompositionGeneration:
    case ErrorCode::StaleCapacityGeneration:
    case ErrorCode::StaleSnapshotRevision:
    case ErrorCode::StaleEvidenceEpoch:
    case ErrorCode::GenerationRegression:
    case ErrorCode::StaleWriterFenced:
    case ErrorCode::SequenceRegression:
    case ErrorCode::NotAuthoritative:
    case ErrorCode::RevalidationRequired:
      return 5;
    case ErrorCode::LifecycleTransitionNotAllowed:
    case ErrorCode::LifecycleMutationForbidden:
    case ErrorCode::RackRetired:
    case ErrorCode::RackQuarantined:
      return 6;
    case ErrorCode::IntegrityCheckFailed:
    case ErrorCode::UnsupportedFormatVersion:
    case ErrorCode::TruncatedState:
    case ErrorCode::CorruptState:
    case ErrorCode::StateTooLarge:
    case ErrorCode::UnsupportedCoordinateModel:
    case ErrorCode::InvalidStateEncoding:
    case ErrorCode::WrongByteOrder:
    case ErrorCode::StoreIncarnationMismatch:
      return 7;
    case ErrorCode::LimitExceeded:
    case ErrorCode::SlotSetTooLarge:
      return 8;
    case ErrorCode::UnsupportedEvidence:
    case ErrorCode::Unavailable:
    case ErrorCode::Indeterminate:
      return 9;
    case ErrorCode::IoFailure:
    case ErrorCode::PermissionDenied:
    case ErrorCode::LockIoFailure:
      return 10;
    case ErrorCode::WriterLockHeld:
    case ErrorCode::WriterLockInvalid:
    case ErrorCode::ReadOnlyStore:
    case ErrorCode::WriterIdentityMismatch:
    case ErrorCode::LockUnavailable:
      return 11;
    case ErrorCode::SlotOutOfBounds:
    case ErrorCode::InvalidSlotInterval:
    case ErrorCode::SlotConflict:
    case ErrorCode::StructuralReservationConflict:
    case ErrorCode::SharedClassCapacityExceeded:
    case ErrorCode::SharedClassMismatch:
    case ErrorCode::ConflictingEvidence:
    case ErrorCode::EvidenceConflict:
    case ErrorCode::MissingCompositionEvidence:
    case ErrorCode::PolicyReferenceMismatch:
    case ErrorCode::EvidenceStale:
    case ErrorCode::UnknownEvidenceSource:
      return 12;
    case ErrorCode::SnapshotInvalid:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::InvariantViolation:
    case ErrorCode::ClosureFailure:
      return 13;
    default:
      return 1;
  }
}

}  // namespace rackcapacity
