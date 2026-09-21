#pragma once
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "reflected_cbor.h"
#include "json_wire.h"
namespace mmltk::frameworks::serialization {
// Persistence reuses the canonical named projection and ingress validation.
// No JSON-specific field or enum inventory is maintained.
template <class T>
[[nodiscard]] nlohmann::json reflected_json(const T& value, std::span<std::byte> scratch, wire::Limits limits) {
 const auto encoded = encode(value, scratch, limits);
 if (!encoded) throw std::runtime_error("record violates reflected persistence constraints");
 return json_from_cbor({scratch.first(*encoded), {}}, limits);
}
template <class T>
[[nodiscard]] T decode_reflected_json(std::string_view text, wire::Limits limits) {
 const auto bytes = json_to_cbor(text, limits);
 auto result = decode<T>({bytes, {}}, limits);
 if (!result) throw std::runtime_error("invalid reflected JSON record");
 return std::move(*result);
}
}  // namespace mmltk::frameworks::serialization
