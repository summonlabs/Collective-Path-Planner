// Collective Path Planner - minimal deterministic test framework.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace cpath_test {
namespace {

std::uint64_t g_run_seed = 0;

std::string hex(std::uint64_t value) {
  static const char* kDigits = "0123456789abcdef";
  std::string out = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const unsigned digit = static_cast<unsigned>((value >> shift) & 0xFu);
    if (digit != 0u || started || shift == 0) {
      out.push_back(kDigits[digit]);
      started = true;
    }
  }
  return out;
}

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(const char* suite, const char* name, TestFn fn, const char* file, int line) {
  cases_.push_back(TestCase{suite, name, fn, file, line});
}

namespace {
thread_local std::vector<std::string> g_failures;
}  // namespace

void reset_failures() { g_failures.clear(); }

const std::vector<std::string>& collected_failures() { return g_failures; }

void record_failure(const char* file, int line, const std::string& message) {
  g_failures.push_back(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

void abort_test(const char* file, int line, const std::string& message) {
  throw TestFailure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

std::uint64_t run_seed() { return g_run_seed; }

Random make_random(std::uint64_t stream) {
  return Random(g_run_seed + 0x9E3779B97F4A7C15ull * (stream + 1u));
}

std::string describe(const std::string& value) { return "\"" + value + "\""; }
std::string describe(const char* value) { return value == nullptr ? "null" : describe(std::string(value)); }
std::string describe(std::string_view value) { return describe(std::string(value)); }
std::string describe(bool value) { return value ? "true" : "false"; }

int Registry::run(int argc, char** argv) const {
  bool list_only = false;
  bool verbose = false;
  std::string filter;
  std::uint64_t seed = 0;
  bool seed_given = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--verbose") {
      verbose = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument.rfind("--seed=", 0) == 0) {
      seed = 0;
      for (const char ch : argument.substr(7)) {
        if (ch < '0' || ch > '9') {
          std::cerr << "runner: --seed requires a decimal number\n";
          return 2;
        }
        seed = seed * 10u + static_cast<std::uint64_t>(ch - '0');
      }
      seed_given = true;
    } else {
      std::cerr << "runner: unknown argument " << argument << "\n";
      return 2;
    }
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : cases_) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    selected.push_back(test);
  }
  std::sort(selected.begin(), selected.end(), [](const TestCase& lhs, const TestCase& rhs) {
    const int suite_order = std::strcmp(lhs.suite, rhs.suite);
    if (suite_order != 0) {
      return suite_order < 0;
    }
    return std::strcmp(lhs.name, rhs.name) < 0;
  });

  if (list_only) {
    for (const TestCase& test : selected) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    std::cout << selected.size() << " test(s)\n";
    return 0;
  }

  if (!seed_given) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    seed = static_cast<std::uint64_t>(now) ^ 0xC0FFEE1234567ull;
  }
  g_run_seed = seed;

  std::cout << "run seed " << seed << " (" << hex(seed) << "); " << selected.size() << " test(s)\n";
  std::size_t passed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : selected) {
    const std::string full = std::string(test.suite) + "." + test.name;
    std::string abort_reason;
    reset_failures();
    try {
      test.fn();
    } catch (const TestFailure& failure) {
      abort_reason = failure.what();
    } catch (const std::exception& error) {
      abort_reason = std::string("unexpected exception: ") + error.what();
    } catch (...) {
      abort_reason = "unexpected non-standard exception";
    }

    std::vector<std::string> problems = collected_failures();
    if (!abort_reason.empty()) {
      problems.push_back(abort_reason);
    }
    if (problems.empty()) {
      ++passed;
      if (verbose) {
        std::cout << "PASS " << full << "\n";
      }
      continue;
    }
    std::cout << "FAIL " << full << " (" << problems.size() << " problem(s))\n";
    for (const std::string& problem : problems) {
      std::cout << "  " << problem << "\n";
      failures.push_back(full + ": " + problem);
    }
  }

  std::cout << (failures.empty() ? "OK " : "FAILED ") << passed << "/" << selected.size()
            << " test(s) passed; seed " << seed << "\n";
  for (const std::string& failure : failures) {
    std::cout << "  " << failure << "\n";
  }
  return failures.empty() ? 0 : 1;
}

}  // namespace cpath_test
