#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <inplace_vector>
#include <iterator>
#include <limits>
#include <meta>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/serialization/cbor_field_annotations.h"
#include "src/frameworks/serialization/cbor_wire.h"
namespace mmltk::frameworks::reflection {

template <>
struct LeafPolicy<mmltk::frameworks::serialization::wire::Value> {
    static constexpr bool bounded_dynamic = true;
};

template <>
struct LeafPolicy<mmltk::frameworks::serialization::wire::FlatValue> {
    static constexpr bool bounded_dynamic = true;
};

}  // namespace mmltk::frameworks::reflection

namespace mmltk::frameworks::serialization::implementation {

class FixedCborEncoder;

namespace detail {

struct VariantEnvelope final {
    static constexpr std::string_view kind_key = "kind";
    static constexpr std::string_view payload_key = "payload";
    static constexpr std::size_t field_count = 2U;
};

using mmltk::frameworks::reflection::materialized_field_policies;
using mmltk::frameworks::reflection::OptionalValueT;
using mmltk::frameworks::reflection::RemoveCvRef;

template <class T>
concept HasDeclarations = requires { materialized_field_policies(std::type_identity<RemoveCvRef<T>>{}); };

template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {
    using value_type = T;
};
template <class T>
inline constexpr bool kIsOptional = IsOptional<RemoveCvRef<T>>::value;

template <class T>
struct IsVector : std::false_type {};
template <class T, class Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {
    using value_type = T;
};
template <class T>
inline constexpr bool kIsVector = IsVector<RemoveCvRef<T>>::value;

template <class T>
struct IsArray : std::false_type {};
template <class T, std::size_t Count>
struct IsArray<std::array<T, Count>> : std::true_type {
    using value_type = T;
    static constexpr std::size_t size = Count;
};
template <class T>
inline constexpr bool kIsArray = IsArray<RemoveCvRef<T>>::value;

template <class T>
struct IsSpan : std::false_type {};
template <class T, std::size_t Count>
struct IsSpan<std::span<T, Count>> : std::true_type {
    // CLEANUP-IGNORE: Span and array expose different compile-time extent semantics through distinct canonical traits.
    using value_type = T;
    // CLEANUP-IGNORE: A span extent and an array size are deliberately exposed under their standard vocabulary.
    static constexpr std::size_t extent = Count;
};
template <class T>
inline constexpr bool kIsSpan = IsSpan<RemoveCvRef<T>>::value;

template <class T>
struct IsInplaceVector : std::false_type {};
template <class T, std::size_t Count>
struct IsInplaceVector<std::inplace_vector<T, Count>> : std::true_type {
    using value_type = T;
    static constexpr std::size_t capacity = Count;
};
template <class T>
inline constexpr bool kIsInplaceVector = IsInplaceVector<RemoveCvRef<T>>::value;

template <class T>
inline constexpr bool kIsSequence = kIsVector<T> || kIsArray<T> || kIsSpan<T> || kIsInplaceVector<T>;

template <class T>
using SequenceElementT = typename std::conditional_t<
    kIsVector<RemoveCvRef<T>>, IsVector<RemoveCvRef<T>>,
    std::conditional_t<kIsArray<RemoveCvRef<T>>, IsArray<RemoveCvRef<T>>,
                       std::conditional_t<kIsSpan<RemoveCvRef<T>>, IsSpan<RemoveCvRef<T>>, IsInplaceVector<RemoveCvRef<T>>>>>::value_type;

template <class T>
using NestedValueT =
    typename std::conditional_t<kIsOptional<RemoveCvRef<T>>, IsOptional<RemoveCvRef<T>>,
                                std::conditional_t<kIsVector<RemoveCvRef<T>>, IsVector<RemoveCvRef<T>>,
                                                   std::conditional_t<kIsArray<RemoveCvRef<T>>, IsArray<RemoveCvRef<T>>,
                                                                      std::conditional_t<kIsSpan<RemoveCvRef<T>>, IsSpan<RemoveCvRef<T>>,
                                                                                         IsInplaceVector<RemoveCvRef<T>>>>>>::value_type;

template <class T>
struct IsVariant : std::false_type {};
template <class... T>
struct IsVariant<std::variant<T...>> : std::true_type {};
template <class T>
inline constexpr bool kIsVariant = IsVariant<RemoveCvRef<T>>::value;

template <class... Types>
struct UniqueTypes : std::true_type {};
template <class First, class... Rest>
struct UniqueTypes<First, Rest...> : std::bool_constant<(!(std::is_same_v<First, Rest> || ...) && UniqueTypes<Rest...>::value)> {};
template <class T>
inline constexpr bool kUniqueVariantAlternatives = []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) {
    return UniqueTypes<Alternatives...>::value;
}(std::type_identity<RemoveCvRef<T>>{});

template <class T>
inline constexpr bool kByteSequence = mmltk::frameworks::reflection::kByteSequence<T>;

template <class T>
inline constexpr bool kFlatScalar =
    std::is_same_v<RemoveCvRef<T>, std::string> || std::is_same_v<RemoveCvRef<T>, wire::ByteBuffer> ||
    std::is_same_v<RemoveCvRef<T>, std::monostate> || std::is_same_v<RemoveCvRef<T>, bool> ||
    std::is_same_v<RemoveCvRef<T>, std::int64_t> || std::is_same_v<RemoveCvRef<T>, std::uint64_t> || std::is_same_v<RemoveCvRef<T>, double>;

template <class Byte>
[[nodiscard]] constexpr std::byte to_wire_byte(const Byte value) noexcept {
    if constexpr (std::is_same_v<RemoveCvRef<Byte>, std::byte>) {
        return value;
    } else {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    }
}

template <class Byte>
[[nodiscard]] constexpr Byte from_wire_byte(const std::byte value) noexcept {
    if constexpr (std::is_same_v<RemoveCvRef<Byte>, std::byte>) {
        return value;
    } else {
        return static_cast<Byte>(std::to_integer<std::uint8_t>(value));
    }
}

[[nodiscard]] wire::EncodeError encode_error(wire::ErrorCode code);
[[nodiscard]] wire::DecodeError decode_error(wire::ErrorCode code);
void prepend_path(wire::Error& error, std::string_view component);

template <class T>
[[nodiscard]] consteval decltype(auto) declarations() {
    return mmltk::frameworks::reflection::field_declarations<T>();
}

template <class T, class Visitor>
constexpr void visit_bases(Visitor&& visitor) {
    mmltk::frameworks::reflection::visit_materialized_bases<T>(std::forward<Visitor>(visitor));
}

template <class T, class Visitor>
constexpr void visit_members(Visitor&& visitor) {
    mmltk::frameworks::reflection::visit_materialized_members<T>(std::forward<Visitor>(visitor));
}

template <class T>
[[nodiscard]] consteval auto enumerators() {
    return mmltk::frameworks::reflection::enum_entries<T>();
}

template <class T>
[[nodiscard]] consteval bool supported_type();
template <class T>
[[nodiscard]] consteval std::string_view static_variant_name();

template <class Variant>
[[nodiscard]] consteval bool unique_variant_names() {
    return []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) consteval {
        constexpr std::array<std::string_view, sizeof...(Alternatives)> names{static_variant_name<Alternatives>()...};
        for (std::size_t left = 0U; left < names.size(); ++left) {
            for (std::size_t right = left + 1U; right < names.size(); ++right) {
                if (names[left] == names[right]) { return false; }
            }
        }
        return true;
    }(std::type_identity<RemoveCvRef<Variant>>{});
}

template <class T>
inline constexpr bool kReflectedObject = [] consteval {
    using U = RemoveCvRef<T>;
    if constexpr (!std::is_class_v<U> || std::is_same_v<U, wire::Value> || std::is_same_v<U, wire::FlatValue> ||
                  std::is_same_v<U, std::string> || std::is_same_v<U, std::filesystem::path> || kIsOptional<U> || kIsVector<U> ||
                  kIsArray<U> || kIsSpan<U> || kIsInplaceVector<U> || kIsVariant<U>) {
        return false;
    } else {
        return std::is_default_constructible_v<U> && HasDeclarations<U>;
    }
}();

template <class T>
[[nodiscard]] consteval std::size_t flattened_member_name_count(const std::string_view name) {
    std::size_t count = 0U;
    visit_bases<T>([&]<class Base>() { count += flattened_member_name_count<Base>(name); });
    visit_members<T>([&]<class Declaration>(const auto& fact) { count += fact.member_name == name ? 1U : 0U; });
    return count;
}

template <class Root, class Current = Root>
[[nodiscard]] consteval bool unique_flattened_member_names() {
    bool unique = true;
    visit_bases<Current>([&]<class Base>() { unique = unique && unique_flattened_member_names<Root, Base>(); });
    visit_members<Current>(
        [&]<class Declaration>(const auto& fact) { unique = unique && flattened_member_name_count<Root>(fact.member_name) == 1U; });
    return unique;
}

template <class T>
inline constexpr bool kByteBoundedMember = std::is_same_v<OptionalValueT<T>, std::string> ||
                                           std::is_same_v<OptionalValueT<T>, std::filesystem::path> || kByteSequence<OptionalValueT<T>>;
template <class T>
inline constexpr bool kNeedsByteLimit = kByteBoundedMember<T> && !kIsArray<OptionalValueT<T>>;

template <class T>
inline constexpr bool kItemBoundedMember =
    !kByteSequence<OptionalValueT<T>> &&
    (kIsVector<OptionalValueT<T>> || kIsArray<OptionalValueT<T>> || kIsSpan<OptionalValueT<T>> || kIsInplaceVector<OptionalValueT<T>>);

template <class Declaration>
[[nodiscard]] consteval mmltk::frameworks::reflection::FieldConstraint serialized_member_policy() {
    if constexpr (requires { Declaration::pointer; }) {
        return mmltk::frameworks::reflection::policy_of_member<Declaration::pointer>();
    } else {
        return Declaration::constraint;
    }
}

template <class Declaration>
[[nodiscard]] consteval bool has_materialized_policy_only_annotations() {
    if constexpr (requires { Declaration::pointer; })
        return mmltk::frameworks::reflection::has_materialized_policy_only_annotations<Declaration::pointer>();
    return true;
}

template <class Declaration>
[[nodiscard]] consteval bool serialized_policy_annotations_are_valid() {
    if constexpr (requires { Declaration::pointer; }) {
        return mmltk::frameworks::reflection::materialized_policy_annotations_are_valid<Declaration::pointer>();
    } else {
        return Declaration::annotations_valid;
    }
}

