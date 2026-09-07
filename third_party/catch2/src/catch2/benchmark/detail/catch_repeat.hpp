

#ifndef CATCH_REPEAT_HPP_INCLUDED
#define CATCH_REPEAT_HPP_INCLUDED

#include <catch2/internal/catch_move_and_forward.hpp>
#include <type_traits>

namespace Catch {
namespace Benchmark {
namespace Detail {
template <typename Fun>
struct repeater {
    void operator()(int k) const {
        for (int i = 0; i < k; ++i) {
            fun();
        }
    }
    Fun fun;
};
template <typename Fun>
repeater<std::decay_t<Fun>> repeat(Fun&& fun) {
    return {CATCH_FORWARD(fun)};
}
}
}
}

#endif  // CATCH_REPEAT_HPP_INCLUDED
