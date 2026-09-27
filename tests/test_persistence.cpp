// Rack Capacity - persistence, recovery and close/reopen tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

RackCapacityInputs input_for(const std::string& rack, std::uint64_t epoch,
                             const std::string& request, std::int64_t draw = 1000) {
  return rctest::Bundle(rack, 42)
      .epoch(epoch)
      .request(request)
      .asset_units("a-0001", 1, 1)
      .asset_draw("a-0001", draw)
      .build();
}

Result<std::unique_ptr<CapacityStore>> open_store(const std::string& path, bool read_only = false,
                                                  bool create = true,
                                                  bool retain_previous = true) {
  StoreOptions options;
  options.path = path;
  options.writer_id = WriterId::create("test-writer").value();
  options.read_only = read_only;
  options.create_if_missing = create;
  options.retain_previous = retain_previous;
  return CapacityStore::open(options);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if (!stream) {
    return bytes;
  }
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    bytes.insert(bytes.end(), buffer, buffer + stream.gcount());
  }
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

RC_TEST(store_round_trips_through_a_real_close_and_reopen) {
  rctest::TempDir dir("store-roundtrip");
  const std::string path = dir.child("capacity.rcstate");

  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    const Result<MutationReceipt> first = catalog.value()->register_rack(
        RegisterRackRequest{input_for("rack-a1", 1, "req-0001"), TimestampNs::trusted(kNow)});
    RC_REQUIRE_OK(first);
    const Result<MutationReceipt> second = catalog.value()->register_rack(
        RegisterRackRequest{input_for("rack-b2", 1, "req-0002"), TimestampNs::trusted(kNow + 1)});
    RC_REQUIRE_OK(second);
    RC_CHECK_EQ(catalog.value()->storage().sequence.value(), 2u);
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  // A real reopen: a fresh object graph reading the bytes the previous one
  // published. Serialization alone would not prove this.
  const Result<std::unique_ptr<CapacityCatalog>> reopened = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(reopened);
  RC_CHECK_EQ(reopened.value()->rack_count(), 2u);
  RC_CHECK(reopened.value()->contains_rack(RackId::create("rack-a1").value()));
  RC_CHECK(reopened.value()->contains_rack(RackId::create("rack-b2").value()));
  RC_CHECK_EQ(reopened.value()->storage().sequence.value(), 2u);
  RC_CHECK_EQ(reopened.value()->storage().recovery_action, RecoveryAction::LoadedCurrent);
  RC_CHECK_EQ(reopened.value()->storage().epoch.value(), 2u);
  RC_REQUIRE(reopened.value()->close().has_value());
}

RC_TEST(store_recovery_falls_back_to_the_retained_previous_generation) {
  rctest::TempDir dir("store-recovery");
  const std::string path = dir.child("capacity.rcstate");
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-b2", 1, "req-0002"),
                                                       TimestampNs::trusted(kNow + 1)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  std::cout << "    diagnostic: current=" << read_bytes(path).size()
            << " previous=" << read_bytes(path + ".prev").size() << "\n";
  // Corrupt the current generation in place: a byte flip inside the payload.
  std::vector<std::uint8_t> current = read_bytes(path);
  RC_REQUIRE(current.size() > 200u);
  current[150] = static_cast<std::uint8_t>(current[150] ^ 0xFFu);
  write_bytes(path, current);

  const Result<std::unique_ptr<CapacityCatalog>> recovered = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(recovered);
  RC_CHECK_EQ(recovered.value()->storage().recovery_action, RecoveryAction::LoadedPrevious);
  RC_CHECK_EQ(recovered.value()->rack_count(), 1u);
  RC_CHECK(recovered.value()->contains_rack(RackId::create("rack-a1").value()));
  RC_CHECK_EQ(recovered.value()->storage().sequence.value(), 1u);
  RC_REQUIRE(recovered.value()->close().has_value());

  // Inspecting the damaged current file reports the same rejection that
  // recovery saw, and inspection is at least as strict as opening.
  const Result<StateFileInfo> info = CapacityStore::inspect(path);
  RC_CHECK(!info.has_value());
  RC_CHECK(info.error().code == ErrorCode::IntegrityCheckFailed ||
           info.error().code == ErrorCode::CorruptState);
}

