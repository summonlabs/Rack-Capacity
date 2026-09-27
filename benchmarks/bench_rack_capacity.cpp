// Rack Capacity - benchmarks of completed operations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Methodology. Every measurement times a *completed* operation: an evaluation
// that returned a snapshot, a fit that returned a verdict, a publication that
// was flushed to storage and verified. Nothing is timed from submission, no
// work is left outstanding when the clock stops, and no operation is retried
// inside a timed region.
//
// Durable measurements include the flush and the read-back verification,
// because that is what a durable publication costs. They are reported
// separately from in-memory measurements and are never mixed with them.
//
// Every workload here is SYNTHETIC: the racks, occupants and envelopes are
// generated in this process and do not describe real hardware. The generator is
// seeded and the seed is printed, so a run is reproducible.
//
// Each benchmark reports the median of `--repetitions` runs after a warm-up,
// along with the minimum and maximum, and verifies the state it created before
// reporting. All stores it creates are removed before the process exits.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;
constexpr std::uint64_t kSeed = 0x5EED1234ull;

struct Timing {
  std::string name{};
  std::string unit{};
  double median = 0;
  double minimum = 0;
  double maximum = 0;
  std::size_t iterations = 0;
};

std::vector<Timing> g_results;

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  std::uint64_t state_;
};

