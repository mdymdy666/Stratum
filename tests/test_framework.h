// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Minimal test framework (zero dependency)
//
// Design notes:
//   1. No gtest/catch2 - consistent with the project's zero-dependency goal.
//   2. Assertions are INLINE FUNCTIONS, not multi-line macros.
//      Reason: multi-line macros are fragile (continuation backslashes) and
//      poor for debugging. Callers pass __FILE__ / __LINE__ explicitly.
//   3. Only two single-line macros remain: STRATUM_TEST (registration)
//      and STRATUM_TEST_MAIN (entry point).
//   4. Parameterized tests supported: register the same function many times.
//
// Usage (standard library headers MUST be included first):
//
//   #include <string>
//   #include "test_framework.h"
//
//   STRATUM_TEST(Suite, Case) {
//     ctx.param_desc = "case-1";
//     STRATUM_CHECK_EQ(1, 1);
//   }
//
//   STRATUM_TEST_MAIN()

#ifndef STRATUM_TEST_FRAMEWORK_H_
#define STRATUM_TEST_FRAMEWORK_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace stratum_test {

// ---------------------------------------------------------------------------
// Assertion failure
// ---------------------------------------------------------------------------
class AssertionFailure : public std::exception {
 public:
  explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

// ---------------------------------------------------------------------------
// Test context (current test name + parameter description)
// ---------------------------------------------------------------------------
struct TestContext {
  std::string name;
  std::string param_desc;

  std::string Where() const {
    if (param_desc.empty()) return name;
    return name + " [" + param_desc + "]";
  }
};

// ---------------------------------------------------------------------------
// Value to string
// ---------------------------------------------------------------------------
template <typename T>
std::string ToString(const T& value) {
  std::ostringstream oss;
  oss << value;
  return oss.str();
}
inline std::string ToString(bool value) { return value ? "true" : "false"; }
inline std::string ToString(const std::string& value) { return value; }

// ---------------------------------------------------------------------------
// Assertion helpers (inline functions - no macros, no backslashes)
// ---------------------------------------------------------------------------
[[noreturn]] inline void Fail(const std::string& what, const std::string& file,
                              int line) {
  std::ostringstream oss;
  oss << what << "  (" << file << ":" << line << ")";
  throw AssertionFailure(oss.str());
}

inline void CheckTrue(bool cond, const char* expr, const std::string& file,
                      int line) {
  if (!cond) {
    Fail(std::string("assertion failed: ") + expr, file, line);
  }
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* expr_a, const char* expr_b,
             const std::string& file, int line) {
  if (!(a == b)) {
    std::ostringstream oss;
    oss << "assertion failed: " << expr_a << " == " << expr_b
        << "  actual=" << ToString(a) << "  expect=" << ToString(b);
    Fail(oss.str(), file, line);
  }
}

template <typename A, typename B>
void CheckNe(const A& a, const B& b, const char* expr_a, const char* expr_b,
             const std::string& file, int line) {
  if (a == b) {
    std::ostringstream oss;
    oss << "assertion failed (should differ): " << expr_a << " != " << expr_b;
    Fail(oss.str(), file, line);
  }
}

template <typename A, typename B>
void CheckLt(const A& a, const B& b, const char* expr_a, const char* expr_b,
             const std::string& file, int line) {
  if (!(a < b)) {
    std::ostringstream oss;
    oss << "assertion failed: " << expr_a << " < " << expr_b
        << "  actual=" << ToString(a) << "  expect<" << ToString(b);
    Fail(oss.str(), file, line);
  }
}

template <typename A, typename B>
void CheckLe(const A& a, const B& b, const char* expr_a, const char* expr_b,
             const std::string& file, int line) {
  if (!(a <= b)) {
    std::ostringstream oss;
    oss << "assertion failed: " << expr_a << " <= " << expr_b
        << "  actual=" << ToString(a) << "  expect<=" << ToString(b);
    Fail(oss.str(), file, line);
  }
}

// Status check (requires stratum/core/status.h to be included by the test)
template <typename S>
void CheckOk(const S& s, const char* expr, const std::string& file, int line) {
  if (!s.ok()) {
    std::ostringstream oss;
    oss << "assertion failed: " << expr << " should be OK"
        << "  actual=" << s.ToString();
    Fail(oss.str(), file, line);
  }
}

// ---------------------------------------------------------------------------
// Test case and registry
// ---------------------------------------------------------------------------
struct TestCase {
  std::string suite;
  std::string name;
  std::string param_desc;
  std::function<void(TestContext&)> body;

  std::string FullName() const {
    return param_desc.empty() ? (suite + "." + name)
                              : (suite + "." + name + "[" + param_desc + "]");
  }
};

class TestRegistry {
 public:
  static TestRegistry& Instance() {
    static TestRegistry registry;
    return registry;
  }

  void Register(const std::string& suite, const std::string& name,
                const std::string& param_desc,
                std::function<void(TestContext&)> body) {
    TestCase tc;
    tc.suite = suite;
    tc.name = name;
    tc.param_desc = param_desc;
    tc.body = std::move(body);
    tests_.push_back(std::move(tc));
  }

