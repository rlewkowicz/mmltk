#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <inplace_vector>
#include <limits>
#include <meta>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "src/controller/browser/application_stable_identity.h"
#include "src/controller/browser/client_record.h"
#include "src/controller/contracts/application_systems.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/workflow_path_dialogs.h"
#include "src/controller/contracts/integration_control.h"
#include "src/controller/contracts/settings_vocabulary.h"
#include "src/controller/contracts/workflows.h"
#include "src/controller/presentation/visual_system_types.h"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/serialization/serialization.h"

namespace mmltk::controller::browser {

struct ApplicationRequestFieldFact final {
    std::uint64_t endpoint_id = 0U;
    std::uint64_t stable_id = 0U;
    std::string_view name{};
    mmltk::frameworks::reflection::FieldConstraint constraint{};
    mmltk::frameworks::reflection::PresentationKind presentation = mmltk::frameworks::reflection::PresentationKind::Default;
    bool file_dialog_identity = false;
    bool settings_update_values = false;
};

struct ApplicationFileDialogFact final {
    std::string_view title{};
    std::string_view filter{};
    std::string_view pattern{};
    mmltk::controller::contracts::FileDialogMode mode = mmltk::controller::contracts::FileDialogMode::OpenFile;
    constexpr bool operator==(const ApplicationFileDialogFact&) const noexcept = default;
};

struct ApplicationSettingsLeafFact final {
    std::string_view path{};
    std::uint64_t stable_id = 0U;
    mmltk::frameworks::reflection::ReflectedMemberIdentity identity{};
    mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy workflows{};
    mmltk::frameworks::reflection::FieldConstraint constraint{};
    mmltk::frameworks::reflection::PresentationKind presentation = mmltk::frameworks::reflection::PresentationKind::Default;
    std::string_view catalog_provider{};
    std::optional<ApplicationFileDialogFact> file_dialog{};
    bool mutable_leaf = false;
};

struct ApplicationCatalogProviderFact final {
    std::string_view name{};
    std::string_view identity{};
    std::string_view row_type{};
    std::uint64_t stable_id = 0U;
};

struct ApplicationCatalogRowFact final {
    std::uint64_t provider_id = 0U;
    std::uint64_t stable_id = 0U;
    std::string_view key{};
    std::size_t index = 0U;
};

struct ApplicationSettingsDefaultFact final {
    std::string_view path{};
    std::uint64_t stable_id = 0U;
};

template <class>
struct SystemMethodSignature;

#define MMLTK_SYSTEM_METHOD_SIGNATURE(Qualifiers)                               \
    template <class Result, class Owner>                                        \
    struct SystemMethodSignature<Result (Owner::*)() Qualifiers> final {        \
        using owner_type = Owner;                                               \
        using result_type = Result;                                             \
        using request_type = void;                                              \
        static constexpr bool has_request = false;                              \
        static constexpr bool request_by_value = true;                          \
        static constexpr bool result_by_value = !std::is_reference_v<Result>;   \
    };                                                                          \
    template <class Result, class Owner, class Request>                         \
    struct SystemMethodSignature<Result (Owner::*)(Request) Qualifiers> final { \
        using owner_type = Owner;                                               \
        using result_type = Result;                                             \
        using request_type = std::remove_cvref_t<Request>;                      \
        static constexpr bool has_request = true;                               \
        static constexpr bool request_by_value = !std::is_reference_v<Request>; \
        static constexpr bool result_by_value = !std::is_reference_v<Result>;   \
    };
MMLTK_SYSTEM_METHOD_SIGNATURE()
MMLTK_SYSTEM_METHOD_SIGNATURE(const)
MMLTK_SYSTEM_METHOD_SIGNATURE(noexcept)
MMLTK_SYSTEM_METHOD_SIGNATURE(const noexcept)
#undef MMLTK_SYSTEM_METHOD_SIGNATURE

namespace application_schema_detail {

using mmltk::frameworks::reflection::materialized_enum_entries;
using mmltk::frameworks::reflection::materialized_field_policies;

template <std::meta::info Member, class Annotation>
[[nodiscard]] consteval std::size_t annotation_count() {
    std::size_t count = 0U;
    template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(Member))) {
        using Actual = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        count += std::same_as<Actual, Annotation> ? 1U : 0U;
    }
    return count;
}

template <std::meta::info Member, class Annotation>
[[nodiscard]] consteval Annotation annotation_value() {
    Annotation result{};
    template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(Member))) {
        using Actual = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        if constexpr (std::same_as<Actual, Annotation>) result = std::meta::extract<Actual>(annotation);
    }
    return result;
}

template <std::meta::info Member>
[[nodiscard]] consteval bool endpoint_member_eligible() {
    return std::meta::is_function(Member) && std::meta::is_public(Member) && std::meta::is_user_declared(Member) &&
           !std::meta::is_constructor(Member) && !std::meta::is_destructor(Member) && !std::meta::is_static_member(Member);
}

template <std::meta::info Member>
[[nodiscard]] consteval bool snapshot_member_eligible() {
    return std::meta::is_function(Member) && std::meta::is_public(Member) && std::meta::is_user_declared(Member) &&
           !std::meta::is_constructor(Member) && !std::meta::is_destructor(Member) && !std::meta::is_static_member(Member);
}

template <std::meta::info Member>
[[nodiscard]] consteval std::size_t endpoint_annotation_count() {
    constexpr auto intents = annotation_count<Member, mmltk::controller::contracts::reflection::direct::IntentEndpoint>();
    constexpr auto interactions = annotation_count<Member, mmltk::controller::contracts::reflection::direct::InteractionEndpoint>();
    static_assert(intents + interactions <= 1U, "ordinary system member has duplicate endpoint annotations");
    return intents + interactions;
}

template <std::meta::info Member>
[[nodiscard]] consteval bool is_endpoint_method() {
    constexpr auto annotations = endpoint_annotation_count<Member>();
    if constexpr (annotations == 0U) {
        return false;
    } else {
        static_assert(endpoint_member_eligible<Member>(),
                      "endpoint annotation requires a public, user-declared, non-static ordinary method");
        return endpoint_member_eligible<Member>();
    }
}

template <std::meta::info Member>
[[nodiscard]] consteval bool is_interaction_method() {
    return annotation_count<Member, mmltk::controller::contracts::reflection::direct::InteractionEndpoint>() == 1U;
}

template <class System, class Visitor>
consteval void visit_endpoint_members(Visitor& visitor) {
    template for (constexpr auto member :
                  std::define_static_array(std::meta::members_of(^^System, std::meta::access_context::unchecked()))) {
        if constexpr (is_endpoint_method<member>()) visitor.template operator()<member>();
    }
}

template <class System>
[[nodiscard]] consteval std::meta::info snapshot_member() {
    std::meta::info result = ^^void;
    std::size_t count = 0U;
    template for (constexpr auto member :
                  std::define_static_array(std::meta::members_of(^^System, std::meta::access_context::unchecked()))) {
        constexpr auto annotations = annotation_count<member, mmltk::controller::contracts::reflection::Snapshot>();
        static_assert(annotations <= 1U, "ordinary system member has duplicate typed Snapshot annotations");
        if constexpr (annotations == 1U) {
            constexpr bool eligible = snapshot_member_eligible<member>();
            static_assert(eligible, "Snapshot annotation requires a public, user-declared, non-static ordinary method");
            if constexpr (eligible) {
                result = member;
                ++count;
            }
        }
    }
    if (count != 1U) throw "ordinary system requires exactly one reflected snapshot method";
    return result;
}

template <class Request, class Visitor>
constexpr void visit_fields(Visitor& visitor) {
    constexpr const auto& policies = materialized_field_policies(std::type_identity<Request>{});
    using Policies = std::remove_cvref_t<decltype(policies)>;
    Policies::base_types::Visit([&]<class Base>() { visit_fields<Base>(visitor); });
    Policies::Visit([&]<class Declaration, std::size_t Index>() { visitor.template operator()<Request, Declaration>(policies[Index]); });
}

[[nodiscard]] constexpr mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy all_feature_workflows() noexcept {
    mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy result{};
    result.count = mmltk::controller::contracts::kFeatureCount;
    for (std::size_t index = 0U; index < result.count; ++index)
        result.workflows[index] = static_cast<mmltk::controller::contracts::FeatureId>(index);
    return result;
}

template <class Declaration>
[[nodiscard]] consteval auto declaration_metadata() {
    struct Result final {
        std::optional<mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy> workflows;
        std::optional<ApplicationFileDialogFact> file_dialog;
        std::string_view catalog_provider;
    };
    Result result{};
    std::size_t workflow_count = 0U;
    std::size_t dialog_count = 0U;
    std::size_t provider_count = 0U;
    Declaration::VisitAnnotations([&]<class Annotation>(const Annotation& annotation) {
        using A = std::remove_cvref_t<Annotation>;
        if constexpr (mmltk::controller::contracts::reflection::is_feature_scope_annotation<A>) {
            ++workflow_count;
            mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy policy{};
            policy.count = annotation.values.size();
            for (std::size_t index = 0U; index < policy.count; ++index)
                policy.workflows[index] = annotation.values[index];
            result.workflows = policy;
        } else if constexpr (mmltk::controller::contracts::reflection::is_file_dialog_annotation<A>) {
            ++dialog_count;
            result.file_dialog = ApplicationFileDialogFact{
                .title = A::title,
                .filter = A::filter,
                .pattern = A::pattern,
                .mode = annotation.mode,
            };
        } else if constexpr (mmltk::frameworks::reflection::is_catalog_provider_annotation<A>) {
            ++provider_count;
            using Provider = typename A::provider_type;
            static_assert(Provider::valid(), "reflected catalog provider is invalid");
            static_assert(std::meta::has_identifier(^^Provider), "catalog providers require canonical declaration identifiers");
            result.catalog_provider = mmltk::frameworks::reflection::type_name<Provider>();
        }
    });
    if (workflow_count > 1U) throw "settings declaration has duplicate feature scopes";
    if (dialog_count > 1U) throw "settings declaration has duplicate file dialogs";
    if (provider_count > 1U) throw "settings declaration has duplicate catalog providers";
    if (result.workflows && !result.workflows->valid()) throw "settings declaration has invalid feature scope";
    if (result.file_dialog &&
        (result.file_dialog->title.empty() || result.file_dialog->filter.empty() || result.file_dialog->pattern.empty() ||
         !mmltk::frameworks::reflection::enum_contains(result.file_dialog->mode)))
        throw "settings declaration has invalid file-dialog metadata";
    return result;
}