template <class Declaration>
[[nodiscard]] consteval bool valid_member_annotations() {
    using MemberType = typename Declaration::member_type;
    constexpr auto application_policy = serialized_member_policy<Declaration>();
    constexpr bool has_byte_minimum = application_policy.minimum_bytes != 0U;
    constexpr bool has_byte_maximum = application_policy.maximum_bytes != 0U;
    std::size_t item_minima = 0U;
    constexpr bool has_item_maximum = application_policy.maximum_items != 0U;
    std::size_t canonical_nonzero_uint64_markers = 0U;
    std::size_t exact_string_constraints = 0U;
    bool valid = serialized_policy_annotations_are_valid<Declaration>();
    std::size_t minimum_bytes = application_policy.minimum_bytes;
    std::size_t maximum_bytes = application_policy.maximum_bytes;
    std::size_t minimum_items = 0U;
    std::size_t maximum_items = application_policy.maximum_items;
    Declaration::VisitAnnotations([&]<class Annotation>(const Annotation& annotation) {
        using A = RemoveCvRef<Annotation>;
        if constexpr (mmltk::frameworks::reflection::kPolicyAnnotation<A>) {
            // Field policy owns minimum, maximum, finite, byte/item
            // maxima, and presentation parsing.
            (void)annotation;
        } else if constexpr (std::is_same_v<A, MinItems>) {
            ++item_minima;
            minimum_items = annotation.value;
            valid = valid &&
                    (kItemBoundedMember<MemberType> || std::is_same_v<OptionalValueT<MemberType>, wire::Value> ||
                     std::is_same_v<OptionalValueT<MemberType>, wire::FlatValue>) &&
                    annotation.value > 0U;
            if constexpr (kIsArray<OptionalValueT<MemberType>>) {
                valid = valid && annotation.value <= IsArray<OptionalValueT<MemberType>>::size;
            } else if constexpr (kIsInplaceVector<OptionalValueT<MemberType>>) {
                valid = valid && annotation.value <= IsInplaceVector<OptionalValueT<MemberType>>::capacity;
            }
        } else if constexpr (std::is_same_v<A, CanonicalNonzeroUint64>) {
            ++canonical_nonzero_uint64_markers;
            valid = valid && std::is_same_v<OptionalValueT<MemberType>, std::string>;
        } else if constexpr (is_exact_string_annotation<A>) {
            ++exact_string_constraints;
            valid = valid && std::is_same_v<OptionalValueT<MemberType>, std::string> && !annotation.view().empty();
        } else if constexpr (is_default_value_annotation<A>) {
            valid = valid && std::is_constructible_v<MemberType, decltype(A::value)>;
        } else {
            // Boundary-specific annotations are audited by their
            // owning projection.
            (void)annotation;
        }
    });
    valid = valid && item_minima <= 1U && canonical_nonzero_uint64_markers <= 1U && exact_string_constraints <= 1U;
    valid = valid && (!has_byte_minimum || !has_byte_maximum || minimum_bytes <= maximum_bytes);
    valid = valid && (item_minima == 0U || !has_item_maximum || minimum_items <= maximum_items);
    if constexpr (kNeedsByteLimit<MemberType>) { valid = valid && has_byte_maximum; }
    if constexpr (kItemBoundedMember<MemberType> && (kIsVector<OptionalValueT<MemberType>> || kIsSpan<OptionalValueT<MemberType>>)) {
        valid = valid && has_item_maximum;
    }
    if constexpr (std::is_same_v<OptionalValueT<MemberType>, wire::Value> || std::is_same_v<OptionalValueT<MemberType>, wire::FlatValue>) {
        // Dynamic CBOR is bounded by the enclosing message unless this
        // declaration opts into a narrower, recursive field budget. A
        // partial budget would describe an unenforceable contract, so both
        // dimensions travel together.
        valid = valid && !has_byte_minimum && item_minima == 0U && (has_byte_maximum == has_item_maximum);
    }
    return valid;
}

template <class Declaration, class Value>
[[nodiscard]] bool member_constraints_accept(const Value& member_value) {
    if constexpr (kIsOptional<Value>) {
        if (!member_value) { return true; }
        return member_constraints_accept<Declaration>(*member_value);
    } else {
        constexpr auto policy = serialized_member_policy<Declaration>();
        bool valid = mmltk::frameworks::reflection::field_value_satisfies(member_value, policy);
        // CLEANUP-IGNORE: Runtime constraint evaluation consumes the compile-time policy but owns dynamic CBOR budgets.
        std::size_t dynamic_max_bytes = policy.maximum_bytes;
        std::size_t dynamic_max_items = policy.maximum_items;
        Declaration::VisitAnnotations([&]<class Annotation>(const Annotation& annotation) {
            using A = RemoveCvRef<Annotation>;
            if constexpr (mmltk::frameworks::reflection::kPolicyAnnotation<A>) {
                // The materialized policy above is the one
                // parser/schema/CBOR source for shared bounds. Do not
                // reapply it by annotation.
                (void)annotation;
            } else if constexpr (std::is_same_v<A, MinItems>) {
                if constexpr (std::is_same_v<RemoveCvRef<Value>, wire::Value> || std::is_same_v<RemoveCvRef<Value>, wire::FlatValue>) {
                    valid = false;
                } else {
                    valid = valid && member_value.size() >= annotation.value;
                }
            } else if constexpr (std::is_same_v<A, CanonicalNonzeroUint64>) {
                valid = valid && is_canonical_nonzero_uint64(member_value);
            } else if constexpr (is_exact_string_annotation<A>) {
                valid = valid && member_value == annotation.view();
            }
        });
        if constexpr (std::is_same_v<RemoveCvRef<Value>, wire::Value> || std::is_same_v<RemoveCvRef<Value>, wire::FlatValue>) {
            if (dynamic_max_bytes != 0U || dynamic_max_items != 0U) {
                valid = valid && wire::dynamic_value_within_limits(member_value, {.max_bytes = dynamic_max_bytes,
                                                                                  .max_items = dynamic_max_items,
                                                                                  .max_depth = wire::kMaximumNestingDepth});
            }
        }
        return valid;
    }
}

template <class T>
[[nodiscard]] consteval bool valid_type_annotations();

template <class T, std::size_t Depth = 0U, class... Active>
[[nodiscard]] consteval bool valid_type_graph() {
    using U = RemoveCvRef<T>;
    if constexpr (!valid_type_annotations<U>()) {
        return false;
    } else if constexpr (Depth > wire::kMaximumNestingDepth || std::is_reference_v<T>) {
        return false;
    } else if constexpr ((std::is_same_v<U, Active> || ...)) {
        // Recursion is valid only after a bounded sequence/optional edge; all
        // direct by-value cycles are already ill-formed C++ object layouts.
        return true;
    } else if constexpr (kIsOptional<U>) {
        return valid_type_graph<typename IsOptional<U>::value_type, Depth + 1U, Active..., U>();
    } else if constexpr (kIsVector<U>) {
        return valid_type_graph<typename IsVector<U>::value_type, Depth + 1U, Active..., U>();
    } else if constexpr (kIsArray<U>) {
        return valid_type_graph<typename IsArray<U>::value_type, Depth + 1U, Active..., U>();
    } else if constexpr (kIsSpan<U>) {
        return valid_type_graph<typename IsSpan<U>::value_type, Depth + 1U, Active..., U>();
    } else if constexpr (kIsInplaceVector<U>) {
        return valid_type_graph<typename IsInplaceVector<U>::value_type, Depth + 1U, Active..., U>();
    } else if constexpr (kIsVariant<U>) {
        return []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) consteval {
            return (valid_type_graph<Alternatives, Depth + 1U, Active..., U>() && ...);
        }(std::type_identity<U>{});
    } else if constexpr (kReflectedObject<U>) {
        if constexpr (!std::is_default_constructible_v<U>) { return false; }
        bool valid = true;
        visit_bases<U>([&]<class Base>() { valid = valid && valid_type_graph<Base, Depth + 1U, Active..., U>(); });
        visit_members<U>([&]<class Declaration>(const auto&) {
            using Member = typename Declaration::member_type;
            valid = valid && valid_member_annotations<Declaration>() && valid_type_graph<Member, Depth + 1U, Active..., U>();
        });
        return valid;
    }
    return supported_type<U>();
}

template <class T>
[[nodiscard]] consteval bool valid_type_annotations() {
    return true;
}

template <class T>
consteval void audit_object() {
    static_assert(kReflectedObject<T>, "Reflected CBOR objects must be default-constructible reflected classes.");
    static_assert(unique_flattened_member_names<T>(), "Reflected CBOR member identifiers, including inherited members, must be unique.");
    static_assert(valid_type_graph<T>(), "Reflected CBOR type graph or annotation contract is invalid.");
    visit_members<T>([]<class Declaration>(const auto&) {
        using Member = typename Declaration::member_type;
        static_assert(supported_type<Member>(), "Reflected CBOR member has no supported codec.");
        static_assert(valid_type_graph<Member>(), "Reflected CBOR member type graph is invalid.");
        static_assert(valid_member_annotations<Declaration>(), "Reflected CBOR member annotations are invalid.");
    });
}

template <class T>
[[nodiscard]] consteval bool supported_type() {
    using U = RemoveCvRef<T>;
    if constexpr (std::is_same_v<U, wire::Value> || std::is_same_v<U, wire::FlatValue> || std::is_same_v<U, std::monostate> ||
                  std::is_same_v<U, bool> || std::is_same_v<U, std::byte> || std::is_enum_v<U> || std::is_integral_v<U> ||
                  (std::is_floating_point_v<U> && sizeof(U) <= sizeof(double)) || std::is_same_v<U, std::string> ||
                  std::is_same_v<U, std::filesystem::path> || kByteSequence<U>) {
        return true;
    } else if constexpr (kIsOptional<U> || kIsSequence<U>) {
        return supported_type<NestedValueT<U>>();
    } else if constexpr (kIsVariant<U>) {
        return kUniqueVariantAlternatives<U> && unique_variant_names<U>() &&
               []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) consteval {
                   return (supported_type<Alternatives>() && ...);
               }(std::type_identity<U>{});
    } else if constexpr (kReflectedObject<U>) {
        return true;
    }
    return false;
}

template <class T>
[[nodiscard]] consteval std::string_view static_variant_name() {
    return mmltk::frameworks::reflection::type_name<T>();
}

template <class T>
[[nodiscard]] std::string variant_name() {
    return std::string(static_variant_name<T>());
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> to_value(const T& value);

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> from_value(const wire::Value& value);

struct OpaqueCborFacade final {
    template <class T>
    [[nodiscard]] static std::expected<wire::Value, wire::EncodeError> EncodeObject(const T& value);

    template <class T>
    [[nodiscard]] static std::expected<T, wire::DecodeError> DecodeObject(const wire::Value& value);

    template <class T>
    [[nodiscard]] static std::expected<T, wire::DecodeError> DecodeProjectedObject(wire::Reader& reader, std::size_t depth);

    template <class T>
    [[nodiscard]] static bool EncodeFixedObject(FixedCborEncoder& writer, const T& value);
};

template <class Integer>
[[nodiscard]] std::expected<Integer, wire::DecodeError> decode_integer_scalar(const auto source) {
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    using Source = RemoveCvRef<decltype(source)>;
    if constexpr (std::is_signed_v<Integer>) {
        std::int64_t decoded = 0;
        if constexpr (std::is_same_v<Source, std::int64_t>) {
            decoded = source;
        } else {
            static_assert(std::is_same_v<Source, std::uint64_t>);
            if (source > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
                return std::unexpected(decode_error(wire::ErrorCode::Overflow));
            }
            decoded = static_cast<std::int64_t>(source);
        }
        if (decoded < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
            decoded > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
            return std::unexpected(decode_error(wire::ErrorCode::Overflow));
        }
        return static_cast<Integer>(decoded);
    } else {
        std::uint64_t decoded = 0U;
        if constexpr (std::is_same_v<Source, std::uint64_t>) {
            decoded = source;
        } else {
            static_assert(std::is_same_v<Source, std::int64_t>);
            if (source < 0) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
            decoded = static_cast<std::uint64_t>(source);
        }
        if (decoded > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
            return std::unexpected(decode_error(wire::ErrorCode::Overflow));
        }
        return static_cast<Integer>(decoded);
    }
}

template <class Float>
[[nodiscard]] std::expected<Float, wire::DecodeError> decode_float_scalar(const auto source) {
    static_assert(std::is_floating_point_v<Float>);
    using Source = RemoveCvRef<decltype(source)>;
    const Float converted = static_cast<Float>(source);
    if (!std::isfinite(converted)) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
    if constexpr (std::is_floating_point_v<Source>) {
        if (static_cast<Source>(converted) != source) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
    } else if (static_cast<long double>(converted) != static_cast<long double>(source)) {
        // The Linux GCC target has a lossless long-double representation of
        // every protocol integer. Compare there rather than converting an
        // out-of-range floating value back to an integer.
        return std::unexpected(decode_error(wire::ErrorCode::Overflow));
    }
    return converted;
}

template <class Number>
[[nodiscard]] std::expected<Number, wire::DecodeError> decode_number(const wire::Value& value) {
    return std::visit(
        [](const auto& storage) -> std::expected<Number, wire::DecodeError> {
            using Storage = RemoveCvRef<decltype(storage)>;
            if constexpr ((std::integral<Number> && (std::same_as<Storage, std::int64_t> || std::same_as<Storage, std::uint64_t>))) {
                return decode_integer_scalar<Number>(storage);
            } else if constexpr (std::floating_point<Number> && (std::same_as<Storage, double> || std::same_as<Storage, std::int64_t> ||
                                                                 std::same_as<Storage, std::uint64_t>)) {
                return decode_float_scalar<Number>(storage);
            } else {
                return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
            }
        },
        value.storage);
}

template <class T>
[[nodiscard]] std::expected<void, wire::DecodeError> from_value_into(T& destination, const wire::Value& value) {
    using U = RemoveCvRef<T>;
    if constexpr (kIsSpan<U>) {
        using Element = typename IsSpan<U>::value_type;
        if constexpr (std::is_const_v<Element>) {
            return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
        } else if constexpr (kByteSequence<U>) {
            const auto* bytes = std::get_if<wire::ByteBuffer>(&value.storage);
            if (bytes == nullptr || bytes->size() != destination.size()) {
                return std::unexpected(decode_error(wire::ErrorCode::Overflow));
            }
            std::transform(bytes->begin(), bytes->end(), destination.begin(),
                           [](const std::byte byte) { return from_wire_byte<std::remove_const_t<Element>>(byte); });
            return {};
        } else {
            const auto* array = std::get_if<wire::Value::Array>(&value.storage);
            if (array == nullptr || array->size() != destination.size()) {
                return std::unexpected(decode_error(wire::ErrorCode::Overflow));
            }
            for (std::size_t index = 0U; index < array->size(); ++index) {
                auto decoded = from_value<std::remove_const_t<Element>>((*array)[index]);
                if (!decoded) { return std::unexpected(decoded.error()); }
                destination[index] = std::move(*decoded);
            }
            return {};
        }
    } else {
        auto decoded = from_value<U>(value);
        if (!decoded) { return std::unexpected(decoded.error()); }
        destination = std::move(*decoded);
        return {};
    }
}

template <class Declaration, class T>
[[nodiscard]] std::expected<void, wire::DecodeError> from_member_value_into(T& destination, const wire::Value& value) {
    using U = RemoveCvRef<T>;
    using MemberValue = OptionalValueT<U>;
    if constexpr (std::is_same_v<MemberValue, wire::FlatValue>) {
        constexpr auto policy = serialized_member_policy<Declaration>();
        static_assert(policy.maximum_bytes != 0U && policy.maximum_items != 0U);
        if constexpr (kIsOptional<U>) {
            if (std::holds_alternative<std::monostate>(value.storage)) {
                destination.reset();
                return {};
            }
        }
        auto decoded = wire::FlatValue::from_value(
            value, {.max_bytes = policy.maximum_bytes, .max_items = policy.maximum_items, .max_depth = wire::kMaximumNestingDepth});
        if (!decoded) { return std::unexpected(decoded.error()); }
        if constexpr (kIsOptional<U>) {
            destination.emplace(std::move(*decoded));
        } else {
            destination = std::move(*decoded);
        }
        return {};
    } else {
        return from_value_into(destination, value);
    }
}

template <class T>
[[nodiscard]] std::size_t reflected_object_present_count(const T& value) {
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<T>) {
        (void)value;
        return declarations<T>().size();
    }
    std::size_t present_count = 0U;
    visit_bases<T>([&]<class Base>() { present_count += reflected_object_present_count(static_cast<const Base&>(value)); });
    visit_members<T>([&]<class Declaration>(const auto&) {
        if constexpr (requires { Declaration::pointer; }) {
            constexpr auto member = Declaration::pointer;
            using MemberType = RemoveCvRef<decltype(value.*member)>;
            if constexpr (kIsOptional<MemberType>) {
                if ((value.*member).has_value()) { ++present_count; }
            } else {
                ++present_count;
            }
        }
    });
    return present_count;
}

