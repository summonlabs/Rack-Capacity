// Rack Capacity - process-level test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program exists so that writer authority, lock release on process death
// and crash recovery can be proved with real operating-system processes rather
// than with threads. It is a test harness and is never installed.
//
//   rc_child hold    --path P --ready-file F [--hold-ms N]
//   rc_child publish --path P --rack R --request Q [--ready-file F]
//   rc_child crash   --path P --stage S --ready-file F
//   rc_child open    --path P [--ready-file F]
//
// Exit codes: 0 for success, 2 for usage, and 100 + the numeric error code for
// a typed refusal, so a parent can assert on the exact reason.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>
#include <windows.h>
#endif

#include "rack_capacity/rack_capacity.hpp"

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000'000ll;

struct Arguments {
  std::string mode{};
  std::string path{};
  std::string ready_file{};
  std::string rack = "rack-child";
  std::string request = "req-child";
  std::string stage{};
  std::int64_t hold_ms = 5000;
};

void write_ready(const std::string& path) {
  if (path.empty()) {
    return;
  }
  std::ofstream stream(path, std::ios::trunc);
  stream << "ready\n";
  stream.flush();
}

[[nodiscard]] int fail(const rackcapacity::CapacityError& error) {
  std::cout << rackcapacity::describe(error) << "\n";
  return 100 + static_cast<int>(rackcapacity::code_value(error.code));
}

[[nodiscard]] bool parse(int argc, char** argv, Arguments& arguments) {
  if (argc < 2) {
    return false;
  }
  arguments.mode = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string key = argv[index];
    const bool has_value = index + 1 < argc;
    if (key == "--path" && has_value) {
      arguments.path = argv[++index];
    } else if (key == "--ready-file" && has_value) {
      arguments.ready_file = argv[++index];
    } else if (key == "--rack" && has_value) {
      arguments.rack = argv[++index];
    } else if (key == "--request" && has_value) {
      arguments.request = argv[++index];
    } else if (key == "--stage" && has_value) {
      arguments.stage = argv[++index];
    } else if (key == "--hold-ms" && has_value) {
      arguments.hold_ms = std::strtoll(argv[++index], nullptr, 10);
    } else {
      return false;
    }
  }
  return !arguments.path.empty();
}


}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  if (!parse(argc, argv, arguments)) {
    std::cout << "usage: rc_child <hold|publish|crash|open> --path P [options]\n";
    return 2;
  }

  rackcapacity::StoreOptions options;
  options.path = arguments.path;
  options.writer_id = rackcapacity::WriterId::create("child-writer").value();
  options.create_if_missing = true;

  if (arguments.mode == "hold") {
    rackcapacity::Result<std::unique_ptr<rackcapacity::CapacityStore>> store =
        rackcapacity::CapacityStore::open(options);
    if (!store.has_value()) {
      return fail(store.error());
    }
    write_ready(arguments.ready_file);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(arguments.hold_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)store.value()->close();
    return 0;
  }

  if (arguments.mode == "open") {
    rackcapacity::Result<std::unique_ptr<rackcapacity::CapacityStore>> store =
        rackcapacity::CapacityStore::open(options);
    if (!store.has_value()) {
      write_ready(arguments.ready_file);
      return fail(store.error());
    }
    write_ready(arguments.ready_file);
    std::cout << "sequence=" << store.value()->sequence().value()
              << " racks=" << store.value()->state().records.size() << "\n";
    (void)store.value()->close();
    return 0;
  }

  if (arguments.mode == "publish") {
    rackcapacity::Result<std::unique_ptr<rackcapacity::CapacityCatalog>> catalog =
        rackcapacity::CapacityCatalog::open(rackcapacity::CatalogOptions{
            arguments.path, rackcapacity::WriterId::create("child-writer").value()});
    if (!catalog.has_value()) {
      write_ready(arguments.ready_file);
      return fail(catalog.error());
    }
    write_ready(arguments.ready_file);
    rackcapacity::RackCapacityInputs inputs;
    {
      rackcapacity::RackCapacityInputs built;
      built.composition.rack = rackcapacity::RackId::create(arguments.rack).value();
      built.composition.generation = rackcapacity::RackCompositionGeneration::trusted(1);
      built.composition.unit_count = 42;
      built.composition.reference.source = rackcapacity::EvidenceSource::RackRegistry;
      built.composition.reference.evidence =
          rackcapacity::EvidenceId::create("ev-composition").value();
      built.composition.reference.version = 1;
      built.composition.reference.observed_at = rackcapacity::TimestampNs::trusted(kNow);
      built.policy.reference.policy = rackcapacity::PolicyId::create("child-policy").value();
      built.policy.reference.version = 1;
      built.epoch = rackcapacity::EvidenceEpoch::trusted(1);
      built.captured_at = rackcapacity::TimestampNs::trusted(kNow);
      built.actor = rackcapacity::ActorId::create("child-actor").value();
      built.request = rackcapacity::RequestId::create(arguments.request).value();
      inputs = built;
    }
    const rackcapacity::Result<rackcapacity::MutationReceipt> receipt =
        catalog.value()->register_rack(
            rackcapacity::RegisterRackRequest{inputs, rackcapacity::TimestampNs::trusted(kNow)});
    if (!receipt.has_value()) {
      return fail(receipt.error());
    }
    std::cout << "published sequence=" << receipt.value().sequence.value() << "\n";
    (void)catalog.value()->close();
    return 0;
  }

  if (arguments.mode == "crash") {
    const rackcapacity::Result<rackcapacity::WriteStage> stage =
        rackcapacity::parse_write_stage(arguments.stage);
    if (!stage.has_value()) {
      return fail(stage.error());
    }
    options.fault_hook = [stage, &arguments](rackcapacity::WriteStage current) {
      if (current == stage.value()) {
        write_ready(arguments.ready_file);
        // A real, abrupt process death in the middle of the publication
        // protocol. No destructor runs, no buffer is flushed, and the operating
        // system releases the exclusive lock because the process is gone.
        //
        // The crash reporting path is disabled first: a crash reporter would
        // raise an interactive window and delay the death, and this harness must
        // die silently and immediately so that the parent observes exactly the
        // store state the protocol left behind.
#if defined(_WIN32)
        ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
        std::abort();
      }
    };
    rackcapacity::Result<std::unique_ptr<rackcapacity::CapacityStore>> store =
        rackcapacity::CapacityStore::open(options);
    if (!store.has_value()) {
      return fail(store.error());
    }
    rackcapacity::CatalogState state;
    state.incarnation = store.value()->incarnation();
    state.epoch = store.value()->epoch();
    state.sequence = store.value()->sequence().next().value();
    const rackcapacity::Result<rackcapacity::StoreSequence> published =
        store.value()->publish(state);
    if (!published.has_value()) {
      return fail(published.error());
    }
    // Reaching this point means the injection never fired, which the parent
    // must be able to tell apart from a successful crash.
    std::cout << "the injected stage was never reached\n";
    return 4;
  }

  std::cout << "unknown mode\n";
  return 2;
}
