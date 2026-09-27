// Rack Capacity - strongly typed identities, generations, revisions and epochs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/export.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Text identities
// ---------------------------------------------------------------------------
//
// Identity text is validated at the construction boundary and never
// normalized. The accepted character set is deliberately narrow: printable
// ASCII with no path separators, no shell metacharacters, no colon (which every
// canonical text form in this library uses as a field separator) and no
// traversal sequences. Rejecting is always preferred over repairing, because a
// silently rewritten identity is a different identity.

namespace detail {

[[nodiscard]] RACK_CAPACITY_API Status validate_identity_text(std::string_view kind,
                                                              std::string_view text);

}  // namespace detail

// One opaque, validated identity string with a distinct C++ type per identity
// family. Two identity families never convert into one another.
template <typename Tag>
class Identity {
 public:
  using tag = Tag;

  Identity() = default;

  [[nodiscard]] static Result<Identity> create(std::string_view text) {
    const Status status = detail::validate_identity_text(Tag::kKind, text);
    if (!status.has_value()) {
      return status.error();
    }
    return Identity(std::string(text));
  }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  [[nodiscard]] bool operator==(const Identity& other) const noexcept {
    return text_ == other.text_;
  }
  [[nodiscard]] bool operator!=(const Identity& other) const noexcept {
    return text_ != other.text_;
  }
  [[nodiscard]] bool operator<(const Identity& other) const noexcept {
    return text_ < other.text_;
  }

 private:
  explicit Identity(std::string text) : text_(std::move(text)) {}
  std::string text_{};
};

struct RackIdTag {
  static constexpr std::string_view kKind = "rack";
};
struct SiteIdTag {
  static constexpr std::string_view kKind = "site";
};
struct HallIdTag {
  static constexpr std::string_view kKind = "hall";
};
struct RowIdTag {
  static constexpr std::string_view kKind = "row";
};
struct RackPositionIdTag {
  static constexpr std::string_view kKind = "position";
};
struct AssetIdTag {
  static constexpr std::string_view kKind = "asset";
};
struct ReservationIdTag {
  static constexpr std::string_view kKind = "reservation";
};
struct PolicyIdTag {
  static constexpr std::string_view kKind = "policy";
};
struct EvidenceIdTag {
  static constexpr std::string_view kKind = "evidence";
};
struct ActorIdTag {
  static constexpr std::string_view kKind = "actor";
};
struct WriterIdTag {
  static constexpr std::string_view kKind = "writer";
};
struct RequestIdTag {
  static constexpr std::string_view kKind = "request";
};
struct SourceReferenceTag {
  static constexpr std::string_view kKind = "source";
};

using RackId = Identity<RackIdTag>;
using SiteId = Identity<SiteIdTag>;
using HallId = Identity<HallIdTag>;
using RowId = Identity<RowIdTag>;
using RackPositionId = Identity<RackPositionIdTag>;
using AssetId = Identity<AssetIdTag>;
using ReservationId = Identity<ReservationIdTag>;
using PolicyId = Identity<PolicyIdTag>;
using EvidenceId = Identity<EvidenceIdTag>;
using ActorId = Identity<ActorIdTag>;
using WriterId = Identity<WriterIdTag>;
using RequestId = Identity<RequestIdTag>;
using SourceReference = Identity<SourceReferenceTag>;

// ---------------------------------------------------------------------------
// Monotonic counters
// ---------------------------------------------------------------------------
//
// Generations are never reused and never reset. Each counter family is a
// distinct type: a CapacityGeneration cannot be passed where an
// EvidenceEpoch is expected, and an increment that would overflow the
// documented bound is refused instead of wrapping.

template <typename Tag>
class Counter {
 public:
  using tag = Tag;
  using rep = std::uint64_t;

  constexpr Counter() noexcept = default;

  [[nodiscard]] static Result<Counter> create(std::uint64_t value) {
    if (value > Tag::kMax) {
      return make_error(ErrorCode::InvalidRange,
                        "counter value exceeds the documented maximum",
                        ErrorDetail{"counter.create", std::string(Tag::kKind), {}, Tag::kMax,
                                    value, {}});
    }
    return Counter(value);
  }

