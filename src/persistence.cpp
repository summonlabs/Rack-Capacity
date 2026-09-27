// Rack Capacity - durable store: transactional publication and writer fencing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/persistence.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "codec.hpp"
#include "rack_capacity/digest.hpp"
#include "store_files.hpp"

namespace rackcapacity {
namespace {

// State file frame.
//
//   0   8  magic "RCAPSTOR"
//   8   2  format major
//  10   2  format minor
//  12   4  byte order marker 0x01020304
//  16   4  header bytes
//  20   4  snapshot layout version
//  24   4  mount slots per rack unit
//  28   4  flags
//  32   8  declared payload bytes
//  40   8  store epoch
//  48   8  store sequence
//  56   4  rack count
//  60   4  occupant count
//  64  32  store incarnation, lowercase hex, zero padded
//  96   4  payload CRC-32C
// 100   4  header CRC-32C over bytes 0..99
// 104     payload
constexpr char kMagic[8] = {'R', 'C', 'A', 'P', 'S', 'T', 'O', 'R'};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint32_t kSwappedEndianMarker = 0x04030201u;
constexpr std::uint32_t kHeaderBytes = 104u;
constexpr std::uint32_t kIncarnationBytes = 32u;
constexpr std::uint32_t kFlagRetainsPrevious = 1u;

struct FrameHeader {
  std::uint16_t format_major = 0;
  std::uint16_t format_minor = 0;
  std::uint32_t layout_version = 0;
  std::uint32_t slots_per_unit = 0;
  std::uint32_t flags = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::uint32_t rack_count = 0;
  std::uint32_t asset_count = 0;
  std::string incarnation;
  std::uint32_t payload_crc = 0;
  std::uint32_t header_crc = 0;
};

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8u));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (index * 8);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8);
  }
  return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_frame(const CatalogState& state,
                                                     const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> header;
  header.reserve(kHeaderBytes);
  header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
  put_u16(header, static_cast<std::uint16_t>(kStateFormatVersion));
  put_u16(header, 0);
  put_u32(header, kEndianMarker);
  put_u32(header, kHeaderBytes);
  put_u32(header, kSnapshotLayoutVersion);
  put_u32(header, kMountSlotsPerRackUnit);
  put_u32(header, kFlagRetainsPrevious);
  put_u64(header, payload.size());
  put_u64(header, state.epoch.value());
  put_u64(header, state.sequence.value());
  put_u32(header, static_cast<std::uint32_t>(
                      std::min<std::size_t>(state.records.size(), 0xFFFFFFFFu)));
  put_u32(header, static_cast<std::uint32_t>(
                      std::min<std::size_t>(state.asset_count(), 0xFFFFFFFFu)));
  std::string incarnation = state.incarnation;
  incarnation.resize(kIncarnationBytes, '0');
  header.insert(header.end(), incarnation.begin(), incarnation.end());
  put_u32(header, crc32c(payload.data(), payload.size()));

  const std::uint32_t header_crc = crc32c(header.data(), header.size());
  put_u32(header, header_crc);

  std::vector<std::uint8_t> frame;
  frame.reserve(header.size() + payload.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

[[nodiscard]] Result<FrameHeader> decode_frame_header(const std::vector<std::uint8_t>& bytes,
                                                      std::uint64_t& payload_offset) {
  if (bytes.size() < kHeaderBytes) {
    return make_error(ErrorCode::TruncatedState,
                      "the state file ends before its header is complete",
                      ErrorDetail{"store.header", {}, {}, kHeaderBytes, bytes.size(), {}});
  }
  if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
    return make_error(ErrorCode::CorruptState,
                      "the file does not begin with the capacity state magic",
                      ErrorDetail{"store.header", "magic", {}, 0, 0, {}});
  }
  const std::uint32_t endian = get_u32(bytes.data() + 12);
  if (endian == kSwappedEndianMarker) {
    return make_error(ErrorCode::WrongByteOrder,
                      "the state file was written with the opposite byte order",
                      ErrorDetail{"store.header", "endian", {}, kEndianMarker, endian, {}});
  }
  if (endian != kEndianMarker) {
    return make_error(ErrorCode::CorruptState, "the state file carries no byte order marker",
                      ErrorDetail{"store.header", "endian", {}, kEndianMarker, endian, {}});
  }
  if (get_u32(bytes.data() + 16) != kHeaderBytes) {
    return make_error(ErrorCode::CorruptState, "the state file declares an unexpected header size",
                      ErrorDetail{"store.header", "header_bytes", {}, kHeaderBytes,
                                  get_u32(bytes.data() + 16), {}});
  }
  FrameHeader header;
  header.format_major = get_u16(bytes.data() + 8);
  header.format_minor = get_u16(bytes.data() + 10);
  header.layout_version = get_u32(bytes.data() + 20);
  header.slots_per_unit = get_u32(bytes.data() + 24);
  header.flags = get_u32(bytes.data() + 28);
  header.payload_bytes = get_u64(bytes.data() + 32);
  header.epoch = get_u64(bytes.data() + 40);
  header.sequence = get_u64(bytes.data() + 48);
  header.rack_count = get_u32(bytes.data() + 56);
  header.asset_count = get_u32(bytes.data() + 60);
  header.incarnation.assign(reinterpret_cast<const char*>(bytes.data() + 64), kIncarnationBytes);
  header.payload_crc = get_u32(bytes.data() + 96);
  header.header_crc = get_u32(bytes.data() + 100);

  if (crc32c(bytes.data(), kHeaderBytes - 4u) != header.header_crc) {
    return make_error(ErrorCode::IntegrityCheckFailed,
                      "the state file header fails its integrity check",
                      ErrorDetail{"store.header", "header_crc", {}, 0, 0, {}});
  }
  if (header.format_major != kStateFormatVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the state file was written by a different major format version",
                      ErrorDetail{"store.header", "format_major", {}, kStateFormatVersion,
                                  header.format_major, {}});
  }
  if (header.layout_version != kSnapshotLayoutVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the state file declares a snapshot layout this build does not implement",
                      ErrorDetail{"store.header", "layout_version", {}, kSnapshotLayoutVersion,
                                  header.layout_version, {}});
  }
  if (header.slots_per_unit != kMountSlotsPerRackUnit) {
    return make_error(ErrorCode::UnsupportedCoordinateModel,
                      "the state file declares a different mount slot resolution",
                      ErrorDetail{"store.header", "slots_per_unit", {}, kMountSlotsPerRackUnit,
                                  header.slots_per_unit, {}});
  }
  if (header.payload_bytes > kMaxStateFileBytes) {
    return make_error(ErrorCode::StateTooLarge,
                      "the state file declares a payload above the documented bound",
                      ErrorDetail{"store.header", "payload_bytes", {}, kMaxStateFileBytes,
                                  header.payload_bytes, {}});
  }
  if (header.payload_bytes != bytes.size() - kHeaderBytes) {
    return make_error(ErrorCode::TruncatedState,
                      "the state file length does not match its declared payload length",
                      ErrorDetail{"store.header", "payload_bytes", {}, header.payload_bytes,
                                  bytes.size() - kHeaderBytes, {}});
  }
  if (crc32c(bytes.data() + kHeaderBytes, static_cast<std::size_t>(header.payload_bytes)) !=
      header.payload_crc) {
    return make_error(ErrorCode::IntegrityCheckFailed,
                      "the state file payload fails its integrity check",
                      ErrorDetail{"store.payload", "payload_crc", {}, 0, 0, {}});
  }
  payload_offset = kHeaderBytes;
  return header;
}

