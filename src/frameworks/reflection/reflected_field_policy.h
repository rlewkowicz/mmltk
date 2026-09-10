#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <inplace_vector>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

namespace mmltk::frameworks::serialization::implementation::detail {
struct OpaqueCborFacade;
}

namespace mmltk::frameworks::reflection {

template <class T>
using RemoveCvRef = std::remove_cvref_t<T>;

struct OpaqueRelationStorage : Annotation {
    static constexpr bool is_opaque_relation_storage = true;
};

template <class Type>
inline constexpr bool kOpaqueRelationStorage = [] consteval {
    std::size_t markers = 0U;
    template for (constexpr auto annotation : reflected_annotations<^^Type>()) {
        using AnnotationType = RemoveCvRef<typename[:std::meta::type_of(annotation):]>;
        markers += std::same_as<AnnotationType, OpaqueRelationStorage> ? 1U : 0U;
    }
    if (markers > 1U) throw "a reflected type has duplicate opaque-relation-storage annotations";
    return markers == 1U;
}();

MMLTK_REFLECT_ENUM(PresentationKind)
MMLTK_REFLECT_ENUM(Violation)
MMLTK_REFLECT_ENUM(FixedTextCharacterPolicy)

template <class Annotation>
inline constexpr bool kFixedTextAnnotation = std::same_as<RemoveCvRef<Annotation>, FixedText>;

template <class Type>
[[nodiscard]] consteval std::size_t fixed_text_annotation_count() {
    return reflected_annotation_count<^^Type>([]<class Annotation> { return kFixedTextAnnotation<Annotation>; });
}

template <class Type>
[[nodiscard]] consteval FixedText fixed_text_policy_of() {
    FixedText result{{}, 0U, FixedTextCharacterPolicy::PrintableAscii};
    template for (constexpr auto annotation : reflected_annotations<^^Type>()) {
        using AnnotationType = RemoveCvRef<typename[:std::meta::type_of(annotation):]>;
        if constexpr (kFixedTextAnnotation<AnnotationType>) result = std::meta::extract<AnnotationType>(annotation);
    }
    return result;
}

template <class T>
struct IsByteSequence : std::false_type {};
template <class Allocator>
struct IsByteSequence<std::vector<std::byte, Allocator>> : std::true_type {};
template <class Allocator>
struct IsByteSequence<std::vector<std::uint8_t, Allocator>> : std::true_type {};
template <std::size_t Count>
struct IsByteSequence<std::array<std::byte, Count>> : std::true_type {};
template <std::size_t Count>
struct IsByteSequence<std::array<std::uint8_t, Count>> : std::true_type {};
template <std::size_t Count>
struct IsByteSequence<std::array<char, Count>> : std::true_type {};
template <class Byte, std::size_t Count>
    requires(std::is_same_v<std::remove_const_t<Byte>, std::byte>)
struct IsByteSequence<std::span<Byte, Count>> : std::true_type {};
template <class Byte, std::size_t Count>
    requires(std::is_same_v<std::remove_const_t<Byte>, std::uint8_t>)
struct IsByteSequence<std::span<Byte, Count>> : std::true_type {};
template <class T>
inline constexpr bool kByteSequence = IsByteSequence<RemoveCvRef<T>>::value;

// Boundary owners specialize this semantics-free policy for types that are
// opaque leaves to reflected field traversal and require both byte and item
// budgets. Reflection owns the policy shape; it knows no wire vocabulary.
template <class T>
struct LeafPolicy {
    static constexpr bool bounded_dynamic = false;
};

template <class T>
inline constexpr bool kBoundedDynamicLeaf = LeafPolicy<RemoveCvRef<T>>::bounded_dynamic;

template <class>
struct InplaceVectorCapacity;

template <class Value, std::size_t Capacity>
struct InplaceVectorCapacity<std::inplace_vector<Value, Capacity>> : std::integral_constant<std::size_t, Capacity> {};

template <class T>
inline constexpr bool kInplaceVector = false;

template <class Value, std::size_t Capacity>
inline constexpr bool kInplaceVector<std::inplace_vector<Value, Capacity>> = true;

template <class T>
[[nodiscard]] consteval bool fixed_sequence_capacity_is_valid(const std::size_t maximum_items) {
    if constexpr (kInplaceVector<T>) { return maximum_items == 0U || maximum_items <= InplaceVectorCapacity<T>::value; }
    return true;
}

template <class T>
struct OptionalValue {
    static constexpr bool value = false;
    using type = RemoveCvRef<T>;
};

template <class T>
struct OptionalValue<std::optional<T>> {
    static constexpr bool value = true;
    using type = RemoveCvRef<T>;
};

template <class T>
using OptionalValueT = typename OptionalValue<RemoveCvRef<T>>::type;

template <class T>
struct MemberPointerOwner;

template <class Owner, class Value>
struct MemberPointerOwner<Value Owner::*> {
    using type = Owner;
};

template <auto Member>
struct MemberTag {};

template <auto Member, class Owner>
[[nodiscard]] consteval FieldConstraint external_field_constraint(MemberTag<Member>, std::type_identity<Owner>) {
    return {};
}

template <auto Member, class Owner>
[[nodiscard]] consteval PresentationKind external_presentation_kind(MemberTag<Member>, std::type_identity<Owner>) {
    return PresentationKind::Default;
}

[[nodiscard]] constexpr FieldConstraint merge(FieldConstraint left, FieldConstraint right) noexcept;

template <std::meta::info Target, class Visitor>
constexpr void visit_annotations(Visitor&& visitor) {
    // GCC does not support annotation-value splice expressions. Keep the
    // extraction in the reflection owner so callers do not duplicate it.
    static constexpr auto annotations = std::define_static_array(std::meta::annotations_of(Target));
    template for (constexpr auto annotation : annotations) {
        using AnnotationType = RemoveCvRef<typename[:std::meta::type_of(annotation):]>;
        visitor(std::meta::extract<AnnotationType>(annotation));
    }
}

// Annotation traits are a compile-time declaration vocabulary, not runtime knobs.
// CLEANUP-IGNORE: Minimum is one entry in the canonical compile-time policy-annotation trait vocabulary.
template <class Annotation, class = void>
struct MinimumAnnotation : std::false_type {};

template <class Annotation>
struct MinimumAnnotation<Annotation, std::void_t<decltype(Annotation::is_minimum), decltype(std::declval<const Annotation&>().value)>>
    // CLEANUP-IGNORE: Trait specializations deliberately share the standard bool-constant detection shape.
    : std::bool_constant<Annotation::is_minimum && std::is_convertible_v<decltype(std::declval<const Annotation&>().value), long double>> {
};

// CLEANUP-IGNORE: Maximum is an independent entry in the canonical compile-time annotation vocabulary.
template <class Annotation, class = void>
struct MaximumAnnotation : std::false_type {};

template <class Annotation>
struct MaximumAnnotation<Annotation, std::void_t<decltype(Annotation::is_maximum), decltype(std::declval<const Annotation&>().value)>>
    : std::bool_constant<Annotation::is_maximum && std::is_convertible_v<decltype(std::declval<const Annotation&>().value), long double>> {
};

// CLEANUP-IGNORE
template <class Annotation, class = void>
struct FiniteAnnotation : std::false_type {};

template <class Annotation>
struct FiniteAnnotation<Annotation, std::void_t<decltype(Annotation::is_finite)>>
    // CLEANUP-IGNORE: Finite uses the same trait result form while detecting a semantically distinct annotation.
    : std::bool_constant<Annotation::is_finite> {};

// CLEANUP-IGNORE: MinBytes is an independent entry in the canonical compile-time annotation vocabulary.
template <class Annotation, class = void>
struct MinBytesAnnotation : std::false_type {};

template <class Annotation>
struct MinBytesAnnotation<Annotation, std::void_t<decltype(Annotation::is_min_bytes), decltype(std::declval<const Annotation&>().value)>>
    : std::bool_constant<Annotation::is_min_bytes &&
                         // CLEANUP-IGNORE: Byte limits and numeric bounds intentionally use parallel trait detection
                         // with different value domains.
                         std::is_convertible_v<decltype(std::declval<const Annotation&>().value), std::size_t>> {};

// CLEANUP-IGNORE: MaxBytes is an independent entry in the canonical compile-time annotation vocabulary.
template <class Annotation, class = void>
struct MaxBytesAnnotation : std::false_type {};

template <class Annotation>
struct MaxBytesAnnotation<Annotation, std::void_t<decltype(Annotation::is_max_bytes), decltype(std::declval<const Annotation&>().value)>>
    : std::bool_constant<Annotation::is_max_bytes &&
                         std::is_convertible_v<decltype(std::declval<const Annotation&>().value), std::size_t>> {};

// CLEANUP-IGNORE: MaxItems is an independent entry in the canonical compile-time annotation vocabulary.
template <class Annotation, class = void>
struct MaxItemsAnnotation : std::false_type {};

template <class Annotation>
struct MaxItemsAnnotation<Annotation, std::void_t<decltype(Annotation::is_max_items), decltype(std::declval<const Annotation&>().value)>>
    : std::bool_constant<Annotation::is_max_items &&
                         std::is_convertible_v<decltype(std::declval<const Annotation&>().value), std::size_t>> {};

// CLEANUP-IGNORE: Presentation is an independent entry in the canonical compile-time annotation vocabulary.
template <class Annotation, class = void>
struct PresentationAnnotation : std::false_type {};

template <class Annotation>
struct PresentationAnnotation<Annotation, std::void_t<decltype(Annotation::kind)>>
    : std::bool_constant<std::is_convertible_v<decltype(Annotation::kind), PresentationKind>> {};

template <class Annotation>
inline constexpr bool kPolicyAnnotation =
    MinimumAnnotation<RemoveCvRef<Annotation>>::value || MaximumAnnotation<RemoveCvRef<Annotation>>::value ||
    FiniteAnnotation<RemoveCvRef<Annotation>>::value || MinBytesAnnotation<RemoveCvRef<Annotation>>::value ||
    MaxBytesAnnotation<RemoveCvRef<Annotation>>::value || MaxItemsAnnotation<RemoveCvRef<Annotation>>::value ||
    PresentationAnnotation<RemoveCvRef<Annotation>>::value || is_catalog_provider_annotation<RemoveCvRef<Annotation>>;

template <class Annotation>
constexpr void apply_field_constraint(FieldConstraint& result, const Annotation& annotation) {
    using A = RemoveCvRef<Annotation>;
    if constexpr (MinimumAnnotation<A>::value) {
        result.has_minimum = true;
        result.minimum = static_cast<long double>(annotation.value);
    } else if constexpr (MaximumAnnotation<A>::value) {
        result.has_maximum = true;
        result.maximum = static_cast<long double>(annotation.value);
    } else if constexpr (FiniteAnnotation<A>::value) {
        result.finite = true;
    } else if constexpr (MinBytesAnnotation<A>::value) {
        result.minimum_bytes = static_cast<std::size_t>(annotation.value);
    } else if constexpr (MaxBytesAnnotation<A>::value) {
        result.maximum_bytes = static_cast<std::size_t>(annotation.value);
    } else if constexpr (MaxItemsAnnotation<A>::value) {
        result.maximum_items = static_cast<std::size_t>(annotation.value);
    }
}

template <class Annotation>
constexpr void apply_presentation_kind(PresentationKind& result, const Annotation&) {
    using A = RemoveCvRef<Annotation>;
    if constexpr (PresentationAnnotation<A>::value) result = static_cast<PresentationKind>(A::kind);
}

template <std::meta::info Member>
[[nodiscard]] consteval FieldConstraint policy_of() {
    FieldConstraint result{};
    visit_annotations<Member>([&](const auto& annotation) { apply_field_constraint(result, annotation); });
    if constexpr (requires { &[:Member:]; }) {
        constexpr auto pointer = &[:Member:];
        using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(pointer)>>::type;
        result = merge(result, external_field_constraint(MemberTag<pointer>{}, std::type_identity<Owner>{}));
    }
    return result;
}

template <std::meta::info Member>
[[nodiscard]] consteval PresentationKind presentation_of() {
    PresentationKind result = PresentationKind::Default;
    visit_annotations<Member>([&](const auto& annotation) { apply_presentation_kind(result, annotation); });
    if constexpr (requires { &[:Member:]; }) {
        constexpr auto pointer = &[:Member:];
        using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(pointer)>>::type;
        const PresentationKind external = external_presentation_kind(MemberTag<pointer>{}, std::type_identity<Owner>{});
        if (external != PresentationKind::Default) result = external;
    }
    return result;
}

template <std::meta::info Member>
[[nodiscard]] consteval bool valid_policy_annotations() {
    using MemberType = OptionalValueT<typename[:std::meta::type_of(Member):]>;
    std::size_t minima = 0U;
    std::size_t maxima = 0U;
    std::size_t finite_markers = 0U;
    std::size_t byte_minima = 0U;
    std::size_t byte_limits = 0U;
    std::size_t item_limits = 0U;
    std::size_t presentation_markers = 0U;
    std::size_t catalog_markers = 0U;
    visit_annotations<Member>([&]<class Annotation>(const Annotation&) {
        using A = RemoveCvRef<Annotation>;
        minima += MinimumAnnotation<A>::value ? 1U : 0U;
        maxima += MaximumAnnotation<A>::value ? 1U : 0U;
        finite_markers += FiniteAnnotation<A>::value ? 1U : 0U;
        byte_minima += MinBytesAnnotation<A>::value ? 1U : 0U;
        byte_limits += MaxBytesAnnotation<A>::value ? 1U : 0U;
        item_limits += MaxItemsAnnotation<A>::value ? 1U : 0U;
        presentation_markers += PresentationAnnotation<A>::value ? 1U : 0U;
        catalog_markers += is_catalog_provider_annotation<A> ? 1U : 0U;
    });
    constexpr bool numeric = std::is_arithmetic_v<MemberType> && !std::is_same_v<MemberType, bool>;
    constexpr bool dynamic_value = kBoundedDynamicLeaf<MemberType>;
    constexpr bool minimum_byte_bounded =
        std::is_same_v<MemberType, std::string> || std::is_same_v<MemberType, std::filesystem::path> || kByteSequence<MemberType>;
    constexpr bool byte_bounded = minimum_byte_bounded || dynamic_value;
    constexpr bool item_bounded = (requires(const MemberType& value) {
                                      typename MemberType::value_type;
                                      value.size();
                                  } && (!byte_bounded || kByteSequence<MemberType>)) || dynamic_value;
    const FieldConstraint policy = policy_of<Member>();
    const bool fixed_sequence_capacity = fixed_sequence_capacity_is_valid<MemberType>(policy.maximum_items);
    return minima <= 1U && maxima <= 1U && finite_markers <= 1U && byte_minima <= 1U && byte_limits <= 1U && item_limits <= 1U &&
           presentation_markers <= 1U && catalog_markers <= 1U && (!policy.has_minimum || numeric) && (!policy.has_maximum || numeric) &&
           (!policy.finite || std::is_floating_point_v<MemberType>) && (policy.minimum_bytes == 0U || minimum_byte_bounded) &&
           (policy.maximum_bytes == 0U || byte_bounded) && (policy.maximum_items == 0U || item_bounded) &&
           (policy.minimum_bytes == 0U || policy.maximum_bytes == 0U || policy.minimum_bytes <= policy.maximum_bytes) &&
           (!policy.has_minimum || !policy.has_maximum || policy.minimum <= policy.maximum) && fixed_sequence_capacity;
}

struct MaterializedFieldPolicy final {
    std::string_view member_name{};
    FieldConstraint constraint{};
    PresentationKind presentation = PresentationKind::Default;
    bool annotations_valid = true;
    bool only_policy_annotations = true;
    bool private_member = false;
    bool default_zero = false;
};

template <class... Bases>
struct MaterializedBaseList final {
    static constexpr std::size_t count = sizeof...(Bases);

