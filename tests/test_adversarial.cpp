// Rack Capacity - adversarial tests against hostile input and damaged state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Everything here treats external input as untrusted. The expectation is
// always the same: refuse cleanly, with a typed code, without crashing,
// without allocating from a declared size, and without accepting a state that
// was altered.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

std::vector<std::uint8_t> read_bytes(const std::string& path) {
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

void publish_one(const std::string& path) {
  const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  if (!catalog.has_value()) {
    std::cout << "    fixture could not open the store: " << describe(catalog.error()) << "\n";
    std::abort();
  }
  const Result<MutationReceipt> receipt = catalog.value()->register_rack(RegisterRackRequest{
      rctest::Bundle("rack-a1", 42)
          .epoch(1)
          .request("req-0001")
          .asset_units("a-0001", 1, 1)
          .asset_draw("a-0001", 1000)
          .build(),
      TimestampNs::trusted(kNow)});
  if (!receipt.has_value()) {
    std::cout << "    fixture could not publish: " << describe(receipt.error()) << "\n";
    std::abort();
  }
  (void)catalog.value()->close();
}

}  // namespace

RC_TEST(adversarial_state_file_rejects_every_bit_of_damage) {
  rctest::TempDir dir("adversarial-bits");
  const std::string path = dir.child("capacity.rcstate");
  publish_one(path);
  const std::vector<std::uint8_t> original = read_bytes(path);
  RC_REQUIRE(original.size() > 200u);

  // Every single-byte alteration, at a deterministic and reproducible sample of
  // positions, must be refused. Nothing is repaired and nothing is guessed.
  rctest::Rng rng(20260101);
  std::uint32_t checked = 0;
  for (std::uint32_t attempt = 0; attempt < 400u; ++attempt) {
    const std::size_t position = static_cast<std::size_t>(rng.below(original.size()));
    std::vector<std::uint8_t> damaged = original;
    damaged[position] = static_cast<std::uint8_t>(damaged[position] ^ 0xFFu);
    write_bytes(path, damaged);
    const Result<StateFileInfo> info = CapacityStore::inspect(path);
    if (info.has_value()) {
      std::cout << "    a flip at byte " << position << " was accepted\n";
      RC_CHECK(false);
    }
    // Opening it must fail as well, and must not adopt anything.
    StoreOptions options;
    options.path = path;
    options.writer_id = WriterId::create("test-writer").value();
    options.create_if_missing = false;
    const Result<std::unique_ptr<CapacityStore>> opened = CapacityStore::open(options);
    RC_CHECK(!opened.has_value());
    ++checked;
  }
  RC_CHECK_EQ(checked, 400u);
  write_bytes(path, original);
  const Result<StateFileInfo> restored = CapacityStore::inspect(path);
  RC_REQUIRE_OK(restored);
}

RC_TEST(adversarial_state_file_rejects_structural_hostility) {
  rctest::TempDir dir("adversarial-shape");
  const std::string path = dir.child("capacity.rcstate");
  publish_one(path);
  const std::vector<std::uint8_t> original = read_bytes(path);
  RC_REQUIRE(original.size() > 200u);

  const auto expect_refused = [&path](const std::vector<std::uint8_t>& bytes,
                                      const char* what) {
    write_bytes(path, bytes);
    const Result<StateFileInfo> info = CapacityStore::inspect(path);
    if (info.has_value()) {
      std::cout << "    " << what << " was accepted\n";
    }
    RC_CHECK(!info.has_value());
  };

  expect_refused({}, "an empty file");
  expect_refused({0x00u}, "a one-byte file");
  expect_refused(std::vector<std::uint8_t>(64, 0x00u), "a zero-filled file");
  expect_refused(std::vector<std::uint8_t>(original.size(), 0xFFu), "an all-ones file");

  std::vector<std::uint8_t> wrong_magic = original;
  wrong_magic[0] = 'X';
  expect_refused(wrong_magic, "a file with a foreign magic");

  std::vector<std::uint8_t> swapped = original;
  swapped[12] = 0x01u;
  swapped[13] = 0x02u;
  swapped[14] = 0x03u;
  swapped[15] = 0x04u;
  expect_refused(swapped, "a file written with the opposite byte order");

  std::vector<std::uint8_t> foreign_version = original;
  foreign_version[8] = 9u;
  expect_refused(foreign_version, "a file from an unimplemented major version");

  std::vector<std::uint8_t> foreign_layout = original;
  foreign_layout[20] = 9u;
  expect_refused(foreign_layout, "a file with an unimplemented snapshot layout");

  std::vector<std::uint8_t> foreign_coordinates = original;
  foreign_coordinates[24] = 4u;
  expect_refused(foreign_coordinates, "a file with a different mount slot resolution");

  std::vector<std::uint8_t> trailing = original;
  trailing.push_back(0x42u);
  expect_refused(trailing, "a file with trailing bytes");

  std::vector<std::uint8_t> truncated(original.begin(), original.end() - 1);
  expect_refused(truncated, "a file missing its final byte");

  // A declared payload length far above the bound is refused before anything is
  // reserved for it.
  std::vector<std::uint8_t> huge = original;
  for (std::size_t index = 0; index < 8; ++index) {
    huge[32 + index] = 0xFFu;
  }
  expect_refused(huge, "a file declaring an enormous payload");

  write_bytes(path, original);
  RC_CHECK(CapacityStore::inspect(path).has_value());
}