// Validates a decoded state exhaustively. A state that is structurally
// decodable but internally inconsistent is refused, never repaired.
[[nodiscard]] Status validate_state(const CatalogState& state, const FrameHeader& header) {
  if (state.incarnation != header.incarnation) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "the state payload and its header name different incarnations",
                             ErrorDetail{"store.validate", "incarnation", {}, 0, 0, {}}));
  }
  if (state.epoch.value() != header.epoch || state.sequence.value() != header.sequence) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "the state payload and its header disagree about epoch or sequence",
                             ErrorDetail{"store.validate", "epoch", {}, header.epoch,
                                         state.epoch.value(), {}}));
  }
  if (state.records.size() != header.rack_count) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "the state payload holds a different number of racks than declared",
                             ErrorDetail{"store.validate", "rack_count", {}, header.rack_count,
                                         state.records.size(), {}}));
  }
  if (state.asset_count() != header.asset_count) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "the state payload holds a different number of occupants than declared",
                             ErrorDetail{"store.validate", "asset_count", {}, header.asset_count,
                                         state.asset_count(), {}}));
  }
  if (state.incarnation.size() != kIncarnationBytes) {
    return Status(make_error(ErrorCode::InvalidStateEncoding,
                             "the store incarnation is not the documented length",
                             ErrorDetail{"store.validate", "incarnation", {}, kIncarnationBytes,
                                         state.incarnation.size(), {}}));
  }
  for (std::size_t index = 0; index < state.records.size(); ++index) {
    const RackCapacityRecord& record = state.records[index];
    if (record.rack.empty()) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "a stored record has no rack identity",
                               ErrorDetail{"store.validate", "records", {}, 0, index, {}}));
    }
    if (index > 0 && !(state.records[index - 1].rack < record.rack)) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "stored records are not in strictly ascending rack order",
                               ErrorDetail{"store.validate", "records", {}, 0, index, {}}));
    }
    if (record.inputs.composition.rack != record.rack) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "a stored record's evidence names a different rack",
                               ErrorDetail{"store.validate", "records", record.rack.text(), 0, 0,
                                           {}}));
    }
    if (record.snapshot.rack != record.rack) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "a stored record's snapshot names a different rack",
                               ErrorDetail{"store.validate", "records", record.rack.text(), 0, 0,
                                           {}}));
    }
    if (record.snapshot.capacity_generation != record.capacity_generation ||
        record.snapshot.revision != record.revision ||
        record.snapshot.composition_generation != record.composition_generation) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "a stored record and its snapshot disagree about generations",
                               ErrorDetail{"store.validate", "records", record.rack.text(), 0, 0,
                                           {}}));
    }
    const Status inputs_ok = validate_inputs(record.inputs);
    if (!inputs_ok.has_value()) {
      return Status(make_error(ErrorCode::CorruptState,
                               "a stored evidence bundle is not valid: " +
                                   inputs_ok.error().message,
                               ErrorDetail{"store.validate", "inputs", record.rack.text(), 0, 0,
                                           {}}));
    }
    if (compute_snapshot_digest(record.snapshot) != record.snapshot.digest) {
      return Status(make_error(ErrorCode::IntegrityCheckFailed,
                               "a stored snapshot does not match its own digest",
                               ErrorDetail{"store.validate", "snapshot", record.rack.text(), 0, 0,
                                           {}}));
    }
    const ClosureReport closure = verify_closure(record.snapshot);
    if (!closure.holds) {
      return Status(make_error(ErrorCode::CorruptState,
                               "a stored snapshot does not close exactly",
                               ErrorDetail{"store.validate", "snapshot", record.rack.text(), 0, 0,
                                           closure.violations}));
    }
    if (record.lifecycle != RackLifecycle::Active && record.lifecycle != RackLifecycle::Retired &&
        record.lifecycle != RackLifecycle::Quarantined) {
      return Status(make_error(ErrorCode::InvalidStateEncoding,
                               "a stored record has an undefined lifecycle state",
                               ErrorDetail{"store.validate", "lifecycle", record.rack.text(), 0, 0,
                                           {}}));
    }
  }
  return Status{};
}

// --- lock record -----------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_lock_record(const WriterLockInfo& info) {
  std::string text;
  text += "rcap-lock 1\n";
  text += "writer=" + info.writer_id.text() + "\n";
  text += "incarnation=" + info.incarnation + "\n";
  text += "pid=" + std::to_string(info.pid) + "\n";
  text += "start=" + std::to_string(info.process_start_marker) + "\n";
  text += "epoch=" + std::to_string(info.epoch.value()) + "\n";
  text += "acquired-at-ns=" + std::to_string(info.acquired_at_unix_ns) + "\n";
  text += "adopted-from=" + (info.adopted_from.empty() ? std::string("-") : info.adopted_from.text()) +
          "\n";
  text += "adopted-from-pid=" + std::to_string(info.adopted_from_pid) + "\n";
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