template <class T>
    requires(!mmltk::frameworks::reflection::kOpaqueRelationStorage<T>)
void append_reflected_object_fields(const T& value, wire::Value::Object& object, std::optional<wire::EncodeError>& failure) {
    visit_bases<T>([&]<class Base>() {
        if (!failure) { append_reflected_object_fields(static_cast<const Base&>(value), object, failure); }
    });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        if constexpr (requires { Declaration::pointer; }) {
            if (!failure) {
                constexpr auto member = Declaration::pointer;
                bool present = true;
                using MemberType = RemoveCvRef<decltype(value.*member)>;
                if constexpr (kIsOptional<MemberType>) { present = (value.*member).has_value(); }
                if (present) {
                    if (!member_constraints_accept<Declaration>(value.*member)) {
                        failure = encode_error(wire::ErrorCode::LimitExceeded);
                        prepend_path(*failure, fact.member_name);
                    } else {
                        auto encoded = to_value(value.*member);
                        if (!encoded) {
                            failure = encoded.error();
                            prepend_path(*failure, fact.member_name);
                        } else {
                            object.emplace_back(std::string(fact.member_name), std::move(*encoded));
                        }
                    }
                }
            }
        }
    });
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> encode_object(const T& value) {
    audit_object<T>();
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<T>) {
        return OpaqueCborFacade::EncodeObject(value);
    } else {
        wire::Value::Object object;
        object.reserve(reflected_object_present_count(value));
        std::optional<wire::EncodeError> failure;
        append_reflected_object_fields(value, object, failure);
        if (failure) return std::unexpected(std::move(*failure));
        return wire::Value(std::move(object));
    }
}

template <class T>
    requires(!mmltk::frameworks::reflection::kOpaqueRelationStorage<T>)
void decode_reflected_object_fields(T& result, const wire::Value::Object& object, std::size_t& source_index,
                                    std::optional<wire::DecodeError>& failure) {
    visit_bases<T>([&]<class Base>() {
        if (!failure) { decode_reflected_object_fields(static_cast<Base&>(result), object, source_index, failure); }
    });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        if constexpr (requires { Declaration::pointer; }) {
            if (!failure) {
                constexpr auto member = Declaration::pointer;
                const std::string_view name = fact.member_name;
                if (source_index < object.size() && object[source_index].first == name) {
                    auto decoded = from_member_value_into<Declaration>(result.*member, object[source_index].second);
                    if (!decoded) {
                        failure = decoded.error();
                        prepend_path(*failure, name);
                    } else if (!member_constraints_accept<Declaration>(result.*member)) {
                        failure = decode_error(wire::ErrorCode::LimitExceeded);
                        prepend_path(*failure, name);
                    } else {
                        ++source_index;
                    }
                } else if constexpr (!kIsOptional<RemoveCvRef<decltype(result.*member)>>) {
                    failure = decode_error(wire::ErrorCode::UnknownKey);
                    prepend_path(*failure, name);
                }
            }
        }
    });
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> finish_decoded_object(T result, const wire::Value::Object& object,
                                                                        const std::size_t source_index,
                                                                        std::optional<wire::DecodeError> failure) {
    if (failure) return std::unexpected(std::move(*failure));
    if (source_index != object.size()) {
        auto error = decode_error(wire::ErrorCode::UnknownKey);
        prepend_path(error, object[source_index].first);
        return std::unexpected(std::move(error));
    }
    return result;
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode_object(const wire::Value& value) {
    audit_object<T>();
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<T>) {
        return OpaqueCborFacade::DecodeObject<T>(value);
    } else {
        const auto* object = std::get_if<wire::Value::Object>(&value.storage);
        if (object == nullptr) return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));

        T result{};
        std::size_t source_index = 0U;
        std::optional<wire::DecodeError> failure;
        decode_reflected_object_fields(result, *object, source_index, failure);
        return finish_decoded_object(std::move(result), *object, source_index, std::move(failure));
    }
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> OpaqueCborFacade::EncodeObject(const T& value) {
    static_assert(mmltk::frameworks::reflection::kOpaqueRelationStorage<T>);
    wire::Value::Object object;
    object.reserve(declarations<T>().size());
    std::optional<wire::EncodeError> failure;
    mmltk::frameworks::reflection::detail::OpaqueRelationStorageAccess::VisitMember(
        value, [&]<std::meta::info Member>(const auto& member_value) {
            if (failure) return;
            constexpr auto policy = mmltk::frameworks::reflection::policy_of<Member>();
            const auto name = std::define_static_string(std::meta::identifier_of(Member));
            if (!mmltk::frameworks::reflection::field_value_satisfies(member_value, policy)) {
                failure = encode_error(wire::ErrorCode::LimitExceeded);
                prepend_path(*failure, name);
                return;
            }
            auto encoded = to_value(member_value);
            if (!encoded) {
                failure = encoded.error();
                prepend_path(*failure, name);
                return;
            }
            object.emplace_back(std::string(name), std::move(*encoded));
        });
    return failure ? std::unexpected(std::move(*failure)) : std::expected<wire::Value, wire::EncodeError>(wire::Value(std::move(object)));
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> OpaqueCborFacade::DecodeObject(const wire::Value& value) {
    static_assert(mmltk::frameworks::reflection::kOpaqueRelationStorage<T>);
    const auto* object = std::get_if<wire::Value::Object>(&value.storage);
    if (object == nullptr) return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));

    T result{};
    std::size_t source_index = 0U;
    std::optional<wire::DecodeError> failure;
    mmltk::frameworks::reflection::detail::OpaqueRelationStorageAccess::VisitMember(
        result, [&]<std::meta::info Member>(auto& member_value) {
            if (failure) return;
            const auto name = std::define_static_string(std::meta::identifier_of(Member));
            if (source_index >= object->size() || (*object)[source_index].first != name) {
                failure = decode_error(wire::ErrorCode::UnknownKey);
                prepend_path(*failure, name);
                return;
            }
            auto decoded = from_value_into(member_value, (*object)[source_index].second);
            if (!decoded) {
                failure = decoded.error();
                prepend_path(*failure, name);
                return;
            }
            constexpr auto policy = mmltk::frameworks::reflection::policy_of<Member>();
            if (!mmltk::frameworks::reflection::field_value_satisfies(member_value, policy)) {
                failure = decode_error(wire::ErrorCode::LimitExceeded);
                prepend_path(*failure, name);
                return;
            }
            ++source_index;
        });
    return finish_decoded_object(std::move(result), *object, source_index, std::move(failure));
}

template <class Variant>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> encode_variant(const Variant& value) {
    wire::Value::Object object;
    object.reserve(VariantEnvelope::field_count);
    std::optional<wire::EncodeError> failure;
    std::visit(
        [&object, &failure](const auto& alternative) {
            using Alternative = RemoveCvRef<decltype(alternative)>;
            auto encoded = to_value(alternative);
            if (!encoded) {
                failure = encoded.error();
                prepend_path(*failure, VariantEnvelope::payload_key);
                return;
            }
            object.emplace_back(VariantEnvelope::kind_key, wire::Value(variant_name<Alternative>()));
            object.emplace_back(VariantEnvelope::payload_key, std::move(*encoded));
        },
        value);
    if (failure) { return std::unexpected(std::move(*failure)); }
    return wire::Value(std::move(object));
}

