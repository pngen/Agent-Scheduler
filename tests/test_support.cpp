// Agent Scheduler — minimal test harness implementation.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "test_support.hpp"

namespace as_test {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void record(std::string suite, std::string name, std::function<void()> body) {
  registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

void fail(const char* file, int line, std::string message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw Failure{stream.str()};
}

}  // namespace as_test

int main(int argc, char** argv) { return ::as_test::run_all(argc, argv); }
