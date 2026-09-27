// Rack Capacity - real multiprocess tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Everything here runs a second, independent operating-system process. Writer
// authority, release on process death and recovery after a real crash are only
// ever proved this way; a thread cannot demonstrate any of them.

#include <chrono>
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

const char* kChildPath = RC_CHILD_EXECUTABLE;

[[nodiscard]] std::string child() { return rctest::helper_executable(kChildPath); }

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

[[nodiscard]] bool store_opens(const std::string& path) {
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("parent-writer").value();
  options.create_if_missing = true;
  const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
  if (!catalog.has_value()) {
    return false;
  }
  (void)catalog.value()->close();
  return true;
}

}  // namespace

RC_TEST(multiprocess_writer_authority_is_exclusive_across_processes) {
  rctest::TempDir dir("multiprocess-exclusive");
  const std::string path = dir.child("capacity.rcstate");
  const std::string ready = dir.child("child.ready");

  rctest::ChildProcess holder =
      rctest::ChildProcess::start(child(), {"hold", "--path", path, "--ready-file", ready,
                                            "--hold-ms", "20000"},
                                  dir.child("child.out"));
  RC_REQUIRE(holder.running());
  RC_CHECK(rctest::wait_for_file(ready));

  // A second process cannot take writer authority while the first holds it.
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("parent-writer").value();
  const Result<std::unique_ptr<CapacityCatalog>> refused = CapacityCatalog::open(options);
  RC_REQUIRE_CODE(refused, ErrorCode::WriterLockHeld);

  // The lock record names the live holder.
  const Result<WriterLockInfo> info = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(info);
  RC_CHECK(info.value().state == WriterLockState::HeldByAnotherProcess);
  RC_CHECK(info.value().pid != 0u);
  RC_CHECK(info.value().writer_id == WriterId::create("child-writer").value());

  // Killing the holder as the operating system would on a crash releases the
  // lock, and nothing in the store has to be repaired first.
  holder.kill();
  RC_CHECK(store_opens(path));

  const Result<WriterLockInfo> after = CapacityStore::query_writer_lock(path);
  RC_REQUIRE_OK(after);
  RC_CHECK(after.value().state == WriterLockState::Unlocked);
}

RC_TEST(multiprocess_lock_is_released_when_the_holder_exits_normally) {
  rctest::TempDir dir("multiprocess-exit");
  const std::string path = dir.child("capacity.rcstate");
  const std::string ready = dir.child("child.ready");
  rctest::ChildProcess holder = rctest::ChildProcess::start(
      child(), {"hold", "--path", path, "--ready-file", ready, "--hold-ms", "50"},
      dir.child("child.out"));
  RC_REQUIRE(holder.running());
  RC_CHECK(rctest::wait_for_file(ready));
  const rctest::ProcessResult result = holder.wait();
  RC_CHECK_EQ(result.exit_code, 0);
  RC_CHECK(store_opens(path));
}

RC_TEST(multiprocess_child_publication_is_visible_to_the_parent) {
  rctest::TempDir dir("multiprocess-publish");
  const std::string path = dir.child("capacity.rcstate");
  const std::string ready = dir.child("child.ready");

  const rctest::ProcessResult published = rctest::run_process(
      child(), {"publish", "--path", path, "--rack", "rack-child", "--request", "req-child",
                "--ready-file", ready});
  RC_CHECK_EQ(published.exit_code, 0);

  // A real restart of the reader: a new process opens what the child published.
  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("parent-writer").value();
  const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
  RC_REQUIRE_OK(catalog);
  RC_CHECK_EQ(catalog.value()->rack_count(), 1u);
  RC_CHECK(catalog.value()->contains_rack(RackId::create("rack-child").value()));
  RC_CHECK_EQ(catalog.value()->storage().sequence.value(), 1u);
  RC_CHECK_EQ(catalog.value()->storage().recovery_action, RecoveryAction::LoadedCurrent);
  const RackCapacityRecord record =
      catalog.value()->record(RackId::create("rack-child").value()).value();
  RC_CHECK(record.standing == RecoveryStanding::PendingRevalidation);
  RC_REQUIRE(catalog.value()->close().has_value());

  // The child process itself reports the same sequence when it reopens.
  const rctest::ProcessResult reopened =
      rctest::run_process(child(), {"open", "--path", path});
  RC_CHECK_EQ(reopened.exit_code, 0);
  RC_CHECK(reopened.output.find("sequence=1") != std::string::npos);
  RC_CHECK(reopened.output.find("racks=1") != std::string::npos);
}