template <class Variant>
[[nodiscard]] std::expected<Variant, wire::DecodeError> decode_variant(const wire::Value& value) {
    const auto* object = std::get_if<wire::Value::Object>(&value.storage);
    if (object == nullptr || object->size() != VariantEnvelope::field_count || (*object)[0].first != VariantEnvelope::kind_key || (*object)[1].first != VariantEnvelope::payload_key) {
        auto error = decode_error(wire::ErrorCode::TypeMismatch);
        prepend_path(error, VariantEnvelope::kind_key);
        return std::unexpected(std::move(error));
    }
    const auto* kind = std::get_if<std::string>(&(*object)[0].second.storage);
    if (kind == nullptr) {
        auto error = decode_error(wire::ErrorCode::TypeMismatch);
        prepend_path(error, VariantEnvelope::kind_key);
        return std::unexpected(std::move(error));
    }

    std::optional<Variant> decoded;
    std::optional<wire::DecodeError> failure;
    []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>, const std::string& expected_kind,
                              const wire::Value& payload, std::optional<Variant>& destination, std::optional<wire::DecodeError>& error) {
        ((expected_kind == variant_name<Alternatives>() && !destination
              ? [&] {
                    auto alternative = from_value<Alternatives>(payload);
                    if (!alternative) {
                        error = alternative.error();
                    } else {
                        destination.emplace(std::in_place_type<Alternatives>, std::move(*alternative));
                    }
                }()
              : void()), ...);
    }(std::type_identity<Variant>{}, *kind, (*object)[1].second, decoded, failure);
    if (failure) {
        prepend_path(*failure, VariantEnvelope::payload_key);
        return std::unexpected(std::move(*failure));
    }
    if (!decoded) {
        auto error = decode_error(wire::ErrorCode::UnknownKey);
        prepend_path(error, VariantEnvelope::kind_key);
        return std::unexpected(std::move(error));
    }
    return std::move(*decoded);
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> to_value(const T& value) {
    using U = RemoveCvRef<T>;
    static_assert(supported_type<U>(), "Attempted to encode an unsupported reflected CBOR type.");
    if constexpr (std::is_same_v<U, wire::Value>) {
        return value;
    } else if constexpr (std::is_same_v<U, wire::FlatValue>) {
        return value.visit([]<class FlatLeaf>(const FlatLeaf& leaf) -> wire::Value {
            using Leaf = RemoveCvRef<FlatLeaf>;
            if constexpr (kFlatScalar<Leaf>) {
                return wire::Value(leaf);
            } else {
                wire::Value::Array encoded;
                encoded.reserve(leaf.size());
                for (const auto& scalar : leaf) {
                    encoded.push_back(std::visit([](const auto& item) { return wire::Value(item); }, scalar));
                }
                return wire::Value(std::move(encoded));
            }
        });
    } else if constexpr (std::is_same_v<U, std::monostate>) {
        return wire::Value(std::monostate{});
    } else if constexpr (std::is_same_v<U, bool>) {
        return wire::Value(value);
    } else if constexpr (std::is_same_v<U, std::byte>) {
        return wire::Value(static_cast<std::uint64_t>(std::to_integer<unsigned char>(value)));
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        return wire::Value(static_cast<std::int64_t>(value));
    } else if constexpr (std::is_integral_v<U>) {
        return wire::Value(static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        if (!std::isfinite(value)) { return std::unexpected(encode_error(wire::ErrorCode::InvalidFloat)); }
        const double encoded = static_cast<double>(value);
        if (static_cast<U>(encoded) != value) { return std::unexpected(encode_error(wire::ErrorCode::Overflow)); }
        return wire::Value(encoded);
    } else if constexpr (std::is_same_v<U, std::string>) {
        return wire::Value(value);
    } else if constexpr (std::is_same_v<U, std::filesystem::path>) {
        return wire::Value(value.generic_string());
    } else if constexpr (std::is_enum_v<U> && !std::is_same_v<U, std::byte>) {
        std::optional<std::string> name;
        for (const auto& enumerator : enumerators<U>()) {
            if (!name && value == enumerator.value) { name = std::string(enumerator.name); }
        }
        if (!name) { return std::unexpected(encode_error(wire::ErrorCode::Overflow)); }
        return wire::Value(std::move(*name));
    } else if constexpr (kByteSequence<U>) {
        wire::ByteBuffer bytes;
        bytes.reserve(value.size());
        std::transform(value.begin(), value.end(), std::back_inserter(bytes), [](const auto byte) { return to_wire_byte(byte); });
        return wire::Value(std::move(bytes));
    } else if constexpr (kIsOptional<U>) {
        return value ? to_value(*value) : std::expected<wire::Value, wire::EncodeError>(wire::Value());
    } else if constexpr (kIsVariant<U>) {
        static_assert(kUniqueVariantAlternatives<U>, "Reflected CBOR variant alternatives require unique type identifiers.");
        return encode_variant(value);
    } else if constexpr (kIsArray<U> || kIsVector<U> || kIsSpan<U> || kIsInplaceVector<U>) {
        wire::Value::Array array;
        array.reserve(value.size());
        for (const auto& element : value) {
            auto encoded = to_value(element);
            if (!encoded) { return std::unexpected(encoded.error()); }
            array.push_back(std::move(*encoded));
        }
        return wire::Value(std::move(array));
    } else {
        return encode_object(value);
    }
}

// The fixed browser result and contract both need a conservative
// encoded-byte bound, but neither may grow a second CBOR vocabulary.  Keep
// this projection beside to_value(): it follows the same reflected type graph
// and the wire::head_size rule shared by Writer and CountingEncoder.
[[nodiscard]] constexpr std::size_t cbor_size_add(const std::size_t left, const std::size_t right) {
    if (left > std::numeric_limits<std::size_t>::max() - right) { throw "reflected CBOR byte bound overflow"; }
    return left + right;
}

[[nodiscard]] constexpr std::size_t cbor_size_multiply(const std::size_t left, const std::size_t right) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) { throw "reflected CBOR byte bound overflow"; }
    return left * right;
}

[[nodiscard]] consteval std::size_t cbor_text_size(const std::size_t bytes) { return cbor_size_add(wire::head_size(bytes), bytes); }

[[nodiscard]] consteval std::size_t cbor_byte_string_size(const std::size_t bytes) { return cbor_size_add(wire::head_size(bytes), bytes); }

[[nodiscard]] consteval std::size_t cbor_maximum(const std::size_t left, const std::size_t right) { return left < right ? right : left; }

template <class Declaration, std::size_t mmltk::frameworks::reflection::FieldConstraint::* Bound>
[[nodiscard]] consteval std::size_t required_member_bound(const char* error) {
    constexpr auto policy = serialized_member_policy<Declaration>();
    constexpr std::size_t result = policy.*Bound;
    if (result == 0U) throw error;
    return result;
}

template <class Declaration>
[[nodiscard]] consteval std::size_t maximum_member_bytes() {
    return required_member_bound<Declaration, &mmltk::frameworks::reflection::FieldConstraint::maximum_bytes>(
        "reflected CBOR text or dynamic value requires one positive byte bound");
}

template <class Declaration>
[[nodiscard]] consteval std::size_t maximum_member_items() {
    return required_member_bound<Declaration, &mmltk::frameworks::reflection::FieldConstraint::maximum_items>(
        "reflected CBOR sequence or dynamic value requires one positive item bound");
}

template <class Declaration>
[[nodiscard]] consteval std::size_t maximum_member_items_or_capacity(const std::size_t capacity) {
    constexpr std::size_t declared = serialized_member_policy<Declaration>().maximum_items;
    return declared == 0U || declared > capacity ? capacity : declared;
}

enum class DynamicValueBound { Full, ExternallyBudgeted };

template <class T, DynamicValueBound Mode = DynamicValueBound::Full>
[[nodiscard]] consteval std::size_t maximum_cbor_bytes();

[[nodiscard]] consteval std::size_t maximum_dynamic_cbor_bytes(const std::size_t maximum_bytes, const std::size_t maximum_items,
                                                               const std::size_t depth) {
    const std::size_t scalar = cbor_maximum(9U, cbor_maximum(cbor_text_size(maximum_bytes), cbor_byte_string_size(maximum_bytes)));
    if (depth == wire::kMaximumNestingDepth) { return cbor_maximum(scalar, wire::head_size(0U)); }
    const std::size_t child = maximum_dynamic_cbor_bytes(maximum_bytes, maximum_items, depth + 1U);
    const std::size_t array = cbor_size_add(wire::head_size(maximum_items), cbor_size_multiply(maximum_items, child));
    const std::size_t object_member = cbor_size_add(cbor_text_size(maximum_bytes), child);
    const std::size_t object = cbor_size_add(wire::head_size(maximum_items), cbor_size_multiply(maximum_items, object_member));
    return cbor_maximum(scalar, cbor_maximum(array, object));
}

[[nodiscard]] consteval std::size_t maximum_flat_dynamic_cbor_bytes(const std::size_t maximum_bytes, const std::size_t maximum_items) {
    const std::size_t scalar = cbor_maximum(9U, cbor_maximum(cbor_text_size(maximum_bytes), cbor_byte_string_size(maximum_bytes)));
    const std::size_t array = cbor_size_add(wire::head_size(maximum_items), cbor_size_multiply(maximum_items, scalar));
    return cbor_maximum(scalar, array);
}

template <class Declaration, class Value, DynamicValueBound Mode = DynamicValueBound::Full>
[[nodiscard]] consteval std::size_t maximum_member_cbor_bytes() {
    using U = RemoveCvRef<Value>;
    if constexpr (kIsOptional<U>) {
        return cbor_maximum(1U, maximum_member_cbor_bytes<Declaration, typename IsOptional<U>::value_type, Mode>());
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::filesystem::path>) {
        return cbor_text_size(maximum_member_bytes<Declaration>());
    } else if constexpr (std::is_same_v<U, wire::Value>) {
        if constexpr (Mode == DynamicValueBound::ExternallyBudgeted) {
            return 0U;
        } else {
            return maximum_dynamic_cbor_bytes(maximum_member_bytes<Declaration>(), maximum_member_items<Declaration>(), 0U);
        }
    } else if constexpr (std::is_same_v<U, wire::FlatValue>) {
        return maximum_flat_dynamic_cbor_bytes(maximum_member_bytes<Declaration>(), maximum_member_items<Declaration>());
    } else if constexpr (kByteSequence<U>) {
        if constexpr (kIsArray<U>) {
            return cbor_byte_string_size(IsArray<U>::size);
        } else if constexpr (kIsSpan<U>) {
            if constexpr (IsSpan<U>::extent != std::dynamic_extent) { return cbor_byte_string_size(IsSpan<U>::extent); }
            return cbor_byte_string_size(maximum_member_bytes<Declaration>());
        } else {
            return cbor_byte_string_size(maximum_member_bytes<Declaration>());
        }
    } else if constexpr (kIsVector<U>) {
        const std::size_t items = maximum_member_items<Declaration>();
        return cbor_size_add(wire::head_size(items),
                             cbor_size_multiply(items, maximum_cbor_bytes<typename IsVector<U>::value_type, Mode>()));
    } else if constexpr (kIsArray<U>) {
        return cbor_size_add(wire::head_size(IsArray<U>::size),
                             cbor_size_multiply(IsArray<U>::size, maximum_cbor_bytes<typename IsArray<U>::value_type, Mode>()));
    } else if constexpr (kIsSpan<U>) {
        const std::size_t items = IsSpan<U>::extent == std::dynamic_extent ? maximum_member_items<Declaration>() : IsSpan<U>::extent;
        return cbor_size_add(wire::head_size(items),
                             cbor_size_multiply(items, maximum_cbor_bytes<std::remove_const_t<typename IsSpan<U>::value_type>, Mode>()));
    } else if constexpr (kIsInplaceVector<U>) {
        const std::size_t items = maximum_member_items_or_capacity<Declaration>(IsInplaceVector<U>::capacity);
        return cbor_size_add(wire::head_size(items),
                             cbor_size_multiply(items, maximum_cbor_bytes<typename IsInplaceVector<U>::value_type, Mode>()));
    } else {
        return maximum_cbor_bytes<U, Mode>();
    }
}

template <class T, class Contribution>
[[nodiscard]] consteval std::size_t reflected_object_member_sum(const Contribution& contribution) {
    std::size_t result = 0U;
    visit_bases<T>([&]<class Base>() { result = cbor_size_add(result, reflected_object_member_sum<Base>(contribution)); });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        result = cbor_size_add(result, contribution.template operator()<T, Declaration>(fact));
    });
    return result;
}

template <class T>
[[nodiscard]] consteval std::size_t maximum_reflected_object_member_count() {
    return reflected_object_member_sum<T>([]<class, class>(const auto&) { return 1U; });
}

template <class T, DynamicValueBound Mode>
[[nodiscard]] consteval std::size_t maximum_reflected_object_members_bytes() {
    return reflected_object_member_sum<T>([]<class Owner, class Declaration>(const auto& fact) {
        using Member = typename Declaration::member_type;
        return cbor_size_add(cbor_text_size(fact.member_name.size()), maximum_member_cbor_bytes<Declaration, Member, Mode>());
    });
}

template <class Variant, DynamicValueBound Mode>
[[nodiscard]] consteval std::size_t maximum_variant_cbor_bytes() {
    return []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) consteval {
        std::size_t kind = 0U;
        std::size_t payload = 0U;
        ((kind = cbor_maximum(kind, cbor_text_size(std::string_view(static_variant_name<Alternatives>()).size())),
          payload = cbor_maximum(payload, maximum_cbor_bytes<Alternatives, Mode>())),
         ...);
        const std::size_t members = cbor_size_add(cbor_text_size(std::string_view(VariantEnvelope::kind_key).size()), kind);
        return cbor_size_add(cbor_size_add(wire::head_size(VariantEnvelope::field_count), members),
                             cbor_size_add(cbor_text_size(std::string_view(VariantEnvelope::payload_key).size()), payload));
    }(std::type_identity<RemoveCvRef<Variant>>{});
}

