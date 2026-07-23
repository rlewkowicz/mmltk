

#ifndef CATCH_CLOCK_HPP_INCLUDED
#define CATCH_CLOCK_HPP_INCLUDED

#include <chrono>

namespace Catch {
namespace Benchmark {
using IDuration = std::chrono::nanoseconds;
using FDuration = std::chrono::duration<double, std::nano>;

template <typename Clock>
using TimePoint = typename Clock::time_point;

using default_clock = std::chrono::steady_clock;
}  
}  

#endif  // CATCH_CLOCK_HPP_INCLUDED