template <class Declaration, class Annotation>
[[nodiscard]] consteval bool declaration_has_annotation() {
    std::size_t count = 0U;
    Declaration::VisitAnnotations(
        [&]<class Actual>(const Actual&) { count += std::same_as<std::remove_cvref_t<Actual>, Annotation> ? 1U : 0U; });
    if (count > 1U) throw "field has a duplicate role annotation";
    return count == 1U;
}

class ScopedFieldPath final {
   public:
    constexpr ScopedFieldPath(std::string& path, const std::string_view member) : path_(path), prior_size_(path.size()) {
        if (!path_.empty()) path_.push_back('.');
        path_.append(member);
    }
    constexpr ~ScopedFieldPath() { path_.resize(prior_size_); }
    ScopedFieldPath(const ScopedFieldPath&) = delete;
    ScopedFieldPath& operator=(const ScopedFieldPath&) = delete;

   private:
    std::string& path_;
    std::size_t prior_size_;
};

enum class BoundaryProjection : std::uint8_t {
    Runtime,
    Catalog,
};

template <BoundaryProjection Projection, class Value>
[[nodiscard]] consteval bool boundary_projectable();

template <class Value>
[[nodiscard]] consteval bool runtime_boundary_projectable() {
    return boundary_projectable<BoundaryProjection::Runtime, Value>();
}

template <class Root, class Value, auto... Prefix, class Visitor>
constexpr void visit_settings_leaves(Visitor& visitor, std::string prefix,
                                     const mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy inherited) {
    using Type = std::remove_cvref_t<Value>;
    namespace vocabulary = mmltk::controller::contracts::settings_vocabulary;
    static_assert(!vocabulary::is_leaf_v<Type>);
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) return;
    constexpr const auto& policies = materialized_field_policies(std::type_identity<Type>{});
    using Policies = std::remove_cvref_t<decltype(policies)>;
    Policies::base_types::Visit([&]<class Base>() { visit_settings_leaves<Root, Base, Prefix...>(visitor, prefix, inherited); });
    Policies::Visit([&]<class Declaration, std::size_t Index>() {
        if constexpr (!mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) {
            using Member = typename Declaration::member_type;
            constexpr auto metadata = declaration_metadata<Declaration>();
            const auto workflows = metadata.workflows.value_or(inherited);
            const auto& policy = policies[Index];
            const ScopedFieldPath field_path(prefix, policy.member_name);
            if constexpr (vocabulary::is_leaf_v<Member>) {
                constexpr auto path = mmltk::frameworks::reflection::member_path<Prefix..., Declaration::pointer>;
                static_assert(runtime_boundary_projectable<Member>(),
                              "settings leaf contains a type unsupported by the runtime Rust projection");
                static_assert(!metadata.file_dialog || std::same_as<Member, std::string> || std::same_as<Member, std::filesystem::path>,
                              "file dialog requires a path-like settings leaf");
                constexpr auto dialog = [&] {
                    if constexpr (const auto workflow_dialog = mmltk::controller::contracts::workflow_path_dialog<path>(); workflow_dialog)
                        return std::optional<ApplicationFileDialogFact>{{workflow_dialog->title, workflow_dialog->filter,
                            workflow_dialog->pattern, workflow_dialog->mode}};
                    else return metadata.file_dialog;
                }();
                visitor.template operator()<Type, Declaration, Member>(ApplicationSettingsLeafFact{
                    .path = prefix,
                    .stable_id = application_settings_field_stable_id(prefix),
                    .identity = mmltk::frameworks::reflection::accessor_member_identity<Root, path>(),
                    .workflows = workflows,
                    .constraint = policy.constraint,
                    .presentation = policy.presentation,
                    .catalog_provider = metadata.catalog_provider,
                    .file_dialog = dialog,
                    .mutable_leaf = vocabulary::is_mutable_member_v<Declaration, Member>,
                });
            } else {
                static_assert(!metadata.file_dialog, "file dialog must annotate a settings leaf");
                visit_settings_leaves<Root, Member, Prefix..., Declaration::pointer>(visitor, prefix, workflows);
            }
        }
    });
}

// CLEANUP-OFF: Default traversal carries runtime values while leaf traversal carries compile-time metadata.
template <class Value, class Visitor>
void visit_settings_defaults(Visitor& visitor, const Value& defaults, std::string prefix,
                             const mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy inherited) {
    using Type = std::remove_cvref_t<Value>;
    namespace vocabulary = mmltk::controller::contracts::settings_vocabulary;
    static_assert(!vocabulary::is_leaf_v<Type>);
    if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) return;
    constexpr const auto& policies = materialized_field_policies(std::type_identity<Type>{});
    using Policies = std::remove_cvref_t<decltype(policies)>;
    Policies::base_types::Visit([&]<class Base>() {
        visit_settings_defaults<Base>(visitor, static_cast<const Base&>(defaults), prefix, inherited);
        // CLEANUP-ON
    });
    Policies::Visit([&]<class Declaration, std::size_t Index>() {
        using Member = typename Declaration::member_type;
        constexpr auto metadata = declaration_metadata<Declaration>();
        const auto workflows = metadata.workflows.value_or(inherited);
        const auto& policy = policies[Index];
        const ScopedFieldPath field_path(prefix, policy.member_name);
        if constexpr (requires { Declaration::pointer; }) {
            const auto& member = defaults.*Declaration::pointer;
            if constexpr (vocabulary::is_leaf_v<Member>) {
                visitor.template operator()<Type, Declaration, Member>(
                    ApplicationSettingsDefaultFact{
                        .path = prefix,
                        .stable_id = application_settings_field_stable_id(prefix),
                    },
                    member);
            } else {
                visit_settings_defaults<Member>(visitor, member, prefix, workflows);
            }
        }
    });
}

template <class>
struct Variant final {
    static constexpr bool value = false;
};
template <class... Value>
struct Variant<std::variant<Value...>> final {
    static constexpr bool value = true;
    template <class Visitor>
    static constexpr void Visit(Visitor&& visitor) {
        (visitor.template operator()<Value>(), ...);
    }
};

template <class>
struct Optional final : std::false_type {};
template <class Value>
struct Optional<std::optional<Value>> final : std::true_type {
    using value_type = Value;
};

template <class>
struct Sequence final : std::false_type {};
template <class Value, class Allocator>
struct Sequence<std::vector<Value, Allocator>> final : std::true_type {
    using value_type = Value;
    static constexpr bool bounded = false;
    static constexpr std::size_t extent = 0U;
};
template <class Value, std::size_t Extent>
struct Sequence<std::array<Value, Extent>> final : std::true_type {
    using value_type = Value;
    static constexpr bool bounded = true;
    static constexpr std::size_t extent = Extent;
};
template <class Value, std::size_t Capacity>
struct Sequence<std::inplace_vector<Value, Capacity>> final : std::true_type {
    using value_type = Value;
    static constexpr bool bounded = true;
    static constexpr std::size_t extent = Capacity;
};

template <class>
struct StaticArray final : std::false_type {};
template <class Value, std::size_t Extent>
struct StaticArray<std::array<Value, Extent>> final : std::true_type {
    using value_type = Value;
    static constexpr std::size_t extent = Extent;
};

template <class>
struct ByteSequence final : std::false_type {};
template <class Allocator>
struct ByteSequence<std::vector<std::byte, Allocator>> final : std::true_type {
    static constexpr std::size_t extent = 0U;
};
template <class Allocator>
struct ByteSequence<std::vector<std::uint8_t, Allocator>> final : std::true_type {
    static constexpr std::size_t extent = 0U;
};
template <std::size_t Extent>
struct ByteSequence<std::array<std::byte, Extent>> final : std::true_type {
    static constexpr std::size_t extent = Extent;
};
template <std::size_t Extent>
struct ByteSequence<std::array<std::uint8_t, Extent>> final : std::true_type {
    static constexpr std::size_t extent = Extent;
};
template <std::size_t Extent>
struct ByteSequence<std::array<char, Extent>> final : std::true_type {
    static constexpr std::size_t extent = Extent;
};

template <class Type>
[[nodiscard]] consteval bool valid_fixed_text_shape() {
    namespace reflection = mmltk::frameworks::reflection;
    constexpr auto annotations = reflection::fixed_text_annotation_count<Type>();
    if constexpr (annotations == 0U) {
        return true;
    } else if constexpr (annotations != 1U) {
        return false;
    } else {
        constexpr auto policy = reflection::fixed_text_policy_of<Type>();
        if (policy.capacity == 0U || policy.characters != reflection::FixedTextCharacterPolicy::PrintableAscii) return false;
        bool bytes = false;
        bool size = false;
        std::size_t members = 0U;
        auto visitor = [&]<class Owner, class Declaration>(const auto& fact) {
            using Field = typename Declaration::member_type;
            ++members;
            if (fact.member_name == "bytes") {
                if constexpr (ByteSequence<Field>::value && Sequence<Field>::value)
                    bytes = ByteSequence<Field>::extent == policy.capacity && std::same_as<typename Sequence<Field>::value_type, char>;
            } else if (fact.member_name == "size") {
                if constexpr (std::unsigned_integral<Field>) size = std::numeric_limits<Field>::max() >= policy.capacity;
            }
        };
        visit_fields<Type>(visitor);
        return members == 2U && bytes && size;
    }
}

template <class Value>
concept ReflectedObject = requires { materialized_field_policies(std::type_identity<Value>{}); };