[[nodiscard]] Result<WriterLockInfo> decode_lock_record(const std::vector<std::uint8_t>& bytes) {
  WriterLockInfo info;
  info.state = WriterLockState::Invalid;
  const std::string text(bytes.begin(), bytes.end());
  if (text.empty()) {
    info.state = WriterLockState::Unlocked;
    return info;
  }
  std::size_t position = 0;
  bool saw_version = false;
  while (position < text.size()) {
    const std::size_t end = text.find('\n', position);
    if (end == std::string::npos) {
      return make_error(ErrorCode::CorruptState, "the writer lock record has no line terminator",
                        ErrorDetail{"lock.decode", {}, {}, 0, position, {}});
    }
    const std::string line = text.substr(position, end - position);
    position = end + 1;
    if (!saw_version) {
      if (line != "rcap-lock 1") {
        return make_error(ErrorCode::CorruptState, "the writer lock record has no version line",
                          ErrorDetail{"lock.decode", "version", {}, 0, 0, {}});
      }
      saw_version = true;
      continue;
    }
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos) {
      return make_error(ErrorCode::CorruptState, "the writer lock record has a malformed field",
                        ErrorDetail{"lock.decode", line, {}, 0, 0, {}});
    }
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "writer") {
      const Result<WriterId> id = WriterId::create(value);
      if (!id.has_value()) {
        return id.error();
      }
      info.writer_id = id.value();
    } else if (key == "incarnation") {
      info.incarnation = value;
    } else if (key == "pid" || key == "start" || key == "epoch" || key == "acquired-at-ns" ||
               key == "adopted-from-pid") {
      std::uint64_t parsed = 0;
      if (!value.empty()) {
        for (const char character : value) {
          if (character < '0' || character > '9') {
            return make_error(ErrorCode::CorruptState,
                              "the writer lock record has a non-numeric field",
                              ErrorDetail{"lock.decode", key, {}, 0, 0, {}});
          }
          parsed = parsed * 10u + static_cast<std::uint64_t>(character - '0');
        }
      }
      if (key == "pid") {
        info.pid = parsed;
      } else if (key == "start") {
        info.process_start_marker = parsed;
      } else if (key == "epoch") {
        const Result<StoreEpoch> epoch = StoreEpoch::create(parsed);
        if (!epoch.has_value()) {
          return epoch.error();
        }
        info.epoch = epoch.value();
      } else if (key == "acquired-at-ns") {
        info.acquired_at_unix_ns = parsed;
      } else {
        info.adopted_from_pid = parsed;
      }
    } else if (key == "adopted-from") {
      if (value != "-") {
        const Result<WriterId> id = WriterId::create(value);
        if (!id.has_value()) {
          return id.error();
        }
        info.adopted_from = id.value();
      }
    } else {
      return make_error(ErrorCode::CorruptState, "the writer lock record has an unknown field",
                        ErrorDetail{"lock.decode", key, {}, 0, 0, {}});
    }
  }
  if (!saw_version) {
    return make_error(ErrorCode::CorruptState, "the writer lock record is empty",
                      ErrorDetail{"lock.decode", {}, {}, 0, 0, {}});
  }
  info.state = WriterLockState::HeldByAnotherProcess;
  return info;
}

