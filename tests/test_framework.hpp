#pragma once

// Minimal single-header test harness: TEST(name) { CHECK(...); } with an
// auto-registering runner. Deliberately dependency-free so the core tests
// build on any platform with nothing but a C++20 compiler.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int g_failures = 0;
inline const char* g_current = "";

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, fn}); }
};

inline void check_failed(const char* expr, const char* file, int line) {
  ++g_failures;
  std::fprintf(stderr, "FAILED  %s\n        %s:%d: CHECK(%s)\n", g_current, file, line, expr);
}

inline int run_all() {
  int failed_tests = 0;
  for (const auto& t : registry()) {
    g_current = t.name;
    const int before = g_failures;
    t.fn();
    if (g_failures == before) {
      std::fprintf(stderr, "ok      %s\n", t.name);
    } else {
      ++failed_tests;
    }
  }
  std::fprintf(stderr, "\n%zu tests, %d failed\n", registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                            \
  static void test_##name();                                                  \
  static ::testfw::Registrar registrar_##name{#name, test_##name};            \
  static void test_##name()

#define CHECK(expr)                                                           \
  do {                                                                        \
    if (!(expr)) ::testfw::check_failed(#expr, __FILE__, __LINE__);           \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