template <class T, DynamicValueBound Mode>
[[nodiscard]] consteval std::size_t maximum_cbor_bytes() {
    using U = RemoveCvRef<T>;
    static_assert(supported_type<U>(), "A reflected CBOR byte bound requires a supported wire type.");
    if constexpr (std::is_same_v<U, wire::Value> && Mode == DynamicValueBound::ExternallyBudgeted) {
        return 0U;
    } else if constexpr (std::is_same_v<U, std::monostate>) {
        return 1U;
    } else if constexpr (std::is_same_v<U, bool>) {
        return 1U;
    } else if constexpr (std::is_same_v<U, std::byte>) {
        return wire::head_size(255U);
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        const std::size_t positive = wire::head_size(static_cast<std::uint64_t>(std::numeric_limits<U>::max()));
        const std::size_t negative = wire::head_size(static_cast<std::uint64_t>(-(std::numeric_limits<U>::lowest() + 1)));
        return cbor_maximum(positive, negative);
    } else if constexpr (std::is_integral_v<U>) {
        return wire::head_size(static_cast<std::uint64_t>(std::numeric_limits<U>::max()));
    } else if constexpr (std::is_floating_point_v<U>) {
        return sizeof(U) <= sizeof(float) ? 5U : 9U;
    } else if constexpr (std::is_enum_v<U>) {
        std::size_t bytes = 0U;
        for (const auto& enumerator : enumerators<U>()) {
            bytes = cbor_maximum(bytes, cbor_text_size(enumerator.name.size()));
        }
        if (bytes == 0U) throw "reflected CBOR enum has no enumerators";
        return bytes;
    } else if constexpr (kIsOptional<U>) {
        return cbor_maximum(1U, maximum_cbor_bytes<typename IsOptional<U>::value_type, Mode>());
    } else if constexpr (kIsVariant<U>) {
        return maximum_variant_cbor_bytes<U, Mode>();
    } else if constexpr (kIsArray<U>) {
        if constexpr (kByteSequence<U>) {
            return cbor_byte_string_size(IsArray<U>::size);
        } else {
            return cbor_size_add(wire::head_size(IsArray<U>::size),
                                 cbor_size_multiply(IsArray<U>::size, maximum_cbor_bytes<typename IsArray<U>::value_type, Mode>()));
        }
    } else if constexpr (kIsInplaceVector<U>) {
        return cbor_size_add(
            wire::head_size(IsInplaceVector<U>::capacity),
            cbor_size_multiply(IsInplaceVector<U>::capacity, maximum_cbor_bytes<typename IsInplaceVector<U>::value_type, Mode>()));
    } else if constexpr (kReflectedObject<U>) {
        audit_object<U>();
        return cbor_size_add(wire::head_size(maximum_reflected_object_member_count<U>()),
                             maximum_reflected_object_members_bytes<U, Mode>());
    } else {
        throw "reflected CBOR byte bound needs declaration-owned dynamic limits";
    }
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> from_value(const wire::Value& value) {
    using U = RemoveCvRef<T>;
    static_assert(supported_type<U>(), "Attempted to decode an unsupported reflected CBOR type.");
    if constexpr (std::is_same_v<U, wire::Value>) {
        return value;
    } else if constexpr (std::is_same_v<U, wire::FlatValue>) {
        // FlatValue is decoded by the reader projection below so its storage
        // never passes through recursive wire::Value. This fallback remains a
        // hard type error for callers that bypass the projected reader.
        return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
    } else if constexpr (std::is_same_v<U, std::monostate>) {
        return std::holds_alternative<std::monostate>(value.storage) ? std::expected<U, wire::DecodeError>(std::monostate{})
                                                                     : std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
    } else if constexpr (std::is_same_v<U, bool>) {
        const auto* decoded = std::get_if<bool>(&value.storage);
        return decoded == nullptr ? std::unexpected(decode_error(wire::ErrorCode::TypeMismatch))
                                  : std::expected<U, wire::DecodeError>(*decoded);
    } else if constexpr (std::is_same_v<U, std::byte>) {
        auto decoded = decode_number<unsigned char>(value);
        return decoded ? std::expected<U, wire::DecodeError>(std::byte(*decoded)) : std::unexpected(decoded.error());
    } else if constexpr (std::is_integral_v<U>) {
        return decode_number<U>(value);
    } else if constexpr (std::is_floating_point_v<U>) {
        return decode_number<U>(value);
    } else if constexpr (std::is_same_v<U, std::string>) {
        const auto* decoded = std::get_if<std::string>(&value.storage);
        return decoded == nullptr ? std::unexpected(decode_error(wire::ErrorCode::TypeMismatch))
                                  : std::expected<U, wire::DecodeError>(*decoded);
    } else if constexpr (std::is_same_v<U, std::filesystem::path>) {
        const auto* decoded = std::get_if<std::string>(&value.storage);
        return decoded == nullptr ? std::unexpected(decode_error(wire::ErrorCode::TypeMismatch))
                                  : std::expected<U, wire::DecodeError>(std::filesystem::path(*decoded));
    } else if constexpr (std::is_enum_v<U>) {
        const auto* name = std::get_if<std::string>(&value.storage);
        if (name == nullptr) { return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch)); }
        std::optional<U> decoded;
        for (const auto& enumerator : enumerators<U>()) {
            if (!decoded && *name == enumerator.name) { decoded = enumerator.value; }
        }
        return decoded ? std::expected<U, wire::DecodeError>(*decoded) : std::unexpected(decode_error(wire::ErrorCode::UnknownKey));
    } else if constexpr (kIsSpan<U>) {
        // A span is a borrowed destination.  Decode it through decode_into(),
        // where the caller supplies storage with an exact required extent.
        return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
    } else if constexpr (kByteSequence<U>) {
        const auto* decoded = std::get_if<wire::ByteBuffer>(&value.storage);
        if (decoded == nullptr) { return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch)); }
        if constexpr (kIsVector<U>) {
            U result;
            result.reserve(decoded->size());
            std::transform(decoded->begin(), decoded->end(), std::back_inserter(result),
                           [](const std::byte byte) { return from_wire_byte<typename IsVector<U>::value_type>(byte); });
            return result;
        } else if constexpr (kIsArray<U>) {
            if (decoded->size() != IsArray<U>::size) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
            U result{};
            std::transform(decoded->begin(), decoded->end(), result.begin(),
                           [](const std::byte byte) { return from_wire_byte<typename IsArray<U>::value_type>(byte); });
            return result;
        } else {
            return std::unexpected(decode_error(wire::ErrorCode::Overflow));
        }
    } else if constexpr (kIsOptional<U>) {
        if (std::holds_alternative<std::monostate>(value.storage)) { return U{}; }
        auto decoded = from_value<typename IsOptional<U>::value_type>(value);
        return decoded ? std::expected<U, wire::DecodeError>(std::move(*decoded)) : std::unexpected(decoded.error());
    } else if constexpr (kIsVariant<U>) {
        return decode_variant<U>(value);
    } else if constexpr (kIsArray<U> || kIsVector<U> || kIsInplaceVector<U>) {
        const auto* array = std::get_if<wire::Value::Array>(&value.storage);
        if (array == nullptr) { return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch)); }
        if constexpr (kIsArray<U>) {
            if (array->size() != IsArray<U>::size) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
            U result{};
            for (std::size_t index = 0U; index < array->size(); ++index) {
                auto decoded = from_value<typename IsArray<U>::value_type>((*array)[index]);
                if (!decoded) { return std::unexpected(decoded.error()); }
                result[index] = std::move(*decoded);
            }
            return result;
        } else if constexpr (kIsVector<U>) {
            U result;
            result.reserve(array->size());
            for (const wire::Value& element : *array) {
                auto decoded = from_value<typename IsVector<U>::value_type>(element);
                if (!decoded) { return std::unexpected(decoded.error()); }
                result.push_back(std::move(*decoded));
            }
            return result;
        } else {
            if (array->size() > IsInplaceVector<U>::capacity) { return std::unexpected(decode_error(wire::ErrorCode::LimitExceeded)); }
            U result;
            for (const wire::Value& element : *array) {
                auto decoded = from_value<typename IsInplaceVector<U>::value_type>(element);
                if (!decoded) { return std::unexpected(decoded.error()); }
                result.push_back(std::move(*decoded));
            }
            return result;
        }
    } else {
        return decode_object<U>(value);
    }
}

struct DecodePathSelector {
    std::string name;
    std::string object_kind;
};

struct DecodeAllocationLimit {
    std::vector<DecodePathSelector> path;
    std::optional<std::size_t> max_bytes;
    std::optional<std::size_t> max_items;
    bool recursive_dynamic = false;
};

struct DecodeAllocationLimits {
    std::vector<DecodeAllocationLimit> entries;
};

template <class T, class... Active>
void append_decode_allocation_limits(DecodeAllocationLimits& limits, std::vector<DecodePathSelector>& path);

template <class Alternative, class... Active>
void append_variant_allocation_limits(DecodeAllocationLimits& limits, std::vector<DecodePathSelector>& path) {
    DecodePathSelector selector;
    selector.name = VariantEnvelope::payload_key;
    selector.object_kind = variant_name<Alternative>();
    path.push_back(std::move(selector));
    DecodeAllocationLimit discriminator_guard;
    discriminator_guard.path = path;
    if constexpr (std::is_aggregate_v<Alternative> && !std::is_same_v<Alternative, wire::Value> && !kIsArray<Alternative> &&
                  !kIsVector<Alternative> && !kIsSpan<Alternative> && !kIsInplaceVector<Alternative> && !kIsOptional<Alternative> &&
                  !kIsVariant<Alternative>) {
        constexpr std::size_t member_count = declarations<Alternative>().size();
        discriminator_guard.max_items = member_count;
    }
    limits.entries.push_back(std::move(discriminator_guard));
    append_decode_allocation_limits<Alternative, Active...>(limits, path);
    path.pop_back();
}

template <class Owner, class Declaration, class... Active>
void append_member_allocation_limits(const std::string_view name, DecodeAllocationLimits& limits, std::vector<DecodePathSelector>& path) {
    using MemberType = typename Declaration::member_type;
    using ValueType = OptionalValueT<MemberType>;
    DecodePathSelector selector;
    selector.name = name;
    path.push_back(std::move(selector));

    DecodeAllocationLimit limit;
    limit.path = path;
    constexpr auto policy = serialized_member_policy<Declaration>();
    if constexpr (policy.maximum_bytes != 0U) { limit.max_bytes = policy.maximum_bytes; }
    if constexpr (policy.maximum_items != 0U) { limit.max_items = policy.maximum_items; }
    if constexpr (kIsArray<ValueType>) {
        if constexpr (kByteSequence<ValueType>) {
            limit.max_bytes = limit.max_bytes ? std::min(*limit.max_bytes, IsArray<ValueType>::size) : IsArray<ValueType>::size;
        } else {
            limit.max_items = limit.max_items ? std::min(*limit.max_items, IsArray<ValueType>::size) : IsArray<ValueType>::size;
        }
    } else if constexpr (kIsInplaceVector<ValueType>) {
        limit.max_items =
            limit.max_items ? std::min(*limit.max_items, IsInplaceVector<ValueType>::capacity) : IsInplaceVector<ValueType>::capacity;
    }
    if constexpr (std::is_same_v<ValueType, wire::Value> || std::is_same_v<ValueType, wire::FlatValue>) { limit.recursive_dynamic = true; }
    if (limit.max_bytes || limit.max_items) { limits.entries.push_back(std::move(limit)); }

    append_decode_allocation_limits<MemberType, Active...>(limits, path);
    path.pop_back();
}