    template <class Visitor>
    static constexpr void Visit(Visitor&& visitor) {
        (visitor.template operator()<Bases>(), ...);
    }
};

template <auto Member, auto... Annotations>
struct MaterializedMemberDeclaration final {
    using member_type = RemoveCvRef<decltype(std::declval<typename MemberPointerOwner<RemoveCvRef<decltype(Member)>>::type&>().*Member)>;
    static constexpr auto pointer = Member;
    static constexpr std::size_t annotation_count = sizeof...(Annotations);

    template <class Visitor>
    static constexpr void VisitAnnotations(Visitor&& visitor) {
        (visitor(Annotations), ...);
    }

    template <class Visitor>
    static constexpr void VisitAnnotationValues(Visitor&& visitor) {
        (visitor.template operator()<Annotations>(), ...);
    }
};

template <class Member, auto... Annotations>
struct MaterializedOpaqueMemberDeclaration final {
    using member_type = Member;
    static constexpr std::size_t annotation_count = sizeof...(Annotations);
    inline static constexpr FieldConstraint constraint = [] {
        FieldConstraint result{};
        (apply_field_constraint(result, Annotations), ...);
        return result;
    }();
    inline static constexpr PresentationKind presentation = [] {
        PresentationKind result = PresentationKind::Default;
        (apply_presentation_kind(result, Annotations), ...);
        return result;
    }();
    inline static constexpr bool annotations_valid = [] {
        std::size_t minima = 0U;
        std::size_t maxima = 0U;
        std::size_t finite = 0U;
        ((minima += MinimumAnnotation<RemoveCvRef<decltype(Annotations)>>::value ? 1U : 0U,
          maxima += MaximumAnnotation<RemoveCvRef<decltype(Annotations)>>::value ? 1U : 0U,
          finite += FiniteAnnotation<RemoveCvRef<decltype(Annotations)>>::value ? 1U : 0U),
         ...);
        constexpr bool numeric = std::is_arithmetic_v<Member> && !std::is_same_v<Member, bool>;
        return minima <= 1U && maxima <= 1U && finite <= 1U && (!constraint.has_minimum || numeric) &&
               (!constraint.has_maximum || numeric) && (!constraint.finite || std::is_floating_point_v<Member>) &&
               (!constraint.has_minimum || !constraint.has_maximum || constraint.minimum <= constraint.maximum);
    }();

