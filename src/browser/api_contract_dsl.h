#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::browser::api {

template <typename... Ts>
struct type_list {
    static constexpr std::size_t size = sizeof...(Ts);
};

template <typename TypeList>
struct type_list_metadata;

template <typename... Ts>
struct unique_type_pack : std::true_type {};

template <typename First, typename... Rest>
struct unique_type_pack<First, Rest...>
    : std::bool_constant<(!(std::is_same_v<First, Rest> || ...) && unique_type_pack<Rest...>::value)> {};

template <typename... Ts>
struct type_list_metadata<type_list<Ts...>> {
    [[nodiscard]] static consteval bool unique_types() {
        return unique_type_pack<Ts...>::value;
    }
};

struct required_t {};
struct optional_t {};
struct additional_properties_t {
    bool enabled = true;
};
struct format_t {
    std::string_view value;
};
struct description_t {
    std::string_view value;
};
struct min_items_t {
    std::size_t value;
};
struct max_items_t {
    std::size_t value;
};

template <typename T>
struct default_value_t {
    T value;
};

template <typename T>
struct minimum_t {
    T value;
};

template <typename T>
struct maximum_t {
    T value;
};

template <typename T>
struct enum_values_t {
    T values;
};

inline constexpr required_t required{};
inline constexpr optional_t optional{};

template <typename T>
[[nodiscard]] constexpr default_value_t<std::decay_t<T>> default_value(T&& value) {
    return default_value_t<std::decay_t<T>>{std::forward<T>(value)};
}

template <typename T>
[[nodiscard]] constexpr minimum_t<T> minimum(T value) {
    return minimum_t<T>{value};
}

template <typename T>
[[nodiscard]] constexpr maximum_t<T> maximum(T value) {
    return maximum_t<T>{value};
}

template <typename T>
[[nodiscard]] constexpr enum_values_t<T> enum_values(T values) {
    return enum_values_t<T>{values};
}

[[nodiscard]] constexpr format_t format(const std::string_view value) {
    return format_t{value};
}

[[nodiscard]] constexpr description_t description(const std::string_view value) {
    return description_t{value};
}

[[nodiscard]] constexpr min_items_t min_items(const std::size_t value) {
    return min_items_t{value};
}

[[nodiscard]] constexpr max_items_t max_items(const std::size_t value) {
    return max_items_t{value};
}

[[nodiscard]] constexpr additional_properties_t additional_properties(const bool enabled) {
    return additional_properties_t{enabled};
}

template <typename MemberPointer>
struct member_pointer_traits;

template <typename Class, typename Member>
struct member_pointer_traits<Member Class::*> {
    using class_type = Class;
    using value_type = Member;
};

template <auto Member>
struct field_descriptor {
    static_assert(std::is_member_object_pointer_v<decltype(Member)>,
                  "api::field descriptors must point to data members");

    static constexpr auto member = Member;
    using class_type = typename member_pointer_traits<decltype(Member)>::class_type;
    using value_type = typename member_pointer_traits<decltype(Member)>::value_type;

    std::string_view json_name;
    bool required = false;
    bool additional_properties = false;
    bool has_minimum = false;
    bool has_maximum = false;
    bool has_min_items = false;
    bool has_max_items = false;
    double minimum = 0.0;
    double maximum = 0.0;
    std::size_t min_items = 0U;
    std::size_t max_items = 0U;
    std::array<std::string_view, 32U> enum_values{};
    std::size_t enum_value_count = 0U;
    std::variant<std::monostate, bool, double, std::string_view> default_value;
    std::string_view format;
    std::string_view description;

    template <typename... Options>
    constexpr explicit field_descriptor(const std::string_view name, Options... options) : json_name(name) {
        (apply_option(options), ...);
    }

   private:
    constexpr void apply_option(default_value_t<bool> value) noexcept {
        default_value = value.value;
    }

    constexpr void apply_option(required_t) noexcept {
        required = true;
    }

    constexpr void apply_option(optional_t) noexcept {
        required = false;
    }

    constexpr void apply_option(additional_properties_t value) noexcept {
        additional_properties = value.enabled;
    }

    constexpr void apply_option(format_t value) noexcept {
        format = value.value;
    }

    constexpr void apply_option(description_t value) noexcept {
        description = value.value;
    }

