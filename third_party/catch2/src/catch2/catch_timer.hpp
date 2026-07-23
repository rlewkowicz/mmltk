

#ifndef CATCH_TIMER_HPP_INCLUDED
#define CATCH_TIMER_HPP_INCLUDED

#include <cstdint>

namespace Catch {

class Timer {
    uint64_t m_nanoseconds = 0;

   public:
    void start();
    auto getElapsedNanoseconds() const -> uint64_t;
    auto getElapsedMicroseconds() const -> uint64_t;
    auto getElapsedMilliseconds() const -> unsigned int;
    auto getElapsedSeconds() const -> double;
};

}  

#endif  // CATCH_TIMER_HPP_INCLUDED
