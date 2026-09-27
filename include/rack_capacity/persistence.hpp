// Rack Capacity - durable store: transactional publication and writer fencing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/digest.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/requests.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// Named boundaries between the durable steps of a publication. A crash at any
// of them must leave either the previous or the new generation authoritative,
// never a mixture. The publication sequence is: verify writer authority,
// stage the new generation (create, write, flush), read it back and verify it,
// publish the superseded generation as the retained previous one, rename the
// staged generation over the current one, then retire any residue. Each stage
// below names the instant immediately after that step completed.
enum class WriteStage : std::uint8_t {
  // Writer authority was re-verified and no file work has happened yet.
  AfterLockAcquired = 0,
  // The staged generation exists, is fully written and is flushed to storage.
  AfterTempCreated = 1,
  // The staged generation was read back and verified byte for byte.
  AfterTempWritten = 2,
  // Everything is staged; the retained previous generation is about to be
  // published. This is the last instant at which the running generation is
  // still authoritative and the new one is not.
  AfterTempSynced = 3,
  // The superseded generation has been renamed to the retained previous path.
  AfterPreviousPublished = 4,
  // The publication rename is about to happen. This is the commit point.
  BeforePublishRename = 5,
  // The new generation has been renamed over the current path.
  AfterPublishRename = 6,
  // Residue from the publication has been retired.
  AfterTempRetired = 7,
};

inline constexpr std::size_t kWriteStageCount = 8;

[[nodiscard]] RACK_CAPACITY_API std::string_view write_stage_name(WriteStage stage) noexcept;
[[nodiscard]] RACK_CAPACITY_API Result<WriteStage> parse_write_stage(std::string_view text);

// Failure-injection seam. It is empty in every normal configuration: the store
// then performs no indirect call at all along the publication path. Tests
// install a hook that throws or terminates the process to simulate a crash at
// exactly one durable step boundary.
//
// A hook runs while the store's writer mutex and the exclusive operating-system
// lock on the lock file are held. It must not call back into the store or into
// any catalog; its only supported behaviours are to throw and to terminate the
// process.
using FaultHook = std::function<void(WriteStage)>;

struct StoreOptions {
  // Path of the state file. The store also owns "<path>.prev" and
  // "<path>.lock" and creates "<path>.tmp-<pid>-<counter>" while publishing.
  std::filesystem::path path{};

  // Identity of the writer that will own this store. Required for a writable
  // store and recorded in the writer lock file.
  WriterId writer_id{};

  // Identity of the component that produces state through this store. Recorded
  // verbatim in every published generation and in the state digest.
  std::optional<SourceReference> producer{};

  // When true the store takes no writer authority: it loads whatever
  // generation is authoritative and refuses every mutation with ReadOnlyStore.
  // Several read-only stores may be open at the same time as one writer.
  bool read_only = false;

  // Retain the previous published generation as "<path>.prev". The retained
  // file is the fallback when the current file fails verification.
  bool retain_previous = true;

  // Create an empty store when no state file exists yet.
  bool create_if_missing = true;

  // Adopt an existing writer lock whose recorded process is provably gone, or
  // whose lock file is unreadable or fails its own integrity check. When false,
  // such a lock is reported as WriterLockHeld and the caller must decide.
  bool adopt_abandoned_lock = true;

  // Refuse to open a store whose recorded incarnation differs from this value.
  // This is how a caller detects that the path now holds an unrelated store
  // that was swapped in behind it.
  std::optional<std::string> expected_incarnation{};

  // See FaultHook.
  FaultHook fault_hook{};
};