    constexpr void apply_option(min_items_t value) noexcept {
        has_min_items = true;
        min_items = value.value;
    }

    constexpr void apply_option(max_items_t value) noexcept {
        has_max_items = true;
        max_items = value.value;
    }

    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    constexpr void apply_option(default_value_t<T> value) noexcept {
        default_value = static_cast<double>(value.value);
    }

    template <typename T>
        requires std::is_convertible_v<T, std::string_view>
    constexpr void apply_option(default_value_t<T> value) noexcept {
        default_value = std::string_view{value.value};
    }

    template <typename T>
    constexpr void apply_option(minimum_t<T> value) noexcept {
        has_minimum = true;
        minimum = static_cast<double>(value.value);
    }

    template <typename T>
    constexpr void apply_option(maximum_t<T> value) noexcept {
        has_maximum = true;
        maximum = static_cast<double>(value.value);
    }

    template <typename T>
    constexpr void apply_option(enum_values_t<T> value) noexcept {
        enum_value_count = 0U;
        for (const auto& item : value.values) {
            if (enum_value_count >= enum_values.size()) {
                return;
            }
            if constexpr (std::is_convertible_v<std::decay_t<decltype(item)>, std::string_view>) {
                enum_values[enum_value_count++] = std::string_view{item};
            }
        }
    }
};

template <auto Member, typename... Options>
[[nodiscard]] constexpr auto field(const std::string_view name, Options... options) {
    return field_descriptor<Member>{name, options...};
}

template <typename Enum>
struct enum_value {
    std::string_view name;
    Enum value;
};

template <typename Enum>
struct enum_traits;

template <typename Enum, typename = void>
struct has_enum_traits : std::false_type {};

template <typename Enum>
struct has_enum_traits<Enum, std::void_t<decltype(enum_traits<Enum>::values())>> : std::true_type {};

template <typename Enum>
inline constexpr bool has_enum_traits_v = has_enum_traits<Enum>::value;

template <typename Enum>
[[nodiscard]] constexpr std::string_view enum_name(const Enum value) noexcept {
    for (const auto item : enum_traits<Enum>::values()) {
        if (item.value == value) {
            return item.name;
        }
    }
    return {};
}

template <typename Enum>
[[nodiscard]] Enum enum_from_name(const std::string_view name) {
    for (const auto item : enum_traits<Enum>::values()) {
        if (item.name == name) {
            return item.value;
        }
    }
    throw std::runtime_error("invalid reflected enum value: " + std::string(name));
}

template <typename T, typename = void>
struct is_reflected : std::false_type {};

template <typename T>
struct is_reflected<T, std::void_t<decltype(T::api_fields())>> : std::true_type {};

template <typename T>
inline constexpr bool is_reflected_v = is_reflected<T>::value;

template <typename T, typename = void>
struct has_type_additional_properties : std::false_type {};

template <typename T>
struct has_type_additional_properties<T, std::void_t<decltype(T::api_additional_properties())>> : std::true_type {};

template <typename T>
inline constexpr bool has_type_additional_properties_v = has_type_additional_properties<T>::value;

template <typename T>
[[nodiscard]] consteval bool type_additional_properties_enabled() {
    if constexpr (has_type_additional_properties_v<T>) {
        return T::api_additional_properties();
    }
    return false;
}

template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template <typename T>
struct is_vector : std::false_type {};

template <typename T, typename Allocator>
struct is_vector<std::vector<T, Allocator>> : std::true_type {
    using value_type = T;
};

template <typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {
    using value_type = T;
};

template <typename T>
inline constexpr bool is_std_array_v = is_std_array<T>::value;

template <typename T>
struct is_variant : std::false_type {};

template <typename... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

template <typename T>
inline constexpr bool is_variant_v = is_variant<T>::value;

template <typename>
inline constexpr bool kUnsupportedApiSchemaType = false;

template <typename T>
void from_json_value(const nlohmann::json& value, T& out);

template <typename T>
void to_json_reflected(nlohmann::json& out, const T& value);

template <typename T>
void from_json_reflected(const nlohmann::json& input, T& out);

