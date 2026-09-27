// Rack Capacity - inspection and administration tool.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The tool is deliberately thin: every decision it makes is a library call, and
// every failure it reports is a typed library error rendered verbatim. It never
// repairs state, never guesses a default that the library would refuse, and
// exits with the documented per-fault code so a script can branch without
// parsing text.
//
//   rackcap version
//   rackcap store init        --path P
//   rackcap store inspect     --path P
//   rackcap store verify      --path P
//   rackcap store lock-status --path P
//   rackcap store force-release --path P [--operator ID]
//   rackcap rack list         --path P
//   rackcap rack show         --path P --rack R
//   rackcap rack apply        --path P --spec FILE
//   rackcap rack fit          --path P --rack R (--units N | --span S) [constraints]
//   rackcap rack retire       --path P --rack R [--reason TEXT]
//   rackcap rack revalidate   --path P --rack R [--all]
//   rackcap rack diff         --path P --rack R
//   rackcap rejections        --path P

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "rack_capacity/rack_capacity.hpp"

namespace {

using namespace rackcapacity;

struct Options {
  std::string command{};
  std::string subcommand{};
  std::string path{};
  std::string rack{};
  std::string spec{};
  std::string reason{};
  std::string actor = "rackcap-cli";
  std::string request{};
  std::string operator_id = "rackcap-operator";
  std::optional<std::int64_t> units{};
  std::optional<std::string> span{};
  std::optional<std::int64_t> watts{};
  std::optional<std::int64_t> heat{};
  std::optional<std::int64_t> grams{};
  std::optional<std::int64_t> front_mm{};
  std::optional<std::int64_t> rear_mm{};
  bool service_access = false;
  bool read_only = false;
  bool all = false;
};

void usage() {
  std::cout
      << "rackcap " << version_string() << " - per-rack capacity accounting\n"
      << "\n"
      << "usage:\n"
      << "  rackcap version\n"
      << "  rackcap store init          --path P\n"
      << "  rackcap store inspect       --path P\n"
      << "  rackcap store verify        --path P\n"
      << "  rackcap store lock-status   --path P\n"
      << "  rackcap store force-release --path P [--operator ID]\n"
      << "  rackcap rack list           --path P\n"
      << "  rackcap rack show           --path P --rack R\n"
      << "  rackcap rack apply          --path P --spec FILE [--actor A] [--request Q]\n"
      << "  rackcap rack fit            --path P --rack R (--units N | --span \"[a,b)\")\n"
      << "                              [--watts W] [--heat W] [--grams G]\n"
      << "                              [--service-access] [--front-mm N] [--rear-mm N]\n"
      << "  rackcap rack retire         --path P --rack R [--reason TEXT]\n"
      << "  rackcap rack revalidate     --path P --rack R [--all] [--actor A] [--request Q]\n"
      << "  rackcap rack diff           --path P --rack R\n"
      << "  rackcap rejections          --path P\n"
      << "\n"
      << "exit codes: 0 ok, 2 usage or input, 3 not found, 5 stale authority,\n"
      << "            6 lifecycle, 7 persistence, 10 io, 11 writer lock,\n"
      << "            12 evidence or coordinates, 13 invariant\n";
}

[[nodiscard]] bool take(int argc, char** argv, int& index, std::string& target) {
  if (index + 1 >= argc) {
    return false;
  }
  target = argv[++index];
  return true;
}

[[nodiscard]] bool parse_integer(const std::string& text, std::int64_t& target) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  target = static_cast<std::int64_t>(value);
  return true;
}