  int RunAll(const std::string& filter = "") {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<std::string> failures;

    std::cout << "registered cases: " << tests_.size();
    if (!filter.empty()) std::cout << " (filter: " << filter << ")";
    std::cout << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    for (const auto& test : tests_) {
      if (!filter.empty() && test.FullName().find(filter) == std::string::npos) {
        ++skipped;
        continue;
      }
      TestContext ctx;
      ctx.name = test.suite + "." + test.name;
      ctx.param_desc = test.param_desc;
      try {
        test.body(ctx);
        ++passed;
        std::cout << "  [PASS] " << test.FullName() << std::endl;
      } catch (const AssertionFailure& e) {
        ++failed;
        failures.push_back(e.what());
        std::cout << "  [FAIL] " << test.FullName() << std::endl;
      } catch (const std::exception& e) {
        ++failed;
        failures.push_back(test.FullName() + " exception: " + e.what());
        std::cout << "  [FAIL] " << test.FullName() << " (exception)"
                  << std::endl;
      }
    }

    std::cout << std::string(72, '-') << std::endl;
    if (!failures.empty()) {
      std::cout << std::endl << "failures:" << std::endl;
      for (const auto& f : failures) std::cout << "  - " << f << std::endl;
      std::cout << std::endl;
    }
    std::cout << "result: " << passed << " passed, " << failed << " failed";
    if (skipped > 0) std::cout << ", " << skipped << " skipped";
    std::cout << std::endl;
    return failed;
  }

  size_t size() const { return tests_.size(); }

 private:
  std::vector<TestCase> tests_;
};

}  // namespace stratum_test

// ---------------------------------------------------------------------------
// Assertion macros (single-line only, forwarding to inline functions)
//
// Callers pass __FILE__ and __LINE__ so failures point at the right place.
// ---------------------------------------------------------------------------
#define STRATUM_CHECK_TRUE(cond) \
  ::stratum_test::CheckTrue((cond), #cond, __FILE__, __LINE__)

#define STRATUM_CHECK_FALSE(cond) \
  ::stratum_test::CheckTrue(!(cond), "!" #cond, __FILE__, __LINE__)

#define STRATUM_CHECK_EQ(a, b) \
  ::stratum_test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

#define STRATUM_CHECK_NE(a, b) \
  ::stratum_test::CheckNe((a), (b), #a, #b, __FILE__, __LINE__)

#define STRATUM_CHECK_LT(a, b) \
  ::stratum_test::CheckLt((a), (b), #a, #b, __FILE__, __LINE__)

#define STRATUM_CHECK_LE(a, b) \
  ::stratum_test::CheckLe((a), (b), #a, #b, __FILE__, __LINE__)

#define STRATUM_CHECK_GT(a, b) \
  ::stratum_test::CheckLt((b), (a), #b, #a, __FILE__, __LINE__)

#define STRATUM_CHECK_GE(a, b) \
  ::stratum_test::CheckLe((b), (a), #b, #a, __FILE__, __LINE__)

#define STRATUM_CHECK_OK(expr) \
  ::stratum_test::CheckOk((expr), #expr, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Test declaration + auto registration
//
// Expands to a plain function plus a static registrar object.
// The macro does NOT end with an unclosed brace, so it cannot break
// the parsing of following code.
// ---------------------------------------------------------------------------
#define STRATUM_TEST(suite, name)                                            \
  static void stratum_case_##suite##_##name([[maybe_unused]] ::stratum_test::TestContext& ctx); \
  namespace {                                                                \
  const bool stratum_registered_##suite##_##name = []() {                    \
    ::stratum_test::TestRegistry::Instance().Register(                       \
        #suite, #name, "",                                                   \
        []([[maybe_unused]] ::stratum_test::TestContext& c) {                                 \
          stratum_case_##suite##_##name(c);                                  \
        });                                                                  \
    return true;                                                             \
  }();                                                                       \
  }                                                                          \
  static void stratum_case_##suite##_##name([[maybe_unused]] ::stratum_test::TestContext& ctx)

// Entry point
#define STRATUM_TEST_MAIN()                                                  \
  int main(int argc, char** argv) {                                          \
    std::string filter;                                                      \
    if (argc > 1) filter = argv[1];                                          \
    const int failed =                                                       \
        ::stratum_test::TestRegistry::Instance().RunAll(filter);             \
    return failed == 0 ? 0 : 1;                                              \
  }


// ---------------------------------------------------------------------------
// Backward-compatible aliases
//
// Early test files used STRATUM_EXPECT_*. Keep them working so that the
// framework can evolve without breaking existing tests.
// ---------------------------------------------------------------------------
#define STRATUM_EXPECT_TRUE(cond, ctx) STRATUM_CHECK_TRUE(cond)
#define STRATUM_EXPECT_FALSE(cond, ctx) STRATUM_CHECK_FALSE(cond)
#define STRATUM_EXPECT_EQ(a, b, ctx) STRATUM_CHECK_EQ((a), (b))
#define STRATUM_EXPECT_NE(a, b, ctx) STRATUM_CHECK_NE((a), (b))
#define STRATUM_EXPECT_LT(a, b, ctx) STRATUM_CHECK_LT((a), (b))
#define STRATUM_EXPECT_LE(a, b, ctx) STRATUM_CHECK_LE((a), (b))
#define STRATUM_EXPECT_GT(a, b, ctx) STRATUM_CHECK_GT((a), (b))
#define STRATUM_EXPECT_GE(a, b, ctx) STRATUM_CHECK_GE((a), (b))
#define STRATUM_EXPECT_OK(expr, ctx) STRATUM_CHECK_OK(expr)

#endif  // STRATUM_TEST_FRAMEWORK_H_
