// Rack Capacity - SHA-256 and canonical state digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/export.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// FIPS 180-4 SHA-256. Implemented here so that integrity checking of durable
// state and canonical capacity digests have no third-party dependency. The
// implementation is verified against the published test vectors in the test
// suite.
class RACK_CAPACITY_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() noexcept = default;

  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;
  void update(const std::vector<std::uint8_t>& data) noexcept;
  // Finalizes and returns the digest. The instance may not be updated after
  // this call without reset().
  [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish() noexcept;
  void reset() noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint8_t, kBlockBytes> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_ = 0;
  bool finalized_ = false;
};

[[nodiscard]] RACK_CAPACITY_API std::array<std::uint8_t, Sha256::kDigestBytes> sha256(
    const std::uint8_t* data, std::size_t size);

// CRC-32C (Castagnoli), used for the redundant per-frame integrity check of
// durable state. SHA-256 remains the authoritative digest; the CRC exists so
// that a truncated or bit-flipped frame is rejected before any structural
// decode is attempted.
[[nodiscard]] RACK_CAPACITY_API std::uint32_t crc32c(const std::uint8_t* data,
                                                     std::size_t size) noexcept;

// A 32-byte digest value. Comparison and ordering are byte-wise.
class StateDigest {
 public:
  StateDigest() = default;
  explicit StateDigest(std::array<std::uint8_t, Sha256::kDigestBytes> bytes) noexcept
      : bytes_(bytes) {}

  [[nodiscard]] static StateDigest of(const std::uint8_t* data, std::size_t size);
  [[nodiscard]] static StateDigest of(const std::vector<std::uint8_t>& data);
  // Domain-separated digest: hashes the domain label, a separator, then the
  // payload, so digests computed for different purposes cannot collide.
  [[nodiscard]] static StateDigest domain(std::string_view domain,
                                          const std::vector<std::uint8_t>& payload);
  [[nodiscard]] static Result<StateDigest> from_hex(std::string_view hex);

  [[nodiscard]] const std::array<std::uint8_t, Sha256::kDigestBytes>& bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] bool is_zero() const noexcept;
  // Short form for human-facing reports: the first 16 hex characters.
  [[nodiscard]] std::string to_short_hex() const;

  [[nodiscard]] bool operator==(const StateDigest& other) const noexcept {
    return bytes_ == other.bytes_;
  }
  [[nodiscard]] bool operator!=(const StateDigest& other) const noexcept {
    return bytes_ != other.bytes_;
  }
  [[nodiscard]] bool operator<(const StateDigest& other) const noexcept {
    return bytes_ < other.bytes_;
  }

 private:
  std::array<std::uint8_t, Sha256::kDigestBytes> bytes_{};
};

}  // namespace rackcapacity
