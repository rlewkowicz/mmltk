

#ifndef CATCH_TIMING_HPP_INCLUDED
#define CATCH_TIMING_HPP_INCLUDED

#include <catch2/benchmark/catch_clock.hpp>
#include <catch2/benchmark/detail/catch_complete_invoke.hpp>

namespace Catch {
namespace Benchmark {
template <typename Result>
struct Timing {
    IDuration elapsed;
    Result result;
    int iterations;
};
template <typename Func, typename... Args>
using TimingOf = Timing<Detail::CompleteType_t<FunctionReturnType<Func, Args...>>>;
}  
}  

#endif  // CATCH_TIMING_HPP_INCLUDED
