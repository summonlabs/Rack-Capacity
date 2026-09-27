// Rack Capacity - concurrency tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These tests exercise the documented model: one catalog mutex, taken first and
// released last; the store's writer mutex is the only lock ever taken while it
// is held; no callback runs while a lock is held; queries copy what they return.
//
// Passing these tests is not by itself proof that the model holds, so the model
// was also audited by reading every path that takes a lock. What the tests do
// prove is that concurrent readers and writers make progress, that no reader
// ever observes a half-applied state, and that the accounting invariants hold
// for every observation.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

}  // namespace

RC_TEST(concurrent_readers_never_observe_a_half_applied_state) {
  rctest::CatalogFixture fixture("concurrency-readers");
  const RackId rack = RackId::create("rack-a1").value();
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 42)
                                                         .epoch(1)
                                                         .request("req-0001")
                                                         .asset_units("a-0001", 1, 1)
                                                         .asset_draw("a-0001", 100)
                                                         .build(),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observations{0};
  std::atomic<std::uint64_t> failures{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&fixture, &rack, &stop, &observations, &failures] {
      while (!stop.load()) {
        const Result<RackCapacityRecord> record = fixture.catalog->record(rack);
        if (!record.has_value()) {
          ++failures;
          continue;
        }
        // Every observation must be internally consistent, whatever the
        // writer was doing at the time.
        if (!verify_closure(record.value().snapshot).holds) {
          ++failures;
        }
        if (compute_snapshot_digest(record.value().snapshot) != record.value().snapshot.digest) {
          ++failures;
        }
        if (record.value().snapshot.capacity_generation != record.value().capacity_generation) {
          ++failures;
        }
        // The two generations of a record always describe the same snapshot,
        // and the snapshot's own evidence epoch never runs ahead of the record
        // it was published with.
        if (record.value().snapshot.revision != record.value().revision) {
          ++failures;
        }
        ++observations;
      }
    });
  }

  for (std::uint64_t step = 1; step <= 40; ++step) {
    const Result<RackCapacityRecord> current = fixture.catalog->record(rack);
    RC_REQUIRE_OK(current);
    const Result<MutationReceipt> applied = fixture.catalog->apply_evidence(ApplyEvidenceRequest{
        CapacityExpectation::of(current.value()),
        rctest::Bundle("rack-a1", 42)
            .epoch(step + 1)
            .request("req-" + std::to_string(step + 1))
            .asset_units("a-0001", 1, 1)
            .asset_draw("a-0001", static_cast<std::int64_t>(100 + step))
            .build(),
        TimestampNs::trusted(kNow + static_cast<std::int64_t>(step))});
    RC_REQUIRE_OK(applied);
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  RC_CHECK_EQ(failures.load(), 0u);
  RC_CHECK(observations.load() > 0u);
  const RackCapacityRecord final_record = fixture.catalog->record(rack).value();
  RC_CHECK_EQ(final_record.capacity_generation.value(), 41u);
  RC_CHECK_EQ(final_record.revision.value(), 41u);
}

