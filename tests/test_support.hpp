// Agent Scheduler — minimal test harness (no external dependencies).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace as_test {

struct Failure {
  std::string message;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

[[nodiscard]] std::vector<TestCase>& registry();
void record(std::string suite, std::string name, std::function<void()> body);

struct Registrar {
  Registrar(std::string suite, std::string name, std::function<void()> body) {
    record(std::move(suite), std::move(name), std::move(body));
  }
};

[[noreturn]] void fail(const char* file, int line, std::string message);

template <class Left, class Right>
void check_equal(const char* file,
                 int line,
                 const char* left_text,
                 const char* right_text,
                 const Left& left,
                 const Right& right) {
  if (!(left == right)) {
    std::ostringstream stream;
    stream << "expected " << left_text << " == " << right_text;
    fail(file, line, stream.str());
  }
}

inline int run_all(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      for (const TestCase& test : registry()) {
        std::cout << test.suite << "." << test.name << "\n";
      }
      return 0;
    }
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }
  int failures = 0;
  int executed = 0;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    std::cout << "RUN  " << full << std::endl;
    try {
      test.body();
      std::cout << "PASS " << full << std::endl;
    } catch (const Failure& failure) {
      ++failures;
      std::cout << "FAIL " << full << ": " << failure.message << std::endl;
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "FAIL " << full << ": unexpected exception: " << error.what() << std::endl;
    }
  }
  std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << " executed=" << executed
            << " failures=" << failures << std::endl;
  return failures == 0 ? 0 : 1;
}

}  // namespace as_test

#define AS_TEST(suite_name, test_name)                                                    \
  static void suite_name##_##test_name##_body();                                          \
  static const ::as_test::Registrar suite_name##_##test_name##_registrar{                 \
      #suite_name, #test_name, suite_name##_##test_name##_body};                          \
  static void suite_name##_##test_name##_body()

#define AS_CHECK(expression)                                                              \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      ::as_test::fail(__FILE__, __LINE__, "check failed: " #expression);                  \
    }                                                                                     \
  } while (false)

#define AS_CHECK_EQ(left, right)                                                          \
  ::as_test::check_equal(__FILE__, __LINE__, #left, #right, (left), (right))

#define AS_REQUIRE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      ::as_test::fail(__FILE__, __LINE__, "require failed: " #expression);                \
    }                                                                                     \
  } while (false)

#define AS_FAIL(message) ::as_test::fail(__FILE__, __LINE__, (message))
