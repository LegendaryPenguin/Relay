#pragma once

// A minimal test harness.
//
// Hand-written rather than pulled in, for the same reason the engine has no
// dependencies: fetching a framework at configure time makes the build depend on
// the network, and the whole of what this project needs from a test framework is
// registration, an assertion that prints both sides, and a non-zero exit code.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// CHECK_EQ prints both operands, so any type it is used on needs an
// operator<<. std::optional is common enough in this codebase's interfaces
// (Balancer::Pick, HashRing::Lookup) to be worth one here rather than making
// every call site unwrap by hand.
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& v) {
    if (!v.has_value()) return os << "nullopt";
    return os << *v;
}

namespace relay::testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline std::string& CurrentTest() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

inline void ReportFailure(const char* file, int line, const std::string& message) {
    ++FailureCount();
    std::cerr << "  FAIL " << file << ":" << line << "\n        " << message << "\n";
}

inline int RunAll(int argc, char** argv) {
    // A single argument filters by substring, so a failing case can be rerun
    // alone under a debugger without recompiling.
    std::string filter = argc > 1 ? argv[1] : "";

    int ran = 0, failed_tests = 0;
    for (auto& tc : Registry()) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;

        CurrentTest() = tc.name;
        int before = FailureCount();
        std::cout << "RUN  " << tc.name << "\n";
        tc.fn();
        ++ran;

        if (FailureCount() > before) {
            ++failed_tests;
            std::cout << "FAIL " << tc.name << "\n";
        } else {
            std::cout << "ok   " << tc.name << "\n";
        }
    }

    std::cout << "\n" << ran << " tests, " << failed_tests << " failed\n";
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace relay::testing

#define RELAY_TEST(name)                                                     \
    static void name();                                                        \
    static ::relay::testing::Registrar _reg_##name(#name, name);              \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::relay::testing::ReportFailure(__FILE__, __LINE__,               \
                                             "CHECK(" #cond ") is false");     \
        }                                                                      \
    } while (false)

// CHECK_EQ prints both sides, because "assertion failed" without the values is
// an invitation to add a print statement and recompile.
//
// The operands are captured BY VALUE, not by `auto&&`. That is deliberate and
// it cost a debugging session to learn: `auto&& _a = expr` lifetime-extends a
// temporary *value*, but it does not extend a reference that points *into* a
// temporary. So `CHECK_EQ(Parse(json)->AsString(), "x")` — where AsString
// returns `const std::string&` and Parse returns a temporary shared_ptr —
// bound a reference to a string whose owner was destroyed at the end of that
// very statement, and the failure output printed freed memory.
//
// Copying costs a allocation per assertion in a test binary, which is nothing,
// and removes the whole class of trap for every accessor that returns a
// reference.
#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                       \
        if (!(_a == _b)) {                                                     \
            std::ostringstream _os;                                            \
            _os << #a " == " #b "\n          left:  " << _a                    \
                << "\n          right: " << _b;                                \
            ::relay::testing::ReportFailure(__FILE__, __LINE__, _os.str());   \
        }                                                                      \
    } while (false)

#define CHECK_NE(a, b)                                                         \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                       \
        if ((_a == _b)) {                                                      \
            std::ostringstream _os;                                            \
            _os << #a " != " #b " but both are " << _a;                        \
            ::relay::testing::ReportFailure(__FILE__, __LINE__, _os.str());   \
        }                                                                      \
    } while (false)

// CHECK_OK unwraps a Status and prints its message, which is the single most
// common thing to want when a storage or parse call fails mid-test.
#define CHECK_OK(expr)                                                         \
    do {                                                                       \
        ::relay::Status _s = (expr);                                          \
        if (!_s.ok()) {                                                        \
            ::relay::testing::ReportFailure(__FILE__, __LINE__,               \
                                             #expr " failed: " + _s.ToString()); \
        }                                                                      \
    } while (false)

#define CHECK_NOT_OK(expr)                                                     \
    do {                                                                       \
        ::relay::Status _s = (expr);                                          \
        if (_s.ok()) {                                                         \
            ::relay::testing::ReportFailure(__FILE__, __LINE__,               \
                                             #expr " unexpectedly succeeded"); \
        }                                                                      \
    } while (false)

#define RELAY_TEST_MAIN()                                                     \
    int main(int argc, char** argv) {                                          \
        return ::relay::testing::RunAll(argc, argv);                          \
    }