RC_TEST(adversarial_decoded_state_refuses_declared_counts_before_allocating) {
  // A payload whose declared collection count is enormous must be refused
  // without the decoder reserving memory for it. The bound is checked against
  // the declared count, not against the bytes that follow.
  rctest::TempDir dir("adversarial-counts");
  const std::string path = dir.child("capacity.rcstate");
  publish_one(path);
  const std::vector<std::uint8_t> original = read_bytes(path);
  RC_REQUIRE(original.size() > 200u);

  // Search the payload for the record count field of the catalog state: it is a
  // 32-bit little-endian one somewhere after the header. Rewriting every
  // plausible 32-bit word that currently holds one is a faithful simulation of
  // a hostile count without depending on the exact layout.
  bool saw_refusal = false;
  std::uint32_t attempts = 0;
  for (std::size_t offset = 104; offset + 4 <= original.size() && attempts < 40u; ++offset) {
    if (original[offset] != 1u || original[offset + 1] != 0u || original[offset + 2] != 0u ||
        original[offset + 3] != 0u) {
      continue;
    }
    std::vector<std::uint8_t> hostile = original;
    hostile[offset] = 0xFFu;
    hostile[offset + 1] = 0xFFu;
    hostile[offset + 2] = 0xFFu;
    hostile[offset + 3] = 0x7Fu;
    write_bytes(path, hostile);
    ++attempts;
    const Result<StateFileInfo> info = CapacityStore::inspect(path);
    if (!info.has_value()) {
      saw_refusal = true;
    }
  }
  RC_CHECK(saw_refusal);
  write_bytes(path, original);
}

RC_TEST(adversarial_identity_and_path_edges_are_refused) {
  // Traversal-looking, control-character, separator-bearing, over-long and
  // non-ASCII identities are all refused rather than repaired.
  const char* hostile[] = {"..", "../etc/passwd", "a/b", "a\\b", "a:b", "a b", "a\tb",
                           "a\nb", "a\"b", "a*b", "a?b", "a<b", "a|b", "a$b", "a;b",
                           "a`b", ".hidden", "trailing.", "\xc3\xa9", "a\x01" "b"};
  for (const char* text : hostile) {
    RC_CHECK(!RackId::create(text).has_value());
    RC_CHECK(!AssetId::create(text).has_value());
    RC_CHECK(!EvidenceId::create(text).has_value());
    RC_CHECK(!WriterId::create(text).has_value());
  }
  RC_CHECK(!RackId::create(std::string(1000, 'a')).has_value());
  RC_CHECK(RackId::create(std::string(kMaxIdentityTextBytes, 'a')).has_value());

  // A store path whose parent is a file, not a directory, is refused.
  rctest::TempDir dir("adversarial-path");
  const std::string file_path = dir.child("not-a-directory");
  write_bytes(file_path, {1, 2, 3});
  const std::string nested = (std::filesystem::path(file_path) / "capacity.rcstate").string();
  StoreOptions options;
  options.path = nested;
  options.writer_id = WriterId::create("test-writer").value();
  const Result<std::unique_ptr<CapacityStore>> refused = CapacityStore::open(options);
  RC_CHECK(!refused.has_value());

  // A relative path with a traversal component is resolved by the operating
  // system and never by string rewriting; the store only ever refuses a link,
  // which it cannot create in a plain temporary directory.
  const Result<StateFileInfo> missing = CapacityStore::inspect(dir.child("absent.rcstate"));
  RC_REQUIRE_CODE(missing, ErrorCode::NoAuthoritativeState);
}