  // Adopts a value the caller has already validated. Values above the
  // documented maximum are clamped, so an out-of-range counter cannot exist.
  [[nodiscard]] static constexpr Counter trusted(std::uint64_t value) noexcept {
    return Counter(value > Tag::kMax ? Tag::kMax : value);
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_initial() const noexcept { return value_ == 0; }
  [[nodiscard]] static constexpr std::uint64_t max_value() noexcept { return Tag::kMax; }

  // The next value in the sequence. Refuses to wrap.
  [[nodiscard]] Result<Counter> next() const {
    if (value_ >= Tag::kMax) {
      return make_error(ErrorCode::LimitExceeded,
                        "counter cannot advance beyond its documented maximum",
                        ErrorDetail{"counter.next", std::string(Tag::kKind), {}, Tag::kMax,
                                    value_, {}});
    }
    return Counter(value_ + 1);
  }

  [[nodiscard]] constexpr bool operator==(const Counter& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const Counter& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Counter& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const Counter& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const Counter& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const Counter& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  constexpr explicit Counter(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

struct RackCompositionGenerationTag {
  static constexpr std::string_view kKind = "rack_composition_generation";
  static constexpr std::uint64_t kMax = 0xffffffffull;
};
struct CapacityGenerationTag {
  static constexpr std::string_view kKind = "capacity_generation";
  static constexpr std::uint64_t kMax = 0xffffffffull;
};
struct SnapshotRevisionTag {
  static constexpr std::string_view kKind = "snapshot_revision";
  static constexpr std::uint64_t kMax = 0xffffffffull;
};
struct EvidenceEpochTag {
  static constexpr std::string_view kKind = "evidence_epoch";
  static constexpr std::uint64_t kMax = 0xffffffffull;
};
struct StoreEpochTag {
  static constexpr std::string_view kKind = "store_epoch";
  static constexpr std::uint64_t kMax = 0xffffffffull;
};
struct StoreSequenceTag {
  static constexpr std::string_view kKind = "store_sequence";
  static constexpr std::uint64_t kMax = 0xffffffffffffffffull;
};
struct AttemptIdTag {
  static constexpr std::string_view kKind = "attempt_id";
  static constexpr std::uint64_t kMax = 0xffffffffffffffffull;
};

// Generation of the rack composition this capacity record was derived from.
// It is supplied by rack composition evidence and is never invented here.
using RackCompositionGeneration = Counter<RackCompositionGenerationTag>;
// Generation of the capacity state authored by this library for one rack.
// Monotonic across composition generations and never reset.
using CapacityGeneration = Counter<CapacityGenerationTag>;
// Revision of one evaluated capacity snapshot.
using SnapshotRevision = Counter<SnapshotRevisionTag>;
// Epoch of the evidence bundle a snapshot was evaluated from.
using EvidenceEpoch = Counter<EvidenceEpochTag>;
// Epoch of writer authority over a durable capacity store.
using StoreEpoch = Counter<StoreEpochTag>;
// Publication sequence of a durable capacity store.
using StoreSequence = Counter<StoreSequenceTag>;
// Identity of one durable publication attempt, used for explicit idempotency.
using AttemptId = Counter<AttemptIdTag>;

// ---------------------------------------------------------------------------
// Policy and evidence references
// ---------------------------------------------------------------------------

// Reference to one exact version of an externally authoritative policy. Rack
// Capacity consumes policy; it never authors or resolves it.
struct PolicyReference {
  PolicyId policy{};
  std::uint32_t version = 0;

  [[nodiscard]] bool is_set() const noexcept { return !policy.empty() && version > 0; }
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] bool operator==(const PolicyReference& other) const noexcept {
    return policy == other.policy && version == other.version;
  }
  [[nodiscard]] bool operator!=(const PolicyReference& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] bool operator<(const PolicyReference& other) const noexcept {
    return policy != other.policy ? policy < other.policy : version < other.version;
  }
};

// External physical location of a rack. Every component is optional because
// location authority belongs to another runtime and may not have been resolved
// yet; an unresolved component is reported as absent, never invented.
struct LocationReference {
  std::optional<SiteId> site{};
  std::optional<HallId> hall{};
  std::optional<RowId> row{};
  std::optional<RackPositionId> position{};

  [[nodiscard]] bool has_any() const noexcept {
    return site.has_value() || hall.has_value() || row.has_value() || position.has_value();
  }
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] bool operator==(const LocationReference& other) const noexcept {
    return site == other.site && hall == other.hall && row == other.row &&
           position == other.position;
  }
  [[nodiscard]] bool operator!=(const LocationReference& other) const noexcept {
    return !(*this == other);
  }
  [[nodiscard]] bool operator<(const LocationReference& other) const noexcept;
};

}  // namespace rackcapacity