[[nodiscard]] RackCapacityInputs synthetic_rack(const std::string& rack,
                                                std::uint32_t unit_count,
                                                std::uint32_t occupant_count,
                                                std::uint64_t seed) {
  Rng rng(seed);
  RackCapacityInputs inputs;
  inputs.composition.rack = RackId::create(rack).value();
  inputs.composition.generation = RackCompositionGeneration::trusted(1);
  inputs.composition.unit_count = unit_count;
  inputs.composition.serviceability.front_clearance = Millimetres::trusted(900);
  inputs.composition.serviceability.rear_clearance = Millimetres::trusted(700);
  inputs.composition.reference.source = EvidenceSource::RackRegistry;
  inputs.composition.reference.evidence = EvidenceId::create("ev-composition").value();
  inputs.composition.reference.version = 1;
  inputs.composition.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.policy.reference.policy = PolicyId::create("synthetic-policy").value();
  inputs.policy.reference.version = 1;

  PowerCapacityEvidence power;
  power.feed_count = 2;
  power.watts_per_feed = Watts::trusted(16000);
  power.redundancy = RedundancyMode::N2;
  power.derate = BasisPoints::trusted(2000);
  power.reference.source = EvidenceSource::PowerCapacity;
  power.reference.evidence = EvidenceId::create("ev-power").value();
  power.reference.version = 1;
  power.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.power = power;

  CoolingCapacityEvidence cooling;
  cooling.nominal_heat_rejection = Watts::trusted(40000);
  cooling.derate = BasisPoints::trusted(1000);
  cooling.reference.source = EvidenceSource::CoolingCapacity;
  cooling.reference.evidence = EvidenceId::create("ev-cooling").value();
  cooling.reference.version = 1;
  cooling.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.cooling = cooling;

  WeightCapacityEvidence weight;
  weight.static_load_limit = Grams::trusted(1'200'000);
  weight.per_unit_point_load_limit = Grams::trusted(250'000);
  weight.reference.source = EvidenceSource::CoolingCapacity;
  weight.reference.evidence = EvidenceId::create("ev-weight").value();
  weight.reference.version = 1;
  weight.reference.observed_at = TimestampNs::trusted(kNow);
  inputs.weight = weight;

  // Occupants are placed at unit boundaries until the requested count is
  // reached or the rack is full.
  std::uint32_t unit = 1;
  for (std::uint32_t index = 0; index < occupant_count && unit + 1 <= unit_count; ++index) {
    const std::uint32_t height = 1u + static_cast<std::uint32_t>(rng.below(3));
    if (unit + height - 1u > unit_count) {
      break;
    }
    AssetOccupancyEvidence asset;
    asset.asset = AssetId::create("a-" + std::to_string(100000u + index)).value();
    asset.span = SlotInterval::for_units(unit, height).value();
    asset.presence = PresenceState::Present;
    asset.nameplate_draw = Watts::trusted(300 + static_cast<std::int64_t>(rng.below(900)));
    asset.declared_heat_rejection = asset.nameplate_draw;
    asset.mass = Grams::trusted(8000 + static_cast<std::int64_t>(rng.below(30000)));
    asset.reference.source = EvidenceSource::AssetRegistry;
    asset.reference.evidence = EvidenceId::create("ev-" + asset.asset.text()).value();
    asset.reference.version = 1;
    asset.reference.observed_at = TimestampNs::trusted(kNow);
    inputs.assets.push_back(asset);
    unit += height;
  }

  inputs.epoch = EvidenceEpoch::trusted(1);
  inputs.captured_at = TimestampNs::trusted(kNow);
  inputs.actor = ActorId::create("benchmark").value();
  inputs.request = RequestId::create("req-benchmark").value();
  return inputs;
}

void report(const Timing& timing) {
  std::cout << "  " << timing.name << "\n";
  std::cout << "    median " << timing.median << " " << timing.unit << "   min " << timing.minimum
            << "   max " << timing.maximum << "   (" << timing.iterations << " runs)\n";
}

[[nodiscard]] double median_of(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const std::size_t middle = samples.size() / 2;
  if (samples.size() % 2 == 1) {
    return samples[middle];
  }
  return (samples[middle - 1] + samples[middle]) / 2.0;
}

template <typename Operation>
void measure(const std::string& name, const std::string& unit, std::size_t runs,
             std::size_t warmup, Operation operation) {
  for (std::size_t index = 0; index < warmup; ++index) {
    operation();
  }
  std::vector<double> samples;
  samples.reserve(runs);
  for (std::size_t index = 0; index < runs; ++index) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto finish = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(finish - start).count());
  }
  Timing timing;
  timing.name = name;
  timing.unit = unit;
  timing.median = median_of(samples);
  timing.minimum = *std::min_element(samples.begin(), samples.end());
  timing.maximum = *std::max_element(samples.begin(), samples.end());
  timing.iterations = runs;
  g_results.push_back(timing);
  report(timing);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t runs = 200;
  std::size_t durable_runs = 40;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--runs" && index + 1 < argc) {
      runs = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (argument == "--durable-runs" && index + 1 < argc) {
      durable_runs = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else {
      std::cout << "usage: bench_rack_capacity [--runs N] [--durable-runs N]\n";
      return 2;
    }
  }
  if (runs == 0 || durable_runs == 0) {
    std::cout << "the repetition counts must be positive\n";
    return 2;
  }

  std::cout << "Rack Capacity " << version_string() << " benchmarks\n";
  std::cout << "workload: SYNTHETIC (generated in this process; not measured hardware)\n";
  std::cout << "generator seed: " << kSeed << ", timed runs: " << runs
            << ", durable runs: " << durable_runs << "\n";
  std::cout << "method: median of completed operations after warm-up; durable operations "
               "include the flush and the read-back verification\n\n";

  // --- snapshot evaluation -------------------------------------------------
  const std::uint32_t sizes[] = {8, 32, 96};
  for (const std::uint32_t occupant_count : sizes) {
    const RackCapacityInputs inputs =
        synthetic_rack("rack-bench", 48, occupant_count, kSeed + occupant_count);
    const Status valid = validate_inputs(inputs);
    if (!valid.has_value()) {
      std::cout << "the synthetic bundle is invalid: " << describe(valid.error()) << "\n";
      return 1;
    }
    std::uint64_t evaluations = 0;
    std::uint64_t failed_verifications = 0;
    measure("snapshot evaluation, " + std::to_string(inputs.assets.size()) + " occupants", "us",
            runs, 20, [&inputs, &evaluations, &failed_verifications] {
              const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
                  inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
                  TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
              if (!snapshot.has_value()) {
                std::cout << "evaluation failed inside a timed region\n";
                std::abort();
              }
              ++evaluations;
            });
    // The measured operation is verified afterwards rather than trusted: every
    // completed evaluation must have produced a snapshot that closes exactly.
    for (std::size_t index = 0; index < runs; ++index) {
      const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
          inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
          TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
      if (!snapshot.has_value() || snapshot.value().digest.is_zero() ||
          !verify_closure(snapshot.value()).holds) {
        ++failed_verifications;
      }
    }
    if (evaluations == 0 || failed_verifications != 0) {
      std::cout << "the measured operation did not verify afterwards\n";
      return 1;
    }
    std::cout << "    (every completed evaluation produced a snapshot that verified: "
              << runs << " checked)\n";
  }

  // --- fit evaluation ------------------------------------------------------
  {
    const RackCapacityInputs inputs = synthetic_rack("rack-bench", 48, 32, kSeed);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    if (!snapshot.has_value()) {
      std::cout << "the benchmark rack could not be evaluated\n";
      return 1;
    }
    std::uint32_t verdicts = 0;
    measure("fit evaluation of one candidate", "us", runs, 20, [&snapshot, &verdicts] {
      FitRequest request;
      request.expected = CapacityExpectation::of(snapshot.value());
      request.unit_height = 1;
      request.draw = Watts::trusted(600);
      request.heat_rejection = Watts::trusted(600);
      request.mass = Grams::trusted(15000);
      const Result<FitEvaluation> evaluation = evaluate_fit(request, snapshot.value());
      if (!evaluation.has_value()) {
        std::cout << "fit evaluation failed inside a timed region\n";
        std::abort();
      }
      verdicts += static_cast<std::uint32_t>(evaluation.value().verdict);
    });
    std::cout << "    (verdict checksum " << verdicts << ")\n";
  }

  // --- serialization -------------------------------------------------------
  {
    const RackCapacityInputs inputs = synthetic_rack("rack-bench", 48, 64, kSeed + 7);
    const Result<RackCapacitySnapshot> snapshot = evaluate_capacity(
        inputs, CapacityGeneration::trusted(1), SnapshotRevision::trusted(1),
        TimestampNs::trusted(kNow), EvidenceFreshness::Fresh);
    if (!snapshot.has_value()) {
      std::cout << "the benchmark rack could not be evaluated\n";
      return 1;
    }
    std::uint64_t bytes = 0;
    measure("canonical encode plus digest of one snapshot", "us", runs, 20,
            [&snapshot, &bytes] { bytes = compute_snapshot_digest(snapshot.value()).to_hex().size(); });
    std::cout << "    (digest rendering is " << bytes << " characters)\n";
  }

  // --- durable publication -------------------------------------------------
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "rcap-benchmark";
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const std::string store_path = (directory / "capacity.rcstate").string();
    const RackCapacityInputs inputs = synthetic_rack("rack-bench", 48, 24, kSeed + 3);

    CatalogOptions options;
    options.path = store_path;
    options.writer_id = WriterId::create("benchmark-writer").value();
    const Result<std::unique_ptr<CapacityCatalog>> catalog = CapacityCatalog::open(options);
    if (!catalog.has_value()) {
      std::cout << "the benchmark store could not be opened: " << describe(catalog.error())
                << "\n";
      return 1;
    }

    std::uint64_t epoch = 1;
    std::uint64_t request_counter = 0;
    std::uint64_t published = 0;
    measure("durable register of one rack (flush, verify, publish)", "us", durable_runs, 2,
            [&catalog, &inputs, &epoch, &request_counter, &published] {
              ++epoch;
              ++request_counter;
              RackCapacityInputs bundle = inputs;
              bundle.epoch = EvidenceEpoch::trusted(epoch);
              bundle.request = RequestId::create("req-" + std::to_string(request_counter)).value();
              bundle.composition.rack =
                  RackId::create("rack-bench-" + std::to_string(request_counter)).value();
              for (AssetOccupancyEvidence& asset : bundle.assets) {
                asset.reference.evidence =
                    EvidenceId::create("ev-" + std::to_string(request_counter) + "-" +
                                       asset.asset.text())
                        .value();
              }
              const Result<MutationReceipt> receipt = catalog.value()->register_rack(
                  RegisterRackRequest{bundle, TimestampNs::trusted(kNow + static_cast<std::int64_t>(
                                                                 request_counter))});
              if (!receipt.has_value()) {
                std::cout << "publication failed inside a timed region: "
                          << describe(receipt.error()) << "\n";
                std::abort();
              }
              ++published;
            });
    std::cout << "    (published racks " << published << ", store sequence "
              << catalog.value()->storage().sequence.value() << ")\n";

    // Durability is verified after the fact, not assumed from the timing.
    const Result<StateFileInfo> info = CapacityStore::inspect(store_path);
    if (!info.has_value()) {
      std::cout << "the benchmark store does not verify: " << describe(info.error()) << "\n";
      return 1;
    }
    std::cout << "    (the published store verifies: " << info.value().rack_count
              << " racks, " << info.value().byte_size << " bytes)\n";

    measure("durable open and full verification of the store", "us", durable_runs, 2,
            [&store_path] {
              const Result<StateFileInfo> reopened = CapacityStore::inspect(store_path);
              if (!reopened.has_value()) {
                std::cout << "inspection failed inside a timed region\n";
                std::abort();
              }
            });

    (void)catalog.value()->close();
  }

  // --- durable open and recovery -------------------------------------------
  {
    const std::string store_path = (directory / "capacity.rcstate").string();
    // A real open: authority is taken, the state is decoded, every record is
    // validated and closure-checked, and the writer lock record is written.
    std::uint64_t opened = 0;
    measure("durable open with writer authority (decode, validate, lock)", "us", durable_runs, 2,
            [&store_path, &opened] {
              StoreOptions options;
              options.path = store_path;
              options.writer_id = WriterId::create("benchmark-writer").value();
              const Result<std::unique_ptr<CapacityStore>> store = CapacityStore::open(options);
              if (!store.has_value()) {
                std::cout << "open failed inside a timed region: " << describe(store.error())
                          << "\n";
                std::abort();
              }
              ++opened;
              (void)store.value()->close();
            });
    std::cout << "    (completed opens " << opened << ")\n";
  }

  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::cout << "\nresidue: benchmark store removed (" << (error ? error.message() : "clean")
            << ")\n";
  std::cout << "note: these figures measure this process on this machine and are not a promise "
               "about any other machine.\n";
  return 0;
}
