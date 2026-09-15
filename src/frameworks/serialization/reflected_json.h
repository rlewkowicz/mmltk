#pragma once

#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "reflected_cbor.h"

namespace mmltk::frameworks::serialization {
// Persistence reuses the canonical named projection and ingress validation.
// No JSON-specific field or enum inventory is maintained.
template <class T>
[[nodiscard]] nlohmann::json reflected_json(const T& value, std::span<std::byte> scratch, wire::Limits limits) {
    const auto encoded = encode(value, scratch, limits);
    if (!encoded) throw std::runtime_error("record violates reflected persistence constraints");
    const auto* first = reinterpret_cast<const std::uint8_t*>(scratch.data());
    return nlohmann::json::from_cbor(first, first + *encoded);
}
template <class T>
[[nodiscard]] T decode_reflected_json(std::string_view text, wire::Limits limits) {
    if (text.size() > limits.max_bytes) throw std::runtime_error("JSON record exceeds its byte bound");
    const auto json = nlohmann::json::parse(text);
    const auto bytes = nlohmann::json::to_cbor(json);
    if (bytes.size() > limits.max_bytes) throw std::runtime_error("JSON record exceeds its encoded byte bound");
    const auto result = decode<T>({std::as_bytes(std::span(bytes)), {}}, limits);
    if (!result) throw std::runtime_error("invalid reflected JSON record");
    return *result;
}
}  // namespace mmltk::frameworks::serialization
