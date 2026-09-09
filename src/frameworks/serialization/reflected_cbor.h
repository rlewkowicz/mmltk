#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include "src/frameworks/serialization/cbor_wire.h"
#include "src/frameworks/serialization/reflected_cbor_detail.h"
namespace mmltk::frameworks::serialization {

using FixedCborEncoder = implementation::FixedCborEncoder;

// CLEANUP-IGNORE: This public template is the single stable facade over the private reflected-CBOR implementation.
template <class T>
[[nodiscard]] bool encode_fixed(FixedCborEncoder& writer, const T& value) {
    return implementation::encode_fixed(writer, value);
}

template <class T>
[[nodiscard]] std::expected<void, wire::EncodeError> encode(const T& value, wire::ByteBuffer& destination, const wire::Limits limits) {
    return implementation::encode(value, destination, limits);
}

template <class T>
[[nodiscard]] std::expected<std::size_t, wire::EncodeError> encode(const T& value, const std::span<std::byte> destination,
                                                                   const wire::Limits limits) {
    return implementation::encode(value, destination, limits);
}

template <class T>
[[nodiscard]] std::expected<std::size_t, wire::EncodeError> measure(const T& value, const wire::Limits limits) {
    return implementation::measure(value, limits);
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode(const wire::ByteSegments bytes, const wire::Limits limits) {
    // CLEANUP-IGNORE: Public decode forwards once to the implementation that owns allocation policy and validation.
    return implementation::decode<T>(bytes, limits);
}

template <class T>
[[nodiscard]] std::expected<void, wire::DecodeError> decode_into(T& destination, const wire::Value& value) {
    return implementation::decode_into(destination, value);
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> reflected_value(const T& value) {
    return implementation::reflected_value(value);
}

template <class T>
[[nodiscard]] bool encode_compact(FixedCborEncoder& writer, const T& value) {
    return implementation::compact_detail::encode(writer, value);
}
template <class T>
[[nodiscard]] bool decode_compact_item(T& value, wire::Reader& reader, const std::size_t depth) {
    return implementation::compact_detail::decode(reader, value, depth);
}
template <class T>
[[nodiscard]] bool decode_compact_into(T& value, wire::ByteSegments bytes, wire::Limits limits) {
    wire::Reader reader(bytes, limits);
    return implementation::compact_detail::decode(reader, value, 0U) && reader.finish().has_value();
}

template <class T>
inline constexpr bool reflected_byte_sequence = implementation::reflected_byte_sequence<T>;

template <class T>
[[nodiscard]] consteval std::size_t reflected_maximum_cbor_bytes() {
    return implementation::reflected_maximum_cbor_bytes<T>();
}

template <class T>
[[nodiscard]] constexpr std::size_t reflected_structural_cbor_bytes(const std::size_t payload_budget = 0U) {
    // The caller admits the sum of complete encoded wire::Value payloads,
    // including their heads and descendants. All other bytes remain structural.
    constexpr auto structural =
        implementation::detail::maximum_cbor_bytes<T, implementation::detail::DynamicValueBound::ExternallyBudgeted>();
    return implementation::detail::cbor_size_add(structural, payload_budget);
}

template <class T>
[[nodiscard]] consteval std::size_t reflected_cbor_member_count() {
    implementation::detail::audit_object<T>();
    return implementation::detail::maximum_reflected_object_member_count<T>();
}

template <class T>
[[nodiscard]] std::string reflected_schema_type_name() {
    return implementation::reflected_schema_type_name<T>();
}

}  // namespace mmltk::frameworks::serialization