template <class Value>
concept ReflectedEnum = std::is_enum_v<Value> && requires { materialized_enum_entries(std::type_identity<Value>{}); };

template <class Provider>
concept ProjectableCatalogProvider = requires(const typename Provider::row_type& row) {
    { Provider::identity } -> std::convertible_to<std::string_view>;
    { Provider::valid() } -> std::same_as<bool>;
    { Provider::row_key(row) } -> std::convertible_to<std::string_view>;
};

template <class Value>
[[nodiscard]] consteval bool runtime_scalar_projectable() {
    using Type = std::remove_cvref_t<Value>;
    return std::same_as<Type, void> || mmltk::frameworks::reflection::kReflectedIntegerScalar<Type> || std::same_as<Type, float> ||
           std::same_as<Type, double> || std::same_as<Type, std::byte> || std::same_as<Type, std::string> ||
           std::same_as<Type, std::filesystem::path> || std::same_as<Type, mmltk::frameworks::serialization::wire::Value> ||
           std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue>;
}

template <BoundaryProjection Projection, class Value>
[[nodiscard]] consteval bool boundary_projectable() {
    using Type = std::remove_cvref_t<Value>;
    if constexpr (Optional<Type>::value) {
        return boundary_projectable<Projection, typename Optional<Type>::value_type>();
    } else if constexpr (ByteSequence<Type>::value) {
        return Projection == BoundaryProjection::Runtime;
    } else if constexpr (Sequence<Type>::value) {
        if constexpr (Projection == BoundaryProjection::Catalog) {
            if constexpr (!StaticArray<Type>::value) {
                return false;
            } else {
                return StaticArray<Type>::extent != 0U && boundary_projectable<Projection, typename Sequence<Type>::value_type>();
            }
        } else {
            return boundary_projectable<Projection, typename Sequence<Type>::value_type>();
        }
    } else if constexpr (Variant<Type>::value) {
        if constexpr (Projection == BoundaryProjection::Catalog) return false;
        bool supported = true;
        Variant<Type>::Visit([&]<class Alternative>() { supported = supported && boundary_projectable<Projection, Alternative>(); });
        return supported;
    } else if constexpr (std::is_enum_v<Type>) {
        return ReflectedEnum<Type>;
    } else if constexpr (ReflectedObject<Type> && !std::same_as<Type, std::string> && !std::same_as<Type, std::filesystem::path>) {
        bool supported = Projection == BoundaryProjection::Catalog || valid_fixed_text_shape<Type>();
        auto visitor = [&]<class Owner, class Declaration>(const auto&) {
            using Field = typename Declaration::member_type;
            supported = supported && boundary_projectable<Projection, Field>();
        };
        visit_fields<Type>(visitor);
        return supported;
    } else if constexpr (Projection == BoundaryProjection::Catalog) {
        return std::same_as<Type, std::string_view> ||
               (runtime_scalar_projectable<Type>() && !std::same_as<Type, void> && !std::same_as<Type, std::string> &&
                !std::same_as<Type, std::filesystem::path> && !std::same_as<Type, std::byte> &&
                !std::same_as<Type, mmltk::frameworks::serialization::wire::Value> &&
                !std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue>);
    } else {
        return runtime_scalar_projectable<Type>();
    }
}

template <class Value>
[[nodiscard]] consteval bool catalog_boundary_projectable() {
    return boundary_projectable<BoundaryProjection::Catalog, Value>();
}

template <class Provider>
consteval void validate_catalog_provider() {
    static_assert(ProjectableCatalogProvider<Provider>, "catalog provider lacks the typed projection surface");
    if constexpr (ProjectableCatalogProvider<Provider>) {
        static_assert(Provider::valid(), "reflected catalog provider is invalid");
        using Row = typename Provider::row_type;
        static_assert(ReflectedObject<Row>, "catalog provider row type must be reflected");
        static_assert(catalog_boundary_projectable<Row>(), "catalog row contains a type unsupported by static Rust projection");
    }
}

template <class System>
concept ReflectedEventVariant = requires { typename System::event_type; } && Variant<typename System::event_type>::value;

template <std::meta::info Method>
concept SupportedEndpointMethod = requires { typename SystemMethodSignature<std::remove_cv_t<decltype(&[:Method:])>>::owner_type; };

template <std::meta::info Method>
concept SupportedSnapshotMethod =
    requires { typename SystemMethodSignature<std::remove_cv_t<decltype(&[:Method:])>>::owner_type; } && [] consteval {
        using Signature = SystemMethodSignature<std::remove_cv_t<decltype(&[:Method:])>>;
        return !Signature::has_request && Signature::result_by_value && !std::is_void_v<typename Signature::result_type>;
    }();

template <class Declaration, class Visitor>
constexpr void visit_declaration_catalog_providers(Visitor& visitor) {
    Declaration::VisitAnnotations([&]<class Annotation>(const Annotation&) {
        using A = std::remove_cvref_t<Annotation>;
        if constexpr (mmltk::frameworks::reflection::is_catalog_provider_annotation<A>)
            visitor.template operator()<typename A::provider_type>();
    });
}

template <class Value, class Visitor>
void visit_type_catalog_providers(Visitor& visitor, std::set<std::string>& seen_types) {
    using Type = std::remove_cvref_t<Value>;
    if constexpr (std::is_void_v<Type>) {
        return;
    } else {
        const std::string type_name(mmltk::frameworks::serialization::reflected_schema_type_name<Type>());
        if (!seen_types.insert(type_name).second) return;
        if constexpr (Optional<Type>::value) {
            visit_type_catalog_providers<typename Optional<Type>::value_type>(visitor, seen_types);
        } else if constexpr (Sequence<Type>::value) {
            visit_type_catalog_providers<typename Sequence<Type>::value_type>(visitor, seen_types);
        } else if constexpr (Variant<Type>::value) {
            Variant<Type>::Visit([&]<class Alternative>() { visit_type_catalog_providers<Alternative>(visitor, seen_types); });
        } else if constexpr (ReflectedObject<Type> && !std::same_as<Type, std::string> && !std::same_as<Type, std::filesystem::path>) {
            auto field_visitor = [&]<class Owner, class Declaration>(const auto&) {
                using Field = typename Declaration::member_type;
                visit_declaration_catalog_providers<Declaration>(visitor);
                visit_type_catalog_providers<Field>(visitor, seen_types);
            };
            visit_fields<Type>(field_visitor);
        }
    }
}

template <class Root, class Value, auto... Prefix, class Visitor>
constexpr void visit_settings_relations(Visitor& visitor) {
    using Type = std::remove_cvref_t<Value>;
    namespace vocabulary = mmltk::controller::contracts::settings_vocabulary;
    if constexpr (vocabulary::is_leaf_v<Type> || mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) {
        return;
    } else {
        mmltk::frameworks::reflection::visit_materialized_bases<Type>(
            [&]<class Base>() { visit_settings_relations<Root, Base, Prefix...>(visitor); });
        mmltk::frameworks::reflection::visit_materialized_members<Type>([&]<class Declaration>(const auto&) {
            if constexpr (requires { Declaration::pointer; }) {
                using Member = typename Declaration::member_type;
                auto provider_visitor = [&]<class Provider>() {
                    if constexpr (mmltk::frameworks::reflection::HasCatalogProviderRelation<Provider>) {
                        using Relation = mmltk::frameworks::reflection::catalog_provider_relation<Provider>;
                        constexpr auto selector = mmltk::frameworks::reflection::member_path<Prefix..., Declaration::pointer>;
                        static_assert(
                            std::same_as<Member, mmltk::frameworks::reflection::accessor_value_t<typename Relation::destination_type,
                                                                                                 Relation::destination_selector>>);
                        constexpr auto override_path =
                            mmltk::frameworks::reflection::rebase_member_path<Root, typename Relation::destination_type>(
                                selector, Relation::destination_override_state);
                        using Override = mmltk::frameworks::reflection::accessor_value_t<Root, override_path>;
                        static_assert(std::same_as<Override, typename Relation::override_state_type>);
                        static_assert(mmltk::frameworks::reflection::kOpaqueRelationStorage<Override>);
                        visitor.template operator()<Provider, Relation, selector>();
                    }
                };
                visit_declaration_catalog_providers<Declaration>(provider_visitor);
                if constexpr (!vocabulary::is_leaf_v<Member>)
                    visit_settings_relations<Root, Member, Prefix..., Declaration::pointer>(visitor);
            }
        });
    }
}

template <class Root, class Relation, auto Selector>
[[nodiscard]] consteval bool audit_settings_relation(std::vector<mmltk::frameworks::reflection::ReflectedMemberIdentity>& claims) {
    using namespace mmltk::frameworks::reflection;
    if constexpr (!requires {
                      { Relation::audit() } -> std::same_as<bool>;
                      Relation::source_selector;
                      Relation::destination_selector;
                      Relation::destination_override_state;
                      Relation::valid_bits;
                  }) {
        return false;
    } else {
        if (!Relation::audit()) return false;
        using SelectorValue = accessor_value_t<Root, Selector>;
        using DestinationSelector = accessor_value_t<typename Relation::destination_type, Relation::destination_selector>;
        if constexpr (!std::same_as<SelectorValue, DestinationSelector>) return false;
        constexpr auto canonical_selector =
            rebase_member_path<Root, typename Relation::destination_type>(Selector, Relation::destination_selector);
        if (accessor_member_identity<Root, Selector>() != accessor_member_identity<Root, canonical_selector>()) return false;

        constexpr auto override_path =
            rebase_member_path<Root, typename Relation::destination_type>(Selector, Relation::destination_override_state);
        using Override = accessor_value_t<Root, override_path>;
        using ValidBits = std::remove_cvref_t<decltype(Relation::valid_bits)>;
        if constexpr (!std::same_as<Override, typename Relation::override_state_type> || !std::unsigned_integral<ValidBits>) {
            return false;
        } else if (!opaque_relation_storage_shape_is_valid<Override>(static_cast<std::uint64_t>(Relation::valid_bits))) {
            return false;
        }
        claims.push_back(accessor_member_identity<Root, override_path>());

        bool valid = true;
        std::vector<ReflectedMemberIdentity> destinations;
        Relation::VisitMembers([&]<class Entry>() {
            constexpr auto destination = rebase_member_path<Root, typename Relation::destination_type>(Selector, Entry::destination);
            constexpr auto identity = accessor_member_identity<Root, destination>();
            constexpr auto rendered = reflected_member_path<Root, destination>();
            for (const auto& prior : destinations)
                valid = valid && prior != identity;
            destinations.push_back(identity);

            std::size_t matches = 0U;
            auto leaf_visitor = [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& leaf) {
                if (leaf.identity != identity) return;
                ++matches;
                using Destination = accessor_value_t<Root, destination>;
                valid = valid && std::same_as<Member, Destination> && leaf.mutable_leaf && leaf.path == rendered.view() &&
                        leaf.stable_id == application_settings_field_stable_id(rendered.view());
            };
            visit_settings_leaves<Root, Root>(leaf_visitor, {}, all_feature_workflows());
            valid = valid && matches == 1U;
        });
        return valid;
    }
}

