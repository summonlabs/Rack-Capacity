// Rack Capacity - command line tool tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The tool is exercised as a real process with real arguments, because that is
// how an operator uses it. Every assertion is on the tool's output and its
// exit code, both of which are part of its documented contract.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "rack_capacity/rack_capacity.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace rackcapacity;

namespace {

const char* kToolPath = RC_CAPACITY_CLI_EXECUTABLE;

[[nodiscard]] std::string tool() { return rctest::helper_executable(kToolPath); }

void write_spec(const std::string& path) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << R"(rcap-spec 1
[composition]
rack = rack-a1
generation = 4
units = 48
structural-reserved = [95,97)
front-clearance-mm = 900
rear-clearance-mm = 700
service-height-limit-u = 42
source = rack-registry
evidence = ev-composition
version = 11
observed-at-ns = 1700000000000000000
[policy]
policy = standard-2026
policy-version = 3
power-headroom-w = 500
slot-headroom = 2
power-derate-bp = 500
max-envelope-age-ns = 3600000000000
[power]
feeds = 2
watts-per-feed = 8000
redundancy = 2n
derate-bp = 2000
source = power-capacity
evidence = ev-power
version = 7
observed-at-ns = 1700000000000000000
[cooling]
nominal-heat-rejection-w = 20000
derate-bp = 1000
source = cooling-capacity
evidence = ev-cooling
version = 3
observed-at-ns = 1700000000000000000
[weight]
static-load-limit-g = 900000
derate-bp = 0
per-unit-point-load-limit-g = 250000
source = cooling-capacity
evidence = ev-weight
version = 2
observed-at-ns = 1700000000000000000
[asset a-0001]
span = [1,5)
kind = full
presence = present
nameplate-draw-w = 1200
declared-heat-w = 1200
mass-g = 32000
source = asset-registry
evidence = ev-asset-0001
version = 5
observed-at-ns = 1700000000000000000
[reservation r-0001]
state = committed
span = [31,33)
kind = full
draw-w = 500
heat-w = 500
mass-g = 9000
source = facility-capacity-reservation
evidence = ev-reservation-0001
version = 2
observed-at-ns = 1700000000000000000
[request]
epoch = 12
captured-at-ns = 1700000000000000000
actor = operator-1
request = req-0001
)";
}

[[nodiscard]] bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

RC_TEST(cli_prints_its_version_and_usage) {
  const rctest::ProcessResult version = rctest::run_process(tool(), {"version"});
  RC_CHECK_EQ(version.exit_code, 0);
  RC_CHECK(contains(version.output, "rackcap 1.0.0"));
  RC_CHECK(contains(version.output, "state format version"));

  const rctest::ProcessResult usage = rctest::run_process(tool(), {"help"});
  RC_CHECK_EQ(usage.exit_code, 0);
  RC_CHECK(contains(usage.output, "usage:"));

  const rctest::ProcessResult nonsense = rctest::run_process(tool(), {"nonsense"});
  RC_CHECK_EQ(nonsense.exit_code, 2);

  const rctest::ProcessResult unknown_option =
      rctest::run_process(tool(), {"rack", "list", "--path", "x", "--nope"});
  RC_CHECK_EQ(unknown_option.exit_code, 2);
  RC_CHECK(contains(unknown_option.output, "invalid_argument"));
}

