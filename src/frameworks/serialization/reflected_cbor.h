#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

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

using CompactShape = implementation::compact_detail::Shape;
template <class T>
inline constexpr CompactShape compact_shape = implementation::compact_detail::shape<T>;
template <class T>
[[nodiscard]] consteval std::size_t compact_maximum_cbor_bytes() {
    return implementation::compact_detail::maximum_bytes<T>();
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

// The named variant vocabulary is structural CBOR policy, shared by generated
// writers and the borrowed projection. Alternative membership remains exhaustive.
template <class Variant, class Alternative>
struct ReflectedVariantEnvelope final {
    static_assert([]<class... T>(std::type_identity<std::variant<T...>>) {
        return (std::same_as<Alternative, T> || ...);
    }(std::type_identity<Variant>{}), "borrowed alternative must belong to its canonical variant");
    static constexpr auto kind_key = implementation::detail::VariantEnvelope::kind_key;
    static constexpr auto payload_key = implementation::detail::VariantEnvelope::payload_key;
    static constexpr auto field_count = implementation::detail::VariantEnvelope::field_count;
    static constexpr std::string_view kind = implementation::detail::static_variant_name<Alternative>();
    [[nodiscard]] static bool Read(wire::Reader& reader) {
        auto count = reader.begin_object_item(0U);
        return count && *count == field_count && reader.expect_text_item(1U, kind_key) &&
               reader.expect_text_item(1U, kind) && reader.expect_text_item(1U, payload_key);
    }
};

template <class Record, auto BytesMember>
class BorrowedByteRecord final {
    Record record_{};
    wire::ByteSegments bytes_{};

   public:
    template <auto Member>
    [[nodiscard]] const auto& Get() const noexcept {
        if constexpr (std::meta::reflect_constant(Member) == std::meta::reflect_constant(BytesMember)) return bytes_;
        else return record_.*Member;
    }
    BorrowedByteRecord() = default;
    explicit BorrowedByteRecord(const Record& record) {
        implementation::detail::visit_members<Record>([&]<class Declaration>(const auto&) {
            if constexpr (std::meta::reflect_constant(Declaration::pointer) == std::meta::reflect_constant(BytesMember))
                bytes_.first = record.*BytesMember;
            else record_.*Declaration::pointer = record.*Declaration::pointer;
        });
    }
    template <class Variant>
    [[nodiscard]] bool Decode(wire::Reader& reader) {
        namespace d = implementation::detail;
        d::audit_object<Record>();
        static_assert([] consteval {
            bool direct = true;
            d::visit_bases<Record>([&]<class>() { direct = false; });
            return direct;
        }(), "borrowed byte records require direct reflected fields");
        static_assert(std::same_as<std::remove_cvref_t<decltype(record_.*BytesMember)>, wire::ByteBuffer>,
                      "borrowed byte projection requires a declaration-bounded byte buffer");
        constexpr auto names = [] consteval {
            std::array<std::string_view, d::flattened_member_count<Record>()> result{};
            std::size_t index = 0U;
            d::visit_members<Record>([&]<class>(const auto& fact) { result[index++] = fact.member_name; });
            return result;
        }();
        if (!ReflectedVariantEnvelope<Variant, Record>::Read(reader)) return false;
        auto count = reader.begin_object_item(1U);
        if (!count || *count != names.size()) return false;
        std::array<bool, names.size()> seen{};
        for (std::size_t field = 0U; field < *count; ++field) {
            auto key = reader.read_text_choice(2U, names);
            if (!key || seen[*key]) return false;
            seen[*key] = true;
            bool valid = false;
            std::size_t index = 0U;
            d::visit_members<Record>([&]<class Declaration>(const auto&) {
                if (index++ != *key) return;
                if constexpr (std::meta::reflect_constant(Declaration::pointer) == std::meta::reflect_constant(BytesMember)) {
                    auto bytes = reader.borrow_bytes_item(2U);
                    constexpr auto policy = d::serialized_member_policy<Declaration>();
                    if (bytes && bytes->size() <= policy.maximum_bytes && bytes->size() >= policy.minimum_bytes &&
                        (policy.maximum_items == 0U || bytes->size() <= policy.maximum_items)) {
                        bytes_ = *bytes;
                        valid = true;
                    }
                } else {
                    using Field = typename Declaration::member_type;
                    static_assert(compact_shape<Field> == CompactShape::Scalar,
                                  "borrowed byte record fields must be scalar or the selected byte field");
                    valid = decode_compact_item(record_.*Declaration::pointer, reader, 2U) &&
                            d::member_constraints_accept<Declaration>(record_.*Declaration::pointer);
                }
            });
            if (!valid) return false;
        }
        return reader.finish().has_value();
    }
};

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