template <class Root, class Value, auto... Prefix>
// CPD-OFF: Relation auditing and relation visitation recurse the same native tree but produce different facts.
consteval void audit_settings_relations(std::vector<mmltk::frameworks::reflection::ReflectedMemberIdentity>& opaque,
                                        std::vector<mmltk::frameworks::reflection::ReflectedMemberIdentity>& claims, bool& valid) {
    using Type = std::remove_cvref_t<Value>;
    namespace vocabulary = mmltk::controller::contracts::settings_vocabulary;
    if constexpr (vocabulary::is_leaf_v<Type> || mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) {
        return;
    } else {
        mmltk::frameworks::reflection::visit_materialized_bases<Type>(
            [&]<class Base>() { audit_settings_relations<Root, Base, Prefix...>(opaque, claims, valid); });
        // CPD-ON
        mmltk::frameworks::reflection::visit_materialized_members<Type>([&]<class Declaration>(const auto&) {
            if constexpr (requires { Declaration::pointer; }) {
                using Member = typename Declaration::member_type;
                constexpr auto path = mmltk::frameworks::reflection::member_path<Prefix..., Declaration::pointer>;
                if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<Member>)
                    opaque.push_back(mmltk::frameworks::reflection::accessor_member_identity<Root, path>());
                auto provider_visitor = [&]<class Provider>() {
                    if constexpr (mmltk::frameworks::reflection::HasCatalogProviderRelation<Provider>) {
                        using Relation = mmltk::frameworks::reflection::catalog_provider_relation<Provider>;
                        valid = valid && audit_settings_relation<Root, Relation, path>(claims);
                    }
                };
                visit_declaration_catalog_providers<Declaration>(provider_visitor);
                if constexpr (!vocabulary::is_leaf_v<Member> && !mmltk::frameworks::reflection::kOpaqueRelationStorage<Member>)
                    audit_settings_relations<Root, Member, Prefix..., Declaration::pointer>(opaque, claims, valid);
            }
        });
    }
}

template <class Settings>
[[nodiscard]] consteval bool settings_relations_are_valid() {
    std::vector<mmltk::frameworks::reflection::ReflectedMemberIdentity> opaque;
    std::vector<mmltk::frameworks::reflection::ReflectedMemberIdentity> claims;
    bool valid = true;
    audit_settings_relations<Settings, Settings>(opaque, claims, valid);
    if (!valid) return false;
    if (opaque.size() != claims.size()) return false;
    for (const auto& storage : opaque) {
        std::size_t count = 0U;
        for (const auto& claim : claims)
            count += claim == storage ? 1U : 0U;
        if (count != 1U) return false;
    }
    return true;
}

class FingerprintSink final {
   public:
    FingerprintSink() { append("mmltk.application.schema"); }

    void append(const std::string_view value) noexcept {
        append_number(value.size());
        for (const unsigned char byte : value)
            append_byte(byte);
    }

    template <std::integral Value>
        requires(!std::same_as<Value, bool>)
    void append_number(const Value value) noexcept {
        using Unsigned = std::make_unsigned_t<Value>;
        auto bits = static_cast<Unsigned>(value);
        for (std::size_t index = 0U; index < sizeof(bits); ++index) {
            append_byte(static_cast<std::uint8_t>(bits & 0xffU));
            bits >>= 8U;
        }
    }

    template <std::floating_point Value>
    void append_number(const Value value) noexcept {
        std::array<char, 64U> text{};
        const auto result = std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::hex);
        if (result.ec == std::errc{}) append(std::string_view(text.data(), static_cast<std::size_t>(result.ptr - text.data())));
    }

    void append_number(const bool value) noexcept { append_byte(value ? 1U : 0U); }

    [[nodiscard]] std::array<std::uint64_t, 2U> words() const noexcept { return words_; }

   private:
    void append_byte(const std::uint8_t value) noexcept {
        words_[0] = (words_[0] ^ value) * 1099511628211ULL;
        words_[1] = (words_[1] ^ value) * 1099511628211ULL;
    }

    std::array<std::uint64_t, 2U> words_{14695981039346656037ULL, 7809847782465536322ULL};
};

inline void append_constraint(FingerprintSink& sink, const mmltk::frameworks::reflection::FieldConstraint& constraint) {
    sink.append_number(constraint.finite);
    sink.append_number(constraint.has_minimum);
    sink.append_number(constraint.has_maximum);
    sink.append_number(constraint.minimum);
    sink.append_number(constraint.maximum);
    sink.append_number(constraint.minimum_bytes);
    sink.append_number(constraint.maximum_bytes);
    sink.append_number(constraint.maximum_items);
}

template <class Value>
void append_value(FingerprintSink& sink, const Value& value) {
    using Type = std::remove_cvref_t<Value>;
    if constexpr (std::same_as<Type, bool> || std::is_arithmetic_v<Type>) {
        sink.append_number(value);
    } else if constexpr (std::is_enum_v<Type>) {
        sink.append_number(static_cast<std::underlying_type_t<Type>>(value));
    } else if constexpr (std::same_as<Type, std::string> || std::same_as<Type, std::string_view>) {
        sink.append(value);
    } else if constexpr (std::same_as<Type, std::filesystem::path>) {
        sink.append(value.native());
    } else if constexpr (Optional<Type>::value) {
        sink.append_number(value.has_value());
        if (value) append_value(sink, *value);
    } else if constexpr (Sequence<Type>::value) {
        sink.append_number(value.size());
        for (const auto& item : value)
            append_value(sink, item);
    } else if constexpr (Variant<Type>::value) {
        sink.append_number(value.index());
        std::visit([&](const auto& item) { append_value(sink, item); }, value);
    } else if constexpr (ReflectedObject<Type>) {
        auto visitor = [&]<class Owner, class Declaration>(const auto& fact) {
            sink.append(fact.member_name);
            append_value(sink, value.*Declaration::pointer);
        };
        visit_fields<Type>(visitor);
    } else {
        static_assert(!sizeof(Type), "unsupported reflected catalog row field type");
    }
}

template <class Kind>
    requires std::is_enum_v<Kind>
void append_integration_command_policy(FingerprintSink& sink) {
    sink.append("integration-command-policy");
    sink.append_number(mmltk::frameworks::reflection::kReflectedEnumEntries<Kind>.size());
    mmltk::controller::contracts::visit_integration_commands<Kind>([&]<auto Value, auto Policy>(const auto name) {
        sink.append(name);
        append_value(sink, Value);
        append_value(sink, Policy);
    });
}

inline void append_wire_value(FingerprintSink& sink, const mmltk::frameworks::serialization::wire::Value& value) {
    std::visit(
        [&](const auto& storage) {
            using Type = std::remove_cvref_t<decltype(storage)>;
            if constexpr (std::same_as<Type, std::monostate>) {
                sink.append("null");
                return;
            } else if constexpr (std::same_as<Type, bool>) {
                sink.append("boolean");
                sink.append_number(storage);
            } else if constexpr (std::is_arithmetic_v<Type>) {
                sink.append(std::is_floating_point_v<Type> ? "floating" : "integer");
                sink.append_number(sizeof(Type));
                sink.append_number(std::is_signed_v<Type>);
                sink.append_number(storage);
            } else if constexpr (std::same_as<Type, std::string>) {
                sink.append("text");
                sink.append(storage);
            } else if constexpr (std::same_as<Type, mmltk::frameworks::serialization::wire::Value::Bytes>) {
                sink.append("bytes");
                sink.append_number(storage.size());
                for (const std::byte byte : storage)
                    sink.append_number(std::to_integer<std::uint8_t>(byte));
            } else if constexpr (std::same_as<Type, mmltk::frameworks::serialization::wire::Value::Array>) {
                sink.append("array");
                sink.append_number(storage.size());
                for (const auto& item : storage)
                    append_wire_value(sink, item);
            } else {
                static_assert(std::same_as<Type, mmltk::frameworks::serialization::wire::Value::Object>);
                sink.append("object");
                sink.append_number(storage.size());
                for (const auto& [name, item] : storage) {
                    sink.append(name);
                    append_wire_value(sink, item);
                }
            }
        },
        value.storage);
}