[[nodiscard]] bool is_temp_name(const std::string& name, const std::string& base) {
  return name.size() > base.size() + 5u && name.compare(0, base.size(), base) == 0 &&
         name.compare(base.size(), 5, ".tmp-") == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

std::string_view write_stage_name(WriteStage stage) noexcept {
  switch (stage) {
    case WriteStage::AfterLockAcquired:
      return "after-lock-acquired";
    case WriteStage::AfterTempCreated:
      return "after-temp-created";
    case WriteStage::AfterTempWritten:
      return "after-temp-written";
    case WriteStage::AfterTempSynced:
      return "after-temp-synced";
    case WriteStage::AfterPreviousPublished:
      return "after-previous-published";
    case WriteStage::BeforePublishRename:
      return "before-publish-rename";
    case WriteStage::AfterPublishRename:
      return "after-publish-rename";
    case WriteStage::AfterTempRetired:
      return "after-temp-retired";
  }
  return "unknown";
}

Result<WriteStage> parse_write_stage(std::string_view name) {
  for (std::uint8_t index = 0; index < kWriteStageCount; ++index) {
    const WriteStage stage = static_cast<WriteStage>(index);
    if (write_stage_name(stage) == name) {
      return stage;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown write stage",
                    ErrorDetail{"parse.write_stage", std::string(name), {}, 0, 0, {}});
}

std::string_view writer_lock_state_name(WriterLockState state) noexcept {
  switch (state) {
    case WriterLockState::Unlocked:
      return "unlocked";
    case WriterLockState::HeldByThisProcess:
      return "held-by-this-process";
    case WriterLockState::HeldByAnotherProcess:
      return "held-by-another-process";
    case WriterLockState::Invalid:
      return "invalid";
  }
  return "unknown";
}

std::string_view recovery_action_name(RecoveryAction action) noexcept {
  switch (action) {
    case RecoveryAction::FreshStore:
      return "fresh-store";
    case RecoveryAction::LoadedCurrent:
      return "loaded-current";
    case RecoveryAction::LoadedPrevious:
      return "loaded-previous";
    case RecoveryAction::NoStateAccepted:
      return "no-state-accepted";
  }
  return "unknown";
}

std::string_view mutation_outcome_name(MutationOutcome outcome) noexcept {
  switch (outcome) {
    case MutationOutcome::Applied:
      return "applied";
    case MutationOutcome::Replayed:
      return "replayed";
    case MutationOutcome::NoChange:
      return "no-change";
  }
  return "unknown";
}

std::optional<IdempotencyRecord> IdempotencyJournal::find(const RequestId& request) const {
  for (const IdempotencyRecord& entry : records) {
    if (entry.request == request) {
      return entry;
    }
  }
  return std::nullopt;
}

Status IdempotencyJournal::record(const IdempotencyRecord& entry) {
  const auto existing =
      std::find_if(records.begin(), records.end(), [&entry](const IdempotencyRecord& candidate) {
        return candidate.request == entry.request;
      });
  if (existing != records.end()) {
    if (existing->snapshot_digest != entry.snapshot_digest || existing->rack != entry.rack) {
      return Status(make_error(ErrorCode::RequestIdConflict,
                               "the request identity was already used for a different request",
                               ErrorDetail{"journal.record", "request", entry.request.text(), 0, 0,
                                           {}}));
    }
    return Status{};
  }
  if (records.size() >= kMaxIdempotencyRecords) {
    records.erase(records.begin());
    ++evicted;
  }
  records.push_back(entry);
  return Status{};
}

std::size_t CatalogState::asset_count() const noexcept {
  std::size_t total = 0;
  for (const RackCapacityRecord& record : records) {
    total += record.inputs.assets.size();
  }
  return total;
}

// ---------------------------------------------------------------------------
// CapacityStore
// ---------------------------------------------------------------------------

struct CapacityStore::Impl {
  StoreOptions options{};
  std::filesystem::path current_path{};
  std::filesystem::path previous_path{};
  std::filesystem::path lock_path{};

  // Leaf lock: serializes publication end to end, including the flush. It is
  // never taken while any other lock of this library is held, and no callback
  // runs while it is held except the documented fault-injection hook.
  mutable std::mutex mutex{};

  CatalogState state{};
  RecoveryReport recovery{};
  WriterLockInfo lock_info{};
  platform::ExclusiveFileLock lock{};
  bool holds_authority = false;
  std::uint64_t temp_counter = 0;

  [[nodiscard]] std::string temp_name() {
    ++temp_counter;
    return current_path.filename().string() + ".tmp-" +
           std::to_string(platform::current_process_id()) + "-" + std::to_string(temp_counter);
  }

  [[nodiscard]] std::filesystem::path temp_path() {
    return current_path.parent_path() / temp_name();
  }

  void hook(WriteStage stage) const {
    if (options.fault_hook) {
      options.fault_hook(stage);
    }
  }

  [[nodiscard]] Status retire_temporaries() {
    const std::filesystem::path parent =
        current_path.parent_path().empty() ? std::filesystem::path(".") : current_path.parent_path();
    const Result<std::vector<platform::DirEntry>> entries = platform::list_directory(parent);
    if (!entries.has_value()) {
      return Status(entries.error());
    }
    const std::string base = current_path.filename().string();
    for (const platform::DirEntry& entry : entries.value()) {
      if (!entry.is_file || entry.is_link) {
        continue;
      }
      if (!is_temp_name(entry.name, base)) {
        continue;
      }
      const std::filesystem::path victim = parent / entry.name;
      const Status removed = platform::remove_file(victim);
      if (!removed.has_value()) {
        return removed;
      }
      recovery.retired_temp_files.push_back(entry.name);
    }
    return Status{};
  }

  [[nodiscard]] Result<CatalogState> load_file(const std::filesystem::path& path,
                                               FrameHeader& header_storage) {
    const Result<platform::PathKind> kind = platform::path_kind(path);
    if (!kind.has_value()) {
      return kind.error();
    }
    if (kind.value() == platform::PathKind::Missing) {
      return make_error(ErrorCode::NoAuthoritativeState, "no state file exists at this path",
                        ErrorDetail{"store.load", path.filename().string(), {}, 0, 0, {}});
    }
    if (kind.value() != platform::PathKind::RegularFile) {
      return make_error(ErrorCode::CorruptState,
                        "the state path is not a regular file; the store never follows a link or a "
                        "redirectable path",
                        ErrorDetail{"store.load", path.filename().string(), {}, 0, 0, {}});
    }
    const Result<std::vector<std::uint8_t>> bytes =
        platform::read_file(path, kMaxStateFileBytes + kHeaderBytes);
    if (!bytes.has_value()) {
      return bytes.error();
    }
    std::uint64_t offset = 0;
    Result<FrameHeader> header = decode_frame_header(bytes.value(), offset);
    if (!header.has_value()) {
      return header.error();
    }
    header_storage = header.value();
    codec::ByteReader reader(bytes.value().data() + offset,
                             bytes.value().size() - static_cast<std::size_t>(offset));
    Result<CatalogState> decoded_state = codec::decode_catalog_state(reader);
    if (!decoded_state.has_value()) {
      return decoded_state.error();
    }
    const Status exhausted = reader.require_exhausted("catalog_state");
    if (!exhausted.has_value()) {
      return exhausted.error();
    }
    const Status valid = validate_state(decoded_state.value(), header_storage);
    if (!valid.has_value()) {
      return valid.error();
    }
    return decoded_state;
  }

  [[nodiscard]] Result<WriterLockInfo> read_lock_record() const {
    const Result<platform::PathKind> kind = platform::path_kind(lock_path);
    if (!kind.has_value()) {
      return kind.error();
    }
    if (kind.value() == platform::PathKind::Missing) {
      WriterLockInfo missing;
      missing.state = WriterLockState::Unlocked;
      return missing;
    }
    if (kind.value() != platform::PathKind::RegularFile) {
      WriterLockInfo invalid;
      invalid.state = WriterLockState::Invalid;
      invalid.detail = "the writer lock path is not a regular file";
      return invalid;
    }
    // When this store holds the operating-system lock the record is read
    // through the very handle that holds it; opening a second handle would work
    // only because the lock deliberately covers a byte range far from the
    // metadata, and reading through the held handle makes the check independent
    // of that choice.
    Result<std::vector<std::uint8_t>> bytes =
        lock.held() ? lock.read_locked(platform::kMaxAuxiliaryFileBytes)
                    : platform::read_file(lock_path, platform::kMaxAuxiliaryFileBytes);
    if (!bytes.has_value()) {
      return bytes.error();
    }
    Result<WriterLockInfo> decoded = decode_lock_record(bytes.value());
    if (!decoded.has_value()) {
      WriterLockInfo invalid;
      invalid.state = WriterLockState::Invalid;
      invalid.detail = decoded.error().message;
      return invalid;
    }
    return decoded;
  }
};

CapacityStore::CapacityStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CapacityStore::~CapacityStore() { (void)close(); }

Result<std::unique_ptr<CapacityStore>> CapacityStore::open(const StoreOptions& options) {
  if (options.path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a capacity store needs a path",
                      ErrorDetail{"store.open", "path", {}, 0, 0, {}});
  }
  if (!options.read_only && options.writer_id.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a writable capacity store needs a writer identity",
                      ErrorDetail{"store.open", "writer_id", {}, 0, 0, {}});
  }

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->current_path = options.path;
  impl->previous_path = options.path.string() + ".prev";
  impl->lock_path = options.path.string() + ".lock";

  const std::filesystem::path parent = impl->current_path.parent_path();
  if (!parent.empty()) {
    const Result<platform::PathKind> kind = platform::path_kind(parent);
    if (!kind.has_value()) {
      return kind.error();
    }
    if (kind.value() == platform::PathKind::Missing) {
      if (!options.create_if_missing) {
        return make_error(ErrorCode::IoFailure, "the store directory does not exist",
                          ErrorDetail{"store.open", "directory", parent.string(), 0, 0, {}});
      }
      const Status created = platform::ensure_directory(parent);
      if (!created.has_value()) {
        return created.error();
      }
    } else if (kind.value() == platform::PathKind::Link) {
      return make_error(ErrorCode::InvalidArgument,
                        "the store directory is a link or reparse point, which the store refuses "
                        "to follow",
                        ErrorDetail{"store.open", "directory", parent.string(), 0, 0, {}});
    } else if (kind.value() != platform::PathKind::Directory) {
      return make_error(ErrorCode::InvalidArgument, "the store parent path is not a directory",
                        ErrorDetail{"store.open", "directory", parent.string(), 0, 0, {}});
    }
  }

  const Result<platform::PathKind> state_kind = platform::path_kind(impl->current_path);
  if (!state_kind.has_value()) {
    return state_kind.error();
  }
  if (state_kind.value() == platform::PathKind::Link) {
    return make_error(ErrorCode::InvalidArgument,
                      "the state path is a link or reparse point, which the store refuses to follow",
                      ErrorDetail{"store.open", "path", impl->current_path.filename().string(), 0, 0,
                                  {}});
  }
  if (state_kind.value() == platform::PathKind::Directory) {
    return make_error(ErrorCode::InvalidArgument, "the state path is a directory",
                      ErrorDetail{"store.open", "path", impl->current_path.filename().string(), 0, 0,
                                  {}});
  }

  // --- writer authority --------------------------------------------------
  if (!options.read_only) {
    Result<platform::ExclusiveFileLock> lock =
        platform::ExclusiveFileLock::acquire(impl->lock_path, true);
    if (!lock.has_value()) {
      if (lock.error().code == ErrorCode::WriterLockHeld) {
        const Result<WriterLockInfo> holder = impl->read_lock_record();
        if (holder.has_value()) {
          return make_error(ErrorCode::WriterLockHeld,
                            "another live process holds writer authority over this store",
                            ErrorDetail{"store.open", "writer_lock",
                                        holder.value().writer_id.text(), holder.value().pid, 0, {}});
        }
      }
      return lock.error();
    }
    impl->lock = std::move(lock).value();
    impl->holds_authority = true;
  }

  // --- load state --------------------------------------------------------
  FrameHeader header_storage;
  Result<CatalogState> loaded = impl->load_file(impl->current_path, header_storage);
  if (!loaded.has_value()) {
    const CapacityError primary = loaded.error();
    if (primary.code != ErrorCode::NoAuthoritativeState) {
      impl->recovery.notes.push_back("the current state file was rejected: " + describe(primary));
      impl->recovery.current_rejection = primary;
    }
    if (options.retain_previous) {
      Result<CatalogState> previous = impl->load_file(impl->previous_path, header_storage);
      if (previous.has_value()) {
        impl->state = std::move(previous).value();
        impl->recovery.action = RecoveryAction::LoadedPrevious;
        impl->recovery.notes.push_back(
            "the retained previous generation became authoritative because the current file could "
            "not be verified");
      } else if (previous.error().code != ErrorCode::NoAuthoritativeState) {
        impl->recovery.notes.push_back("the retained previous generation was also rejected: " +
                                       describe(previous.error()));
      }
    }
    if (impl->recovery.action != RecoveryAction::LoadedPrevious) {
      if (primary.code != ErrorCode::NoAuthoritativeState) {
        impl->recovery.action = RecoveryAction::NoStateAccepted;
        return primary;
      }
      if (!options.create_if_missing) {
        return make_error(ErrorCode::NoAuthoritativeState,
                          "no state file exists and the store was not allowed to start empty",
                          ErrorDetail{"store.open", "path", impl->current_path.filename().string(),
                                      0, 0, {}});
      }
      impl->recovery.action = RecoveryAction::FreshStore;
      impl->state = CatalogState{};
      impl->state.incarnation = platform::random_hex(kIncarnationBytes / 2u);
    }
  } else {
    impl->state = std::move(loaded).value();
    impl->recovery.action = RecoveryAction::LoadedCurrent;
  }

  if (impl->state.incarnation.empty()) {
    impl->state.incarnation = platform::random_hex(kIncarnationBytes / 2u);
  }

  if (options.expected_incarnation.has_value() &&
      options.expected_incarnation.value() != impl->state.incarnation) {
    if (impl->holds_authority) {
      (void)impl->lock.release();
      impl->holds_authority = false;
    }
    return make_error(ErrorCode::StoreIncarnationMismatch,
                      "the store at this path is not the store this caller expects",
                      ErrorDetail{"store.open", "incarnation", impl->state.incarnation, 0, 0,
                                  {options.expected_incarnation.value()}});
  }

  // Records recovered from durable state are not authoritative for capacity
  // until they have been revalidated in this process. Recovery never promotes
  // persisted evidence to current by itself.
  for (RackCapacityRecord& record : impl->state.records) {
    record.standing = RecoveryStanding::PendingRevalidation;
  }

  impl->recovery.incarnation = impl->state.incarnation;
  impl->recovery.rack_count = impl->state.records.size();
  impl->recovery.sequence = impl->state.sequence;

  // --- epoch and lock record --------------------------------------------
  std::uint64_t previous_epoch = 0;
  if (impl->holds_authority) {
    const Result<WriterLockInfo> existing = impl->read_lock_record();
    if (existing.has_value() && existing.value().state != WriterLockState::Unlocked) {
      previous_epoch = existing.value().epoch.value();
      if (!existing.value().writer_id.empty() && existing.value().writer_id != options.writer_id) {
        impl->lock_info.adopted_from = existing.value().writer_id;
        impl->lock_info.adopted_from_pid = existing.value().pid;
      }
      if (existing.value().pid != 0 && existing.value().pid != platform::current_process_id() &&
          platform::process_is_alive(existing.value().pid)) {
        if (!options.adopt_abandoned_lock) {
          (void)impl->lock.release();
          impl->holds_authority = false;
          return make_error(ErrorCode::WriterLockHeld,
                            "the writer lock names a live process and adoption was refused",
                            ErrorDetail{"store.open", "writer_lock", existing.value().writer_id.text(),
                                        existing.value().pid, 0, {}});
        }
        impl->recovery.notes.push_back(
            "writer authority was adopted from a process that no longer holds the "
            "operating-system lock");
      }
    }
    const std::uint64_t base = std::max(previous_epoch, impl->state.epoch.value());
    const Result<StoreEpoch> next = StoreEpoch::create(base + 1u);
    if (!next.has_value()) {
      return next.error();
    }
    impl->state.epoch = next.value();
    impl->recovery.epoch = next.value();

    impl->lock_info.writer_id = options.writer_id;
    impl->lock_info.incarnation = impl->state.incarnation;
    impl->lock_info.pid = platform::current_process_id();
    const Result<std::uint64_t> marker = platform::process_start_marker(impl->lock_info.pid);
    impl->lock_info.process_start_marker = marker.has_value() ? marker.value() : 0;
    impl->lock_info.epoch = next.value();
    impl->lock_info.acquired_at_unix_ns = static_cast<std::uint64_t>(system_now().value());
    impl->lock_info.state = WriterLockState::HeldByThisProcess;
    const Status written = impl->lock.write_locked(encode_lock_record(impl->lock_info));
    if (!written.has_value()) {
      return written.error();
    }
  } else {
    impl->recovery.epoch = impl->state.epoch;
    const Result<WriterLockInfo> existing = impl->read_lock_record();
    if (existing.has_value()) {
      impl->lock_info = existing.value();
    }
  }

  // --- retire residue ----------------------------------------------------
  const Status retired = impl->retire_temporaries();
  if (!retired.has_value()) {
    return retired.error();
  }
  if (!impl->recovery.retired_temp_files.empty()) {
    impl->recovery.notes.push_back("temporary files left by an interrupted publication were "
                                   "retired");
  }

  return std::unique_ptr<CapacityStore>(new CapacityStore(std::move(impl)));
}

