#pragma once

#ifdef CHECK
#undef CHECK
#endif

#ifdef assert
#undef assert
#endif

#include <catch2/catch_test_macros.hpp>

#ifdef assert
#undef assert
#endif

#define assert(...) REQUIRE((__VA_ARGS__))

#define MMLTK_REGISTER_TEST_CASE(tags, fn) \
    TEST_CASE(#fn, tags) {                \
        fn();                             \
    }