template <class Annotation>
void append_annotation(FingerprintSink& sink, const Annotation& annotation) {
    using A = std::remove_cvref_t<Annotation>;
    static_assert(std::meta::has_identifier(^^A), "wire annotations require canonical declaration identifiers");
    sink.append(std::meta::identifier_of(^^A));
    if constexpr (mmltk::frameworks::reflection::MinimumAnnotation<A>::value ||
                  mmltk::frameworks::reflection::MaximumAnnotation<A>::value ||
                  mmltk::frameworks::reflection::MinBytesAnnotation<A>::value ||
                  mmltk::frameworks::reflection::MaxBytesAnnotation<A>::value ||
                  mmltk::frameworks::reflection::MaxItemsAnnotation<A>::value) {
        sink.append_number(annotation.value);
    } else if constexpr (mmltk::frameworks::reflection::PresentationAnnotation<A>::value) {
        sink.append_number(static_cast<std::uint8_t>(A::kind));
    } else if constexpr (mmltk::controller::contracts::reflection::is_feature_scope_annotation<A>) {
        sink.append_number(annotation.values.size());
        for (const auto feature : annotation.values)
            sink.append_number(static_cast<std::uint8_t>(feature));
    } else if constexpr (mmltk::controller::contracts::reflection::is_file_dialog_annotation<A>) {
        sink.append(A::title);
        sink.append(A::filter);
        sink.append(A::pattern);
        sink.append_number(static_cast<std::uint8_t>(annotation.mode));
    } else if constexpr (mmltk::frameworks::serialization::is_exact_string_annotation<A>) {
        sink.append(annotation.view());
    } else if constexpr (mmltk::frameworks::serialization::is_default_value_annotation<A>) {
        using Default = std::remove_cvref_t<decltype(A::value)>;
        if constexpr (std::is_arithmetic_v<Default>)
            sink.append_number(A::value);
        else if constexpr (std::is_enum_v<Default>)
            sink.append_number(static_cast<std::underlying_type_t<Default>>(A::value));
        else
            sink.append(std::string_view(A::value));
    } else if constexpr (std::same_as<A, mmltk::controller::contracts::reflection::OperationStateField>) {
        sink.append_number(static_cast<std::uint8_t>(annotation.semantic));
    } else if constexpr (std::same_as<A, mmltk::controller::contracts::reflection::ProgressField>) {
        sink.append_number(static_cast<std::uint8_t>(annotation.semantic));
    } else if constexpr (mmltk::frameworks::reflection::is_catalog_provider_annotation<A>) {
        using Provider = typename A::provider_type;
        static_assert(ProjectableCatalogProvider<Provider>, "catalog provider lacks the typed projection surface");
        static_assert(Provider::valid(), "reflected catalog provider is invalid");
        sink.append(Provider::identity);
    } else if constexpr (mmltk::frameworks::reflection::kFixedTextAnnotation<A>) {
        sink.append_number(annotation.capacity);
        sink.append_number(static_cast<std::uint8_t>(annotation.characters));
    }
}

template <class Declaration>
void append_annotations(FingerprintSink& sink) {
    Declaration::VisitAnnotations([&]<class Annotation>(const Annotation& annotation) { append_annotation(sink, annotation); });
}

template <class Declaration>
void append_request_field_fingerprint(FingerprintSink& sink, const ApplicationRequestFieldFact& field) {
    sink.append("request-field");
    sink.append_number(field.endpoint_id);
    sink.append_number(field.stable_id);
    sink.append(field.name);
    append_constraint(sink, field.constraint);
    sink.append_number(static_cast<std::uint8_t>(field.presentation));
    sink.append_number(field.file_dialog_identity);
    sink.append_number(field.settings_update_values);
    append_annotations<Declaration>(sink);
}

template <class Type>
void append_type_annotations(FingerprintSink& sink) {
    template for (constexpr auto annotation : mmltk::frameworks::reflection::reflected_annotations<^^Type>()) {
        using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        append_annotation(sink, std::meta::extract<Annotation>(annotation));
    }
}

template <class Value>
void append_type(FingerprintSink& sink) {
    using Type = std::remove_cvref_t<Value>;
    static_assert(valid_fixed_text_shape<Type>(),
                  "fixed-text annotation requires matching char bytes, unsigned size, capacity, and policy");
    sink.append("type");
    append_type_annotations<Type>(sink);
    if constexpr (Optional<Type>::value) {
        sink.append("optional");
        append_type<typename Optional<Type>::value_type>(sink);
    } else if constexpr (ByteSequence<Type>::value) {
        sink.append("bytes");
        sink.append_number(StaticArray<Type>::value);
        sink.append_number(ByteSequence<Type>::extent);
    } else if constexpr (Sequence<Type>::value) {
        sink.append("sequence");
        sink.append_number(StaticArray<Type>::value);
        sink.append_number(Sequence<Type>::bounded);
        sink.append_number(Sequence<Type>::extent);
        append_type<typename Sequence<Type>::value_type>(sink);
    } else if constexpr (std::is_enum_v<Type> && !std::same_as<Type, std::byte>) {
        sink.append("enum");
        using Underlying = std::underlying_type_t<Type>;
        sink.append_number(sizeof(Underlying));
        sink.append_number(std::is_signed_v<Underlying>);
        sink.append_number(mmltk::frameworks::reflection::enum_entries<Type>().size());
        for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>()) {
            sink.append(entry.name);
            sink.append_number(static_cast<Underlying>(entry.value));
        }
    } else if constexpr (Variant<Type>::value) {
        sink.append("variant");
        sink.append_number(std::variant_size_v<Type>);
        Variant<Type>::Visit([&]<class Alternative>() {
            static_assert(std::meta::has_identifier(^^Alternative), "wire alternatives require canonical declaration identifiers");
            sink.append(std::meta::identifier_of(^^Alternative));
            append_type<Alternative>(sink);
        });
    } else if constexpr (ReflectedObject<Type> && !std::same_as<Type, std::string> && !std::same_as<Type, std::filesystem::path>) {
        sink.append("object");
        constexpr auto count = [] consteval {
            std::size_t result = 0U;
            auto field = [&]<class, class>(const auto&) { ++result; };
            visit_fields<Type>(field);
            return result;
        }();
        sink.append_number(count);
        auto visitor = [&]<class Owner, class Declaration>(const auto& fact) {
            using Field = typename Declaration::member_type;
            sink.append(fact.member_name);
            append_constraint(sink, fact.constraint);
            sink.append_number(static_cast<std::uint8_t>(fact.presentation));
            append_annotations<Declaration>(sink);
            append_type<Field>(sink);
        };
        visit_fields<Type>(visitor);
    } else if constexpr (std::same_as<Type, mmltk::frameworks::serialization::wire::Value> ||
                         std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue>) {
        sink.append(std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue> ? "flat-dynamic" : "dynamic");
    } else if constexpr (std::same_as<Type, std::string_view>) {
        sink.append("static-string");
    } else if constexpr (std::same_as<Type, std::string> || std::same_as<Type, std::filesystem::path>) {
        sink.append("text");
    } else if constexpr (std::same_as<Type, bool>) {
        sink.append("boolean");
    } else if constexpr (std::is_arithmetic_v<Type> || std::same_as<Type, std::byte>) {
        sink.append(std::is_floating_point_v<Type> ? "floating" : "integer");
        sink.append_number(sizeof(Type));
        sink.append_number(std::is_signed_v<Type>);
    } else {
        static_assert(!sizeof(Type), "unsupported reachable application boundary type");
    }
    sink.append("end-type");
}

}  // namespace application_schema_detail

template <class Composition, std::meta::info Member>
struct ReflectedSystem final {
    using member_type = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
    static_assert(std::is_pointer_v<member_type>, "ApplicationSystems members must be non-owning ordinary-system pointers");
    using type = std::remove_pointer_t<member_type>;
    static constexpr std::meta::info member = Member;
    static constexpr auto pointer = &[:Member:];
    static constexpr std::string_view name = std::meta::identifier_of(Member);
    static constexpr std::uint64_t stable_id = application_stable_id(name);
};

template <class Composition, auto Member>
[[nodiscard]] consteval std::meta::info application_system_member() {
    constexpr std::meta::info target = std::meta::reflect_constant(Member);
    template for (constexpr std::meta::info cell :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::reflect_constant(&[:cell:]) == target) return cell;
    }
    throw "event sink must name a reflected composition member";
}

template <class Composition, auto Member>
[[nodiscard]] consteval std::uint64_t application_system_stable_id() {
    return ReflectedSystem<Composition, application_system_member<Composition, Member>()>::stable_id;
}

template <auto Member>
[[nodiscard]] consteval std::uint64_t application_system_stable_id() {
    return application_system_stable_id<mmltk::controller::ApplicationSystems, Member>();
}

template <class Composition, auto Member, class Event>
struct ApplicationEventIdentity final {
    using system_cell = ReflectedSystem<Composition, application_system_member<Composition, Member>()>;
    static constexpr std::uint64_t system_id = system_cell::stable_id;
    static_assert(std::meta::has_identifier(^^Event), "application events require canonical declaration identifiers");
    static constexpr std::uint64_t event_id = application_stable_id(system_cell::name, std::meta::identifier_of(^^Event));
};

template <class Composition, auto Member, class Event>
[[nodiscard]] consteval bool application_event_is_member() {
    using Cell = typename ApplicationEventIdentity<Composition, Member, Event>::system_cell;
    bool found = false;
    application_schema_detail::Variant<typename Cell::type::event_type>::Visit(
        [&]<class Candidate>() { found = found || std::same_as<Candidate, Event>; });
    return found;
}

template <class Composition, auto Member, class Event>
struct ApplicationEventDescriptor final {
    using identity = ApplicationEventIdentity<Composition, Member, Event>;
    using system_cell = typename identity::system_cell;
    static_assert(application_event_is_member<Composition, Member, Event>(), "event must belong to the selected system event variant");
    static_assert(application_schema_detail::annotation_count<^^Event, contracts::reflection::Event>() == 1U,
                  "event requires exactly one delivery annotation");
    static constexpr auto metadata = application_schema_detail::annotation_value<^^Event, contracts::reflection::Event>();
    static constexpr auto delivery = metadata.delivery;
    static_assert(mmltk::frameworks::reflection::enum_contains(delivery), "invalid event delivery");
    static_assert(application_schema_detail::runtime_boundary_projectable<Event>(),
                  "event record contains an unsupported or unreflected reachable type");
    static_assert(
        delivery != contracts::reflection::EventDelivery::LatestState || requires(const Event& event) {
            { event.snapshot.revision } -> std::same_as<const std::uint64_t&>;
        }, "LatestState requires the canonical state snapshot revision");
    static constexpr auto system_id = identity::system_id;
    static constexpr auto event_id = identity::event_id;
    [[nodiscard]] static constexpr std::uint64_t StateRevision(const Event& event) noexcept {
        if constexpr (delivery == contracts::reflection::EventDelivery::LatestState) { return event.snapshot.revision; }
        return 0U;
    }
};