Result<StateFileInfo> CapacityStore::inspect(const std::filesystem::path& path) {
  const Result<platform::PathKind> kind = platform::path_kind(path);
  if (!kind.has_value()) {
    return kind.error();
  }
  if (kind.value() == platform::PathKind::Link) {
    return make_error(ErrorCode::InvalidArgument,
                      "the state path is a link or reparse point, which inspection refuses to "
                      "follow",
                      ErrorDetail{"store.inspect", "path", path.filename().string(), 0, 0, {}});
  }
  if (kind.value() != platform::PathKind::RegularFile) {
    return make_error(ErrorCode::NoAuthoritativeState, "no regular state file exists at this path",
                      ErrorDetail{"store.inspect", "path", path.filename().string(), 0, 0, {}});
  }
  const Result<std::vector<std::uint8_t>> bytes =
      platform::read_file(path, kMaxStateFileBytes + kHeaderBytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  std::uint64_t offset = 0;
  Result<FrameHeader> header = decode_frame_header(bytes.value(), offset);
  if (!header.has_value()) {
    return header.error();
  }
  codec::ByteReader reader(bytes.value().data() + offset,
                           bytes.value().size() - static_cast<std::size_t>(offset));
  Result<CatalogState> state = codec::decode_catalog_state(reader);
  if (!state.has_value()) {
    return state.error();
  }
  const Status exhausted = reader.require_exhausted("catalog_state");
  if (!exhausted.has_value()) {
    return exhausted.error();
  }
  const Status valid = validate_state(state.value(), header.value());
  if (!valid.has_value()) {
    return valid.error();
  }
  StateFileInfo info;
  info.format_version = header.value().format_major;
  info.snapshot_layout_version = header.value().layout_version;
  info.mount_slots_per_rack_unit = header.value().slots_per_unit;
  info.incarnation = header.value().incarnation;
  const Result<StoreEpoch> epoch = StoreEpoch::create(header.value().epoch);
  if (!epoch.has_value()) {
    return epoch.error();
  }
  info.epoch = epoch.value();
  const Result<StoreSequence> sequence = StoreSequence::create(header.value().sequence);
  if (!sequence.has_value()) {
    return sequence.error();
  }
  info.sequence = sequence.value();
  info.rack_count = state.value().records.size();
  info.asset_count = state.value().asset_count();
  info.payload_digest = StateDigest::domain("rcap.state.v1", bytes.value());
  info.byte_size = bytes.value().size();
  return info;
}

Result<WriterLockInfo> CapacityStore::query_writer_lock(const std::filesystem::path& path) {
  const std::filesystem::path lock_path = path.string() + ".lock";
  const Result<platform::PathKind> kind = platform::path_kind(lock_path);
  if (!kind.has_value()) {
    return kind.error();
  }
  if (kind.value() == platform::PathKind::Missing) {
    WriterLockInfo info;
    info.state = WriterLockState::Unlocked;
    return info;
  }
  if (kind.value() != platform::PathKind::RegularFile) {
    WriterLockInfo info;
    info.state = WriterLockState::Invalid;
    info.detail = "the writer lock path is not a regular file";
    return info;
  }
  const Result<std::vector<std::uint8_t>> bytes =
      platform::read_file(lock_path, platform::kMaxAuxiliaryFileBytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  Result<WriterLockInfo> info = decode_lock_record(bytes.value());
  if (!info.has_value()) {
    WriterLockInfo invalid;
    invalid.state = WriterLockState::Invalid;
    invalid.detail = info.error().message;
    return invalid;
  }
  const bool ours = info.value().pid == platform::current_process_id() &&
                    platform::process_is_alive(info.value().pid);
  if (info.value().state == WriterLockState::HeldByAnotherProcess) {
    const bool alive = platform::process_is_alive(info.value().pid);
    info.value().state = alive ? (ours ? WriterLockState::HeldByThisProcess
                                       : WriterLockState::HeldByAnotherProcess)
                               : WriterLockState::Unlocked;
    if (!alive) {
      info.value().detail = "the lock record names a process that is no longer running";
    }
  }
  return info;
}

Result<WriterLockInfo> CapacityStore::force_release(const std::filesystem::path& path,
                                                    WriterId operator_id,
                                                    std::uint64_t observed_at_unix_ns) {
  if (path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a capacity store needs a path",
                      ErrorDetail{"store.force_release", "path", {}, 0, 0, {}});
  }
  if (operator_id.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "releasing writer authority requires the identity of the operator",
                      ErrorDetail{"store.force_release", "operator", {}, 0, 0, {}});
  }
  const std::filesystem::path lock_path = path.string() + ".lock";
  Result<platform::ExclusiveFileLock> lock =
      platform::ExclusiveFileLock::acquire(lock_path, true);
  if (!lock.has_value()) {
    if (lock.error().code == ErrorCode::WriterLockHeld) {
      return make_error(ErrorCode::WriterLockHeld,
                        "a live process holds the operating-system lock; authority cannot be taken "
                        "from a running writer, stop it first",
                        ErrorDetail{"store.force_release", "writer_lock", {}, 0, 0, {}});
    }
    return lock.error();
  }

  WriterLockInfo previous;
  const Result<std::vector<std::uint8_t>> bytes =
      lock.value().read_locked(platform::kMaxAuxiliaryFileBytes);
  if (bytes.has_value() && !bytes.value().empty()) {
    Result<WriterLockInfo> decoded = decode_lock_record(bytes.value());
    if (decoded.has_value()) {
      previous = decoded.value();
    }
  }
  const Result<StoreEpoch> next = StoreEpoch::create(previous.epoch.value() + 1u);
  if (!next.has_value()) {
    return next.error();
  }
  WriterLockInfo released;
  released.state = WriterLockState::Unlocked;
  released.writer_id = operator_id;
  released.incarnation = previous.incarnation;
  released.pid = 0;
  released.process_start_marker = 0;
  released.epoch = next.value();
  released.acquired_at_unix_ns = observed_at_unix_ns;
  released.adopted_from = previous.writer_id;
  released.adopted_from_pid = previous.pid;
  released.detail = "writer authority was released by an operator; the store epoch advanced so "
                    "any publication planned against the previous epoch is refused";
  const Status written = lock.value().write_locked(encode_lock_record(released));
  if (!written.has_value()) {
    return written.error();
  }
  const Status status = lock.value().release();
  if (!status.has_value()) {
    return status.error();
  }
  return released;
}