RC_TEST(multiprocess_crash_during_publication_keeps_exactly_one_whole_generation) {
  const char* stages[] = {"after-lock-acquired",   "after-temp-created", "after-temp-written",
                          "after-temp-synced",     "after-previous-published",
                          "before-publish-rename", "after-publish-rename",
                          "after-temp-retired"};
  for (const char* stage : stages) {
    rctest::TempDir dir("multiprocess-crash");
    const std::string path = dir.child("capacity.rcstate");
    const std::string ready = dir.child("child.ready");

    // Establish one published generation first, so the crash has something to
    // preserve and something to supersede.
    const rctest::ProcessResult seeded = rctest::run_process(
        child(), {"publish", "--path", path, "--rack", "rack-seed", "--request", "req-seed"});
    RC_CHECK_EQ(seeded.exit_code, 0);
    const std::vector<std::uint8_t> before = read_bytes(path);
    RC_REQUIRE(!before.empty());

    const std::string crash_ready = dir.child("crash.ready");
    const rctest::ProcessResult crashed =
        rctest::run_process(child(), {"crash", "--path", path, "--stage", stage, "--ready-file",
                                      crash_ready});
    if (crashed.exit_code == 4) {
      std::cout << "    the injection at " << stage << " never fired\n";
      RC_CHECK(false);
      continue;
    }
    // The child died abnormally, and it reached the stage it was told to die at.
    RC_CHECK(crashed.exit_code != 0);
    RC_CHECK(std::filesystem::exists(crash_ready));

    // Whatever the crash left behind, a fresh process must see exactly one
    // whole generation: either the seed or a state that decodes completely.
    CatalogOptions options;
    options.path = path;
    options.writer_id = WriterId::create("parent-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
    RC_REQUIRE_OK(catalog);
    const std::size_t racks = catalog.value()->rack_count();
    RC_CHECK(racks <= 1u);
    if (racks == 1u) {
      const RackCapacityRecord record = catalog.value()->records()[0];
      RC_CHECK(verify_closure(record.snapshot).holds);
      RC_CHECK_EQ(record.snapshot.digest, compute_snapshot_digest(record.snapshot));
    }
    // No temporary file survives, and the store is immediately writable again.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir.path())) {
      const std::string name = entry.path().filename().string();
      RC_CHECK(name.find(".tmp-") == std::string::npos);
    }
    RC_REQUIRE(catalog.value()->close().has_value());

    // The store still accepts a publication after the crash.
    const rctest::ProcessResult after = rctest::run_process(
        child(), {"publish", "--path", path, "--rack", "rack-after", "--request",
                  std::string("req-after-") + stage});
    RC_CHECK_EQ(after.exit_code, 0);
    RC_CHECK(store_opens(path));
  }
}

RC_TEST(multiprocess_forced_release_requires_the_holder_to_be_gone) {
  rctest::TempDir dir("multiprocess-force");
  const std::string path = dir.child("capacity.rcstate");
  const std::string ready = dir.child("child.ready");

  rctest::ChildProcess holder = rctest::ChildProcess::start(
      child(), {"hold", "--path", path, "--ready-file", ready, "--hold-ms", "20000"},
      dir.child("child.out"));
  RC_REQUIRE(holder.running());
  RC_CHECK(rctest::wait_for_file(ready));

  // Authority cannot be taken from a live writer: the operating-system lock is
  // held by that process, so an operator release is refused with the reason.
  const Result<WriterLockInfo> refused = CapacityStore::force_release(
      path, WriterId::create("operator-1").value(), static_cast<std::uint64_t>(kNow));
  RC_REQUIRE_CODE(refused, ErrorCode::WriterLockHeld);

  holder.kill();

  // Once the holder is gone, an operator release succeeds and advances the
  // store epoch, so any publication planned against the previous epoch is
  // refused.
  const Result<WriterLockInfo> released = CapacityStore::force_release(
      path, WriterId::create("operator-1").value(), static_cast<std::uint64_t>(kNow));
  RC_REQUIRE_OK(released);
  RC_CHECK_EQ(released.value().epoch.value(), 2u);
  RC_CHECK(released.value().adopted_from_pid != 0u);

  const rctest::ProcessResult reopened = rctest::run_process(child(), {"open", "--path", path});
  RC_CHECK_EQ(reopened.exit_code, 0);
  RC_CHECK(reopened.output.find("sequence=0") != std::string::npos);
}

RC_TEST(multiprocess_two_writers_serialize_on_the_store) {
  rctest::TempDir dir("multiprocess-serialize");
  const std::string path = dir.child("capacity.rcstate");

  // Two children publish into the same store, one after the other. Each must
  // see the other's generation.
  const rctest::ProcessResult first = rctest::run_process(
      child(), {"publish", "--path", path, "--rack", "rack-first", "--request", "req-first"});
  RC_CHECK_EQ(first.exit_code, 0);
  const rctest::ProcessResult second = rctest::run_process(
      child(), {"publish", "--path", path, "--rack", "rack-second", "--request", "req-second"});
  RC_CHECK_EQ(second.exit_code, 0);

  CatalogOptions options;
  options.path = path;
  options.writer_id = WriterId::create("parent-writer").value();
  const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
  RC_REQUIRE_OK(catalog);
  RC_CHECK_EQ(catalog.value()->rack_count(), 2u);
  RC_CHECK_EQ(catalog.value()->storage().sequence.value(), 2u);
  RC_REQUIRE(catalog.value()->close().has_value());

  // And the other way round: a parent that holds the store blocks a child.
  rctest::ChildProcess holder = rctest::ChildProcess::start(
      child(), {"hold", "--path", path, "--ready-file", dir.child("second.ready"), "--hold-ms",
                "20000"},
      dir.child("child.out"));
  RC_REQUIRE(holder.running());
  RC_CHECK(rctest::wait_for_file(dir.child("second.ready")));
  const rctest::ProcessResult blocked = rctest::run_process(
      child(), {"publish", "--path", path, "--rack", "rack-blocked", "--request", "req-blocked"});
  RC_CHECK(blocked.exit_code != 0);
  RC_CHECK(blocked.output.find("writer_lock_held") != std::string::npos);
  holder.kill();
}
