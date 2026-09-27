// Rack Capacity - operating system file, lock, process and timing primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This is the only place in the library that talks to the operating system. It
// exposes exactly the primitives the capacity store needs and nothing more, so
// the durability and authority rules live in one reviewable layer.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rack_capacity/result.hpp"

namespace rackcapacity::platform {

enum class PathKind : std::uint8_t {
  Missing = 0,
  // A regular file that is not a link or a reparse point.
  RegularFile = 1,
  Directory = 2,
  // A symbolic link, junction or other reparse point. The store never follows
  // one, because a redirectable store path defeats every authority check the
  // store makes about where its bytes live.
  Link = 3,
  Other = 4,
};

struct DirEntry {
  std::string name;
  bool is_file = false;
  bool is_directory = false;
  bool is_link = false;
};

[[nodiscard]] Result<PathKind> path_kind(const std::filesystem::path& path);
[[nodiscard]] Status ensure_directory(const std::filesystem::path& path);
// Reads a whole file, refusing anything longer than `max_bytes` before it
// allocates the buffer.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                          std::uint64_t max_bytes);
// Writes a whole file and flushes it to stable storage before returning.
[[nodiscard]] Status write_file_flushed(const std::filesystem::path& path,
                                        const std::vector<std::uint8_t>& bytes);
// Renames over an existing file atomically with respect to other readers.
[[nodiscard]] Status rename_replace(const std::filesystem::path& from,
                                    const std::filesystem::path& to);
[[nodiscard]] Status remove_file(const std::filesystem::path& path) noexcept;
// Makes a directory entry durable. A no-op on platforms where the rename that
// precedes it is already durable.
[[nodiscard]] Status sync_directory(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<DirEntry>> list_directory(const std::filesystem::path& path);

[[nodiscard]] std::uint64_t current_process_id() noexcept;
// A value that changes when a process identifier is reused by a different
// process, so a stale lock that names a recycled identifier is detectable.
[[nodiscard]] Result<std::uint64_t> process_start_marker(std::uint64_t pid);
[[nodiscard]] bool process_is_alive(std::uint64_t pid) noexcept;

// An unpredictable lowercase hex string. Used as a store incarnation marker,
// which detects that a store was swapped for an unrelated one. It is not a
// secret and is never used as a credential.
[[nodiscard]] std::string random_hex(std::size_t bytes);

// Additional bytes beyond the file length needed to read a small auxiliary file
// such as a lock record.
inline constexpr std::uint64_t kMaxAuxiliaryFileBytes = 64u * 1024u;

// Byte offset of the range the exclusive lock covers.
//
// The lock is deliberately taken far beyond the metadata the lock file carries,
// because an operating-system byte-range lock also blocks reads of the locked
// bytes by any other handle, including a second handle in the same process.
// Locking a distant byte keeps the writer identity in the first bytes readable
// by inspection tools and by the lock owner itself, which is what makes
// "who holds this store" answerable without acquiring authority.
inline constexpr std::uint64_t kLockRangeOffset = 4096u;

// An exclusive advisory lock on a lock file, held for as long as the object
// lives. The operating system releases it when the process exits, including
// after a crash, which is what makes a dead writer unable to keep authority.
class ExclusiveFileLock {
 public:
  ExclusiveFileLock() = default;
  ~ExclusiveFileLock();
  ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

  // Takes the lock without blocking. Returns LockIoFailure when the failure is
  // not a plain contention, and WriterLockHeld when another process holds it.
  [[nodiscard]] static Result<ExclusiveFileLock> acquire(const std::filesystem::path& path,
                                                        bool create_if_missing);
  [[nodiscard]] bool held() const noexcept { return held_; }
  [[nodiscard]] Status release() noexcept;
  // Reads the current content of the locked file. Reading through the same
  // handle that holds the lock avoids a second path resolution.
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_locked(std::uint64_t max_bytes) const;
  // Truncates and rewrites the locked file, flushing before returning.
  [[nodiscard]] Status write_locked(const std::vector<std::uint8_t>& bytes) const;

 private:
  void close() noexcept;

  std::filesystem::path path_{};
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
  bool held_ = false;
};

}  // namespace rackcapacity::platform