    template <class Visitor>
    static constexpr void VisitAnnotations(Visitor&& visitor) {
        (visitor(Annotations), ...);
    }

    template <class Visitor>
    static constexpr void VisitAnnotationValues(Visitor&& visitor) {
        (visitor.template operator()<Annotations>(), ...);
    }
};

template <class BaseList, class... Members>
struct MaterializedFieldPolicyProduct final {
    using base_types = BaseList;
    std::array<MaterializedFieldPolicy, sizeof...(Members)> values{};

    [[nodiscard]] static consteval std::size_t size() noexcept { return sizeof...(Members); }
    [[nodiscard]] static consteval bool empty() noexcept { return sizeof...(Members) == 0U; }
    [[nodiscard]] constexpr const MaterializedFieldPolicy& operator[](const std::size_t index) const noexcept { return values[index]; }
    [[nodiscard]] constexpr auto begin() const noexcept { return values.begin(); }
    [[nodiscard]] constexpr auto end() const noexcept { return values.end(); }
    template <class Visitor>
    static constexpr void Visit(Visitor&& visitor) {
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (visitor.template operator()<Members, Index>(), ...);
        }(std::index_sequence_for<Members...>{});
    }
};

struct FieldPolicyMaterializer final {
    template <class Owner, class Reflection>
    [[nodiscard]] consteval auto operator()() const {
        constexpr std::meta::info product = [] consteval {
            std::vector<std::meta::info> base_types;
            base_types.reserve(Reflection::bases.size());
            template for (constexpr auto base : Reflection::bases) base_types.push_back(std::meta::type_of(base));
            const std::meta::info base_list = std::meta::substitute(^^MaterializedBaseList, base_types);
            std::vector<std::meta::info> product_arguments;
            product_arguments.reserve(Reflection::members.size() + 1U);
            product_arguments.push_back(base_list);
            template for (constexpr auto member : Reflection::members) {
                std::vector<std::meta::info> declaration_arguments;
                if constexpr (kOpaqueRelationStorage<Owner>)
                    declaration_arguments.push_back(std::meta::type_of(member));
                else
                    declaration_arguments.push_back(std::meta::reflect_constant(&[:member:]));
                template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(member))) {
                    using AnnotationType = RemoveCvRef<typename[:std::meta::type_of(annotation):]>;
                    declaration_arguments.push_back(std::meta::reflect_constant(std::meta::extract<AnnotationType>(annotation)));
                }
                product_arguments.push_back(std::meta::substitute(
                    kOpaqueRelationStorage<Owner> ? ^^MaterializedOpaqueMemberDeclaration : ^^MaterializedMemberDeclaration,
                    declaration_arguments));
            }
            return std::meta::substitute(^^MaterializedFieldPolicyProduct, product_arguments);
        }();
        typename[:product:] result{};
        [[maybe_unused]] std::size_t index = 0U;
        template for (constexpr auto member : Reflection::members) {
            std::size_t annotation_count = 0U;
            std::size_t policy_annotation_count = 0U;
            visit_annotations<member>([&]<class Annotation>(const Annotation&) {
                ++annotation_count;
                if constexpr (kPolicyAnnotation<Annotation>) ++policy_annotation_count;
            });
            bool default_zero = false;
            if constexpr (kOpaqueRelationStorage<Owner>) {
                constexpr auto pointer = &[:member:];
                using Member = RemoveCvRef<decltype(std::declval<Owner&>().*pointer)>;
                default_zero = Owner{}.*pointer == Member{};
            }
            result.values[index++] = {
                .member_name = std::define_static_string(std::meta::identifier_of(member)),
                .constraint = policy_of<member>(),
                .presentation = presentation_of<member>(),
                .annotations_valid = valid_policy_annotations<member>(),
                .only_policy_annotations = annotation_count == policy_annotation_count,
                .private_member = std::meta::is_private(member),
                .default_zero = default_zero,
            };
        }
        return result;
    }
};