template <class T, class... Active>
void append_decode_allocation_limits(DecodeAllocationLimits& limits, std::vector<DecodePathSelector>& path) {
    using U = RemoveCvRef<T>;
    if constexpr (!(std::is_same_v<U, Active> || ...)) {
        if constexpr (kIsOptional<U> || kIsSequence<U>) {
            append_decode_allocation_limits<NestedValueT<U>, Active..., U>(limits, path);
        } else if constexpr (kIsVariant<U>) {
            []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>, DecodeAllocationLimits& target,
                                      std::vector<DecodePathSelector>& selectors) {
                (append_variant_allocation_limits<Alternatives, Active..., U>(target, selectors), ...);
            }(std::type_identity<U>{}, limits, path);
        } else if constexpr (kReflectedObject<U>) {
            visit_bases<U>([&]<class Base>() { append_decode_allocation_limits<Base, Active..., U>(limits, path); });
            visit_members<U>([&]<class Declaration>(const auto& fact) {
                append_member_allocation_limits<U, Declaration, Active..., U>(fact.member_name, limits, path);
            });
        }
    }
}

template <class T>
[[nodiscard]] const DecodeAllocationLimits& decode_allocation_limits() {
    static const DecodeAllocationLimits limits = [] {
        DecodeAllocationLimits result;
        std::vector<DecodePathSelector> path;
        append_decode_allocation_limits<T>(result, path);
        return result;
    }();
    return limits;
}

struct DecodePolicyContext {
    const DecodeAllocationLimits* reflected = nullptr;
    wire::AllocationPolicy upstream{};
};

[[nodiscard]] bool allocation_path_names_match(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept;
[[nodiscard]] bool allocation_discriminators_match(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept;
[[nodiscard]] bool allocation_limit_accepts(const DecodeAllocationLimit& limit, const wire::AllocationRequest& request) noexcept;
[[nodiscard]] bool allocation_allowed(const void* raw_context, const wire::AllocationRequest& request) noexcept;

// A FlatValue-bearing root uses the canonical reader as a streaming typed
// projection. The reader remains the only CBOR parser; this layer supplies
// reflected member ownership and converts ordinary scalar leaves only after
// the reader has already validated their wire representation.
template <class T, class... Active>
[[nodiscard]] consteval bool contains_flat_value() {
    using U = RemoveCvRef<T>;
    if constexpr (std::is_same_v<U, wire::FlatValue>) {
        return true;
    } else if constexpr (std::is_same_v<U, wire::Value> || (std::is_same_v<U, Active> || ...)) {
        return false;
    } else if constexpr (kIsOptional<U>) {
        return contains_flat_value<typename IsOptional<U>::value_type, Active..., U>();
    } else if constexpr (kIsVector<U>) {
        return contains_flat_value<typename IsVector<U>::value_type, Active..., U>();
    } else if constexpr (kIsArray<U>) {
        return contains_flat_value<typename IsArray<U>::value_type, Active..., U>();
    } else if constexpr (kIsSpan<U>) {
        return contains_flat_value<typename IsSpan<U>::value_type, Active..., U>();
    } else if constexpr (kIsInplaceVector<U>) {
        return contains_flat_value<typename IsInplaceVector<U>::value_type, Active..., U>();
    } else if constexpr (kIsVariant<U>) {
        return []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>) consteval {
            return (contains_flat_value<Alternatives, Active..., U>() || ...);
        }(std::type_identity<U>{});
    } else if constexpr (kReflectedObject<U>) {
        bool contains = false;
        visit_bases<U>([&]<class Base>() { contains = contains || contains_flat_value<Base, Active..., U>(); });
        visit_members<U>([&]<class Declaration>(const auto&) {
            using Member = typename Declaration::member_type;
            contains = contains || contains_flat_value<Member, Active..., U>();
        });
        return contains;
    }
    return false;
}

template <class T>
inline constexpr bool kContainsFlatValue = contains_flat_value<T>();

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode_projected(wire::Reader& reader, std::size_t depth);

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> from_projected_scalar(wire::FlatValue scalar) {
    using U = RemoveCvRef<T>;
    return std::move(scalar).visit([]<class Leaf>(Leaf&& leaf) -> std::expected<U, wire::DecodeError> {
        using L = RemoveCvRef<Leaf>;
        if constexpr (std::is_same_v<U, std::monostate>) {
            if constexpr (std::is_same_v<L, std::monostate>) return std::monostate{};
        } else if constexpr (std::is_same_v<U, bool>) {
            if constexpr (std::is_same_v<L, bool>) return leaf;
        } else if constexpr (std::is_same_v<U, std::byte>) {
            if constexpr (std::is_same_v<L, std::uint64_t>) {
                if (leaf <= std::numeric_limits<unsigned char>::max()) { return std::byte(static_cast<unsigned char>(leaf)); }
                return std::unexpected(decode_error(wire::ErrorCode::Overflow));
            }
        } else if constexpr (std::is_integral_v<U>) {
            if constexpr (std::is_same_v<L, std::int64_t> || std::is_same_v<L, std::uint64_t>) { return decode_integer_scalar<U>(leaf); }
        } else if constexpr (std::is_floating_point_v<U>) {
            if constexpr (std::is_same_v<L, std::int64_t> || std::is_same_v<L, std::uint64_t> || std::is_same_v<L, double>) {
                return decode_float_scalar<U>(leaf);
            }
        } else if constexpr (std::is_same_v<U, std::string>) {
            if constexpr (std::is_same_v<L, std::string>) return std::forward<Leaf>(leaf);
        } else if constexpr (std::is_same_v<U, std::filesystem::path>) {
            if constexpr (std::is_same_v<L, std::string>) return std::filesystem::path(std::forward<Leaf>(leaf));
        } else if constexpr (std::is_enum_v<U>) {
            if constexpr (std::is_same_v<L, std::string>) {
                std::optional<U> decoded;
                for (const auto& enumerator : enumerators<U>()) {
                    if (!decoded && leaf == enumerator.name) decoded = enumerator.value;
                }
                return decoded ? std::expected<U, wire::DecodeError>(*decoded) : std::unexpected(decode_error(wire::ErrorCode::UnknownKey));
            }
        } else if constexpr (kByteSequence<U>) {
            if constexpr (std::is_same_v<L, wire::ByteBuffer>) {
                if constexpr (kIsVector<U>) {
                    U result;
                    result.reserve(leaf.size());
                    std::transform(leaf.begin(), leaf.end(), std::back_inserter(result),
                                   [](const std::byte byte) { return from_wire_byte<typename IsVector<U>::value_type>(byte); });
                    return result;
                } else if constexpr (kIsArray<U>) {
                    if (leaf.size() != IsArray<U>::size) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
                    U result{};
                    std::transform(leaf.begin(), leaf.end(), result.begin(),
                                   [](const std::byte byte) { return from_wire_byte<typename IsArray<U>::value_type>(byte); });
                    return result;
                }
            }
        }
        return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch));
    });
}

template <class T>
[[nodiscard]] consteval std::size_t flattened_member_count() {
    std::size_t count = declarations<T>().size();
    visit_bases<T>([&]<class Base>() { count += flattened_member_count<Base>(); });
    return count;
}

template <class T>
    requires(!mmltk::frameworks::reflection::kOpaqueRelationStorage<T>)
[[nodiscard]] std::expected<bool, wire::DecodeError> decode_projected_member(T& destination, wire::Reader& reader, const std::size_t depth,
                                                                             const std::string_view name) {
    bool matched = false;
    std::optional<wire::DecodeError> failure;
    visit_bases<T>([&]<class Base>() {
        if (!matched && !failure) {
            auto decoded = decode_projected_member(static_cast<Base&>(destination), reader, depth, name);
            if (!decoded) {
                failure = decoded.error();
            } else {
                matched = *decoded;
            }
        }
    });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        if constexpr (requires { Declaration::pointer; }) {
            constexpr auto member = Declaration::pointer;
            if (!matched && !failure && name == fact.member_name) {
                using Member = RemoveCvRef<decltype(destination.*member)>;
                auto scope = reader.enter_path(name);
                auto decoded = decode_projected<Member>(reader, depth + 1U);
                if (!decoded) {
                    failure = reader.contextualize(decoded.error());
                } else if (!member_constraints_accept<Declaration>(*decoded)) {
                    failure = reader.contextualize(decode_error(wire::ErrorCode::LimitExceeded));
                } else {
                    destination.*member = std::move(*decoded);
                    matched = true;
                }
            }
        }
    });
    return failure ? std::unexpected(std::move(*failure)) : std::expected<bool, wire::DecodeError>(matched);
}

[[nodiscard]] inline wire::DecodeError contextual_key_error(wire::Reader& reader, const wire::ErrorCode code, const std::string_view key) {
    auto error = reader.contextualize(decode_error(code));
    if (!error.path.empty()) error.path.push_back('.');
    error.path.append(key);
    return error;
}

template <class Seen>
[[nodiscard]] std::expected<std::string, wire::DecodeError> read_unique_object_key(wire::Reader& reader, const std::size_t depth,
                                                                                   const Seen& seen) {
    auto key = reader.read_object_key(depth + 1U);
    if (!key) return std::unexpected(key.error());
    if (std::ranges::find(seen, *key) != seen.end())
        return std::unexpected(contextual_key_error(reader, wire::ErrorCode::DuplicateKey, *key));
    return key;
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> OpaqueCborFacade::DecodeProjectedObject(wire::Reader& reader, const std::size_t depth) {
    static_assert(mmltk::frameworks::reflection::kOpaqueRelationStorage<T>);
    auto member_count = reader.begin_object_item(depth);
    if (!member_count) return std::unexpected(member_count.error());

    T result{};
    std::inplace_vector<std::string, 1U> seen;
    for (std::size_t index = 0U; index < *member_count; ++index) {
        auto key = read_unique_object_key(reader, depth, seen);
        if (!key) return std::unexpected(key.error());

        std::expected<bool, wire::DecodeError> decoded = false;
        mmltk::frameworks::reflection::detail::OpaqueRelationStorageAccess::VisitMember(result, [&]<std::meta::info Member>(
                                                                                                    auto& member_value) {
            const auto member_name = std::define_static_string(std::meta::identifier_of(Member));
            if (!decoded || *decoded || *key != member_name) return;
            using MemberType = RemoveCvRef<decltype(member_value)>;
            auto scope = reader.enter_path(*key);
            auto member = decode_projected<MemberType>(reader, depth + 1U);
            if (!member) {
                decoded = std::unexpected(reader.contextualize(member.error()));
            } else if (!mmltk::frameworks::reflection::field_value_satisfies(*member, mmltk::frameworks::reflection::policy_of<Member>())) {
                decoded = std::unexpected(reader.contextualize(decode_error(wire::ErrorCode::LimitExceeded)));
            } else {
                member_value = std::move(*member);
                decoded = true;
            }
        });
        if (!decoded) return std::unexpected(decoded.error());
        if (!*decoded) return std::unexpected(contextual_key_error(reader, wire::ErrorCode::UnknownKey, *key));
        seen.push_back(std::move(*key));
    }
    if (seen.empty()) {
        std::string_view missing_name;
        mmltk::frameworks::reflection::detail::OpaqueRelationStorageAccess::VisitMember(
            result, [&]<std::meta::info Member>(auto&) { missing_name = std::define_static_string(std::meta::identifier_of(Member)); });
        return std::unexpected(contextual_key_error(reader, wire::ErrorCode::UnknownKey, missing_name));
    }
    return result;
}

template <class T, class Seen>
void require_projected_members(wire::Reader& reader, const Seen& seen, std::optional<wire::DecodeError>& failure) {
    visit_bases<T>([&]<class Base>() {
        if (!failure) { require_projected_members<Base>(reader, seen, failure); }
    });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        using Member = typename Declaration::member_type;
        if (!failure && !kIsOptional<Member> && std::ranges::find(seen, fact.member_name) == seen.end()) {
            auto error = reader.contextualize(decode_error(wire::ErrorCode::UnknownKey));
            if (!error.path.empty()) { error.path.push_back('.'); }
            error.path.append(fact.member_name);
            failure = std::move(error);
        }
    });
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode_projected_object(wire::Reader& reader, const std::size_t depth) {
    audit_object<T>();
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<T>) {
        return OpaqueCborFacade::DecodeProjectedObject<T>(reader, depth);
    } else {
        auto member_count = reader.begin_object_item(depth);
        if (!member_count) return std::unexpected(member_count.error());

        T result{};
        std::inplace_vector<std::string, flattened_member_count<T>()> seen;
        for (std::size_t index = 0U; index < *member_count; ++index) {
            auto key = read_unique_object_key(reader, depth, seen);
            if (!key) return std::unexpected(key.error());
            auto decoded = decode_projected_member(result, reader, depth, *key);
            if (!decoded) {
                auto error = decoded.error();
                if (error.path.empty()) prepend_path(error, *key);
                return std::unexpected(std::move(error));
            }
            if (!*decoded) return std::unexpected(contextual_key_error(reader, wire::ErrorCode::UnknownKey, *key));
            seen.push_back(std::move(*key));
        }

        std::optional<wire::DecodeError> missing;
        require_projected_members<T>(reader, seen, missing);
        if (missing) return std::unexpected(std::move(*missing));
        return result;
    }
}

