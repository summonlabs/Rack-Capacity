// Rack Capacity - minimal test framework implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <cstdio>
#include <iostream>

namespace rctest {
namespace {

struct FailureState {
  std::string current_test;
  int failure_count = 0;
  int check_failures = 0;
};

FailureState& state() {
  static FailureState instance;
  return instance;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

bool register_test(std::string name, TestFunction function) {
  registry().push_back(TestCase{std::move(name), function});
  return true;
}

void report_failure(const char* file, int line, const std::string& message) {
  FailureState& current = state();
  ++current.check_failures;
  std::cout << "FAIL " << current.current_test << "\n  " << file << ":" << line << "\n  " << message
            << "\n";
}

int run_all(std::string_view filter) {
  FailureState& current = state();
  int failed = 0;
  int executed = 0;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    current.current_test = test.name;
    const int before = current.check_failures;
    test.function();
    ++executed;
    if (current.check_failures != before) {
      ++failed;
      ++current.failure_count;
      std::cout << "FAILED " << test.name << " (" << (current.check_failures - before)
                << " failed check(s))\n";
    } else {
      std::cout << "ok     " << test.name << "\n";
    }
  }
  std::cout << "\n" << executed << " test(s) run, " << failed << " failed, "
            << current.check_failures << " failed check(s)\n";
  std::cout.flush();
  return failed;
}

}  // namespace rctest

int main(int argc, char** argv) {
  // Test output is unbuffered: if a test aborts, everything it reported before
  // the abort must still reach the console.
  std::cout.setf(std::ios::unitbuf);
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--filter" && index + 1 < argc) {
      filter = argv[++index];
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else {
      std::cout << "usage: " << argv[0] << " [--filter substring]\n";
      return 2;
    }
  }
  return rctest::run_all(filter) == 0 ? 0 : 1;
}