const CatalogState& CapacityStore::state() const { return impl_->state; }

const IdempotencyJournal& CapacityStore::journal() const { return impl_->state.idempotency; }

Result<StoreSequence> CapacityStore::publish(const CatalogState& next_state) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->options.read_only) {
    return make_error(ErrorCode::ReadOnlyStore, "the store was opened read only",
                      ErrorDetail{"store.publish", "path",
                                  impl_->current_path.filename().string(), 0, 0, {}});
  }
  if (!impl_->holds_authority) {
    return make_error(ErrorCode::WriterLockInvalid, "the store does not hold writer authority",
                      ErrorDetail{"store.publish", "path",
                                  impl_->current_path.filename().string(), 0, 0, {}});
  }
  if (next_state.incarnation != impl_->state.incarnation) {
    return make_error(ErrorCode::StoreIncarnationMismatch,
                      "the published state names a different store incarnation",
                      ErrorDetail{"store.publish", "incarnation", next_state.incarnation, 0, 0,
                                  {impl_->state.incarnation}});
  }
  if (next_state.epoch != impl_->state.epoch) {
    return make_error(ErrorCode::StaleWriterFenced,
                      "the state was planned against a superseded store epoch",
                      ErrorDetail{"store.publish", "epoch", {}, impl_->state.epoch.value(),
                                  next_state.epoch.value(), {}});
  }
  const Result<StoreSequence> expected = impl_->state.sequence.next();
  if (!expected.has_value()) {
    return expected.error();
  }
  if (next_state.sequence != expected.value()) {
    return make_error(ErrorCode::SequenceRegression,
                      "publication must advance the store sequence by exactly one",
                      ErrorDetail{"store.publish", "sequence", {}, expected.value().value(),
                                  next_state.sequence.value(), {}});
  }
  if (next_state.records.size() > kMaxRacks) {
    return make_error(ErrorCode::LimitExceeded, "the state holds more racks than the bound allows",
                      ErrorDetail{"store.publish", "records", {}, kMaxRacks, next_state.records.size(),
                                  {}});
  }
  if (next_state.idempotency.records.size() > kMaxIdempotencyRecords) {
    return make_error(ErrorCode::LimitExceeded,
                      "the replay journal holds more receipts than the bound allows",
                      ErrorDetail{"store.publish", "idempotency", {}, kMaxIdempotencyRecords,
                                  next_state.idempotency.records.size(), {}});
  }

  // Validate every record against the running image before a byte is written,
  // so an invalid state is refused rather than published.
  for (std::size_t index = 0; index < next_state.records.size(); ++index) {
    const RackCapacityRecord& record = next_state.records[index];
    if (index > 0 && !(next_state.records[index - 1].rack < record.rack)) {
      return make_error(ErrorCode::InvalidArgument,
                        "records must be in strictly ascending rack order",
                        ErrorDetail{"store.publish", "records", record.rack.text(), 0, index, {}});
    }
    const Status valid = validate_inputs(record.inputs);
    if (!valid.has_value()) {
      return valid.error();
    }
    if (compute_snapshot_digest(record.snapshot) != record.snapshot.digest) {
      return make_error(ErrorCode::SnapshotInvalid,
                        "a record's snapshot does not match its own digest",
                        ErrorDetail{"store.publish", "snapshot", record.rack.text(), 0, 0, {}});
    }
    const ClosureReport closure = verify_closure(record.snapshot);
    if (!closure.holds) {
      return make_error(ErrorCode::ClosureFailure, "a record's snapshot does not close exactly",
                        ErrorDetail{"store.publish", "snapshot", record.rack.text(), 0, 0,
                                    closure.violations});
    }
  }

  const Result<std::vector<std::uint8_t>> payload = codec::encode_catalog_state_bytes(next_state);
  if (!payload.has_value()) {
    return payload.error();
  }
  const std::vector<std::uint8_t> frame = encode_frame(next_state, payload.value());
  if (frame.size() > kMaxStateFileBytes + kHeaderBytes) {
    return make_error(ErrorCode::StateTooLarge, "the encoded state exceeds the documented bound",
                      ErrorDetail{"store.publish", "bytes", {}, kMaxStateFileBytes, frame.size(),
                                  {}});
  }

  // The lock record is re-read immediately before publication: a writer whose
  // authority was taken away while it was planning must not publish.
  const Result<WriterLockInfo> current = impl_->read_lock_record();
  if (!current.has_value()) {
    return make_error(ErrorCode::WriterLockInvalid,
                      "the writer lock record could not be re-read before publication",
                      ErrorDetail{"store.publish", "writer_lock", {}, 0, 0, {}});
  }
  if (current.value().incarnation != next_state.incarnation ||
      current.value().epoch.value() != next_state.epoch.value() ||
      current.value().writer_id != impl_->options.writer_id) {
    return make_error(ErrorCode::StaleWriterFenced,
                      "writer authority changed while this publication was planned; it is refused",
                      ErrorDetail{"store.publish", "writer_lock", current.value().writer_id.text(),
                                  current.value().epoch.value(), next_state.epoch.value(), {}});
  }

  const std::filesystem::path temporary = impl_->temp_path();
  impl_->hook(WriteStage::AfterLockAcquired);
  const Status written = platform::write_file_flushed(temporary, frame);
  if (!written.has_value()) {
    (void)platform::remove_file(temporary);
    return written.error();
  }
  impl_->hook(WriteStage::AfterTempCreated);

  // Verify by reading the temporary back before it can become authoritative.
  const Result<std::vector<std::uint8_t>> verify =
      platform::read_file(temporary, kMaxStateFileBytes + kHeaderBytes);
  if (!verify.has_value()) {
    (void)platform::remove_file(temporary);
    return verify.error();
  }
  if (verify.value() != frame) {
    (void)platform::remove_file(temporary);
    return make_error(ErrorCode::IntegrityCheckFailed,
                      "the staged publication does not read back byte for byte",
                      ErrorDetail{"store.publish", "staging", temporary.filename().string(), 0, 0,
                                  {}});
  }
  impl_->hook(WriteStage::AfterTempWritten);
  impl_->hook(WriteStage::AfterTempSynced);

  if (impl_->options.retain_previous) {
    const Result<platform::PathKind> existing = platform::path_kind(impl_->current_path);
    if (existing.has_value() && existing.value() == platform::PathKind::RegularFile) {
      const Status kept = platform::rename_replace(impl_->current_path, impl_->previous_path);
      if (!kept.has_value()) {
        (void)platform::remove_file(temporary);
        return kept.error();
      }
    }
    impl_->hook(WriteStage::AfterPreviousPublished);
  }

  impl_->hook(WriteStage::BeforePublishRename);
  const Status published = platform::rename_replace(temporary, impl_->current_path);
  if (!published.has_value()) {
    (void)platform::remove_file(temporary);
    return published.error();
  }
  impl_->hook(WriteStage::AfterPublishRename);

  if (!impl_->current_path.parent_path().empty()) {
    const Status synced = platform::sync_directory(impl_->current_path.parent_path());
    if (!synced.has_value()) {
      return synced.error();
    }
  }

  // The temporary is normally gone already because it was renamed over the
  // current file; removing it is a no-op that also covers a store configured
  // without retention.
  const Status retired = platform::remove_file(temporary);
  if (!retired.has_value()) {
    return retired.error();
  }
  impl_->hook(WriteStage::AfterTempRetired);

  impl_->state = next_state;
  impl_->recovery.sequence = next_state.sequence;
  return next_state.sequence;
}