namespace application_schema_detail {
template <class Composition, auto Member, class Visitor>
struct EventVisitor final {
    Visitor& visitor;
    template <class Event>
    constexpr void operator()() const {
        constexpr bool published = annotation_count<^^Event, contracts::reflection::Event>() != 0U;
        if constexpr (published) {
            using Identity = ApplicationEventDescriptor<Composition, Member, Event>;
            visitor.template operator()<Identity, Event>(Identity::metadata);
        }
    }
};
}  // namespace application_schema_detail

template <class SystemCell, std::meta::info Method>
struct ReflectedEndpoint final {
    static_assert(application_schema_detail::SupportedEndpointMethod<Method>,
                  "annotated endpoint must be a public non-static method with zero or one by-value request and "
                  "supported cv/noexcept qualifiers");
    using system_cell = SystemCell;
    using system_type = typename SystemCell::type;
    static constexpr auto method = &[:Method:];
    using signature = SystemMethodSignature<std::remove_cv_t<decltype(method)>>;
    using request_type = typename signature::request_type;
    using result_type = typename signature::result_type;
    static constexpr std::string_view name = std::meta::identifier_of(Method);
    static constexpr std::uint64_t stable_id = application_stable_id(SystemCell::name, name);
    static constexpr bool interaction = application_schema_detail::is_interaction_method<Method>();
    static constexpr bool replaceable = [] consteval {
        if constexpr (interaction)
            return application_schema_detail::annotation_value<Method, contracts::reflection::direct::InteractionEndpoint>().replaceable;
        return false;
    }();
    static_assert(signature::request_by_value && signature::result_by_value, "annotated endpoint request and result must be values");
    static_assert(application_schema_detail::runtime_boundary_projectable<request_type>(),
                  "annotated endpoint request contains an unsupported or unreflected reachable type");
    static_assert(application_schema_detail::runtime_boundary_projectable<result_type>(),
                  "annotated endpoint result contains an unsupported or unreflected reachable type");
    static_assert(!interaction || mmltk::frameworks::serialization::compact_shape<request_type> !=
                                      mmltk::frameworks::serialization::CompactShape::Unsupported,
                  "interaction request has no compact wire projection");
    static_assert(!interaction || (signature::has_request && std::is_void_v<result_type>),
                  "InteractionEndpoint requires one typed request and no reply");
};

template <class... Endpoint>
struct ApplicationEndpointSurface final {
    template <class Visitor>
    static constexpr void Visit(Visitor&& visitor) {
        (visitor.template operator()<Endpoint>(), ...);
    }
    static constexpr std::size_t count = sizeof...(Endpoint);
    [[nodiscard]] static consteval bool HasUniqueStableIds() {
        constexpr std::array values{Endpoint::stable_id...};
        for (std::size_t left = 0U; left < values.size(); ++left) {
            if (values[left] == 0U) return false;
            for (std::size_t right = left + 1U; right < values.size(); ++right)
                if (values[left] == values[right]) return false;
        }
        return true;
    }
};

template <class Composition>
[[nodiscard]] consteval std::meta::info endpoint_surface_info() {
    std::vector<std::meta::info> endpoints;
    template for (constexpr auto cell :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
        using SystemCell = ReflectedSystem<Composition, cell>;
        auto append = [&]<std::meta::info method>() { endpoints.push_back(^^ReflectedEndpoint<SystemCell, method>); };
        application_schema_detail::visit_endpoint_members<typename SystemCell::type>(append);
    }
    return std::meta::substitute(^^ApplicationEndpointSurface, endpoints);
}

template <class Composition>
using ApplicationIntentSurface = typename[:endpoint_surface_info<Composition>():];

namespace application_schema_detail {

template <class>
struct MemberObjectPointer;

template <class Value, class Owner>
struct MemberObjectPointer<Value Owner::*> final {
    using value_type = Value;
    using owner_type = Owner;
};

template <class System>
concept SettingsSurfaceCarrier = requires {
    typename System::settings_surface;
    System::settings_surface::snapshot_member;
    System::settings_surface::defaults_factory;
};

template <class Composition>
// CPD-OFF: This selects one settings-system type; snapshot discovery selects one annotated method.
[[nodiscard]] consteval std::meta::info settings_surface_system_info() {
    std::meta::info selected = ^^void;
    std::size_t count = 0U;
    template for (constexpr auto cell :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
        using System = typename ReflectedSystem<Composition, cell>::type;
        // CPD-ON
        if constexpr (SettingsSurfaceCarrier<System>) {
            selected = ^^System;
            ++count;
        }
    }
    return count == 1U ? selected : ^^void;
}

template <class Composition>
using settings_surface_system = typename[:settings_surface_system_info<Composition>():];

template <class System, bool = SettingsSurfaceCarrier<System>>
struct SettingsSurfaceDescriptor final {
    using settings_type = void;
};

template <class System>
struct SettingsSurfaceDescriptor<System, true> final {
    using surface = typename System::settings_surface;
    using member_pointer = decltype(surface::snapshot_member);
    using member = MemberObjectPointer<std::remove_cv_t<member_pointer>>;
    using settings_type = typename member::value_type;
    using snapshot_type = typename member::owner_type;
};

template <class Composition>
using CompositionSettingsSurface = SettingsSurfaceDescriptor<settings_surface_system<Composition>>;

}  // namespace application_schema_detail

template <class Composition>
[[nodiscard]] consteval bool application_settings_surface_is_valid() {
    using System = application_schema_detail::settings_surface_system<Composition>;
    if constexpr (std::same_as<System, void>) {
        return false;
    } else {
        using Descriptor = application_schema_detail::CompositionSettingsSurface<Composition>;
        using Surface = typename Descriptor::surface;
        using Settings = typename Descriptor::settings_type;
        using Snapshot = typename Descriptor::snapshot_type;
        if constexpr (!application_schema_detail::ReflectedObject<Settings> || !std::invocable<decltype(Surface::defaults_factory)> ||
                      !std::same_as<std::invoke_result_t<decltype(Surface::defaults_factory)>, Settings>) {
            return false;
        } else {
            constexpr auto snapshot = application_schema_detail::snapshot_member<System>();
            using Signature = SystemMethodSignature<decltype(&[:snapshot:])>;
            return std::same_as<typename Signature::result_type, Snapshot>;
        }
    }
}

template <class Composition>
struct ApplicationSchema final {
    using settings_type = typename application_schema_detail::CompositionSettingsSurface<Composition>::settings_type;