RC_TEST(store_refuses_truncated_and_oversized_state) {
  rctest::TempDir dir("store-truncated");
  const std::string path = dir.child("capacity.rcstate");
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }
  const std::vector<std::uint8_t> good = read_bytes(path);
  RC_REQUIRE(good.size() > 120u);

  // Truncation is refused, never partially decoded.
  std::vector<std::uint8_t> truncated(good.begin(), good.begin() + 100);
  write_bytes(path, truncated);
  const Result<StateFileInfo> info = CapacityStore::inspect(path);
  RC_CHECK(!info.has_value());

  // A header declaring a payload above the documented bound is refused before
  // anything is allocated.
  std::vector<std::uint8_t> oversized = good;
  const std::uint64_t huge = 1ull << 60;
  for (int index = 0; index < 8; ++index) {
    oversized[32 + index] = static_cast<std::uint8_t>((huge >> (index * 8)) & 0xFFu);
  }
  write_bytes(path, oversized);
  const Result<StateFileInfo> oversized_info = CapacityStore::inspect(path);
  RC_REQUIRE_CODE(oversized_info, ErrorCode::IntegrityCheckFailed);
}

RC_TEST(store_refuses_a_swapped_or_foreign_state_file) {
  rctest::TempDir dir("store-swap");
  const std::string path = dir.child("capacity.rcstate");
  const std::string other = dir.child("other.rcstate");
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{other, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-z9", 1, "req-0009"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  const Result<StateFileInfo> original = CapacityStore::inspect(path);
  RC_REQUIRE_OK(original);
  const std::string incarnation = original.value().incarnation;

  // Swap the unrelated store in behind the caller's back.
  write_bytes(path, read_bytes(other));

  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("test-writer").value();
  options.expected_incarnation = incarnation;
  const Result<std::unique_ptr<CapacityCatalog>> refused = CapacityCatalog::open(options);
  RC_REQUIRE_CODE(refused, ErrorCode::StoreIncarnationMismatch);

  // Without the expectation the swap is accepted, because nothing in the file
  // claims otherwise; the incarnation is what makes detection possible.
  const Result<std::unique_ptr<CapacityCatalog>> accepted = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(accepted);
  RC_CHECK(accepted.value()->contains_rack(RackId::create("rack-z9").value()));
  RC_REQUIRE(accepted.value()->close().has_value());
}

RC_TEST(store_refuses_a_link_or_reparse_point_path) {
  rctest::TempDir dir("store-link");
  const std::string real_path = dir.child("real.rcstate");
  const std::string link_path = dir.child("link.rcstate");
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{real_path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  std::error_code error;
  std::filesystem::create_symlink(real_path, link_path, error);
  if (error) {
    // Creating a symbolic link needs a privilege this environment may not
    // grant. The refusal path itself is covered by the directory and
    // non-regular-file cases below, which need no privilege.
    std::cout << "    note: symbolic link creation unavailable: " << error.message() << "\n";
  } else {
    const Result<StateFileInfo> linked = CapacityStore::inspect(link_path);
    RC_REQUIRE_CODE(linked, ErrorCode::InvalidArgument);
  }

  // A directory where a state file is expected is refused rather than opened.
  const std::string directory_path = dir.child("directory.rcstate");
  std::filesystem::create_directories(directory_path, error);
  const Result<StateFileInfo> as_directory = CapacityStore::inspect(directory_path);
  RC_REQUIRE_CODE(as_directory, ErrorCode::NoAuthoritativeState);

  const Result<std::unique_ptr<CapacityStore>> opened = open_store(directory_path);
  RC_REQUIRE_CODE(opened, ErrorCode::InvalidArgument);
}

RC_TEST(store_retires_residue_left_by_an_interrupted_publication) {
  rctest::TempDir dir("store-residue");
  const std::string path = dir.child("capacity.rcstate");
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{input_for("rack-a1", 1, "req-0001"),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  // Simulate a writer that died between staging and publishing.
  const std::string residue = dir.child("capacity.rcstate.tmp-9999-1");
  write_bytes(residue, std::vector<std::uint8_t>(64, 0x7Fu));
  RC_CHECK(std::filesystem::exists(residue));

  const Result<std::unique_ptr<CapacityCatalog>> reopened = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(reopened);
  RC_CHECK(!std::filesystem::exists(residue));
  RC_CHECK_EQ(reopened.value()->rack_count(), 1u);
  RC_REQUIRE(reopened.value()->close().has_value());

  // Residue that is a directory is never removed by recovery.
  const std::string directory_residue = dir.child("capacity.rcstate.tmp-9999-2");
  std::error_code error;
  std::filesystem::create_directories(directory_residue, error);
  const Result<std::unique_ptr<CapacityCatalog>> again = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(again);
  RC_CHECK(std::filesystem::exists(directory_residue));
  RC_REQUIRE(again.value()->close().has_value());
}

RC_TEST(store_is_created_and_inspected_strictly) {
  rctest::TempDir dir("store-create");
  const std::string path = dir.child("nested/capacity.rcstate");

  // A store inside a directory that does not exist yet is created when the
  // caller allows it. A fresh store has authority but no state file until
  // something is published, because publication is the commit point.
  const Result<std::unique_ptr<CapacityStore>> created = open_store(path);
  RC_REQUIRE_OK(created);
  RC_CHECK(created.value()->holds_writer_authority());
  RC_CHECK(!created.value()->is_read_only());
  RC_REQUIRE_CODE(CapacityStore::inspect(path), ErrorCode::NoAuthoritativeState);
  CatalogState empty;
  empty.incarnation = created.value()->incarnation();
  empty.epoch = created.value()->epoch();
  empty.sequence = created.value()->sequence().next().value();
  RC_CHECK(created.value()->publish(empty).has_value());
  RC_REQUIRE(created.value()->close().has_value());

  const Result<StateFileInfo> info = CapacityStore::inspect(path);
  RC_REQUIRE_OK(info);
  RC_CHECK_EQ(info.value().format_version, kStateFormatVersion);
  RC_CHECK_EQ(info.value().snapshot_layout_version, kSnapshotLayoutVersion);
  RC_CHECK_EQ(info.value().mount_slots_per_rack_unit, kMountSlotsPerRackUnit);
  RC_CHECK_EQ(info.value().rack_count, 0u);
  RC_CHECK_EQ(info.value().incarnation.size(), 32u);
  RC_CHECK(!info.value().payload_digest.is_zero());
  RC_CHECK(info.value().to_text().find("format version") != std::string::npos);

  // A store that is not allowed to start empty reports that there is nothing.
  const Result<std::unique_ptr<CapacityStore>> refused =
      open_store(dir.child("absent.rcstate"), false, false);
  RC_REQUIRE_CODE(refused, ErrorCode::NoAuthoritativeState);

  // A writable store needs a writer identity.
  StoreOptions anonymous;
  anonymous.path = dir.child("anonymous.rcstate");
  RC_REQUIRE_CODE(CapacityStore::open(anonymous), ErrorCode::InvalidArgument);

  // An empty path is refused.
  RC_REQUIRE_CODE(CapacityStore::open(StoreOptions{}), ErrorCode::InvalidArgument);
}

RC_TEST(store_refuses_a_publication_whose_epoch_or_sequence_is_stale) {
  rctest::TempDir dir("store-fence");
  const std::string path = dir.child("capacity.rcstate");
  const Result<std::unique_ptr<CapacityStore>> store = open_store(path);
  RC_REQUIRE_OK(store);

  CatalogState state;
  state.incarnation = store.value()->incarnation();
  state.epoch = store.value()->epoch();
  const Result<StoreSequence> next = store.value()->sequence().next();
  RC_REQUIRE_OK(next);
  state.sequence = next.value();
  RC_CHECK(store.value()->publish(state).has_value());

  // Replaying the same sequence is a regression.
  RC_REQUIRE_CODE(store.value()->publish(state), ErrorCode::SequenceRegression);

  // Publishing under a superseded epoch is refused.
  CatalogState stale = state;
  stale.epoch = StoreEpoch::trusted(state.epoch.value() + 5);
  const Result<StoreSequence> following = store.value()->sequence().next();
  RC_REQUIRE_OK(following);
  stale.sequence = following.value();
  RC_REQUIRE_CODE(store.value()->publish(stale), ErrorCode::StaleWriterFenced);

  // Publishing into a different incarnation is refused.
  CatalogState foreign = state;
  foreign.incarnation = std::string(32, 'a');
  const Result<StoreSequence> third = store.value()->sequence().next();
  RC_REQUIRE_OK(third);
  foreign.sequence = third.value();
  RC_REQUIRE_CODE(store.value()->publish(foreign), ErrorCode::StoreIncarnationMismatch);

  RC_REQUIRE(store.value()->close().has_value());
}

RC_TEST(store_publishes_only_whole_consistent_states) {
  rctest::TempDir dir("store-whole");
  const std::string path = dir.child("capacity.rcstate");
  const Result<std::unique_ptr<CapacityStore>> store = open_store(path);
  RC_REQUIRE_OK(store);

  const RackCapacityInputs inputs = input_for("rack-a1", 1, "req-0001");
  const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
      inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
      TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
  RC_REQUIRE_OK(snapshot);

  RackCapacityRecord record;
  record.rack = snapshot.value().rack;
  record.composition_generation = snapshot.value().composition_generation;
  record.capacity_generation = snapshot.value().capacity_generation;
  record.revision = snapshot.value().revision;
  record.inputs = inputs;
  record.snapshot = snapshot.value();
  record.accepted_at = TimestampNs::trusted(kNow);

  CatalogState state;
  state.incarnation = store.value()->incarnation();
  state.epoch = store.value()->epoch();
  state.sequence = store.value()->sequence().next().value();
  state.records.push_back(record);
  RC_CHECK(store.value()->publish(state).has_value());

  // A record whose snapshot has been altered after evaluation is refused.
  CatalogState tampered = state;
  tampered.sequence = store.value()->sequence().next().value();
  tampered.records[0].snapshot.slots.total_slots = 1u;
  RC_REQUIRE_CODE(store.value()->publish(tampered), ErrorCode::SnapshotInvalid);

  // A record whose evidence no longer validates is refused.
  CatalogState invalid = state;
  invalid.sequence = store.value()->sequence().next().value();
  invalid.records[0].inputs.composition.unit_count = 0;
  RC_REQUIRE_CODE(store.value()->publish(invalid), ErrorCode::InvalidRange);

  // Out-of-order records are refused.
  CatalogState unordered = state;
  unordered.sequence = store.value()->sequence().next().value();
  unordered.records.push_back(record);
  RC_REQUIRE_CODE(store.value()->publish(unordered), ErrorCode::InvalidArgument);

  RC_REQUIRE(store.value()->close().has_value());
}

RC_TEST(store_survives_a_fault_injected_at_every_durable_stage) {
  // Each stage is injected in turn. Whatever happens, the store must open
  // afterwards with either the previous or the new generation, never a mixture.
  for (std::uint8_t stage_index = 0; stage_index < kWriteStageCount; ++stage_index) {
    const WriteStage stage = static_cast<WriteStage>(stage_index);
    rctest::TempDir dir("store-fault");
    const std::string path = dir.child("capacity.rcstate");
    bool raised = false;
    {
      StoreOptions options;
      options.path = path;
      options.writer_id = WriterId::create("test-writer").value();
      options.fault_hook = [stage, &raised](WriteStage current) {
        if (current == stage) {
          raised = true;
          throw std::runtime_error("injected fault at " + std::string(write_stage_name(stage)));
        }
      };
      const Result<std::unique_ptr<CapacityStore>> store = CapacityStore::open(options);
      RC_REQUIRE_OK(store);
      CatalogState state;
      state.incarnation = store.value()->incarnation();
      state.epoch = store.value()->epoch();
      state.sequence = store.value()->sequence().next().value();
      try {
        (void)store.value()->publish(state);
      } catch (const std::runtime_error&) {
        // The injected fault is the point of the test.
      }
      RC_CHECK(raised);
      RC_REQUIRE(store.value()->close().has_value());
    }

    // Reopen without injection: the store must be whole and usable.
    const Result<std::unique_ptr<CapacityStore>> reopened = open_store(path);
    RC_REQUIRE_OK(reopened);
    const Result<StateFileInfo> info = CapacityStore::inspect(path);
    if (!info.has_value()) {
      // A fault before the publish rename leaves no state file at all, which is
      // the whole point of the commit point: nothing half-written is visible.
      RC_CHECK(info.error().code == ErrorCode::NoAuthoritativeState);
    }
    const Result<StoreSequence> next = reopened.value()->sequence().next();
    RC_REQUIRE_OK(next);
    CatalogState state;
    state.incarnation = reopened.value()->incarnation();
    state.epoch = reopened.value()->epoch();
    state.sequence = next.value();
    RC_CHECK(reopened.value()->publish(state).has_value());
    RC_REQUIRE(reopened.value()->close().has_value());
  }
}

RC_TEST(store_writer_lock_is_visible_and_releasable_by_an_operator) {
  rctest::TempDir dir("store-lock");
  const std::string path = dir.child("capacity.rcstate");
  const Result<std::unique_ptr<CapacityStore>> store = open_store(path);
  RC_REQUIRE_OK(store);

  const Result<WriterLockInfo> info = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(info);
  RC_CHECK(info.value().state == WriterLockState::HeldByThisProcess);
  RC_CHECK(info.value().pid != 0u);
  RC_CHECK(info.value().epoch.value() == 1u);
  RC_CHECK(info.value().to_text().find("held-by-this-process") != std::string::npos);

  RC_REQUIRE(store.value()->close().has_value());

  const Result<WriterLockInfo> after = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(after);
  RC_CHECK(after.value().state == WriterLockState::Unlocked);

  // An operator release advances the epoch so a stale publication is refused.
  const Result<WriterLockInfo> released = CapacityStore::force_release(
      path, WriterId::create("operator-9").value(), static_cast<std::uint64_t>(kNow));
  RC_REQUIRE_OK(released);
  RC_CHECK_EQ(released.value().epoch.value(), 2u);
  RC_CHECK(released.value().pid == 0u);

  const Result<std::unique_ptr<CapacityStore>> adopted = open_store(path);
  RC_REQUIRE_OK(adopted);
  RC_CHECK_EQ(adopted.value()->epoch().value(), 3u);
  RC_REQUIRE(adopted.value()->close().has_value());

  // Releasing needs an operator identity.
  RC_REQUIRE_CODE(CapacityStore::force_release(path, WriterId{}, 0), ErrorCode::InvalidArgument);
}

RC_TEST(store_timeouts_are_not_used_and_open_is_repeatable) {
  // Repeatedly opening and closing one store in sequence must always succeed:
  // the writer lock is released deterministically, never left to a timeout.
  rctest::TempDir dir("store-repeat");
  const std::string path = dir.child("capacity.rcstate");
  for (int iteration = 0; iteration < 12; ++iteration) {
    const Result<std::unique_ptr<CapacityStore>> store = open_store(path);
    RC_REQUIRE_OK(store);
    RC_CHECK(store.value()->holds_writer_authority());
    RC_REQUIRE(store.value()->close().has_value());
  }
}
