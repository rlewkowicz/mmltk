

#ifndef CATCH_TEST_RUN_INFO_HPP_INCLUDED
#define CATCH_TEST_RUN_INFO_HPP_INCLUDED

#include <catch2/internal/catch_stringref.hpp>

namespace Catch {

struct TestRunInfo {
    constexpr TestRunInfo(StringRef _name) : name(_name) {}
    StringRef name;
};

}  

#endif  // CATCH_TEST_RUN_INFO_HPP_INCLUDED