enum class WriterLockState : std::uint8_t {
  // No lock file exists.
  Unlocked = 0,
  // The lock file exists and names this store's writer identity and process.
  HeldByThisProcess = 1,
  // The lock file exists and names a different, live process.
  HeldByAnotherProcess = 2,
  // The lock file exists but is unreadable, truncated, or fails its integrity
  // check. It is never trusted.
  Invalid = 3,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view writer_lock_state_name(
    WriterLockState state) noexcept;

struct WriterLockInfo {
  WriterLockState state = WriterLockState::Unlocked;
  WriterId writer_id{};
  std::string incarnation{};
  std::uint64_t pid = 0;
  std::uint64_t process_start_marker = 0;
  StoreEpoch epoch{};
  std::uint64_t acquired_at_unix_ns = 0;
  // Set when authority was adopted from a holder that is provably gone.
  WriterId adopted_from{};
  std::uint64_t adopted_from_pid = 0;
  std::string detail{};

  [[nodiscard]] std::string to_text() const;
};

// What happened when the store was opened.
enum class RecoveryAction : std::uint8_t {
  // No state file existed and an empty store was created.
  FreshStore = 0,
  // The current state file verified and became authoritative.
  LoadedCurrent = 1,
  // The current state file was missing, unreadable or failed verification, and
  // the retained previous generation became authoritative instead.
  LoadedPrevious = 2,
  // Neither file verified. No state was accepted; the store is empty and the
  // current file was left untouched.
  NoStateAccepted = 3,
};

[[nodiscard]] RACK_CAPACITY_API std::string_view recovery_action_name(
    RecoveryAction action) noexcept;

struct RecoveryReport {
  RecoveryAction action = RecoveryAction::FreshStore;
  StoreEpoch epoch{};
  StoreSequence sequence{};
  std::string incarnation{};
  std::size_t rack_count = 0;
  // Set when the current state file existed but was rejected, naming the code
  // and explanation. The rejected file is never modified.
  std::optional<CapacityError> current_rejection{};
  // Temporary files left behind by an interrupted publication, retired during
  // recovery.
  std::vector<std::string> retired_temp_files{};
  // Ordered notes describing what recovery did and why.
  std::vector<std::string> notes{};

  [[nodiscard]] std::string to_text() const;
};

// Metadata of a state file, read without taking writer authority.
struct StateFileInfo {
  std::uint32_t format_version = 0;
  std::uint32_t snapshot_layout_version = 0;
  std::uint32_t mount_slots_per_rack_unit = 0;
  std::string incarnation{};
  StoreEpoch epoch{};
  StoreSequence sequence{};
  std::size_t rack_count = 0;
  std::size_t asset_count = 0;
  StateDigest payload_digest{};
  std::uint64_t byte_size = 0;

  [[nodiscard]] std::string to_text() const;
};

// One recorded idempotency receipt. Replay coverage is bounded: when the
// journal is full the oldest entry is evicted and the eviction count is
// preserved so operators can see that coverage is bounded rather than
// unlimited.
struct IdempotencyRecord {
  RequestId request{};
  RackId rack{};
  AttemptId attempt{};
  StoreSequence sequence{};
  MutationOutcome outcome = MutationOutcome::Applied;
  CapacityGeneration capacity_generation{};
  SnapshotRevision revision{};
  // Digest of the evidence facts the request carried, and the digest of the
  // snapshot it produced. Together they let a replay be recognized exactly: the
  // same request identity with different facts is a conflict, not a replay.
  StateDigest inputs_digest{};
  StateDigest snapshot_digest{};
  TimestampNs accepted_at{};
};

// The replay journal travels inside the published state, so a receipt and the
// state it describes are committed by the same atomic publication and
// idempotency survives a restart.
struct IdempotencyJournal {
  std::vector<IdempotencyRecord> records{};
  std::uint64_t evicted = 0;

  [[nodiscard]] std::optional<IdempotencyRecord> find(const RequestId& request) const;
  [[nodiscard]] Status record(const IdempotencyRecord& entry);
};

// The complete authoritative state of one capacity catalog.
struct CatalogState {
  std::string incarnation{};
  StoreEpoch epoch{};
  StoreSequence sequence{};
  TimestampNs written_at{};
  // Each record carries the policy reference it was accounted under. There is
  // deliberately no catalog-level policy summary: two racks may be accounted
  // under different policies, and a summary would have to invent a way to
  // choose between them.
  std::vector<RackCapacityRecord> records{};
  IdempotencyJournal idempotency{};