template <class Owner>
inline constexpr auto kReflectedFieldPolicies = [] consteval {
    if constexpr (kOpaqueRelationStorage<Owner>)
        return materialize_opaque_field_policy<Owner>(FieldPolicyMaterializer{});
    else
        return materialize<Owner>(FieldPolicyMaterializer{});
}();

#define MMLTK_REFLECT_FIELDS(Type)                                                              \
    [[nodiscard]] consteval const auto& materialized_field_policies(std::type_identity<Type>) { \
        return ::mmltk::frameworks::reflection::kReflectedFieldPolicies<Type>;                  \
    }

template <class Owner>
consteval void materialized_field_policies(std::type_identity<Owner>) = delete;

template <class Owner>
[[nodiscard]] consteval decltype(auto) field_declarations() {
    return materialized_field_policies(std::type_identity<RemoveCvRef<Owner>>{});
}

template <class Owner, class Visitor>
constexpr void visit_materialized_bases(Visitor&& visitor) {
    using Product = RemoveCvRef<decltype(field_declarations<Owner>())>;
    Product::base_types::Visit(std::forward<Visitor>(visitor));
}

template <class Owner, class Visitor>
constexpr void visit_materialized_members(Visitor&& visitor) {
    constexpr const auto& product = field_declarations<Owner>();
    using Product = RemoveCvRef<decltype(product)>;
    Product::Visit([&]<class Declaration, std::size_t Index>() { visitor.template operator()<Declaration>(product[Index]); });
}

