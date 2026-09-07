

#ifndef CATCH_STREAM_END_STOP_HPP_INCLUDED
#define CATCH_STREAM_END_STOP_HPP_INCLUDED

#include <catch2/internal/catch_stringref.hpp>

namespace Catch {

struct StreamEndStop {
    constexpr StringRef operator+() const {
        return StringRef();
    }

    template <typename T>
    constexpr friend T const& operator+(T const& value, StreamEndStop) {
        // Catch's expression-decomposition chain consumes this reference within the same full expression.
        return value;  // NOLINT(bugprone-return-const-ref-from-parameter)
    }
};

}

#endif  // CATCH_STREAM_END_STOP_HPP_INCLUDED