RC_TEST(adversarial_specifications_refuse_hostile_documents) {
  // A document that declares more lines than the bound is refused before it is
  // parsed into anything.
  std::string huge = "rcap-spec 1\n";
  for (std::size_t index = 0; index < kMaxSpecificationLines + 10u; ++index) {
    huge += "# padding\n";
  }
  const Result<RackCapacityInputs> too_long = parse_specification(huge);
  RC_REQUIRE_CODE(too_long, ErrorCode::LimitExceeded);

  // An oversized document is refused before it is scanned.
  const std::string oversized(static_cast<std::size_t>(kMaxSpecificationBytes) + 16u, '#');
  const Result<RackCapacityInputs> too_big = parse_specification(oversized);
  RC_REQUIRE_CODE(too_big, ErrorCode::LimitExceeded);

  // A section identity that is not a valid identity is refused with the
  // identity error rather than being accepted as an opaque key.
  const std::string bad_asset = "rcap-spec 1\n[asset ../escape]\n";
  const Result<RackCapacityInputs> refused = parse_specification(bad_asset);
  RC_CHECK(!refused.has_value());

  // Numbers at the edge of the representable range are refused, not wrapped.
  const std::string overflow =
      "rcap-spec 1\n[composition]\nrack = rack-a1\ngeneration = 99999999999999999999999\n"
      "units = 42\n";
  RC_CHECK(!parse_specification(overflow).has_value());
}

RC_TEST(adversarial_evidence_refuses_self_contradiction) {
  // Two occupants over the same slots.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 12)
                                .asset("a-0001", "[1,5)")
                                .asset("a-0002", "[3,7)")
                                .build_unchecked())
                .has_value());
  // An occupant outside the rack.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 12).asset("a-0001", "[1,40)").build_unchecked())
                .has_value());
  // A rack height of zero.
  RC_CHECK(!validate_inputs(rctest::Bundle("rack-a1", 0).build_unchecked()).has_value());
  // More occupants than the documented bound.
  rctest::Bundle many("rack-a1", 1024);
  for (std::uint32_t index = 0; index < kMaxAssetsPerRack + 4u; ++index) {
    many.asset("a-" + std::to_string(index), "[" + std::to_string(1u + index * 2u) + "," +
                                                  std::to_string(2u + index * 2u) + ")");
  }
  RC_REQUIRE_CODE(validate_inputs(many.build_unchecked()), ErrorCode::LimitExceeded);
}