template <auto MemberPointer, class Product>
[[nodiscard]] consteval std::size_t member_index(const Product& product) {
    std::size_t result = product.size();
    Product::Visit([&]<class Declaration, std::size_t Index>() {
        if constexpr (std::is_same_v<RemoveCvRef<decltype(Declaration::pointer)>, RemoveCvRef<decltype(MemberPointer)>>) {
            if (Declaration::pointer == MemberPointer) result = Index;
        }
    });
    if (result != product.size()) return result;
    throw "member pointer does not identify a reflected data member";
}

template <auto MemberPointer>
[[nodiscard]] consteval decltype(auto) materialized_member_declaration() {
    using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(MemberPointer)>>::type;
    constexpr const auto& materialized = materialized_field_policies(std::type_identity<Owner>{});
    constexpr std::size_t index = member_index<MemberPointer>(materialized);
    static_assert(index < materialized.size());
    return (materialized[index]);
}

template <auto MemberPointer>
[[nodiscard]] consteval std::string_view materialized_member_name() {
    return materialized_member_declaration<MemberPointer>().member_name;
}

template <auto MemberPointer>
[[nodiscard]] consteval FieldConstraint policy_of_member() {
    using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(MemberPointer)>>::type;
    return merge(materialized_member_declaration<MemberPointer>().constraint,
                 external_field_constraint(MemberTag<MemberPointer>{}, std::type_identity<Owner>{}));
}

template <auto MemberPointer>
[[nodiscard]] consteval bool has_materialized_policy_only_annotations() {
    return materialized_member_declaration<MemberPointer>().only_policy_annotations;
}

template <auto MemberPointer>
[[nodiscard]] consteval bool materialized_policy_annotations_are_valid() {
    return materialized_member_declaration<MemberPointer>().annotations_valid;
}

template <auto MemberPointer>
[[nodiscard]] consteval PresentationKind presentation_of_member() {
    using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(MemberPointer)>>::type;
    const PresentationKind result = materialized_member_declaration<MemberPointer>().presentation;
    const PresentationKind external = external_presentation_kind(MemberTag<MemberPointer>{}, std::type_identity<Owner>{});
    return external == PresentationKind::Default ? result : external;
}

