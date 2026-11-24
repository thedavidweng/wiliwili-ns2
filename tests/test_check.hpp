// Shared host-test helpers: every test is one ctest case that counts its own
// failures, so a failing check reports its line instead of aborting the run.
//
// WILIWILI_SOURCE_DIR is the repository root, set by tests/CMakeLists.txt for
// the checks that read a file instead of calling into it.

#pragma once

#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <type_traits>

#ifndef WILIWILI_SOURCE_DIR
#error "test_check.hpp needs WILIWILI_SOURCE_DIR, see tests/CMakeLists.txt"
#endif

namespace test_check {

inline int failures = 0;

inline void fail(const std::string& what, int line) {
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
    ++failures;
}

inline void check(bool ok, const std::string& what, int line) {
    if (!ok) fail(what, line);
}

// Numbers print as numbers, everything else as text: CHECK_EQ is used for both.
template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_arithmetic<T>::value)
        return std::to_string(value);
    else
        return std::string(value);
}

template <typename Actual, typename Expected>
void checkEqual(const Actual& actual, const Expected& expected, const std::string& what, int line) {
    if (actual == expected) return;

    std::ostringstream message;
    message << what << " (got \"" << describe(actual) << "\", want \"" << describe(expected) << "\")";
    fail(message.str(), line);
}

// A repository file read from the source root. Used for the contracts a host
// build cannot exercise: the boot log, the pinned configuration, the code that
// only a console reaches.
inline std::string readSource(const std::string& relative) {
    std::ifstream file(std::string(WILIWILI_SOURCE_DIR) + "/" + relative);
    if (!file) {
        std::cerr << "FAIL: cannot read " << relative << '\n';
        ++failures;
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// 0 when every check passed, 1 with a summary line when one did not.
inline int report(const char* passed) {
    if (failures == 0) {
        std::cout << passed << '\n';
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}

}  // namespace test_check

#define CHECK(cond) ::test_check::check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) \
    ::test_check::checkEqual((actual), (expected), std::string(#actual) + " == " + #expected, __LINE__)