    template <class Visitor>
    static constexpr void VisitVisualSources(Visitor&& visitor) {
        VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
            using System = typename SystemCell::type;
            if constexpr (requires { typename System::visual_source; }) {
                using Projection = typename System::visual_source;
                using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
                static_assert(Projection::valid(), "malformed visual producer descriptor");
                static_assert(std::same_as<typename Projection::snapshot_type, typename Signature::result_type>,
                              "visual projection must name the system snapshot");
                static_assert(
                    requires(const System& system) {
                        { system.BorrowFrame() } -> std::same_as<mmltk::frameworks::gpu::BorrowedImageProductReadView>;
                        { system.BorrowWorkspace() } -> std::same_as<mmltk::frameworks::gpu::BorrowedImageWorkspace>;
                        { system.ObserveWorkspace() } -> std::same_as<mmltk::frameworks::gpu::ImageWorkspaceObservation>;
                    }, "visual producer must expose borrowed-product access");
                static_assert(
                    requires(System& system, VisualWorkspaceRequest request) {
                        { system.RequestWorkspace(std::move(request)) } -> std::same_as<void>;
                    }, "visual producer must service workspace requests on its owner");
                visitor.template operator()<SystemCell, Snapshot, Projection>();
            }
        });
    }

    [[nodiscard]] static consteval bool VisualSourcesAreUnique() {
        std::array<bool, presentation_source_metadata.size()> seen{};
        bool unique = true;
        VisitVisualSources([&]<class, std::meta::info, class Projection>() {
            std::size_t index = 0U;
            for (; index < presentation_source_metadata.size(); ++index)
                if (presentation_source_metadata[index].kind == Projection::kind) break;
            if (index == seen.size() || seen[index]) {
                unique = false;
                return;
            }
            seen[index] = true;
        });
        return unique;
    }

    [[nodiscard]] static consteval std::size_t VisualSourceCount() {
        static_assert(VisualSourcesAreUnique(), "visual producer source kinds must be unique");
        std::size_t count = 0U;
        VisitVisualSources([&]<class, std::meta::info, class>() { ++count; });
        return count;
    }

    template <class Visitor>
    static constexpr void VisitSystems(Visitor&& visitor) {
        template for (constexpr auto cell : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
            using SystemCell = ReflectedSystem<Composition, cell>;
            constexpr auto snapshot = application_schema_detail::snapshot_member<typename SystemCell::type>();
            constexpr auto metadata =
                application_schema_detail::annotation_value<snapshot, mmltk::controller::contracts::reflection::Snapshot>();
            static_assert(metadata.byte_budget != 0U, "Snapshot annotation requires a positive byte budget");
            static_assert(application_schema_detail::SupportedSnapshotMethod<snapshot>,
                          "Snapshot annotation requires a supported zero-argument, unqualified or const, "
                          "optionally noexcept method with a non-void value result");
            if constexpr (application_schema_detail::SupportedSnapshotMethod<snapshot>) {
                using Signature = SystemMethodSignature<decltype(&[:snapshot:])>;
                static_assert(application_schema_detail::runtime_boundary_projectable<typename Signature::result_type>(),
                              "snapshot contains an unsupported or unreflected reachable type");
                visitor.template operator()<SystemCell, snapshot>();
            }
        }
    }

    template <class Visitor>
    static void VisitSnapshotDefaults(Visitor&& visitor) {
        VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
            using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
            using Value = typename Signature::result_type;
            static_assert(std::default_initializable<Value>, "reflected snapshot must expose native defaults");
            visitor.template operator()<SystemCell, Value>(Value{});
        });
    }

    [[nodiscard]] static consteval std::size_t BootstrapPayloadBudget() {
        std::size_t bytes = 0U;
        std::size_t count = 0U;
        VisitSystems([&]<class, std::meta::info Snapshot>() {
            constexpr auto metadata =
                application_schema_detail::annotation_value<Snapshot, mmltk::controller::contracts::reflection::Snapshot>();
            static_assert(metadata.byte_budget <= kMaxOutputValueBytes, "snapshot exceeds output value admission ceiling");
            bytes = mmltk::frameworks::serialization::implementation::detail::cbor_size_add(bytes, metadata.byte_budget);
            ++count;
        });
        if (count > kMaxSnapshotCount) throw "application snapshot count exceeds bootstrap capacity";
        return bytes;
    }

    template <class Visitor>
    static constexpr void VisitEndpoints(Visitor&& visitor) {
        static_assert(ApplicationIntentSurface<Composition>::HasUniqueStableIds(), "reflected endpoint identities collide");
        ApplicationIntentSurface<Composition>::Visit(std::forward<Visitor>(visitor));
    }

    template <class Visitor>
    static constexpr void VisitEvents(Visitor&& visitor) {
        template for (constexpr auto cell : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
            using SystemCell = ReflectedSystem<Composition, cell>;
            static_assert(application_schema_detail::ReflectedEventVariant<typename SystemCell::type>,
                          "ordinary system event_type must be one reflected std::variant");
            // clang-format off: the formatter splits the C++26 splice tokens in this template argument.
            application_schema_detail::Variant<typename SystemCell::type::event_type>::Visit(
                application_schema_detail::EventVisitor<Composition, &[:cell:], std::remove_reference_t<Visitor>>{visitor});
            // clang-format on
        }
    }

    template <class Value, class Visitor>
    static constexpr void VisitFields(Visitor&& visitor) {
        application_schema_detail::visit_fields<Value>(visitor);
    }

    template <class Endpoint, class Declaration, class Policy>
    [[nodiscard]] static constexpr ApplicationRequestFieldFact RequestFieldFact(const Policy& policy) {
        constexpr bool file_dialog_identity = application_schema_detail::declaration_has_annotation<
            Declaration, mmltk::controller::contracts::reflection::direct::FileDialogFieldIdentity>();
        constexpr bool settings_update_values =
            application_schema_detail::declaration_has_annotation<Declaration,
                                                                  mmltk::controller::contracts::reflection::direct::SettingsUpdateValues>();
        return {
            .endpoint_id = Endpoint::stable_id,
            .stable_id = application_field_stable_id(Endpoint::stable_id, policy.member_name),
            .name = policy.member_name,
            .constraint = policy.constraint,
            .presentation = policy.presentation,
            .file_dialog_identity = file_dialog_identity,
            .settings_update_values = settings_update_values,
        };
    }

    template <class Endpoint, class Visitor>
    static constexpr void VisitRequestFields(Visitor&& visitor) {
        if constexpr (Endpoint::signature::has_request) {
            auto field_visitor = [&]<class Owner, class Declaration>(const auto& policy) {
                visitor.template operator()<Owner, Declaration>(RequestFieldFact<Endpoint, Declaration>(policy));
            };
            application_schema_detail::visit_fields<typename Endpoint::request_type>(field_visitor);
        }
    }

    template <class Visitor>
    static constexpr void VisitRequestFields(Visitor&& visitor) {
        VisitEndpoints([&]<class Endpoint>() { VisitRequestFields<Endpoint>(visitor); });
    }

    template <class Endpoint, class Visitor>
    static void VisitRequestDefaults(Visitor&& visitor) {
        if constexpr (Endpoint::signature::has_request) {
            using Request = typename Endpoint::request_type;
            static_assert(std::default_initializable<Request>, "reflected request must expose native defaults");
            const Request defaults{};
            auto field_visitor = [&]<class Owner, class Declaration>(const auto& policy) {
                using Member = typename Declaration::member_type;
                visitor.template operator()<Owner, Declaration, Member>(RequestFieldFact<Endpoint, Declaration>(policy),
                                                                        static_cast<const Owner&>(defaults).*Declaration::pointer);
            };
            application_schema_detail::visit_fields<Request>(field_visitor);
        }
    }

    template <class Visitor>
    static void VisitRequestDefaults(Visitor&& visitor) {
        VisitEndpoints([&]<class Endpoint>() { VisitRequestDefaults<Endpoint>(visitor); });
    }

    template <class Settings, class Visitor>
    static void VisitSettingsLeaves(Visitor&& visitor) {
        application_schema_detail::visit_settings_leaves<Settings, Settings>(visitor, {},
                                                                             application_schema_detail::all_feature_workflows());
    }

    [[nodiscard]] static settings_type SettingsDefaults()
        requires(application_settings_surface_is_valid<Composition>())
    {
        using Surface = typename application_schema_detail::CompositionSettingsSurface<Composition>::surface;
        return Surface::defaults_factory();
    }

    template <class Visitor>
    static void VisitApplicationSettingsLeaves(Visitor&& visitor)
        requires(application_settings_surface_is_valid<Composition>())
    {
        VisitSettingsLeaves<settings_type>(std::forward<Visitor>(visitor));
    }

    template <class Visitor>
    static void VisitApplicationSettingsDefaults(Visitor&& visitor)
        requires(application_settings_surface_is_valid<Composition>())
    {
        const auto defaults = SettingsDefaults();
        VisitSettingsDefaults<settings_type>(defaults, std::forward<Visitor>(visitor));
    }

    template <class Settings, class Visitor>
    static void VisitSettingsDefaults(const Settings& defaults, Visitor&& visitor) {
        application_schema_detail::visit_settings_defaults<Settings>(visitor, defaults, {},
                                                                     application_schema_detail::all_feature_workflows());
    }

    template <class Settings, class Visitor>
    static constexpr void VisitSettingsRelations(Visitor&& visitor) {
        static_assert(application_schema_detail::settings_relations_are_valid<Settings>(),
                      "settings relations must resolve uniquely to mutable canonical leaves and claim opaque storage once");
        application_schema_detail::visit_settings_relations<Settings, Settings>(visitor);
    }

    template <class Provider, class Visitor>
    static void VisitCatalogRows(Visitor&& visitor) {
        application_schema_detail::validate_catalog_provider<Provider>();
        using Row = typename Provider::row_type;
        constexpr std::string_view provider_identity = Provider::identity;
        if (provider_identity.empty()) throw std::logic_error("catalog provider identity must not be empty");
        const std::uint64_t provider_id = application_stable_id(provider_identity);
        std::map<std::uint64_t, std::string> row_identities;
        std::set<std::string> row_keys;
        Provider::VisitRows([&](const Row& row, const std::size_t index) {
            const std::string_view key = Provider::row_key(row);
            if (key.empty()) throw std::logic_error("catalog provider emitted an empty row key");
            if (!row_keys.emplace(key).second)
                throw std::logic_error("catalog provider emitted duplicate row key `" + std::string(key) + "`");
            const std::uint64_t stable_id = application_stable_id(provider_identity, key);
            const auto [prior, inserted] = row_identities.emplace(stable_id, std::string(key));
            if (!inserted)
                throw std::logic_error("catalog row identity collision between `" + prior->second + "` and `" + std::string(key) + "`");
            visitor.template operator()<Provider, Row>(
                ApplicationCatalogRowFact{
                    .provider_id = provider_id,
                    .stable_id = stable_id,
                    .key = key,
                    .index = index,
                },
                row);
        });
    }

    template <class Visitor>
    static void VisitCatalogProviders(Visitor&& visitor) {
        std::set<std::string> seen_types;
        std::map<std::uint64_t, std::string> providers;
        auto visit_provider = [&]<class Provider>() {
            application_schema_detail::validate_catalog_provider<Provider>();
            using Row = typename Provider::row_type;
            static_assert(std::meta::has_identifier(^^Provider) && std::meta::has_identifier(^^Row),
                          "catalog providers and rows require canonical declaration identifiers");
            constexpr std::string_view name = std::meta::identifier_of(^^Provider);
            constexpr std::string_view identity = Provider::identity;
            constexpr std::string_view row_type = mmltk::frameworks::reflection::type_name<Row>();
            if (identity.empty()) throw std::logic_error("catalog provider identity must not be empty");
            const std::uint64_t stable_id = application_stable_id(identity);
            const auto [prior, inserted] = providers.emplace(stable_id, std::string(name));
            if (!inserted) {
                if (prior->second != name)
                    throw std::logic_error("catalog provider identity collision between `" + prior->second + "` and `" + std::string(name) +
                                           "`");
                return;
            }
            visitor.template operator()<Provider, Row>(ApplicationCatalogProviderFact{
                .name = name,
                .identity = identity,
                .row_type = row_type,
                .stable_id = stable_id,
            });
        };
        const auto visit_type = [&]<class Type>() {
            application_schema_detail::visit_type_catalog_providers<Type>(visit_provider, seen_types);
        };
        static_assert(application_settings_surface_is_valid<Composition>(),
                      "application settings and catalog APIs require exactly one valid settings surface");
        visit_type.template operator()<settings_type>();
        VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
            using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
            visit_type.template operator()<typename Signature::result_type>();
        });
        VisitEndpoints([&]<class Endpoint>() {
            if constexpr (Endpoint::signature::has_request) visit_type.template operator()<typename Endpoint::request_type>();
            if constexpr (!std::is_void_v<typename Endpoint::result_type>) visit_type.template operator()<typename Endpoint::result_type>();
        });
        VisitEvents([&]<class Identity, class Event>(const auto&) { visit_type.template operator()<Event>(); });
    }
};