RC_TEST(cli_initialises_inspects_and_verifies_a_store) {
  rctest::TempDir dir("cli-store");
  const std::string store = dir.child("capacity.rcstate");

  const rctest::ProcessResult init = rctest::run_process(tool(), {"store", "init", "--path", store});
  RC_CHECK_EQ(init.exit_code, 0);
  RC_CHECK(contains(init.output, "fresh-store"));
  RC_CHECK(std::filesystem::exists(store));

  const rctest::ProcessResult init_again =
      rctest::run_process(tool(), {"store", "init", "--path", store});
  RC_CHECK_EQ(init_again.exit_code, 0);
  RC_CHECK(contains(init_again.output, "already_initialised"));

  const rctest::ProcessResult inspect =
      rctest::run_process(tool(), {"store", "inspect", "--path", store});
  RC_CHECK_EQ(inspect.exit_code, 0);
  RC_CHECK(contains(inspect.output, "format version       : 1"));
  RC_CHECK(contains(inspect.output, "slots per rack unit  : 2"));

  const rctest::ProcessResult verify =
      rctest::run_process(tool(), {"store", "verify", "--path", store});
  RC_CHECK_EQ(verify.exit_code, 0);
  RC_CHECK(contains(verify.output, "decodes, checksums and validates"));

  const rctest::ProcessResult lock =
      rctest::run_process(tool(), {"store", "lock-status", "--path", store});
  RC_CHECK_EQ(lock.exit_code, 0);
  RC_CHECK(contains(lock.output, "unlocked"));

  // Inspecting something that is not a store is reported, not repaired.
  const rctest::ProcessResult missing =
      rctest::run_process(tool(), {"store", "inspect", "--path", dir.child("absent.rcstate")});
  RC_CHECK_EQ(missing.exit_code, 3);
  RC_CHECK(contains(missing.output, "no_authoritative_state"));

  // Corrupting the state makes inspection fail with the persistence code.
  {
    std::ofstream stream(store, std::ios::binary | std::ios::app);
    stream << "junk";
  }
  const rctest::ProcessResult damaged =
      rctest::run_process(tool(), {"store", "verify", "--path", store});
  RC_CHECK(damaged.exit_code == 7);
  RC_CHECK(contains(damaged.output, "truncated_state") ||
           contains(damaged.output, "integrity_check_failed"));
}

RC_TEST(cli_applies_a_specification_and_reports_capacity) {
  rctest::TempDir dir("cli-apply");
  const std::string store = dir.child("capacity.rcstate");
  const std::string spec = dir.child("rack-a1.spec");
  write_spec(spec);

  RC_CHECK_EQ(rctest::run_process(tool(), {"store", "init", "--path", store}).exit_code, 0);

  const rctest::ProcessResult apply =
      rctest::run_process(tool(), {"rack", "apply", "--path", store, "--spec", spec});
  RC_CHECK_EQ(apply.exit_code, 0);
  RC_CHECK(contains(apply.output, "mutation applied for rack rack-a1"));
  RC_CHECK(contains(apply.output, "composition gen   : 4"));

  // A second apply of the same document is refused as a repeated epoch with
  // different facts only if the facts differ; here the document is identical,
  // so the catalog reports that nothing changed.
  const rctest::ProcessResult again =
      rctest::run_process(tool(), {"rack", "apply", "--path", store, "--spec", spec});
  RC_CHECK_EQ(again.exit_code, 0);

  const rctest::ProcessResult list =
      rctest::run_process(tool(), {"rack", "list", "--path", store});
  RC_CHECK_EQ(list.exit_code, 0);
  RC_CHECK(contains(list.output, "rack-a1"));
  RC_CHECK(contains(list.output, "lifecycle=active"));

  const rctest::ProcessResult show =
      rctest::run_process(tool(), {"rack", "show", "--path", store, "--rack", "rack-a1"});
  RC_CHECK_EQ(show.exit_code, 0);
  RC_CHECK(contains(show.output, "rack capacity snapshot for rack-a1"));
  RC_CHECK(contains(show.output, "revalidated against its own evidence"));

  // A fit that fits exits zero.
  const rctest::ProcessResult fits = rctest::run_process(
      tool(), {"rack", "fit", "--path", store, "--rack", "rack-a1", "--units", "2", "--watts",
               "1500", "--heat", "1500", "--grams", "40000"});
  RC_CHECK_EQ(fits.exit_code, 0);
  RC_CHECK(contains(fits.output, "verdict           : fits"));

  // A fit that cannot fit exits non-zero and names the binding dimension.
  const rctest::ProcessResult refuses = rctest::run_process(
      tool(), {"rack", "fit", "--path", store, "--rack", "rack-a1", "--units", "2", "--watts",
               "99000"});
  RC_CHECK_EQ(refuses.exit_code, 1);
  RC_CHECK(contains(refuses.output, "does-not-fit"));
  RC_CHECK(contains(refuses.output, "binding dimension : power"));

  // A fit against a rack that does not exist is a not-found failure.
  const rctest::ProcessResult unknown = rctest::run_process(
      tool(), {"rack", "fit", "--path", store, "--rack", "rack-none", "--units", "1"});
  RC_CHECK_EQ(unknown.exit_code, 3);

  // A fit with neither a span nor a height is a usage failure.
  const rctest::ProcessResult incomplete =
      rctest::run_process(tool(), {"rack", "fit", "--path", store, "--rack", "rack-a1"});
  RC_CHECK_EQ(incomplete.exit_code, 2);

  // A malformed specification is refused with the evidence code.
  const std::string bad_spec = dir.child("bad.spec");
  {
    std::ofstream stream(bad_spec, std::ios::binary | std::ios::trunc);
    stream << "rcap-spec 1\n[composition]\nrack = rack-b2\n";
  }
  const rctest::ProcessResult bad =
      rctest::run_process(tool(), {"rack", "apply", "--path", store, "--spec", bad_spec});
  RC_CHECK(bad.exit_code != 0);
  RC_CHECK(contains(bad.output, "empty_value") || contains(bad.output, "invalid"));

  const rctest::ProcessResult rejections =
      rctest::run_process(tool(), {"rejections", "--path", store});
  RC_CHECK_EQ(rejections.exit_code, 0);
  RC_CHECK(contains(rejections.output, "rejections retained"));
}

