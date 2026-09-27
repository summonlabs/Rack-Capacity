// Rack Capacity - documented resource bounds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>

#include "rack_capacity/export.hpp"
#include "rack_capacity/version.hpp"

namespace rackcapacity {

// Every bound below is enforced before allocation or mutation, on both the
// in-memory path and the decoding path, so a hostile state file cannot drive
// unbounded growth. Changing a bound changes what a valid capacity catalog may
// hold; the values are part of the documented contract and are asserted by
// tests.

// Maximum rack units in one rack. 1024 units is far beyond any physical rack
// enclosure; the bound exists to keep coordinate arithmetic and payload sizes
// finite. It matches the bound published by the Rack Registry runtime so that
// every composition fact Rack Registry can express is expressible here.
inline constexpr std::uint32_t kMaxRackUnits = 1024;

// Maximum mount slot index (exclusive upper bound) for one rack. Slot numbers
// are 1-based and half-open ranges must satisfy end <= kMaxMountSlotExclusive.
inline constexpr std::uint32_t kMaxMountSlotExclusive =
    kMaxRackUnits * kMountSlotsPerRackUnit + 1;

// Maximum number of intervals retained in one normalized slot set. A capacity
// record can never need more intervals than the rack has slots, so the bound is
// generous while still finite.
inline constexpr std::size_t kMaxSlotSetIntervals = 4096;

// Maximum racks held by one catalog.
inline constexpr std::size_t kMaxRacks = 4096;

// Maximum occupancy entries carried by one rack's evidence bundle.
inline constexpr std::size_t kMaxAssetsPerRack = 4096;

// Maximum reservation entries carried by one rack's evidence bundle.
inline constexpr std::size_t kMaxReservationsPerRack = 4096;

// Maximum distinct shared mount classes referenced by one rack.
inline constexpr std::size_t kMaxSharedClassesPerRack = 256;

// Maximum explanations retained on one capacity snapshot.
inline constexpr std::size_t kMaxExplanationsPerSnapshot = 1024;

// Maximum constraint pressure entries retained on one capacity snapshot. There
// is one entry per capacity dimension, so this is a small fixed bound.
inline constexpr std::size_t kMaxConstraintPressures = 8;

// Maximum number of recorded idempotency receipts retained by one catalog. The
// oldest receipt is evicted first; the eviction count is itself preserved so
// that operators can see that replay coverage is bounded.
inline constexpr std::size_t kMaxIdempotencyRecords = 256;

// Maximum rejection explanations retained by one catalog for inspection.
inline constexpr std::size_t kMaxRejectionJournalEntries = 512;

// Text bounds, in bytes, of externally supplied strings.
inline constexpr std::size_t kMaxIdentityTextBytes = 192;
inline constexpr std::size_t kMaxOpaqueReferenceBytes = 160;
inline constexpr std::size_t kMaxActorBytes = 64;
inline constexpr std::size_t kMaxRequestIdBytes = 64;
inline constexpr std::size_t kMaxLabelBytes = 128;
inline constexpr std::size_t kMaxNoteBytes = 160;

// Maximum byte length of one encoded state file that a reader will accept. The
// declared payload length is checked against this bound before any buffer is
// reserved.
inline constexpr std::uint64_t kMaxStateFileBytes = 256ull * 1024ull * 1024ull;

// Maximum number of bytes accepted for one human-authorable specification
// document.
inline constexpr std::uint64_t kMaxSpecificationBytes = 8ull * 1024ull * 1024ull;

// Maximum accepted element count in one decoded collection header. Decoding
// rejects a declared count above the matching bound before growing a container.
inline constexpr std::uint32_t kMaxDecodedCollectionCount = 1u << 20;

// Maximum number of lines accepted in one specification document.
inline constexpr std::size_t kMaxSpecificationLines = 200000;

// ---------------------------------------------------------------------------
// Exact arithmetic envelope
// ---------------------------------------------------------------------------
//
// Capacity accounting is exact integer arithmetic. The bounds below are the
// documented envelope inside which every operation this library performs is
// provably free of overflow in 64-bit signed arithmetic. They are chosen far
// above any physical data center value: a single rack cannot plausibly present
// a terawatt of input power, a teragram of static load, or a kilometre of
// clearance, and no derating factor exceeds 10000 basis points.
//
//   derate(v, bp) = v * (10000 - bp) / 10000
//                 <= 10^12 * 10^4 = 10^16  <  2^63 - 1
//
// so the widest intermediate product this library forms stays well inside the
// signed 64-bit range. Every operation is nevertheless performed through the
// checked helpers in measures.hpp, which refuse to produce an out-of-range
// result instead of wrapping.

inline constexpr std::int64_t kMaxWatts = 1'000'000'000'000;      // 1 TW
inline constexpr std::int64_t kMaxGrams = 1'000'000'000'000;      // 10^9 kg
inline constexpr std::int64_t kMaxMillimetres = 1'000'000'000;    // 1000 km
inline constexpr std::int64_t kMaxMilliCelsius = 1'000'000;       // 1000 C
inline constexpr std::int64_t kMinMilliCelsius = -1'000'000;
inline constexpr std::int64_t kMaxBasisPoints = 10'000;           // 100 %

// Maximum number of feeds on one rack power envelope, and the maximum
// redundancy-aware feed multiplier this library will compute.
inline constexpr std::uint32_t kMaxFeeds = 8;

// Maximum number of assets that can co-occupy one shared mount span.
inline constexpr std::uint32_t kMaxShareCapacity = 4096;

}  // namespace rackcapacity