struct ApplicationSchemaFingerprint final {
    std::array<std::uint64_t, 2U> words{};
    constexpr bool operator==(const ApplicationSchemaFingerprint&) const noexcept = default;
};

template <class Composition, class Visitor>
void visit_interaction_opcodes(Visitor&& visitor) {
    std::uint64_t opcode = 0U;
    ApplicationSchema<Composition>::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (Endpoint::interaction) visitor.template operator()<Endpoint>(opcode++);
    });
}

namespace application_schema_detail {

template <class Composition>
[[nodiscard]] FingerprintSink application_schema_sink() {
    FingerprintSink sink;
    std::map<std::uint64_t, std::string> identities;
    const auto reserve_identity = [&identities](const std::uint64_t identity, const std::string& source) {
        if (identity == 0U) throw std::logic_error("application schema emitted a zero identity for " + source);
        const auto [prior, inserted] = identities.emplace(identity, source);
        if (!inserted) throw std::logic_error("application stable identity collision between " + prior->second + " and " + source);
    };
    sink.append_number(kBrowserProtocolVersion);
    sink.append("positional-output-objects-v1");
    sink.append("compact-numeric-interactions");
    application_schema_detail::append_type<CompactInteraction>(sink);
    visit_interaction_opcodes<Composition>([&]<class Endpoint>(const auto opcode) {
        sink.append_number(opcode);
        sink.append_number(Endpoint::stable_id);
    });
    application_schema_detail::append_type<ClientRecord>(sink);
    application_schema_detail::append_type<ServerRecord>(sink);
    application_schema_detail::append_type<WorkspaceImageMetadata>(sink);
    sink.append("visual-source-projections");
    application_schema_detail::append_type<VisualSourceObservation>(sink);
    application_schema_detail::append_type<VisualCleanContentIdentity>(sink);
    sink.append_number(ApplicationSchema<Composition>::VisualSourceCount());
    for (const auto metadata : presentation_source_metadata) {
        sink.append(mmltk::frameworks::reflection::enum_name(metadata.kind));
        sink.append_number(metadata.session);
    }
    ApplicationSchema<Composition>::VisitVisualSources([&]<class Cell, std::meta::info, class Projection>() {
        application_schema_detail::append_type<typename Projection::image_type>(sink);
        sink.append_number(Cell::stable_id);
        sink.append(mmltk::frameworks::reflection::enum_name(Projection::kind));
        Projection::relation::VisitMembers([&]<class Entry>() {
            constexpr auto source =
                mmltk::frameworks::reflection::reflected_member_path<typename Projection::snapshot_type, Entry::source>();
            constexpr auto destination =
                mmltk::frameworks::reflection::reflected_member_path<VisualSourceObservation, Entry::destination>();
            sink.append(source.view());
            sink.append(destination.view());
        });
    });
    sink.append("visual-clean-content-relation");
    VisualCleanContentRelation::VisitMembers([&]<class Entry>() {
        constexpr auto source = mmltk::frameworks::reflection::reflected_member_path<VisualFrame, Entry::source>();
        constexpr auto destination = mmltk::frameworks::reflection::reflected_member_path<VisualCleanContentIdentity, Entry::destination>();
        sink.append(source.view());
        sink.append(destination.view());
    });
    constexpr auto fallback_source =
        mmltk::frameworks::reflection::reflected_member_path<VisualFrame, VisualCleanContentRelation::zero_fallback_source>();
    constexpr auto fallback_destination =
        mmltk::frameworks::reflection::reflected_member_path<VisualCleanContentIdentity,
                                                             VisualCleanContentRelation::zero_fallback_destination>();
    sink.append("zero-fallback");
    sink.append(fallback_source.view());
    sink.append(fallback_destination.view());
    ApplicationSchema<Composition>::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
        static_assert(!Signature::has_request, "snapshot method must not accept a request");
        constexpr auto metadata =
            application_schema_detail::annotation_value<Snapshot, mmltk::controller::contracts::reflection::Snapshot>();
        static_assert(metadata.byte_budget != 0U, "snapshot requires a positive byte budget");
        reserve_identity(SystemCell::stable_id, "system " + std::string(SystemCell::name));
        sink.append("system");
        sink.append(SystemCell::name);
        sink.append_number(SystemCell::stable_id);
        sink.append_number(metadata.byte_budget);
        application_schema_detail::append_type<typename Signature::result_type>(sink);
        const typename Signature::result_type default_snapshot{};
        auto encoded = mmltk::frameworks::serialization::reflected_value(default_snapshot);
        if (!encoded) throw std::logic_error("unsupported reflected snapshot default");
        sink.append("snapshot-default");
        sink.append_number(SystemCell::stable_id);
        application_schema_detail::append_wire_value(sink, *encoded);
    });
    ApplicationSchema<Composition>::VisitEndpoints([&]<class Endpoint>() {
        const std::string endpoint_source = "endpoint " + std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name);
        reserve_identity(Endpoint::stable_id, endpoint_source);
        sink.append(Endpoint::interaction ? "interaction" : "intent");
        if constexpr (Endpoint::interaction) sink.append_number(Endpoint::replaceable);
        sink.append(Endpoint::system_cell::name);
        sink.append(Endpoint::name);
        sink.append_number(Endpoint::stable_id);
        if constexpr (Endpoint::signature::has_request) {
            ApplicationSchema<Composition>::template VisitRequestFields<Endpoint>(
                [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) {
                    reserve_identity(field.stable_id, endpoint_source + "." + std::string(field.name));
                    application_schema_detail::append_request_field_fingerprint<Declaration>(sink, field);
                });
            ApplicationSchema<Composition>::template VisitRequestDefaults<Endpoint>(
                [&]<class Owner, class Declaration, class Member>(const ApplicationRequestFieldFact& field, const Member& value) {
                    sink.append("request-default");
                    sink.append_number(field.endpoint_id);
                    sink.append_number(field.stable_id);
                    sink.append(field.name);
                    auto encoded = mmltk::frameworks::serialization::reflected_value(value);
                    if (!encoded) throw std::logic_error("unsupported reflected request default");
                    application_schema_detail::append_wire_value(sink, *encoded);
                });
            application_schema_detail::append_type<typename Endpoint::request_type>(sink);
        }
        if constexpr (!std::is_void_v<typename Endpoint::result_type>)
            application_schema_detail::append_type<typename Endpoint::result_type>(sink);
    });
    ApplicationSchema<Composition>::VisitEvents(
        [&]<class Identity, class Event>(const mmltk::controller::contracts::reflection::Event metadata) {
            reserve_identity(Identity::event_id, "event " + std::string(Identity::system_cell::name) + "." +
                                                     std::string(mmltk::frameworks::reflection::type_name<Event>()));
            sink.append("event");
            sink.append_number(Identity::system_id);
            sink.append_number(Identity::event_id);
            sink.append_number(static_cast<std::uint8_t>(metadata.delivery));
            application_schema_detail::append_type<Event>(sink);
        });
    ApplicationSchema<Composition>::VisitApplicationSettingsLeaves(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& field) {
            reserve_identity(field.stable_id, "settings field " + std::string(field.path));
            sink.append("settings-field");
            sink.append(field.path);
            sink.append_number(field.stable_id);
            sink.append_number(field.mutable_leaf);
            sink.append_number(field.workflows.count);
            for (std::size_t index = 0U; index < field.workflows.count; ++index)
                sink.append_number(static_cast<std::uint8_t>(field.workflows.workflows[index]));
            application_schema_detail::append_constraint(sink, field.constraint);
            sink.append_number(static_cast<std::uint8_t>(field.presentation));
            sink.append(field.catalog_provider);
            application_schema_detail::append_annotations<Declaration>(sink);
            if (field.file_dialog) {
                sink.append("file-dialog");
                sink.append(field.file_dialog->title);
                sink.append(field.file_dialog->filter);
                sink.append(field.file_dialog->pattern);
                sink.append_number(static_cast<std::uint8_t>(field.file_dialog->mode));
            }
        });
    ApplicationSchema<Composition>::VisitApplicationSettingsDefaults(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsDefaultFact& fact, const Member& value) {
            sink.append("settings-default");
            sink.append_number(fact.stable_id);
            sink.append(fact.path);
            application_schema_detail::append_value(sink, value);
        });
    ApplicationSchema<Composition>::VisitCatalogProviders([&]<class Provider, class Row>(const ApplicationCatalogProviderFact& provider) {
        reserve_identity(provider.stable_id, "catalog provider " + std::string(provider.name));
        sink.append("catalog-provider");
        sink.append(provider.name);
        sink.append(provider.identity);
        sink.append(provider.row_type);
        sink.append_number(provider.stable_id);
        application_schema_detail::append_type<Row>(sink);
        ApplicationSchema<Composition>::template VisitCatalogRows<Provider>(
            [&]<class ActualProvider, class ActualRow>(const ApplicationCatalogRowFact& fact, const ActualRow& row) {
                reserve_identity(fact.stable_id, "catalog row " + std::string(provider.name) + "." + std::string(fact.key));
                sink.append("catalog-row");
                sink.append_number(fact.provider_id);
                sink.append_number(fact.stable_id);
                sink.append(fact.key);
                sink.append_number(fact.index);
                application_schema_detail::append_value(sink, row);
            });
    });
    return sink;
}

}  // namespace application_schema_detail

template <class Composition>
[[nodiscard]] ApplicationSchemaFingerprint application_schema_fingerprint() {
    static const ApplicationSchemaFingerprint fingerprint = [] {
        auto sink = application_schema_detail::application_schema_sink<Composition>();
        application_schema_detail::append_integration_command_policy<mmltk::controller::contracts::IntegrationControlKind>(sink);
        return ApplicationSchemaFingerprint{sink.words()};
    }();
    return fingerprint;
}

}  // namespace mmltk::controller::browser