template <class Sequence>
[[nodiscard]] std::expected<Sequence, wire::DecodeError> decode_projected_sequence(wire::Reader& reader, const std::size_t depth) {
    using U = RemoveCvRef<Sequence>;
    using Element = SequenceElementT<U>;
    auto count = reader.begin_array_item(depth);
    if (!count) { return std::unexpected(count.error()); }
    if constexpr (kIsArray<U>) {
        if (*count != IsArray<U>::size) { return std::unexpected(decode_error(wire::ErrorCode::Overflow)); }
        U result{};
        for (std::size_t index = 0U; index < *count; ++index) {
            auto decoded = decode_projected<Element>(reader, depth + 1U);
            if (!decoded) { return std::unexpected(decoded.error()); }
            result[index] = std::move(*decoded);
        }
        return result;
    } else {
        U result;
        if constexpr (kIsVector<U>) {
            result.reserve(*count);
        } else if (*count > IsInplaceVector<U>::capacity) {
            return std::unexpected(decode_error(wire::ErrorCode::LimitExceeded));
        }
        for (std::size_t index = 0U; index < *count; ++index) {
            auto decoded = decode_projected<Element>(reader, depth + 1U);
            if (!decoded) { return std::unexpected(decoded.error()); }
            result.push_back(std::move(*decoded));
        }
        return result;
    }
}

template <class Variant>
[[nodiscard]] std::expected<Variant, wire::DecodeError> decode_projected_variant(wire::Reader& reader, const std::size_t depth) {
    auto member_count = reader.begin_object_item(depth);
    if (!member_count) { return std::unexpected(member_count.error()); }
    if (*member_count != VariantEnvelope::field_count) { return std::unexpected(decode_error(wire::ErrorCode::TypeMismatch)); }
    auto kind_key = reader.read_object_key(depth + 1U);
    if (!kind_key) { return std::unexpected(kind_key.error()); }
    if (*kind_key != VariantEnvelope::kind_key) {
        auto error = reader.contextualize(decode_error(wire::ErrorCode::TypeMismatch));
        if (!error.path.empty()) { error.path.push_back('.'); }
        error.path.append(VariantEnvelope::kind_key);
        return std::unexpected(std::move(error));
    }

    std::string kind;
    {
        auto scope = reader.enter_path(*kind_key);
        auto decoded = reader.read_scalar_item(depth + 1U);
        if (!decoded) return std::unexpected(decoded.error());
        auto converted = from_projected_scalar<std::string>(std::move(*decoded));
        if (!converted) { return std::unexpected(reader.contextualize(converted.error())); }
        kind = std::move(*converted);
    }

    auto payload_key = reader.read_object_key(depth + 1U);
    if (!payload_key) { return std::unexpected(payload_key.error()); }
    if (*payload_key != VariantEnvelope::payload_key) {
        auto error = reader.contextualize(decode_error(wire::ErrorCode::TypeMismatch));
        if (!error.path.empty()) { error.path.push_back('.'); }
        error.path.append(VariantEnvelope::payload_key);
        return std::unexpected(std::move(error));
    }

    std::optional<Variant> result;
    std::optional<wire::DecodeError> failure;
    {
        auto scope = reader.enter_path(*payload_key);
        []<class... Alternatives>(std::type_identity<std::variant<Alternatives...>>, const std::string& expected_kind, wire::Reader& source,
                                  const std::size_t item_depth, std::optional<Variant>& destination,
                                  std::optional<wire::DecodeError>& error) {
            ((expected_kind == variant_name<Alternatives>() && !destination
                  ? [&] {
                        auto decoded = decode_projected<Alternatives>(source, item_depth + 1U);
                        if (!decoded) {
                            error = source.contextualize(decoded.error());
                        } else {
                            destination.emplace(std::in_place_type<Alternatives>, std::move(*decoded));
                        }
                    }()
                  : void()), ...);
        }(std::type_identity<Variant>{}, kind, reader, depth, result, failure);
    }
    if (failure) { return std::unexpected(std::move(*failure)); }
    if (!result) return std::unexpected(contextual_key_error(reader, wire::ErrorCode::UnknownKey, VariantEnvelope::kind_key));
    return std::move(*result);
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode_projected(wire::Reader& reader, const std::size_t depth) {
    using U = RemoveCvRef<T>;
    if constexpr (std::is_same_v<U, wire::Value>) {
        return reader.read_value_item(depth);
    } else if constexpr (std::is_same_v<U, wire::FlatValue>) {
        return reader.read_flat_item(depth);
    } else if constexpr (kIsOptional<U>) {
        using Element = typename IsOptional<U>::value_type;
        auto absent = reader.next_is_null();
        if (!absent) { return std::unexpected(absent.error()); }
        if (*absent) {
            auto ignored = reader.read_scalar_item(depth);
            if (!ignored) { return std::unexpected(ignored.error()); }
            return U{};
        }
        auto decoded = decode_projected<Element>(reader, depth);
        return decoded ? std::expected<U, wire::DecodeError>(U(std::move(*decoded))) : std::unexpected(decoded.error());
    } else if constexpr (kByteSequence<U>) {
        auto decoded = reader.read_scalar_item(depth);
        return decoded ? from_projected_scalar<U>(std::move(*decoded)) : std::unexpected(decoded.error());
    } else if constexpr (kIsArray<U> || kIsVector<U> || kIsInplaceVector<U>) {
        return decode_projected_sequence<U>(reader, depth);
    } else if constexpr (kIsVariant<U>) {
        return decode_projected_variant<U>(reader, depth);
    } else if constexpr (kReflectedObject<U>) {
        return decode_projected_object<U>(reader, depth);
    } else {
        auto decoded = reader.read_scalar_item(depth);
        return decoded ? from_projected_scalar<U>(std::move(*decoded)) : std::unexpected(decoded.error());
    }
}

}  // namespace detail

class FixedCborEncoder final {
   public:
    explicit FixedCborEncoder(std::span<std::byte> destination) noexcept;
    [[nodiscard]] bool unsigned_integer(std::uint64_t value) noexcept;
    [[nodiscard]] bool signed_integer(std::int64_t value) noexcept;
    [[nodiscard]] bool floating(double value) noexcept;
    [[nodiscard]] bool boolean(bool value) noexcept;
    [[nodiscard]] bool null() noexcept;
    [[nodiscard]] bool text(std::string_view value) noexcept;
    template <class Range>
    [[nodiscard]] bool bytes(const Range& value) noexcept {
        if (!head(2U, value.size())) return false;
        for (const auto byte : value)
            if (!put(detail::to_wire_byte(byte))) return false;
        return true;
    }
    [[nodiscard]] bool array(std::size_t size) noexcept;
    [[nodiscard]] bool object(std::size_t size) noexcept;
    [[nodiscard]] bool append_authorized_item(std::span<const std::byte> encoded) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool capacity_exceeded() const noexcept;
    [[nodiscard]] wire::ErrorCode error() const noexcept;

   private:
    [[nodiscard]] bool put(std::byte value) noexcept;
    [[nodiscard]] bool append(std::span<const std::byte> value) noexcept;
    [[nodiscard]] bool head(std::uint8_t major, std::uint64_t value) noexcept;
    [[nodiscard]] bool big_endian(std::uint64_t value, std::size_t bytes) noexcept;

    std::span<std::byte> destination_{};
    std::size_t position_ = 0U;
    bool capacity_exceeded_ = false;
};

namespace detail {

template <class T>
[[nodiscard]] bool encode_fixed_projection(FixedCborEncoder& writer, const T& value);

template <class T>
    requires(!mmltk::frameworks::reflection::kOpaqueRelationStorage<T>)
[[nodiscard]] bool encode_fixed_object_fields(FixedCborEncoder& writer, const T& value) {
    bool valid = true;
    visit_bases<T>([&]<class Base>() { valid = valid && encode_fixed_object_fields(writer, static_cast<const Base&>(value)); });
    visit_members<T>([&]<class Declaration>(const auto& fact) {
        if constexpr (requires { Declaration::pointer; }) {
            if (!valid) return;
            constexpr auto member = Declaration::pointer;
            using Member = RemoveCvRef<decltype(value.*member)>;
            bool present = true;
            if constexpr (kIsOptional<Member>) present = (value.*member).has_value();
            if (present)
                valid = member_constraints_accept<Declaration>(value.*member) && writer.text(fact.member_name) &&
                        encode_fixed_projection(writer, value.*member);
        }
    });
    return valid;
}

template <class T>
[[nodiscard]] bool OpaqueCborFacade::EncodeFixedObject(FixedCborEncoder& writer, const T& value) {
    static_assert(mmltk::frameworks::reflection::kOpaqueRelationStorage<T>);
    bool valid = writer.object(declarations<T>().size());
    mmltk::frameworks::reflection::detail::OpaqueRelationStorageAccess::VisitMember(
        value, [&]<std::meta::info Member>(const auto& member_value) {
            if (!valid) return;
            const auto name = std::define_static_string(std::meta::identifier_of(Member));
            valid =
                mmltk::frameworks::reflection::field_value_satisfies(member_value, mmltk::frameworks::reflection::policy_of<Member>()) &&
                writer.text(name) && encode_fixed_projection(writer, member_value);
        });
    return valid;
}

template <class T>
[[nodiscard]] bool encode_fixed_projection(FixedCborEncoder& writer, const T& value) {
    using U = RemoveCvRef<T>;
    static_assert(supported_type<U>(), "Attempted to encode an unsupported reflected CBOR type.");
    if constexpr (std::is_same_v<U, wire::Value>) {
        return std::visit(
            [&writer](const auto& item) {
                using Item = RemoveCvRef<decltype(item)>;
                if constexpr (std::is_same_v<Item, wire::Value::Array>) {
                    if (!writer.array(item.size())) return false;
                    for (const auto& child : item)
                        if (!encode_fixed_projection(writer, child)) return false;
                    return true;
                } else if constexpr (std::is_same_v<Item, wire::Value::Object>) {
                    if (!writer.object(item.size())) return false;
                    for (const auto& [key, child] : item)
                        if (!writer.text(key) || !encode_fixed_projection(writer, child)) return false;
                    return true;
                } else {
                    return encode_fixed_projection(writer, item);
                }
            },
            value.storage);
    } else if constexpr (std::is_same_v<U, wire::FlatValue>) {
        return value.visit([&writer](const auto& leaf) {
            using Leaf = RemoveCvRef<decltype(leaf)>;
            if constexpr (kFlatScalar<Leaf>) {
                return encode_fixed_projection(writer, leaf);
            } else {
                if (!writer.array(leaf.size())) return false;
                for (const auto& scalar : leaf)
                    if (!std::visit([&writer](const auto& item) { return encode_fixed_projection(writer, item); }, scalar)) return false;
                return true;
            }
        });
    } else if constexpr (std::is_same_v<U, std::monostate>) {
        return writer.null();
    } else if constexpr (std::is_same_v<U, bool>) {
        return writer.boolean(value);
    } else if constexpr (std::is_same_v<U, std::byte>) {
        return writer.unsigned_integer(std::to_integer<unsigned char>(value));
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        return writer.signed_integer(static_cast<std::int64_t>(value));
    } else if constexpr (std::is_integral_v<U>) {
        return writer.unsigned_integer(static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        const double encoded = static_cast<double>(value);
        return std::isfinite(value) && static_cast<U>(encoded) == value && writer.floating(encoded);
    } else if constexpr (std::is_same_v<U, std::string>) {
        return writer.text(value);
    } else if constexpr (std::is_same_v<U, std::filesystem::path>) {
        return writer.text(value.native());
    } else if constexpr (std::is_enum_v<U>) {
        std::string_view name;
        for (const auto& enumerator : enumerators<U>())
            if (value == enumerator.value) name = enumerator.name;
        return !name.empty() && writer.text(name);
    } else if constexpr (kByteSequence<U>) {
        return writer.bytes(value);
    } else if constexpr (kIsOptional<U>) {
        return value ? encode_fixed_projection(writer, *value) : writer.null();
    } else if constexpr (kIsVariant<U>) {
        static_assert(kUniqueVariantAlternatives<U>, "Reflected CBOR variant alternatives require unique type identifiers.");
        return std::visit(
            [&writer](const auto& alternative) {
                return writer.object(VariantEnvelope::field_count) && writer.text(VariantEnvelope::kind_key) && writer.text(static_variant_name<RemoveCvRef<decltype(alternative)>>()) &&
                       writer.text(VariantEnvelope::payload_key) && encode_fixed_projection(writer, alternative);
            },
            value);
    } else if constexpr (kIsArray<U> || kIsVector<U> || kIsSpan<U> || kIsInplaceVector<U>) {
        if (!writer.array(value.size())) return false;
        for (const auto& element : value)
            if (!encode_fixed_projection(writer, element)) return false;
        return true;
    } else {
        audit_object<U>();
        if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<U>) {
            return OpaqueCborFacade::EncodeFixedObject(writer, value);
        } else {
            return writer.object(reflected_object_present_count(value)) && encode_fixed_object_fields(writer, value);
        }
    }
}

}  // namespace detail

