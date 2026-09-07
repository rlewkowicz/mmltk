#pragma once

#include <array>
#include <meta>
#include <type_traits>

namespace mmltk::frameworks::reflection {

struct FieldPolicyMaterializer;

enum class MaterializerQuery {
    NonstaticDataMembers,
    AllMembers,
};

namespace detail {

template <class Owner>
[[nodiscard]] consteval bool opaque_relation_storage_owner() {
    bool opaque = false;
    template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(^^Owner))) {
        using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        if constexpr (requires { Annotation::is_opaque_relation_storage; }) opaque = opaque || Annotation::is_opaque_relation_storage;
    }
    return opaque;
}

template <class Owner>
struct EnumMaterializationInput final {
    inline static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Owner));
};

template <class Owner>
    requires(!opaque_relation_storage_owner<Owner>())
struct AllMemberMaterializationInput final {
    inline static constexpr auto members = std::define_static_array(std::meta::members_of(^^Owner, std::meta::access_context::unchecked()));
};

template <class Owner>
    requires(!opaque_relation_storage_owner<Owner>())
struct FieldMaterializationInput final {
    inline static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^Owner, std::meta::access_context::unchecked()));
    inline static constexpr auto bases = std::define_static_array(std::meta::bases_of(^^Owner, std::meta::access_context::unchecked()));
};

template <class Owner>
    requires(opaque_relation_storage_owner<Owner>())
class OpaqueFieldMaterializationInput final {
    friend struct ::mmltk::frameworks::reflection::FieldPolicyMaterializer;

    inline static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^Owner, std::meta::access_context::unchecked()));
    inline static constexpr auto bases = std::define_static_array(std::meta::bases_of(^^Owner, std::meta::access_context::unchecked()));
};

}  // namespace detail

// The policy owns the product vocabulary and the interpretation of the
// reflected declarations.  This header owns only the single reflection query
// used to hand those declarations to that policy beside the canonical owner.
template <class Owner, class Policy>
    requires(!detail::opaque_relation_storage_owner<Owner>())
[[nodiscard]] consteval auto materialize(Policy policy) {
    if constexpr (std::is_enum_v<Owner>) {
        return policy.template operator()<Owner, detail::EnumMaterializationInput<Owner>>();
    } else {
        if constexpr (requires { Policy::query; }) {
            static_assert(Policy::query == MaterializerQuery::AllMembers);
            return policy.template operator()<Owner, detail::AllMemberMaterializationInput<Owner>>();
        } else {
            return policy.template operator()<Owner, detail::FieldMaterializationInput<Owner>>();
        }
    }
}

template <class Owner, class Policy>
    requires std::is_same_v<std::remove_cvref_t<Policy>, FieldPolicyMaterializer>
[[nodiscard]] consteval auto materialize_opaque_field_policy(Policy policy) {
    static_assert(detail::opaque_relation_storage_owner<Owner>());
    return policy.template operator()<Owner, detail::OpaqueFieldMaterializationInput<Owner>>();
}

}  // namespace mmltk::frameworks::reflection