template <typename Field>
void validate_field_json_value(const Field& field, const nlohmann::json& value) {
    if (field.has_minimum && value.is_number() && value.get<double>() < field.minimum) {
        throw std::runtime_error("reflected API field `" + std::string(field.json_name) + "` is below minimum");
    }
    if (field.has_maximum && value.is_number() && value.get<double>() > field.maximum) {
        throw std::runtime_error("reflected API field `" + std::string(field.json_name) + "` is above maximum");
    }
    if (field.has_min_items && value.is_array() && value.size() < field.min_items) {
        throw std::runtime_error("reflected API field `" + std::string(field.json_name) + "` has too few items");
    }
    if (field.has_max_items && value.is_array() && value.size() > field.max_items) {
        throw std::runtime_error("reflected API field `" + std::string(field.json_name) + "` has too many items");
    }
    if (field.enum_value_count > 0U && value.is_string()) {
        const std::string candidate = value.get<std::string>();
        for (std::size_t i = 0U; i < field.enum_value_count; ++i) {
            if (field.enum_values[i] == candidate) {
                return;
            }
        }
        throw std::runtime_error("reflected API field `" + std::string(field.json_name) +
                                 "` is not one of the declared enum values");
    }
}

template <typename T>
[[nodiscard]] nlohmann::json to_json_value(const T& value) {
    if constexpr (has_enum_traits_v<T>) {
        return std::string(enum_name(value));
    } else if constexpr (is_optional_v<T>) {
        if (!value.has_value()) {
            return nullptr;
        }
        return to_json_value(*value);
    } else if constexpr (is_vector_v<T>) {
        nlohmann::json out = nlohmann::json::array();
        auto& array = out.get_ref<nlohmann::json::array_t&>();
        array.reserve(value.size());
        for (const auto& entry : value) {
            array.push_back(to_json_value(entry));
        }
        return out;
    } else if constexpr (is_reflected_v<T>) {
        nlohmann::json out;
        to_json_reflected(out, value);
        return out;
    } else {
        return nlohmann::json(value);
    }
}

template <typename T>
void from_json_value(const nlohmann::json& value, T& out) {
    if constexpr (has_enum_traits_v<T>) {
        out = enum_from_name<T>(value.get<std::string>());
    } else if constexpr (is_optional_v<T>) {
        if (value.is_null()) {
            out.reset();
            return;
        }
        typename is_optional<T>::value_type inner{};
        from_json_value(value, inner);
        out = std::move(inner);
    } else if constexpr (is_vector_v<T>) {
        if (!value.is_array()) {
            throw std::runtime_error("reflected API vector field must decode from a JSON array");
        }
        out.clear();
        out.reserve(value.size());
        for (const nlohmann::json& entry : value) {
            typename is_vector<T>::value_type item{};
            from_json_value(entry, item);
            out.push_back(std::move(item));
        }
    } else if constexpr (is_reflected_v<T>) {
        from_json_reflected(value, out);
    } else {
        value.get_to(out);
    }
}

template <typename T>
void to_json_reflected(nlohmann::json& out, const T& value) {
    out = nlohmann::json::object();
    std::apply(
        [&](const auto&... fields) {
            ((out[std::string(fields.json_name)] = to_json_value(value.*(std::decay_t<decltype(fields)>::member))),
             ...);
        },
        T::api_fields());
}

template <typename T>
void from_json_reflected(const nlohmann::json& input, T& out) {
    if (!input.is_object()) {
        throw std::runtime_error("reflected API DTO must decode from a JSON object");
    }
    std::apply(
        [&](const auto&... fields) {
            (([&] {
                 const auto it = input.find(std::string(fields.json_name));
                 if (it == input.end() || it->is_null()) {
                     if (fields.required) {
                         throw std::runtime_error("missing required reflected API field `" +
                                                  std::string(fields.json_name) + "`");
                     }
                     if constexpr (is_optional_v<typename std::decay_t<decltype(fields)>::value_type>) {
                         out.*(std::decay_t<decltype(fields)>::member) = std::nullopt;
                     }
                     return;
                 }
                 validate_field_json_value(fields, *it);
                 from_json_value(*it, out.*(std::decay_t<decltype(fields)>::member));
             }()),
             ...);
        },
        T::api_fields());
}

template <typename T>
void validate_reflected(const nlohmann::json& input) {
    T decoded{};
    from_json_reflected(input, decoded);
}

template <typename T>
[[nodiscard]] nlohmann::json schema_for();

template <typename T>
struct variant_schema_builder;

