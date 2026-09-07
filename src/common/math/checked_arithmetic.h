#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace mmltk::common::math {

template <typename T, typename U>
[[nodiscard]] T checked_cast(const U value, const char* const context) {
    static_assert(std::is_integral_v<T>, "checked_cast target must be an integer type");
    static_assert(std::is_integral_v<U>, "checked_cast source must be an integer type");

    if constexpr (std::is_signed_v<U>) {
        const auto signed_value = static_cast<std::intmax_t>(value);
        if constexpr (std::is_signed_v<T>) {
            if (signed_value < static_cast<std::intmax_t>(std::numeric_limits<T>::min()) ||
                signed_value > static_cast<std::intmax_t>(std::numeric_limits<T>::max())) {
                throw std::overflow_error(context);
            }
        } else if (signed_value < 0 ||
                   static_cast<std::uintmax_t>(signed_value) > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
            throw std::overflow_error(context);
        }
    } else {
        const auto unsigned_value = static_cast<std::uintmax_t>(value);
        if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) { throw std::overflow_error(context); }
    }
    return static_cast<T>(value);
}

template <typename T>
[[nodiscard]] T checked_add(const T left, const std::type_identity_t<T> right, const char* const context) {
    static_assert(std::is_unsigned_v<T>, "checked_add requires an unsigned integer type");
    if (right > std::numeric_limits<T>::max() - left) { throw std::overflow_error(context); }
    return left + right;
}

template <typename T>
[[nodiscard]] T checked_multiply(const T left, const std::type_identity_t<T> right, const char* const context) {
    static_assert(std::is_unsigned_v<T>, "checked_multiply requires an unsigned integer type");
    if (left != 0U && right > std::numeric_limits<T>::max() / left) { throw std::overflow_error(context); }
    return left * right;
}

}  // namespace mmltk::common::math