[[nodiscard]] bool parse(int argc, char** argv, Options& options, int& exit_code) {
  if (argc < 2) {
    usage();
    exit_code = 2;
    return false;
  }
  options.command = argv[1];
  if (options.command == "version" || options.command == "help" || options.command == "--help" ||
      options.command == "-h") {
    return true;
  }
  if (argc < 3) {
    usage();
    exit_code = 2;
    return false;
  }
  // Commands that take no subcommand start their options immediately; the
  // others take one word for the subcommand first.
  int first_option = 2;
  if (options.command != "rejections") {
    options.subcommand = argv[2];
    first_option = 3;
  }
  for (int index = first_option; index < argc; ++index) {
    const std::string key = argv[index];
    std::string value;
    if (key == "--path") {
      if (!take(argc, argv, index, options.path)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--rack") {
      if (!take(argc, argv, index, options.rack)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--spec") {
      if (!take(argc, argv, index, options.spec)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--reason") {
      if (!take(argc, argv, index, options.reason)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--actor") {
      if (!take(argc, argv, index, options.actor)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--request") {
      if (!take(argc, argv, index, options.request)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--operator") {
      if (!take(argc, argv, index, options.operator_id)) {
        exit_code = 2;
        return false;
      }
    } else if (key == "--span") {
      if (!take(argc, argv, index, value)) {
        exit_code = 2;
        return false;
      }
      options.span = value;
    } else if (key == "--units" || key == "--watts" || key == "--heat" || key == "--grams" ||
               key == "--front-mm" || key == "--rear-mm") {
      if (!take(argc, argv, index, value)) {
        exit_code = 2;
        return false;
      }
      std::int64_t number = 0;
      if (!parse_integer(value, number)) {
        std::cout << "invalid_argument: " << key << " needs a decimal integer\n";
        exit_code = 2;
        return false;
      }
      if (key == "--units") {
        options.units = number;
      } else if (key == "--watts") {
        options.watts = number;
      } else if (key == "--heat") {
        options.heat = number;
      } else if (key == "--grams") {
        options.grams = number;
      } else if (key == "--front-mm") {
        options.front_mm = number;
      } else {
        options.rear_mm = number;
      }
    } else if (key == "--service-access") {
      options.service_access = true;
    } else if (key == "--read-only") {
      options.read_only = true;
    } else if (key == "--all") {
      options.all = true;
    } else {
      std::cout << "invalid_argument: unknown option " << key << "\n";
      exit_code = 2;
      return false;
    }
  }
  return true;
}

[[nodiscard]] int report(const CapacityError& error) {
  std::cout << describe(error) << "\n";
  return static_cast<int>(exit_code_for(error.code));
}

[[nodiscard]] std::string generated_request(const std::string& prefix) {
  const TimestampNs now = system_now();
  return prefix + "-" + std::to_string(now.value());
}

[[nodiscard]] Result<CatalogOptions> open_options(const Options& options, bool writable) {
  CatalogOptions settings;
  if (options.path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a store path is required",
                      ErrorDetail{"cli", "--path", {}, 0, 0, {}});
  }
  settings.path = options.path;
  settings.writer_id = WriterId::create(writable ? "rackcap-cli" : "rackcap-cli-reader").value();
  settings.read_only = !writable;
  settings.create_if_missing = writable;
  return settings;
}

[[nodiscard]] Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "the specification file could not be opened",
                      ErrorDetail{"cli", path, {}, 0, 0, {}});
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  if (text.size() > kMaxSpecificationBytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "the specification file exceeds the documented size bound",
                      ErrorDetail{"cli", path, {}, kMaxSpecificationBytes, text.size(), {}});
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  int exit_code = 0;
  if (!parse(argc, argv, options, exit_code)) {
    return exit_code;
  }

  if (options.command == "help" || options.command == "--help" || options.command == "-h") {
    usage();
    return 0;
  }
  if (options.command == "version") {
    std::cout << "rackcap " << version_string() << "\n";
    std::cout << "state format version   : " << kStateFormatVersion << "\n";
    std::cout << "snapshot layout version: " << kSnapshotLayoutVersion << "\n";
    std::cout << "specification version  : " << kSpecificationFormatVersion << "\n";
    std::cout << "mount slots per unit   : " << kMountSlotsPerRackUnit << "\n";
    return 0;
  }

  if (options.command == "store") {
    if (options.path.empty()) {
      std::cout << "invalid_argument: --path is required\n";
      return 2;
    }
    if (options.subcommand == "inspect" || options.subcommand == "verify") {
      const Result<StateFileInfo> info = CapacityStore::inspect(options.path);
      if (!info.has_value()) {
        return report(info.error());
      }
      std::cout << info.value().to_text();
      if (options.subcommand == "verify") {
        std::cout << "verification: the state file decodes, checksums and validates\n";
      }
      return 0;
    }
    if (options.subcommand == "lock-status") {
      const Result<WriterLockInfo> info = CapacityStore::query_writer_lock(options.path);
      if (!info.has_value()) {
        return report(info.error());
      }
      std::cout << info.value().to_text() << "\n";
      return 0;
    }
    if (options.subcommand == "force-release") {
      const Result<WriterId> id = WriterId::create(options.operator_id);
      if (!id.has_value()) {
        return report(id.error());
      }
      const Result<WriterLockInfo> released = CapacityStore::force_release(
          options.path, id.value(), static_cast<std::uint64_t>(system_now().value()));
      if (!released.has_value()) {
        return report(released.error());
      }
      std::cout << released.value().to_text() << "\n";
      return 0;
    }
    if (options.subcommand == "init") {
      if (options.path.empty()) {
        std::cout << "invalid_argument: --path is required\n";
        return 2;
      }
      StoreOptions settings;
      settings.path = options.path;
      settings.writer_id = WriterId::create("rackcap-cli").value();
      settings.create_if_missing = true;
      const Result<std::unique_ptr<CapacityStore>> store = CapacityStore::open(settings);
      if (!store.has_value()) {
        return report(store.error());
      }
      // A store has no state file until something is published, because
      // publication is the commit point. Initialising therefore publishes one
      // empty generation, which is what makes the store exist on disk.
      CatalogState empty;
      empty.incarnation = store.value()->incarnation();
      empty.epoch = store.value()->epoch();
      const Result<StoreSequence> next = store.value()->sequence().next();
      if (!next.has_value()) {
        return report(next.error());
      }
      empty.sequence = next.value();
      empty.written_at = system_now();
      if (store.value()->sequence().value() != 0u) {
        std::cout << "already_initialised: the store already holds a published generation\n";
        std::cout << store.value()->recovery().to_text();
        (void)store.value()->close();
        return 0;
      }
      const Result<StoreSequence> published = store.value()->publish(empty);
      if (!published.has_value()) {
        return report(published.error());
      }
      std::cout << store.value()->recovery().to_text();
      (void)store.value()->close();
      const Result<StateFileInfo> info = CapacityStore::inspect(options.path);
      if (!info.has_value()) {
        return report(info.error());
      }
      std::cout << info.value().to_text();
      return 0;
    }
    std::cout << "invalid_argument: unknown store subcommand\n";
    return 2;
  }

  if (options.command == "rack" || options.command == "rejections") {
    const bool writable = options.command == "rack" && (options.subcommand == "apply" ||
                                                        options.subcommand == "retire" ||
                                                        options.subcommand == "revalidate");
    const Result<CatalogOptions> settings = open_options(options, writable);
    if (!settings.has_value()) {
      return report(settings.error());
    }
    const Result<std::unique_ptr<CapacityCatalog>> catalog =
        CapacityCatalog::open(settings.value());
    if (!catalog.has_value()) {
      return report(catalog.error());
    }

    if (options.command == "rejections") {
      const std::vector<RejectionRecord> rejections = catalog.value()->rejections();
      for (const RejectionRecord& record : rejections) {
        std::cout << record.to_text() << "\n";
      }
      std::cout << catalog.value()->stats().to_text();
      (void)catalog.value()->close();
      return 0;
    }

    if (options.subcommand == "list") {
      for (const RackCapacityRecord& record : catalog.value()->records()) {
        std::cout << record.rack.text() << " lifecycle=" << rack_lifecycle_name(record.lifecycle)
                  << " standing=" << recovery_standing_name(record.standing)
                  << " composition=" << record.composition_generation.value()
                  << " capacity=" << record.capacity_generation.value()
                  << " revision=" << record.revision.value()
                  << " occupants=" << record.inputs.assets.size()
                  << " slots_free=";
        if (record.snapshot.slots.free.is_known()) {
          std::cout << record.snapshot.slots.free.lower();
          if (record.snapshot.slots.free.upper() != record.snapshot.slots.free.lower()) {
            std::cout << ".." << record.snapshot.slots.free.upper();
          }
        } else {
          std::cout << "unknown";
        }
        std::cout << "\n";
      }
      (void)catalog.value()->close();
      return 0;
    }

    if (options.rack.empty() && options.subcommand != "apply") {
      std::cout << "invalid_argument: --rack is required\n";
      return 2;
    }
    Result<RackId> rack_id = RackId::create("unset");
    if (!options.rack.empty()) {
      rack_id = RackId::create(options.rack);
      if (!rack_id.has_value()) {
        return report(rack_id.error());
      }
    }

    if (options.subcommand == "show") {
      const Result<RackCapacityRecord> record = catalog.value()->record(rack_id.value());
      if (!record.has_value()) {
        return report(record.error());
      }
      // The inspection path verifies the record against its own evidence before
      // displaying it, so a recovered record can be inspected without being
      // promoted to authoritative and without publishing anything.
      const Result<InspectedSnapshot> inspected = catalog.value()->inspect_snapshot(
          CapacityExpectation::of(record.value()), system_now());
      if (!inspected.has_value()) {
        return report(inspected.error());
      }
      std::cout << inspected.value().to_text();
      std::cout << verify_closure(inspected.value().snapshot).to_text();
      (void)catalog.value()->close();
      return 0;
    }

    if (options.subcommand == "diff") {
      const Result<CapacityDiff> diff = catalog.value()->diff_with_previous(rack_id.value());
      if (!diff.has_value()) {
        return report(diff.error());
      }
      std::cout << diff.value().to_text();
      (void)catalog.value()->close();
      return 0;
    }

    if (options.subcommand == "fit") {
      const Result<RackCapacityRecord> record = catalog.value()->record(rack_id.value());
      if (!record.has_value()) {
        return report(record.error());
      }
      const Result<InspectedSnapshot> inspected = catalog.value()->inspect_snapshot(
          CapacityExpectation::of(record.value()), system_now());
      if (!inspected.has_value()) {
        return report(inspected.error());
      }
      FitRequest request;
      request.expected = CapacityExpectation::of(inspected.value().snapshot);
      std::uint32_t unit_height = 0;
      if (options.units.has_value()) {
        if (options.units.value() < 1 ||
            options.units.value() > static_cast<std::int64_t>(kMaxRackUnits)) {
          std::cout << "invalid_range: --units is outside the documented rack height range\n";
          return 2;
        }
        unit_height = static_cast<std::uint32_t>(options.units.value());
        request.unit_height = unit_height;
      } else if (options.span.has_value()) {
        const Result<SlotSet> parsed = SlotSet::parse(options.span.value());
        if (!parsed.has_value()) {
          return report(parsed.error());
        }
        if (parsed.value().interval_count() != 1) {
          std::cout << "invalid_slot_interval: --span must be exactly one interval\n";
          return 2;
        }
        request.span = parsed.value().intervals().front();
      } else {
        std::cout << "invalid_argument: one of --units or --span is required\n";
        return 2;
      }
      if (options.watts.has_value()) {
        const Result<Watts> value = Watts::create(options.watts.value());
        if (!value.has_value()) {
          return report(value.error());
        }
        request.draw = value.value();
      }
      if (options.heat.has_value()) {
        const Result<Watts> value = Watts::create(options.heat.value());
        if (!value.has_value()) {
          return report(value.error());
        }
        request.heat_rejection = value.value();
      }
      if (options.grams.has_value()) {
        const Result<Grams> value = Grams::create(options.grams.value());
        if (!value.has_value()) {
          return report(value.error());
        }
        request.mass = value.value();
      }
      if (options.front_mm.has_value()) {
        const Result<Millimetres> value = Millimetres::create(options.front_mm.value());
        if (!value.has_value()) {
          return report(value.error());
        }
        request.required_front_clearance = value.value();
      }
      if (options.rear_mm.has_value()) {
        const Result<Millimetres> value = Millimetres::create(options.rear_mm.value());
        if (!value.has_value()) {
          return report(value.error());
        }
        request.required_rear_clearance = value.value();
      }
      request.requires_service_access = options.service_access;
      const Result<FitEvaluation> evaluation =
          evaluate_fit(request, inspected.value().snapshot);
      if (!evaluation.has_value()) {
        return report(evaluation.error());
      }
      std::cout << evaluation.value().to_text();
      (void)catalog.value()->close();
      // A definite rejection is reported through the exit code; an undecided
      // evaluation is not a failure of the command.
      return evaluation.value().verdict == FitVerdict::DoesNotFit ? 1 : 0;
    }

    if (options.subcommand == "retire") {
      const Result<RackCapacityRecord> record = catalog.value()->record(rack_id.value());
      if (!record.has_value()) {
        return report(record.error());
      }
      RetireRackRequest request;
      request.expected = CapacityExpectation::of(record.value());
      request.reason = options.reason;
      request.actor = ActorId::create(options.actor).value();
      const Result<RequestId> id = RequestId::create(
          options.request.empty() ? generated_request("cli-retire") : options.request);
      if (!id.has_value()) {
        return report(id.error());
      }
      request.request = id.value();
      request.requested_at = system_now();
      const Result<MutationReceipt> receipt = catalog.value()->retire_rack(request);
      if (!receipt.has_value()) {
        return report(receipt.error());
      }
      std::cout << receipt.value().to_text();
      (void)catalog.value()->close();
      return 0;
    }

    if (options.subcommand == "revalidate") {
      const Result<RackCapacityRecord> record = catalog.value()->record(rack_id.value());
      if (!record.has_value()) {
        return report(record.error());
      }
      RevalidateRequest request;
      request.expected = CapacityExpectation::of(record.value());
      request.now = system_now();
      request.actor = ActorId::create(options.actor).value();
      const Result<RequestId> id = RequestId::create(
          options.request.empty() ? generated_request("cli-revalidate") : options.request);
      if (!id.has_value()) {
        return report(id.error());
      }
      request.request = id.value();
      request.requested_at = request.now;
      const Result<MutationReceipt> receipt = catalog.value()->revalidate(request);
      if (!receipt.has_value()) {
        return report(receipt.error());
      }
      std::cout << receipt.value().to_text();
      if (options.all) {
        std::cout << catalog.value()->stats().to_text();
      }
      (void)catalog.value()->close();
      return 0;
    }

    if (options.subcommand == "apply") {
      if (options.spec.empty()) {
        std::cout << "invalid_argument: --spec is required\n";
        return 2;
      }
      const Result<std::string> text = read_text_file(options.spec);
      if (!text.has_value()) {
        return report(text.error());
      }
      SpecificationParseOptions parse_options;
      parse_options.source_name = options.spec;
      const Result<RackCapacityInputs> inputs = parse_specification(text.value(), parse_options);
      if (!inputs.has_value()) {
        return report(inputs.error());
      }
      const RackId target = inputs.value().composition.rack;
      const Result<RackCapacityRecord> existing = catalog.value()->record(target);
      if (!existing.has_value()) {
        if (existing.error().code != ErrorCode::UnknownRackId) {
          return report(existing.error());
        }
        const Result<MutationReceipt> receipt = catalog.value()->register_rack(
            RegisterRackRequest{inputs.value(), system_now()});
        if (!receipt.has_value()) {
          return report(receipt.error());
        }
        std::cout << receipt.value().to_text();
        (void)catalog.value()->close();
        return 0;
      }
      ApplyEvidenceRequest request;
      request.expected = CapacityExpectation::of(existing.value());
      request.inputs = inputs.value();
      request.requested_at = system_now();
      const Result<MutationReceipt> receipt = catalog.value()->apply_evidence(request);
      if (!receipt.has_value()) {
        return report(receipt.error());
      }
      std::cout << receipt.value().to_text();
      (void)catalog.value()->close();
      return 0;
    }

    std::cout << "invalid_argument: unknown rack subcommand\n";
    return 2;
  }

  usage();
  return 2;
}