template <typename... Ts>
struct variant_schema_builder<std::variant<Ts...>> {
    [[nodiscard]] static nlohmann::json schema() {
        nlohmann::json variants = nlohmann::json::array();
        (variants.push_back(schema_for<Ts>()), ...);
        return {{"oneOf", std::move(variants)}};
    }
};

template <typename Field>
[[nodiscard]] nlohmann::json schema_for_field(const Field& field) {
    nlohmann::json schema = schema_for<typename Field::value_type>();
    if (field.additional_properties) {
        if (!schema.is_object() || (!schema.contains("type") && !schema.contains("properties"))) {
            schema = {{"type", "object"}, {"additionalProperties", true}};
        } else {
            schema["additionalProperties"] = true;
        }
    }
    if (field.has_minimum) {
        schema["minimum"] = field.minimum;
    }
    if (field.has_maximum) {
        schema["maximum"] = field.maximum;
    }
    if (field.has_min_items) {
        schema["minItems"] = field.min_items;
    }
    if (field.has_max_items) {
        schema["maxItems"] = field.max_items;
    }
    if (field.enum_value_count > 0U) {
        nlohmann::json values = nlohmann::json::array();
        for (std::size_t i = 0U; i < field.enum_value_count; ++i) {
            values.push_back(std::string(field.enum_values[i]));
        }
        schema["enum"] = std::move(values);
    }
    std::visit(
        [&](const auto& default_value) {
            using Default = std::decay_t<decltype(default_value)>;
            if constexpr (!std::is_same_v<Default, std::monostate>) {
                schema["default"] = default_value;
            }
        },
        field.default_value);
    if (!field.format.empty()) {
        schema["format"] = std::string(field.format);
    }
    if (!field.description.empty()) {
        schema["description"] = std::string(field.description);
    }
    return schema;
}

template <typename T>
[[nodiscard]] nlohmann::json scalar_schema() {
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return {{"type", "string"}};
    } else if constexpr (std::is_same_v<T, bool>) {
        return {{"type", "boolean"}};
    } else if constexpr (std::is_integral_v<T>) {
        return {{"type", "integer"}};
    } else if constexpr (std::is_floating_point_v<T>) {
        return {{"type", "number"}};
    } else if constexpr (std::is_same_v<T, nlohmann::json>) {
        return nlohmann::json::object();
    } else {
        static_assert(kUnsupportedApiSchemaType<T>, "unsupported reflected API schema field type");
    }
}

template <typename T>
[[nodiscard]] nlohmann::json schema_for() {
    if constexpr (has_enum_traits_v<T>) {
        nlohmann::json values = nlohmann::json::array();
        for (const auto item : enum_traits<T>::values()) {
            values.push_back(std::string(item.name));
        }
        return {{"type", "string"}, {"enum", std::move(values)}};
    } else if constexpr (is_optional_v<T>) {
        nlohmann::json variants = nlohmann::json::array();
        variants.push_back(schema_for<typename is_optional<T>::value_type>());
        variants.push_back(nlohmann::json{{"type", "null"}});
        return {{"oneOf", std::move(variants)}};
    } else if constexpr (is_vector_v<T>) {
        return {{"type", "array"}, {"items", schema_for<typename is_vector<T>::value_type>()}};
    } else if constexpr (is_std_array_v<T>) {
        return {{"type", "array"}, {"items", schema_for<typename is_std_array<T>::value_type>()}};
    } else if constexpr (is_variant_v<T>) {
        return variant_schema_builder<T>::schema();
    } else if constexpr (is_reflected_v<T>) {
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required_fields = nlohmann::json::array();
        std::apply(
            [&](const auto&... fields) {
                (([&] {
                     properties[std::string(fields.json_name)] = schema_for_field(fields);
                     if (fields.required) {
                         required_fields.push_back(std::string(fields.json_name));
                     }
                 }()),
                 ...);
            },
            T::api_fields());
        constexpr bool additional_properties = type_additional_properties_enabled<T>();
        nlohmann::json schema = {
            {"type", "object"},
            {"additionalProperties", additional_properties},
            {"properties", properties},
        };
        if (!required_fields.empty()) {
            schema["required"] = std::move(required_fields);
        }
        return schema;
    } else {
        return scalar_schema<T>();
    }
}

template <typename WorkflowEnum, std::size_t N>
struct workflow_set_descriptor {
    std::array<WorkflowEnum, N> workflows;
};