const RecoveryReport& CapacityStore::recovery() const { return impl_->recovery; }

WriterLockInfo CapacityStore::writer_lock() const { return impl_->lock_info; }

bool CapacityStore::holds_writer_authority() const { return impl_->holds_authority; }

bool CapacityStore::is_read_only() const { return impl_->options.read_only; }

StoreEpoch CapacityStore::epoch() const { return impl_->state.epoch; }

StoreSequence CapacityStore::sequence() const { return impl_->state.sequence; }

const std::string& CapacityStore::incarnation() const { return impl_->state.incarnation; }

const std::filesystem::path& CapacityStore::path() const { return impl_->current_path; }

Status CapacityStore::close() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->holds_authority) {
    return Status{};
  }
  // The lock record is cleared before the operating-system lock is released, so
  // a reader never sees a record that still names a writer which has already
  // let go. A process that dies without reaching this point leaves a record
  // naming a dead process, which readers classify as not held.
  WriterLockInfo released_record = impl_->lock_info;
  released_record.pid = 0;
  released_record.process_start_marker = 0;
  released_record.detail = "released cleanly by the writer that held it";
  const Status cleared = impl_->lock.write_locked(encode_lock_record(released_record));
  const Status retired = impl_->retire_temporaries();
  const Status released = impl_->lock.release();
  impl_->holds_authority = false;
  impl_->lock_info.state = WriterLockState::Unlocked;
  impl_->lock_info.pid = 0;
  if (!cleared.has_value()) {
    return cleared;
  }
  if (!retired.has_value()) {
    return retired;
  }
  return released;
}

}  // namespace rackcapacity
