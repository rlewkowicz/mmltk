#pragma once

// Test assertions and registrations intentionally keep each scenario's oracle at its call site.
// CLEANUP-IGNORE-MACRO CHECK
// CLEANUP-IGNORE-MACRO CHECK_FALSE
// CLEANUP-IGNORE-MACRO CHECK_THROWS
// CLEANUP-IGNORE-MACRO CHECK_THROWS_AS
// CLEANUP-IGNORE-MACRO CAPTURE
// CLEANUP-IGNORE-MACRO FAIL
// CLEANUP-IGNORE-MACRO FAIL_CHECK
// CLEANUP-IGNORE-MACRO INFO
// CLEANUP-IGNORE-MACRO MMLTK_ASSERT
// CLEANUP-IGNORE-MACRO MMLTK_REGISTER_TEST_CASE
// CLEANUP-IGNORE-MACRO REQUIRE
// CLEANUP-IGNORE-MACRO REQUIRE_FALSE
// CLEANUP-IGNORE-MACRO REQUIRE_THROWS_AS
// CLEANUP-IGNORE-MACRO SKIP
// CLEANUP-IGNORE-MACRO STATIC_CHECK
// CLEANUP-IGNORE-MACRO STATIC_CHECK_FALSE

#ifndef CATCH_TEST_MACROS_HPP_INCLUDED
#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>
#endif
#include <optional>

#define MMLTK_ASSERT(...) REQUIRE((__VA_ARGS__))

// Catch2's REQUIRE aborts the case before any dereference runs, but the optional
// analysis cannot see through the macro expansion. Tests reach an engaged
// optional's value through here, so the one unchecked access in the suites lives
// in a single reviewed place instead of at every call site.
template <typename T>
[[nodiscard]] T& require_engaged(std::optional<T>& value) {
    REQUIRE(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
}

template <typename T>
[[nodiscard]] const T& require_engaged(const std::optional<T>& value) {
    REQUIRE(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
}

#define MMLTK_REGISTER_TEST_CASE(tags, fn) \
    TEST_CASE(#fn, tags) { fn(); }

// Defines and registers a Catch2 test case in one place, so a suite needs no separate registration
// table. Use as `MMLTK_TEST_CASE("[tag]", test_name) { ... }`. Suites normally wrap this once with a
// suite-local macro that binds the tag, leaving each case a single `SUITE_TEST_CASE(test_name) { ... }`.
#define MMLTK_TEST_CASE(tags, fn) TEST_CASE(#fn, tags)