  [[nodiscard]] std::size_t asset_count() const noexcept;
};

// Durable, transactional store for authoritative capacity state.
//
// Publication is a single sequence: plan, validate, serialize into a new
// temporary file, flush it, verify its integrity by reading it back, publish it
// atomically, then retire the superseded temporary state. A crash or partial
// write therefore leaves either the previous generation or the new one
// authoritative, never a mixture, and never a file that decodes into a
// half-applied capacity change.
//
// Concurrency model: one writer per state file, enforced by an exclusive
// operating-system lock on the lock file. The lock is held for the lifetime of
// a writable store and is released by the operating system when the process
// exits for any reason, including a crash, so a dead writer can never keep
// authority. Within one process a writer mutex serializes every mutation end to
// end, including the durable flush.
class RACK_CAPACITY_API CapacityStore {
 public:
  // Opens a store. Fails with IoFailure when the state cannot be read or
  // written, with IntegrityCheckFailed or CorruptState when no generation can
  // be verified and the store was not allowed to start empty, and with
  // WriterLockHeld when another live writer owns the file.
  [[nodiscard]] static Result<std::unique_ptr<CapacityStore>> open(const StoreOptions& options);

  // Reads and validates the state file at `path` without taking writer
  // authority and without mutating anything on disk.
  [[nodiscard]] static Result<StateFileInfo> inspect(const std::filesystem::path& path);

  // Reports the current writer lock without taking it.
  [[nodiscard]] static Result<WriterLockInfo> query_writer_lock(
      const std::filesystem::path& path);

  // Operator action: releases writer authority recorded in the lock file when
  // no live process holds the operating-system lock. Taking authority away from
  // a live process is deliberately impossible: the operating-system lock is
  // held by that process, so this returns WriterLockHeld and the operator must
  // stop the holder first. The store epoch advances, so any publication that
  // was planned against the previous epoch is refused.
  [[nodiscard]] static Result<WriterLockInfo> force_release(
      const std::filesystem::path& path, WriterId operator_id, std::uint64_t observed_at_unix_ns);

  ~CapacityStore();
  // A store is owned through the std::unique_ptr that open() returns, and it is
  // not movable: a moved-from store would hold no implementation and every
  // method would have to be defensive about it. Removing the operation removes
  // the whole class of mistake.
  CapacityStore(CapacityStore&&) = delete;
  CapacityStore& operator=(CapacityStore&&) = delete;
  CapacityStore(const CapacityStore&) = delete;
  CapacityStore& operator=(const CapacityStore&) = delete;

  // Publishes a whole new state. The declared epoch and sequence must be the
  // ones this store issued; anything else is refused as stale writer authority.
  // Returns the sequence the new state was published at.
  [[nodiscard]] Result<StoreSequence> publish(const CatalogState& next_state);

  // Loads the authoritative state, already validated and closure-checked.
  [[nodiscard]] const CatalogState& state() const;
  // The idempotency journal that was loaded and published with the state.
  [[nodiscard]] const IdempotencyJournal& journal() const;

  [[nodiscard]] const RecoveryReport& recovery() const;
  [[nodiscard]] WriterLockInfo writer_lock() const;
  [[nodiscard]] bool holds_writer_authority() const;
  [[nodiscard]] bool is_read_only() const;
  [[nodiscard]] StoreEpoch epoch() const;
  [[nodiscard]] StoreSequence sequence() const;
  [[nodiscard]] const std::string& incarnation() const;
  [[nodiscard]] const std::filesystem::path& path() const;

  // Releases writer authority, retires any temporary file this store still owns
  // and returns accounting to its baseline. Idempotent. A store that is
  // destroyed without close() performs the same work.
  [[nodiscard]] Status close();

 private:
  struct Impl;
  explicit CapacityStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rackcapacity
