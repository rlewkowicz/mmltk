#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "catch2_compat.hpp"
namespace mmltk::testsupport {
// Runs `fn` and requires that it throws `Error` whose message contains `needle`.
// An empty `needle` only requires that the matching exception type is thrown.
template <typename Error = std::runtime_error, typename Fn>
void expect_error_contains(Fn&& fn, const std::string_view needle = {}) {
    bool threw = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const Error& error) {
        threw = true;
        MMLTK_ASSERT(std::string_view(error.what()).find(needle) != std::string_view::npos);
    }
    MMLTK_ASSERT(threw);
}
template <typename Fn>
void expect_runtime_error_contains(Fn&& fn, const std::string_view needle = {}) {
    expect_error_contains<std::runtime_error>(std::forward<Fn>(fn), needle);
}
}  // namespace mmltk::testsupport
