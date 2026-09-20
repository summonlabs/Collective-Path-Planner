// Collective Path Planner - minimal deterministic test framework.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// The framework is intentionally small: a static registry, a filterable runner,
// hard assertions that abort one test, and a seeded random helper whose seed is
// reported on failure so that a randomized failure can always be reproduced.
#ifndef CPATH_TEST_FRAMEWORK_HPP
#define CPATH_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <exception>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cpath_test {

// Thrown by REQUIRE/CHECK helpers; the runner catches it per test.
class TestFailure : public std::exception {
 public:
  explicit TestFailure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

using TestFn = void (*)();

struct TestCase {
  const char* suite;
  const char* name;
  TestFn fn;
  const char* file;
  int line;
};

class Registry {
 public:
  static Registry& instance();

  void add(const char* suite, const char* name, TestFn fn, const char* file, int line);
  const std::vector<TestCase>& cases() const noexcept { return cases_; }

  // Runs every selected test. Returns the process exit code: 0 when all tests
  // passed, 1 otherwise. Recognised arguments:
  //   --list              print "suite.name" for every registered test
  //   --filter=SUBSTRING  run only tests whose "suite.name" contains SUBSTRING
  //   --seed=N            fix the random seed instead of deriving it from time
  //   --verbose           print one line per passing test
  int run(int argc, char** argv) const;

 private:
  std::vector<TestCase> cases_{};
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFn fn, const char* file, int line) {
    Registry::instance().add(suite, name, fn, file, line);
  }
};

// Deterministic random source. Constructed from the run seed; every property
// test failure reports the seed that produced it.
class Random {
 public:
  explicit Random(std::uint64_t seed) : engine_(seed), seed_(seed) {}

  std::uint64_t seed() const noexcept { return seed_; }
  std::uint64_t next_u64() { return engine_(); }
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : engine_() % bound; }
  std::uint64_t between(std::uint64_t low, std::uint64_t high) {
    return high <= low ? low : low + below(high - low + 1u);
  }
  bool chance(unsigned numerator, unsigned denominator) {
    return denominator != 0u && below(denominator) < numerator;
  }
  template <class T>
  const T& pick(const std::vector<T>& values) {
    return values[static_cast<std::size_t>(below(values.size()))];
  }

 private:
  std::mt19937_64 engine_;
  std::uint64_t seed_;
};

// Seed selected for this process by the runner.
std::uint64_t run_seed();
// Fresh Random derived from the run seed plus a stream index.
Random make_random(std::uint64_t stream);

std::string describe(const std::string& value);
std::string describe(const char* value);
std::string describe(std::string_view value);
std::string describe(bool value);

// One template covers every arithmetic type, so no overload can collide on
// platforms where size_t and uint64_t are the same underlying type.
template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
std::string describe(T value) {
  return std::to_string(value);
}

// Reports a failure and lets the current test continue, so that one run reports
// every failed assertion in a test rather than only the first.
void record_failure(const char* file, int line, const std::string& message);
// Reports a failure and aborts the current test.
[[noreturn]] void abort_test(const char* file, int line, const std::string& message);

// The runner clears the collected failures before each test and reads them
// afterwards. Both are called only from the runner's thread.
void reset_failures();
const std::vector<std::string>& collected_failures();

}  // namespace cpath_test

#define CPATH_TEST(suite, name) \
  static void suite##_##name##_body(); \
  namespace { \
  const ::cpath_test::Registrar suite##_##name##_registrar(#suite, #name, &suite##_##name##_body, \
                                                            __FILE__, __LINE__); \
  } \
  static void suite##_##name##_body()

#define CPATH_CHECK(condition) \
  do { \
    if (!(condition)) { \
      ::cpath_test::record_failure(__FILE__, __LINE__, "CHECK failed: " #condition); \
    } \
  } while (false)

#define CPATH_REQUIRE(condition) \
  do { \
    if (!(condition)) { \
      ::cpath_test::abort_test(__FILE__, __LINE__, "REQUIRE failed: " #condition); \
    } \
  } while (false)

#define CPATH_CHECK_EQ(actual, expected) \
  do { \
    const auto cpath_check_actual = (actual); \
    const auto cpath_check_expected = (expected); \
    if (!(cpath_check_actual == cpath_check_expected)) { \
      ::cpath_test::record_failure( \
          __FILE__, __LINE__, \
          "CHECK_EQ failed: " #actual " == " #expected " (actual=" + \
              ::cpath_test::describe(cpath_check_actual) + \
              ", expected=" + ::cpath_test::describe(cpath_check_expected) + ")"); \
    } \
  } while (false)


#define CPATH_REQUIRE_EQ(actual, expected) \
  do { \
    const auto cpath_check_actual = (actual); \
    const auto cpath_check_expected = (expected); \
    if (!(cpath_check_actual == cpath_check_expected)) { \
      ::cpath_test::abort_test( \
          __FILE__, __LINE__, \
          "REQUIRE_EQ failed: " #actual " == " #expected " (actual=" + \
              ::cpath_test::describe(cpath_check_actual) + \
              ", expected=" + ::cpath_test::describe(cpath_check_expected) + ")"); \
    } \
  } while (false)


#define CPATH_FAIL(message) ::cpath_test::abort_test(__FILE__, __LINE__, (message))

#endif  // CPATH_TEST_FRAMEWORK_HPP
