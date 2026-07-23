

#ifndef CATCH_MEASURE_HPP_INCLUDED
#define CATCH_MEASURE_HPP_INCLUDED

#include <catch2/benchmark/detail/catch_complete_invoke.hpp>
#include <catch2/benchmark/detail/catch_timing.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

namespace Catch {
namespace Benchmark {
namespace Detail {
template <typename Clock, typename Fun, typename... Args>
TimingOf<Fun, Args...> measure(Fun&& fun, Args&&... args) {
    auto start = Clock::now();
    auto&& r = Detail::complete_invoke(CATCH_FORWARD(fun), CATCH_FORWARD(args)...);
    auto end = Clock::now();
    auto delta = end - start;
    return {delta, CATCH_FORWARD(r), 1};
}
}  
}  
}  

#endif  // CATCH_MEASURE_HPP_INCLUDED