RC_TEST(cli_retires_and_differs) {
  rctest::TempDir dir("cli-retire");
  const std::string store = dir.child("capacity.rcstate");
  const std::string spec = dir.child("rack-a1.spec");
  write_spec(spec);
  RC_CHECK_EQ(rctest::run_process(tool(), {"store", "init", "--path", store}).exit_code, 0);
  RC_CHECK_EQ(
      rctest::run_process(tool(), {"rack", "apply", "--path", store, "--spec", spec}).exit_code, 0);

  const rctest::ProcessResult retire = rctest::run_process(
      tool(), {"rack", "retire", "--path", store, "--rack", "rack-a1", "--reason", "test"});
  RC_CHECK_EQ(retire.exit_code, 0);
  RC_CHECK(contains(retire.output, "lifecycle         : retired"));

  // Capacity is never quoted for a retired rack again.
  const rctest::ProcessResult show =
      rctest::run_process(tool(), {"rack", "show", "--path", store, "--rack", "rack-a1"});
  RC_CHECK_EQ(show.exit_code, 6);
  RC_CHECK(contains(show.output, "rack_retired"));

  RC_CHECK_EQ(rctest::run_process(tool(), {"store", "lock-status", "--path", store}).exit_code, 0);
}

RC_TEST(cli_lock_holder_is_visible_and_authority_can_be_released) {
  rctest::TempDir dir("cli-lock");
  const std::string store = dir.child("capacity.rcstate");
  RC_CHECK_EQ(rctest::run_process(tool(), {"store", "init", "--path", store}).exit_code, 0);
  RC_CHECK_EQ(rctest::run_process(tool(), {"store", "lock-status", "--path", store}).exit_code, 0);

  const rctest::ProcessResult released = rctest::run_process(
      tool(), {"store", "force-release", "--path", store, "--operator", "operator-7"});
  RC_CHECK_EQ(released.exit_code, 0);
  RC_CHECK(contains(released.output, "operator-7"));
  RC_CHECK(contains(released.output, "epoch 2"));

  // Releasing needs an operator identity that is a valid identity.
  const rctest::ProcessResult invalid = rctest::run_process(
      tool(), {"store", "force-release", "--path", store, "--operator", "not/a/name"});
  RC_CHECK_EQ(invalid.exit_code, 2);
}