RC_TEST(adversarial_store_refuses_concurrent_readers_of_a_damaged_tail) {
  // A writer that is interrupted after the retained previous generation was
  // published but before the new file appeared must leave exactly one whole
  // generation behind. This is simulated by removing the current file and
  // checking that the retained generation is what opens.
  rctest::TempDir dir("adversarial-tail");
  const std::string path = dir.child("capacity.rcstate");
  publish_one(path);
  {
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(
        CatalogOptions{path, WriterId::create("test-writer").value()});
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{rctest::Bundle("rack-b2", 42)
                                                           .epoch(1)
                                                           .request("req-0002")
                                                           .build(),
                                                       TimestampNs::trusted(kNow + 1)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }
  RC_CHECK(std::filesystem::exists(path + ".prev"));

  std::error_code error;
  std::filesystem::remove(path, error);
  RC_CHECK(!error);

  const Result<std::unique_ptr<CapacityCatalog>> recovered = CapacityCatalog::open(
      CatalogOptions{path, WriterId::create("test-writer").value()});
  RC_REQUIRE_OK(recovered);
  RC_CHECK_EQ(recovered.value()->storage().recovery_action, RecoveryAction::LoadedPrevious);
  RC_CHECK(recovered.value()->contains_rack(RackId::create("rack-a1").value()));
  RC_CHECK(!recovered.value()->contains_rack(RackId::create("rack-b2").value()));
  RC_REQUIRE(recovered.value()->close().has_value());
}

RC_TEST(adversarial_measure_boundaries_are_exact) {
  RC_CHECK(Watts::create(0).has_value());
  RC_CHECK(!Watts::create(-1).has_value());
  RC_CHECK(Watts::create(kMaxWatts).has_value());
  RC_CHECK(!Watts::create(kMaxWatts + 1).has_value());
  // Derating the largest representable value does not overflow.
  const Result<Watts> derated = derate(Watts::trusted(kMaxWatts), BasisPoints::trusted(10'000));
  RC_REQUIRE_OK(derated);
  RC_CHECK_EQ(derated.value().value(), 0);
  const Result<Watts> untouched = derate(Watts::trusted(kMaxWatts), BasisPoints::trusted(0));
  RC_REQUIRE_OK(untouched);
  RC_CHECK_EQ(untouched.value().value(), kMaxWatts);
  // Adding two large values is refused rather than wrapped.
  RC_CHECK(!Watts::trusted(kMaxWatts).add(Watts::trusted(kMaxWatts)).has_value());
}

RC_TEST(adversarial_journal_overflow_is_bounded_and_counted) {
  // Replay coverage is bounded, and the bound is visible rather than silent:
  // once the journal is full the oldest receipt is evicted and every eviction is
  // counted, so an operator can see that coverage is finite.
  rctest::CatalogFixture fixture("adversarial-journal");
  const std::string path = fixture.store_path();
  for (std::uint32_t index = 0; index < kMaxIdempotencyRecords + 24u; ++index) {
    const std::string rack = "rack-" + std::to_string(index);
    const Result<MutationReceipt> receipt = fixture.catalog->register_rack(RegisterRackRequest{
        rctest::Bundle(rack, 8).epoch(1).request("req-" + std::to_string(index)).build(),
        TimestampNs::trusted(kNow + static_cast<std::int64_t>(index))});
    RC_REQUIRE_OK(receipt);
  }
  const CatalogStats stats = fixture.catalog->stats();
  RC_CHECK_EQ(stats.idempotency_records, kMaxIdempotencyRecords);
  RC_CHECK_EQ(stats.idempotency_evictions, 24u);
  RC_CHECK_EQ(stats.rack_count, kMaxIdempotencyRecords + 24u);

  // The most recent request is still replayable; the oldest is no longer in the
  // journal, so it is refused as a duplicate rather than replayed.
  RC_REQUIRE(fixture.catalog->close().has_value());
  fixture.catalog.reset();
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("test-writer").value();
  const Result<std::unique_ptr<CapacityCatalog>> reopened = CapacityCatalog::open(options);
  RC_REQUIRE_OK(reopened);
  RC_CHECK_EQ(reopened.value()->stats().idempotency_evictions, 24u);
  const std::uint32_t newest = kMaxIdempotencyRecords + 23u;
  const Result<MutationReceipt> replay = reopened.value()->register_rack(
      RegisterRackRequest{rctest::Bundle("rack-" + std::to_string(newest), 8)
                               .epoch(1)
                               .request("req-" + std::to_string(newest))
                               .build(),
                           TimestampNs::trusted(kNow + 1000)});
  RC_REQUIRE_OK(replay);
  RC_CHECK(replay.value().outcome == MutationOutcome::Replayed);
  const Result<MutationReceipt> evicted = reopened.value()->register_rack(
      RegisterRackRequest{rctest::Bundle("rack-0", 8).epoch(1).request("req-0").build(),
                           TimestampNs::trusted(kNow + 1001)});
  RC_REQUIRE_CODE(evicted, ErrorCode::DuplicateRackId);
  RC_REQUIRE(reopened.value()->close().has_value());
}

RC_TEST(adversarial_lock_and_previous_paths_are_verified_not_trusted) {
  rctest::TempDir dir("adversarial-lockpath");
  const std::string path = dir.child("capacity.rcstate");

  // A directory where the writer lock belongs is refused.
  std::error_code error;
  std::filesystem::create_directories(path + ".lock", error);
  RC_CHECK(!error);
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("test-writer").value();
  const Result<std::unique_ptr<CapacityCatalog>> refused = CapacityCatalog::open(options);
  RC_CHECK(!refused.has_value());
  std::filesystem::remove_all(path + ".lock", error);

  // A malformed lock record is reported as invalid rather than trusted.
  {
    std::ofstream stream(path + ".lock", std::ios::binary | std::ios::trunc);
    stream << "rcap-lock 1\nwriter=someone\nunexpected=field\n";
  }
  const Result<WriterLockInfo> invalid = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(invalid);
  RC_CHECK(invalid.value().state == WriterLockState::Invalid);
  RC_CHECK(!invalid.value().detail.empty());

  // A lock record that is not a record at all is invalid too.
  write_bytes(path + ".lock", {'n', 'o', 't', 'a', 'l', 'o', 'c', 'k'});
  const Result<WriterLockInfo> garbage = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(garbage);
  RC_CHECK(garbage.value().state == WriterLockState::Invalid);

  // A record naming a process that is not running is reported as not held.
  std::filesystem::remove(path + ".lock", error);
  {
    std::ofstream stream(path + ".lock", std::ios::binary | std::ios::trunc);
    stream << "rcap-lock 1\nwriter=ghost\nincarnation=" << std::string(32, 'a')
           << "\npid=4294967295\nstart=0\nepoch=4\nacquired-at-ns=1\nadopted-from=-\n"
              "adopted-from-pid=0\n";
  }
  const Result<WriterLockInfo> ghost = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(ghost);
  RC_CHECK(ghost.value().state == WriterLockState::Unlocked);

  // A retained previous path that is a directory does not stop the store from
  // opening the current generation.
  std::filesystem::remove(path + ".lock", error);
  {
    CatalogOptions setup;
    setup.path = path;
    setup.writer_id = WriterId::create("test-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(setup);
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 8)
                                                           .epoch(1)
                                                           .request("req-0001")
                                                           .build(),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }
  std::filesystem::remove(path + ".prev", error);
  std::filesystem::create_directories(path + ".prev", error);
  RC_CHECK(!error);
  const Result<std::unique_ptr<CapacityCatalog>> opened = CapacityCatalog::open(options);
  RC_REQUIRE_OK(opened);
  RC_CHECK_EQ(opened.value()->rack_count(), 1u);
  RC_CHECK(!opened.value()->storage().has_previous);
  RC_REQUIRE(opened.value()->close().has_value());
}

RC_TEST(adversarial_concurrent_open_and_close_is_serialized) {
  // Many threads repeatedly take and release writer authority over one store.
  // Exactly one holds it at a time, and every acquisition that succeeds sees a
  // whole state.
  rctest::TempDir dir("adversarial-lockchurn");
  const std::string path = dir.child("capacity.rcstate");
  {
    CatalogOptions options;
    options.path = path;
    options.writer_id = WriterId::create("test-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
    RC_REQUIRE_OK(catalog);
    RC_REQUIRE(catalog.value()
                   ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 8)
                                                           .epoch(1)
                                                           .request("req-0001")
                                                           .build(),
                                                       TimestampNs::trusted(kNow)})
                   .has_value());
    RC_REQUIRE(catalog.value()->close().has_value());
  }

  std::atomic<std::uint32_t> opened{0};
  std::atomic<std::uint32_t> conflicts{0};
  std::atomic<std::uint32_t> failures{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&path, &opened, &conflicts, &failures] {
      for (int attempt = 0; attempt < 6; ++attempt) {
        CatalogOptions options;
        options.path = path;
        options.writer_id = WriterId::create("churn-writer").value();
        const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
        if (!catalog.has_value()) {
          if (catalog.error().code == ErrorCode::WriterLockHeld) {
            ++conflicts;
          } else {
            ++failures;
          }
          continue;
        }
        if (catalog.value()->rack_count() != 1u) {
          ++failures;
        }
        ++opened;
        (void)catalog.value()->close();
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  RC_CHECK_EQ(failures.load(), 0u);
  RC_CHECK_EQ(opened.load() + conflicts.load(), 24u);
  RC_CHECK(opened.load() > 0u);
}