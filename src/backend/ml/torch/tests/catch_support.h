#pragma once
// Admit Torch's headers before test assertions so its CHECK cannot replace
// Catch2's nonfatal assertion. Preserve an already admitted Catch2 definition.
#pragma push_macro("CHECK")
#include <torch/types.h>
#pragma pop_macro("CHECK")
#if defined(CHECK) && !defined(CATCH_TEST_MACROS_HPP_INCLUDED)
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>
