#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::testsupport {

template <typename T>
[[nodiscard]] const T& require_optional_ref(const std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) { throw std::runtime_error(std::string(message)); }
    return *value;
}

template <typename T>
[[nodiscard]] T& require_optional_ref(std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) { throw std::runtime_error(std::string(message)); }
    return *value;
}

template <typename T>
[[nodiscard]] T require_optional_value(const std::optional<T>& value, const std::string_view message) {
    return require_optional_ref(value, message);
}

template <typename T>
[[nodiscard]] const T& require_pointer(const T* value, const std::string_view message) {
    if (value == nullptr) { throw std::runtime_error(std::string(message)); }
    return *value;
}

}  // namespace mmltk::testsupport