template <class Value>
[[nodiscard]] constexpr bool field_value_satisfies(const Value& value, const FieldConstraint& constraint) noexcept;

template <class Value>
[[nodiscard]] constexpr std::optional<Violation> field_value_violation(const Value& value, const FieldConstraint& constraint) noexcept;

template <auto MemberPointer>
[[nodiscard]] consteval bool member_default_satisfies() {
    using Owner = typename MemberPointerOwner<RemoveCvRef<decltype(MemberPointer)>>::type;
    if constexpr (std::is_trivially_default_constructible_v<Owner>) {
        constexpr Owner defaults{};
        return field_value_satisfies(defaults.*MemberPointer, policy_of_member<MemberPointer>());
    }
    return true;
}

[[nodiscard]] constexpr FieldConstraint merge(const FieldConstraint left, const FieldConstraint right) noexcept {
    FieldConstraint result = left;
    result.finite = left.finite || right.finite;
    if (right.has_minimum) {
        result.has_minimum = true;
        result.minimum = right.minimum;
    }
    if (right.has_maximum) {
        result.has_maximum = true;
        result.maximum = right.maximum;
    }
    if (right.minimum_bytes != 0U) result.minimum_bytes = right.minimum_bytes;
    if (right.maximum_bytes != 0U) result.maximum_bytes = right.maximum_bytes;
    if (right.maximum_items != 0U) result.maximum_items = right.maximum_items;
    return result;
}

template <class Value>
[[nodiscard]] constexpr bool field_value_satisfies(const Value& value, const FieldConstraint& constraint) noexcept {
    return !field_value_violation(value, constraint).has_value();
}

template <class Value>
[[nodiscard]] constexpr std::optional<Violation> field_value_violation(const Value& value, const FieldConstraint& constraint) noexcept {
    using V = RemoveCvRef<Value>;
    if constexpr (kByteSequence<V>) {
        if (value.size() < constraint.minimum_bytes) return Violation::TooFewBytes;
        if (constraint.maximum_bytes != 0U && value.size() > constraint.maximum_bytes) return Violation::TooManyBytes;
        return satisfies_item_count(value, constraint) ? std::nullopt : std::optional{Violation::TooManyItems};
    } else if constexpr (requires {
                             typename V::value_type;
                             value.size();
                         } && !std::is_same_v<V, std::string> && !std::is_same_v<V, std::filesystem::path>) {
        return satisfies_item_count(value, constraint) ? std::nullopt : std::optional{Violation::TooManyItems};
    } else if constexpr (std::is_same_v<V, std::string>) {
        if (value.size() < constraint.minimum_bytes) return Violation::TooFewBytes;
        return satisfies(std::string_view(value), constraint) ? std::nullopt : std::optional{Violation::TooManyBytes};
    } else if constexpr (std::is_same_v<V, std::filesystem::path>) {
        if (value.native().size() < constraint.minimum_bytes) return Violation::TooFewBytes;
        return satisfies(value, constraint) ? std::nullopt : std::optional{Violation::TooManyBytes};
    } else if constexpr (std::is_arithmetic_v<V> && !std::is_same_v<V, bool>) {
        return numeric_violation(value, constraint);
    } else if constexpr (std::is_enum_v<V>) {
        for (const auto entry : enum_entries<V>()) {
            if (value == entry.value) return std::nullopt;
        }
        return Violation::InvalidValue;
    } else {
        return std::nullopt;
    }
}

template <class T>
[[nodiscard]] constexpr std::optional<ValidationError> validate_reflected_fields(const T& value) noexcept;

template <class T>
[[nodiscard]] consteval bool opaque_relation_storage_shape_is_valid(std::uint64_t valid_bits) noexcept;

namespace detail {

class OpaqueRelationStorageAccess final {
    friend struct OpaqueRelationStorageValidation;
    friend struct ::mmltk::frameworks::serialization::implementation::detail::OpaqueCborFacade;

    template <class Owner, class Visitor>
    static constexpr void VisitMember(Owner&& owner, Visitor&& visitor) {
        using U = RemoveCvRef<Owner>;
        static_assert(kOpaqueRelationStorage<U>);
        template for (constexpr auto member :
                      std::define_static_array(std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::unchecked()))) {
            constexpr auto pointer = &[:member:];
            visitor.template operator()<member>(std::forward<Owner>(owner).*pointer);
        }
    }
};

struct OpaqueRelationStorageValidation final {
    template <class T>
    [[nodiscard]] static constexpr std::optional<ValidationError> Validate(const T& value) noexcept {
        std::optional<ValidationError> result;
        OpaqueRelationStorageAccess::VisitMember(value, [&]<std::meta::info Member>(const auto& member_value) {
            if (result) return;
            constexpr FieldConstraint member_policy = policy_of<Member>();
            if (auto violation = field_value_violation(member_value, member_policy); violation) {
                result = ValidationError{std::define_static_string(std::meta::identifier_of(Member)), *violation};
            }
        });
        return result;
    }
};

}  // namespace detail