template <typename First, typename... Rest>
[[nodiscard]] constexpr auto workflow_set(First first, Rest... rest) {
    using WorkflowEnum = std::decay_t<First>;
    static_assert((std::is_same_v<WorkflowEnum, std::decay_t<Rest>> && ...),
                  "all workflow_set entries must have the same enum type");
    return workflow_set_descriptor<WorkflowEnum, 1U + sizeof...(Rest)>{
        std::array<WorkflowEnum, 1U + sizeof...(Rest)>{first, rest...},
    };
}

template <typename WorkflowSet>
struct intent_descriptor {
    std::string_view id;
    WorkflowSet workflows;
    std::string_view payload_schema;
};

template <typename WorkflowSet>
[[nodiscard]] constexpr auto intent(const std::string_view id, WorkflowSet workflows,
                                    const std::string_view payload_schema = {}) {
    return intent_descriptor<WorkflowSet>{id, workflows, payload_schema};
}

template <typename T, typename = void>
struct payload_intent_descriptors {
    [[nodiscard]] static constexpr auto values() {
        return std::tuple{T::api_intent()};
    }
};

template <typename T>
struct payload_intent_descriptors<T, std::void_t<decltype(T::api_intents())>> {
    [[nodiscard]] static constexpr auto values() {
        return T::api_intents();
    }
};

template <typename TypeList>
struct intent_metadata;

template <typename... Payloads>
struct intent_metadata<type_list<Payloads...>> {
    [[nodiscard]] static consteval std::size_t intent_count() {
        return (std::tuple_size_v<decltype(payload_intent_descriptors<Payloads>::values())> + ... + 0U);
    }

    [[nodiscard]] static consteval bool valid() {
        return payload_descriptors_are_valid() && intent_ids_are_unique();
    }

    [[nodiscard]] static consteval auto intent_ids() {
        constexpr std::size_t count = intent_count();
        std::array<std::string_view, count> ids{};
        std::size_t index = 0U;
        (append_payload_intent_ids<Payloads>(ids, index), ...);
        return ids;
    }

   private:
    template <typename Payload, std::size_t N>
    static consteval void append_payload_intent_ids(std::array<std::string_view, N>& ids, std::size_t& index) {
        std::apply([&](const auto&... descriptors) { ((ids[index++] = descriptors.id), ...); },
                   payload_intent_descriptors<Payload>::values());
    }

    template <typename Descriptor>
    [[nodiscard]] static consteval bool descriptor_is_valid(const Descriptor& descriptor) {
        if (descriptor.id.empty()) {
            return false;
        }
        for (std::size_t i = 0U; i < descriptor.workflows.workflows.size(); ++i) {
            if (enum_name(descriptor.workflows.workflows[i]).empty()) {
                return false;
            }
            for (std::size_t j = i + 1U; j < descriptor.workflows.workflows.size(); ++j) {
                if (descriptor.workflows.workflows[i] == descriptor.workflows.workflows[j]) {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename Payload>
    [[nodiscard]] static consteval bool payload_descriptor_is_valid() {
        bool valid_payload = true;
        std::apply(
            [&](const auto&... descriptors) {
                ((valid_payload = valid_payload && descriptor_is_valid(descriptors)), ...);
            },
            payload_intent_descriptors<Payload>::values());
        return valid_payload;
    }

    [[nodiscard]] static consteval bool payload_descriptors_are_valid() {
        return (payload_descriptor_is_valid<Payloads>() && ...);
    }

    [[nodiscard]] static consteval bool intent_ids_are_unique() {
        constexpr auto ids = intent_ids();
        for (std::size_t i = 0U; i < ids.size(); ++i) {
            for (std::size_t j = i + 1U; j < ids.size(); ++j) {
                if (ids[i] == ids[j]) {
                    return false;
                }
            }
        }
        return true;
    }
};

template <typename TypeList>
struct factory;

template <typename... Payloads>
struct factory<type_list<Payloads...>> {
    using payload_variant = std::variant<Payloads...>;

    template <typename Variant, typename Handler>
    static decltype(auto) dispatch(Variant&& payload, Handler&& handler) {
        return std::visit(
            [&](auto&& typed_payload) -> decltype(auto) {
                return std::forward<Handler>(handler)(std::forward<decltype(typed_payload)>(typed_payload));
            },
            std::forward<Variant>(payload));
    }
};

}  