template <class T>
[[nodiscard]] bool encode_fixed(FixedCborEncoder& writer, const T& value) {
    return detail::encode_fixed_projection(writer, value);
}

template <class T>
[[nodiscard]] std::expected<void, wire::EncodeError> encode(const T& value, wire::ByteBuffer& destination, const wire::Limits limits) {
    auto encoded = detail::to_value(value);
    if (!encoded) { return std::unexpected(encoded.error()); }
    return wire::encode(*encoded, destination, limits);
}

template <class T>
[[nodiscard]] std::expected<std::size_t, wire::EncodeError> encode(const T& value, const std::span<std::byte> destination,
                                                                   const wire::Limits limits) {
    if (destination.size() > limits.max_bytes) return std::unexpected(detail::encode_error(wire::ErrorCode::LimitExceeded));
    FixedCborEncoder writer(destination);
    if (!encode_fixed(writer, value)) return std::unexpected(detail::encode_error(writer.error()));
    return writer.size();
}

template <class T>
[[nodiscard]] std::expected<std::size_t, wire::EncodeError> measure(const T& value, const wire::Limits limits) {
    auto encoded = detail::to_value(value);
    if (!encoded) { return std::unexpected(encoded.error()); }
    return wire::CountingEncoder(limits).measure(*encoded);
}

template <class T>
[[nodiscard]] std::expected<T, wire::DecodeError> decode(const wire::ByteSegments bytes, const wire::Limits limits) {
    const detail::DecodePolicyContext context{
        .reflected = &detail::decode_allocation_limits<T>(),
        .upstream = limits.allocation_policy,
    };
    wire::Limits bounded_limits = limits;
    bounded_limits.allocation_policy = {.context = &context, .allows = detail::allocation_allowed};
    if constexpr (detail::kContainsFlatValue<T>) {
        wire::Reader reader(bytes, bounded_limits);
        auto projected = detail::decode_projected<T>(reader, 0U);
        if (!projected) { return std::unexpected(projected.error()); }
        auto completed = reader.finish();
        return completed ? std::expected<T, wire::DecodeError>(std::move(*projected)) : std::unexpected(completed.error());
    }
    auto decoded = wire::decode(bytes, bounded_limits);
    if (!decoded) { return std::unexpected(decoded.error()); }
    return detail::from_value<T>(*decoded);
}

template <class T>
[[nodiscard]] std::expected<void, wire::DecodeError> decode_into(T& destination, const wire::Value& value) {
    return detail::from_value_into(destination, value);
}

template <class T>
[[nodiscard]] std::expected<wire::Value, wire::EncodeError> reflected_value(const T& value) {
    return detail::to_value(value);
}

template <class T>
inline constexpr bool reflected_byte_sequence = detail::kByteSequence<T>;

template <class T>
[[nodiscard]] consteval std::size_t reflected_maximum_cbor_bytes() {
    return detail::maximum_cbor_bytes<T>();
}

template <class T>
[[nodiscard]] std::string reflected_schema_type_name() {
    return std::string(mmltk::frameworks::reflection::type_name<T>());
}

// Positional projection for schema-agreed interaction records. Scalars use the
// canonical Reader/encoder; structure and validation derive from declarations.
namespace compact_detail {
enum class Shape { Unsupported, Scalar, Optional, Sequence, Enum, Object };

template <class T>
inline constexpr Shape shape = [] consteval {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, bool> || std::same_as<U, std::uint8_t> || std::same_as<U, std::uint16_t> ||
                  std::same_as<U, std::uint32_t> || std::same_as<U, std::uint64_t> || std::same_as<U, std::int8_t> ||
                  std::same_as<U, std::int16_t> || std::same_as<U, std::int32_t> || std::same_as<U, std::int64_t> ||
                  (std::same_as<U, char> && std::is_signed_v<char>)) {
        return Shape::Scalar;
    } else if constexpr (std::same_as<U, float> || std::same_as<U, double>) {
        return Shape::Scalar;
    } else if constexpr (detail::kByteSequence<U> || std::same_as<U, std::monostate> ||
                         mmltk::frameworks::reflection::kOpaqueRelationStorage<U>) {
        return Shape::Unsupported;
    } else if constexpr (std::is_enum_v<U>) {
        return detail::enumerators<U>().empty() || shape<std::underlying_type_t<U>> != Shape::Scalar ? Shape::Unsupported : Shape::Enum;
    } else if constexpr (detail::kIsOptional<U>) {
        return shape<typename detail::IsOptional<U>::value_type> == Shape::Unsupported ? Shape::Unsupported : Shape::Optional;
    } else if constexpr (detail::kIsArray<U> || detail::kIsInplaceVector<U>) {
        return shape<detail::SequenceElementT<U>> == Shape::Unsupported ? Shape::Unsupported : Shape::Sequence;
    } else if constexpr (detail::kReflectedObject<U>) {
        bool supported = true;
        detail::visit_bases<U>([&]<class Base>() { supported = supported && shape<Base> == Shape::Object; });
        detail::visit_members<U>([&]<class Declaration>(const auto&) {
            supported = supported && shape<typename Declaration::member_type> != Shape::Unsupported;
        });
        return supported ? Shape::Object : Shape::Unsupported;
    }
    return Shape::Unsupported;
}();

template <class T>
[[nodiscard]] consteval std::size_t maximum_bytes() {
    static_assert(shape<T> != Shape::Unsupported, "unsupported compact wire projection");
    if constexpr (shape<T> == Shape::Optional) {
        return detail::cbor_maximum(1U, maximum_bytes<typename detail::IsOptional<T>::value_type>());
    } else if constexpr (shape<T> == Shape::Sequence) {
        constexpr std::size_t count = [] consteval {
            if constexpr (detail::kIsArray<T>) return detail::IsArray<T>::size;
            else return detail::IsInplaceVector<T>::capacity;
        }();
        return detail::cbor_size_add(wire::head_size(count), detail::cbor_size_multiply(count, maximum_bytes<detail::SequenceElementT<T>>()));
    } else if constexpr (shape<T> == Shape::Enum) {
        std::size_t result = 0U;
        for (const auto enumerator : detail::enumerators<T>()) {
            std::uint64_t argument = 0U;
            if constexpr (std::is_signed_v<std::underlying_type_t<T>>) {
                const auto value = static_cast<std::int64_t>(enumerator.value);
                argument = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) : static_cast<std::uint64_t>(value);
            } else {
                argument = static_cast<std::uint64_t>(enumerator.value);
            }
            result = detail::cbor_maximum(result, wire::head_size(argument));
        }
        return result;
    } else if constexpr (shape<T> == Shape::Object) {
        detail::audit_object<T>();
        return detail::cbor_size_add(wire::head_size(detail::flattened_member_count<T>()),
            detail::reflected_object_member_sum<T>([]<class, class Declaration>(const auto&) {
                return maximum_bytes<typename Declaration::member_type>();
            }));
    } else {
        return detail::maximum_cbor_bytes<T>();
    }
}

template <class T, class Visitor>
void visit_fields(T& value, Visitor& visitor) {
    using U = std::remove_cvref_t<T>;
    detail::visit_bases<U>([&]<class Base>() {
        using QualifiedBase = std::conditional_t<std::is_const_v<T>, const Base, Base>;
        visit_fields(static_cast<QualifiedBase&>(value), visitor);
    });
    detail::visit_members<U>([&]<class Declaration>(const auto&) {
        visitor.template operator()<Declaration>(value.*Declaration::pointer);
    });
}
template <class T>
[[nodiscard]] bool encode(FixedCborEncoder& writer, const T& value) {
    namespace d = detail;
    static_assert(shape<T> != Shape::Unsupported, "unsupported compact wire projection");
    if constexpr (shape<T> == Shape::Optional) {
        return value ? encode(writer, *value) : writer.null();
    } else if constexpr (shape<T> == Shape::Sequence) {
        if (!writer.array(value.size())) return false;
        for (const auto& item : value) if (!encode(writer, item)) return false;
        return true;
    } else if constexpr (shape<T> == Shape::Enum) {
        return mmltk::frameworks::reflection::enum_contains(value) && encode(writer, static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (shape<T> == Shape::Object) {
        d::audit_object<T>();
        bool valid = writer.array(d::flattened_member_count<T>());
        auto field = [&]<class Declaration>(const auto& member) {
            valid = valid && d::member_constraints_accept<Declaration>(member) && encode(writer, member);
        };
        visit_fields(value, field);
        return valid;
    } else {
        return encode_fixed(writer, value);
    }
}
template <class T>
[[nodiscard]] bool decode(wire::Reader& reader, T& value, const std::size_t depth) {
    namespace d = detail;
    static_assert(shape<T> != Shape::Unsupported, "unsupported compact wire projection");
    if constexpr (shape<T> == Shape::Optional) {
        auto absent = reader.next_is_null();
        if (!absent) return false;
        if (*absent) { value.reset(); return reader.read_scalar_item(depth).has_value(); }
        value.emplace();
        return decode(reader, *value, depth);
    } else if constexpr (shape<T> == Shape::Sequence) {
        auto count = reader.begin_array_item(depth);
        if (!count) return false;
        if constexpr (d::kIsArray<T>) {
            if (*count != value.size()) return false;
        } else {
            if (*count > value.capacity()) return false;
            value.resize(*count);
        }
        for (auto& item : value) if (!decode(reader, item, depth + 1U)) return false;
        return true;
    } else if constexpr (shape<T> == Shape::Enum) {
        std::underlying_type_t<T> raw{};
        if (!decode(reader, raw, depth)) return false;
        value = static_cast<T>(raw);
        return mmltk::frameworks::reflection::enum_contains(value);
    } else if constexpr (shape<T> == Shape::Object) {
        d::audit_object<T>();
        auto count = reader.begin_array_item(depth);
        if (!count || *count != d::flattened_member_count<T>()) return false;
        bool valid = true;
        auto field = [&]<class Declaration>(auto& member) {
            valid = valid && decode(reader, member, depth + 1U) && d::member_constraints_accept<Declaration>(member);
        };
        visit_fields(value, field);
        return valid;
    } else {
        auto scalar = d::decode_projected<T>(reader, depth);
        if (!scalar) return false;
        value = std::move(*scalar);
        return true;
    }
}
} // namespace compact_detail


}  // namespace mmltk::frameworks::serialization::implementation