template <class T>
[[nodiscard]] consteval bool opaque_relation_storage_shape_is_valid(const std::uint64_t valid_bits) noexcept {
    using U = RemoveCvRef<T>;
    if constexpr (!kOpaqueRelationStorage<U> || !std::equality_comparable<U> || !std::is_default_constructible_v<U>) { return false; }
    constexpr const auto& declarations = field_declarations<U>();
    using Product = RemoveCvRef<decltype(declarations)>;
    if constexpr (Product::base_types::count != 0U || Product::size() != 1U) {
        return false;
    } else {
        bool valid = false;
        Product::Visit([&]<class Declaration, std::size_t Index>() {
            using Member = typename Declaration::member_type;
            const auto& policy = declarations[Index];
            valid = policy.private_member && policy.default_zero && policy.annotations_valid && policy.only_policy_annotations &&
                    std::unsigned_integral<Member> && !std::same_as<Member, bool> && policy.constraint.has_maximum &&
                    policy.constraint.maximum == static_cast<long double>(valid_bits);
        });
        return valid;
    }
}

template <class Value>
[[nodiscard]] constexpr std::optional<ValidationError> validate_reflected_value(const Value& value) noexcept {
    using V = RemoveCvRef<Value>;
    if constexpr (requires { value.has_value(); }) {
        return value.has_value() ? validate_reflected_value(*value) : std::nullopt;
    } else if constexpr (requires {
                             typename V::value_type;
                             value.begin();
                             value.end();
                         } && !std::is_same_v<V, std::string> && !std::is_same_v<V, std::filesystem::path>) {
        for (const auto& item : value) {
            if (auto invalid = validate_reflected_value(item); invalid) return invalid;
        }
        return std::nullopt;
    } else if constexpr (std::is_class_v<V> && !std::is_same_v<V, std::string> && !std::is_same_v<V, std::filesystem::path>) {
        return validate_reflected_fields(value);
    } else {
        return std::nullopt;
    }
}

template <class T>
[[nodiscard]] constexpr std::optional<ValidationError> validate_reflected_fields(const T& value) noexcept {
    using U = RemoveCvRef<T>;
    std::optional<ValidationError> result;
    if constexpr (kOpaqueRelationStorage<U>) return detail::OpaqueRelationStorageValidation::Validate(value);
    visit_materialized_bases<U>([&]<class Base>() {
        if (result) return;
        if (auto invalid = validate_reflected_fields(static_cast<const Base&>(value)); invalid) result = invalid;
    });
    visit_materialized_members<U>([&]<class Declaration>(const auto& fact) {
        if constexpr (requires { Declaration::pointer; }) {
            if (result) return;
            const auto& member_value = value.*Declaration::pointer;
            constexpr FieldConstraint member_policy = policy_of_member<Declaration::pointer>();
            if constexpr (requires { member_value.has_value(); }) {
                if (member_value.has_value()) {
                    if (auto violation = field_value_violation(*member_value, member_policy); violation) {
                        result = ValidationError{fact.member_name, *violation};
                        return;
                    }
                }
            } else if (auto violation = field_value_violation(member_value, member_policy); violation) {
                result = ValidationError{fact.member_name, *violation};
                return;
            }
            if (auto invalid = validate_reflected_value(member_value); invalid) result = invalid;
        }
    });
    return result;
}

template <class Number>
constexpr void normalize_reflected_number(Number& value, const Number default_value, const FieldConstraint& constraint) noexcept {
    if constexpr (std::is_floating_point_v<Number>) {
        if (constraint.finite && !std::isfinite(value)) value = default_value;
    }
    if (constraint.has_minimum && static_cast<long double>(value) < constraint.minimum) { value = static_cast<Number>(constraint.minimum); }
    if (constraint.has_maximum && static_cast<long double>(value) > constraint.maximum) { value = static_cast<Number>(constraint.maximum); }
}

// Persistence repairs numeric values from the same reflected policy consumed by
// CLI, IPC, and system-boundary validation. Text and container violations remain terminal
// because truncating a path or silently dropping user entries changes meaning.
template <class T>
constexpr void normalize_reflected_intrinsics(T& value, const T& defaults) noexcept {
    using U = RemoveCvRef<T>;
    if constexpr (kOpaqueRelationStorage<U>) { return; }
    visit_materialized_bases<U>(
        [&]<class Base>() { normalize_reflected_intrinsics(static_cast<Base&>(value), static_cast<const Base&>(defaults)); });
    visit_materialized_members<U>([&]<class Declaration>(const auto&) {
        if constexpr (requires { Declaration::pointer; }) {
            auto& field = value.*Declaration::pointer;
            const auto& default_field = defaults.*Declaration::pointer;
            using Field = RemoveCvRef<decltype(field)>;
            constexpr FieldConstraint member_policy = policy_of_member<Declaration::pointer>();
            if constexpr (requires { field.has_value(); }) {
                if (field) {
                    using Item = RemoveCvRef<decltype(*field)>;
                    const Item fallback = default_field ? *default_field : Item{};
                    if constexpr (std::is_arithmetic_v<Item> && !std::is_same_v<Item, bool>) {
                        normalize_reflected_number(*field, fallback, member_policy);
                    } else if constexpr (std::is_enum_v<Item>) {
                        if (field_value_violation(*field, member_policy)) *field = fallback;
                    } else if constexpr (std::is_class_v<Item> && !std::is_same_v<Item, std::string> &&
                                         !std::is_same_v<Item, std::filesystem::path> && !kBoundedDynamicLeaf<Item>) {
                        normalize_reflected_intrinsics(*field, fallback);
                    }
                }
            } else if constexpr (std::is_arithmetic_v<Field> && !std::is_same_v<Field, bool>) {
                normalize_reflected_number(field, default_field, member_policy);
            } else if constexpr (std::is_enum_v<Field>) {
                if (field_value_violation(field, member_policy)) field = default_field;
            } else if constexpr (std::is_class_v<Field> && !std::is_same_v<Field, std::string> &&
                                 !std::is_same_v<Field, std::filesystem::path> && !kBoundedDynamicLeaf<Field> &&
                                 !requires { typename Field::value_type; }) {
                normalize_reflected_intrinsics(field, default_field);
            }
        }
    });
}

