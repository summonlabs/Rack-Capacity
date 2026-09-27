// Rack Capacity - canonical byte encoding primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every persisted and digested byte of this library is produced by these two
// types. The encoding is fixed width, little endian, length prefixed and free
// of padding, so a given value has exactly one representation and two
// independently produced encodings of equal values are byte identical. That is
// what makes the canonical snapshot digest meaningful.
//
// The writer enforces a hard ceiling on its own size, so a caller that skipped
// validation cannot make the encoder allocate without bound. The reader refuses
// to produce a value from a truncated, over-long or non-canonical encoding.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_capacity/digest.hpp"
#include "rack_capacity/limits.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity::codec {

class ByteWriter {
 public:
  explicit ByteWriter(std::uint64_t limit = kMaxStateFileBytes) noexcept : limit_(limit) {}

  void u8(std::uint8_t value) { raw(&value, 1); }
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void raw(const void* data, std::size_t size);
  void string(std::string_view text);
  void digest(const StateDigest& value) { raw(value.bytes().data(), value.bytes().size()); }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(data_); }

  // Records that the writer is no longer usable, without throwing.
  void fail() noexcept { ok_ = false; }

 private:
  std::vector<std::uint8_t> data_{};
  std::uint64_t limit_ = kMaxStateFileBytes;
  bool ok_ = true;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::string> string(std::size_t max_bytes);
  [[nodiscard]] Result<StateDigest> digest();
  [[nodiscard]] Result<std::vector<std::uint8_t>> raw(std::size_t size);
  // Reads a collection count and refuses anything above `max_count` before the
  // caller can use it to size a container.
  [[nodiscard]] Result<std::uint32_t> count(std::size_t max_count, std::string_view field);

  [[nodiscard]] bool at_end() const noexcept { return position_ == size_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // Refuses any trailing byte. Called at the end of every decode.
  [[nodiscard]] Status require_exhausted(std::string_view subject) const;

 private:
  [[nodiscard]] Status need(std::size_t bytes) const;

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t position_ = 0;
};

}  // namespace rackcapacity::codec