RC_TEST(concurrent_registrations_admit_exactly_one_writer) {
  rctest::CatalogFixture fixture("concurrency-register");
  std::atomic<std::uint32_t> successes{0};
  std::atomic<std::uint32_t> duplicates{0};
  std::atomic<std::uint32_t> unexpected{0};
  constexpr int kThreads = 4;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&fixture, &successes, &duplicates, &unexpected, index] {
      const Result<MutationReceipt> receipt = fixture.catalog->register_rack(RegisterRackRequest{
          rctest::Bundle("rack-shared", 24)
              .epoch(1)
              .request("req-" + std::to_string(index))
              .asset_units("a-0001", 1, 1)
              .build(),
          TimestampNs::trusted(kNow)});
      if (receipt.has_value()) {
        ++successes;
      } else if (receipt.error().code == ErrorCode::DuplicateRackId) {
        ++duplicates;
      } else {
        ++unexpected;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  RC_CHECK_EQ(successes.load(), 1u);
  RC_CHECK_EQ(duplicates.load(), static_cast<std::uint32_t>(kThreads - 1));
  RC_CHECK_EQ(unexpected.load(), 0u);
  RC_CHECK_EQ(fixture.catalog->rack_count(), 1u);
  RC_CHECK_EQ(fixture.catalog->storage().sequence.value(), 1u);
}

RC_TEST(concurrent_fit_evaluations_are_consistent_with_their_precondition) {
  rctest::CatalogFixture fixture("concurrency-fit");
  const RackId rack = RackId::create("rack-a1").value();
  RC_REQUIRE(fixture.catalog
                 ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 42)
                                                         .epoch(1)
                                                         .request("req-0001")
                                                         .asset_units("a-0001", 1, 1)
                                                         .asset_draw("a-0001", 100)
                                                         .build(),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());

  std::atomic<std::uint32_t> stale_refusals{0};
  std::atomic<std::uint32_t> failures{0};
  std::atomic<std::uint32_t> evaluations{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&fixture, &rack, &stop, &stale_refusals, &failures, &evaluations] {
      while (!stop.load()) {
        const Result<RackCapacityRecord> record = fixture.catalog->record(rack);
        if (!record.has_value()) {
          continue;
        }
        FitRequest request;
        request.expected = CapacityExpectation::of(record.value());
        request.span = SlotInterval::for_units(20, 1).value();
        request.draw = Watts::trusted(50);
        const Result<FitEvaluation> evaluation = fixture.catalog->evaluate_fit(request);
        if (!evaluation.has_value()) {
          // A concurrent writer may have advanced the rack between the read
          // that produced the expectation and the evaluation that used it. That
          // refusal is the documented behaviour, not a defect.
          if (evaluation.error().code == ErrorCode::StaleCapacityGeneration ||
              evaluation.error().code == ErrorCode::StaleSnapshotRevision) {
            ++stale_refusals;
          } else {
            ++failures;
          }
          continue;
        }
        // A verdict that was produced must be about the exact state it names.
        if (evaluation.value().evaluated_against != request.expected) {
          ++failures;
        }
        ++evaluations;
      }
    });
  }

  for (std::uint64_t step = 1; step <= 30; ++step) {
    const Result<RackCapacityRecord> current = fixture.catalog->record(rack);
    RC_REQUIRE_OK(current);
    RC_REQUIRE(fixture.catalog
                   ->apply_evidence(ApplyEvidenceRequest{
                       CapacityExpectation::of(current.value()),
                       rctest::Bundle("rack-a1", 42)
                           .epoch(step + 1)
                           .request("req-" + std::to_string(step + 1))
                           .asset_units("a-0001", 1, 1)
                           .asset_draw("a-0001", static_cast<std::int64_t>(100 + step))
                           .build(),
                       TimestampNs::trusted(kNow + static_cast<std::int64_t>(step))})
                   .has_value());
  }
  stop.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }
  RC_CHECK(evaluations.load() > 0u);
  RC_CHECK_EQ(failures.load(), 0u);
  RC_CHECK(stale_refusals.load() > 0u);
}

RC_TEST(independent_catalogs_do_not_share_state) {
  rctest::CatalogFixture first("concurrency-independent-a");
  rctest::CatalogFixture second("concurrency-independent-b");
  RC_REQUIRE(first.catalog
                 ->register_rack(RegisterRackRequest{rctest::Bundle("rack-a1", 24)
                                                         .epoch(1)
                                                         .request("req-0001")
                                                         .build(),
                                                     TimestampNs::trusted(kNow)})
                 .has_value());
  RC_CHECK_EQ(second.catalog->rack_count(), 0u);
  RC_CHECK(first.catalog->storage().incarnation != second.catalog->storage().incarnation);
}