template <class Declaration, class Owner>
[[nodiscard]] consteval bool member_policy_is_complete();

enum class ReflectedPolicyAudit : unsigned char {
    AnnotationValidity,
    Completeness,
};

template <ReflectedPolicyAudit Audit, class Declaration, class Owner>
[[nodiscard]] consteval bool reflected_member_policy_satisfies() {
    if constexpr (Audit == ReflectedPolicyAudit::AnnotationValidity) {
        if constexpr (requires { Declaration::pointer; }) {
            return materialized_policy_annotations_are_valid<Declaration::pointer>();
        } else {
            return Declaration::annotations_valid;
        }
    } else if constexpr (requires { Declaration::pointer; }) {
        return member_policy_is_complete<Declaration, Owner>();
    } else {
        constexpr FieldConstraint policy = Declaration::constraint;
        using Member = typename Declaration::member_type;
        return (!std::is_same_v<Member, std::string> && !std::is_same_v<Member, std::filesystem::path> && !kBoundedDynamicLeaf<Member>) ||
               policy.maximum_bytes != 0U;
    }
}

template <ReflectedPolicyAudit Audit, class T>
[[nodiscard]] consteval bool reflected_policies_satisfy() {
    using U = RemoveCvRef<T>;
    bool valid = true;
    visit_materialized_bases<U>([&]<class Base>() { valid = valid && reflected_policies_satisfy<Audit, Base>(); });
    visit_materialized_members<U>([&]<class Declaration>(const auto&) {
        valid = valid && reflected_member_policy_satisfies<Audit, Declaration, U>();
        using Member = OptionalValueT<typename Declaration::member_type>;
        if constexpr (requires { typename Member::value_type; }) {
            using Element = RemoveCvRef<typename Member::value_type>;
            if constexpr (std::is_class_v<Element> && !kBoundedDynamicLeaf<Element> && !std::is_same_v<Element, std::string> &&
                          !std::is_same_v<Element, std::filesystem::path>) {
                valid = valid && reflected_policies_satisfy<Audit, Element>();
            }
        } else if constexpr (std::is_class_v<Member> && !kBoundedDynamicLeaf<Member> && !std::is_same_v<Member, std::string> &&
                             !std::is_same_v<Member, std::filesystem::path>) {
            valid = valid && reflected_policies_satisfy<Audit, Member>();
        }
    });
    return valid;
}

template <class T>  // CLEANUP-IGNORE Named canonical-walker validation.
[[nodiscard]] consteval bool reflected_policies_are_valid() {
    return reflected_policies_satisfy<ReflectedPolicyAudit::AnnotationValidity, T>();
}

template <class Declaration, class Owner>
[[nodiscard]] consteval bool member_policy_is_complete() {
    using MemberType = OptionalValueT<decltype(std::declval<Owner&>().*Declaration::pointer)>;
    constexpr FieldConstraint policy = policy_of_member<Declaration::pointer>();
    if constexpr (kBoundedDynamicLeaf<MemberType>) {
        return policy.maximum_bytes != 0U && policy.maximum_items != 0U;
    } else if constexpr (kByteSequence<MemberType> && !requires { std::tuple_size<MemberType>::value; }) {
        return policy.maximum_bytes != 0U;
    } else if constexpr (std::is_floating_point_v<MemberType>) {
        return policy.finite;
    } else if constexpr (std::is_same_v<MemberType, std::string> || std::is_same_v<MemberType, std::filesystem::path>) {
        return policy.maximum_bytes != 0U;
    } else if constexpr (requires { typename MemberType::value_type; } && !requires { std::tuple_size<MemberType>::value; }) {
        return policy.maximum_items != 0U;
    }
    return true;
}

template <class T>
[[nodiscard]] consteval bool reflected_policies_are_complete() {
    return reflected_policies_satisfy<ReflectedPolicyAudit::Completeness, T>();
}

template <class T>
[[nodiscard]] consteval bool reflected_defaults_are_valid() {
    return !validate_reflected_fields(T{}).has_value();
}

template <class... Roots>
[[nodiscard]] consteval bool ingress_policies_are_valid() {
    return (reflected_policies_are_valid<Roots>() && ...) && (reflected_policies_are_complete<Roots>() && ...);
}

}  // namespace mmltk::frameworks::reflection
