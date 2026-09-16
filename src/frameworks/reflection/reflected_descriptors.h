#pragma once
#include <array>
#include <bitset>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <meta>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include "mmltk/frameworks/reflection/member_path.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
// Host-only command descriptors.  A command owns a static array of these
// descriptors; parsing never constructs an option graph or retains erased
// callbacks.  Request values are the only successful-parse storage.
namespace mmltk::frameworks::reflection {
enum class ParseErrorCode : unsigned char {
    UnknownOption,
    MissingValue,
    MissingRequired,
    DuplicateOption,
    InvalidBoolean,
    InvalidInteger,
    InvalidFiniteNumber,
    InvalidValue,
    UnknownCommand,
    Help,
};
struct ParseError final : std::runtime_error {
    ParseErrorCode code;
    std::string token;
    ParseError(ParseErrorCode error_code, const std::string_view error_token, const std::string_view detail)
        : std::runtime_error(std::string(detail)), code(error_code), token(error_token) {}
};
inline constexpr std::size_t kMaximumCommandOptions = 128U;
class PresenceSet final {
   public:
    [[nodiscard]] constexpr bool test(const std::size_t index) const noexcept { return bits_.test(index); }
    constexpr void set(const std::size_t index) noexcept { bits_.set(index); }

   private:
    std::bitset<kMaximumCommandOptions> bits_{};
};
enum class OptionKind : unsigned char {
    Value,
    Flag,
    RepeatableValue,
    Positional,
};
[[nodiscard]] constexpr char ascii_lower(const char value) noexcept { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; }
[[nodiscard]] constexpr bool enum_spelling_matches(std::string_view symbol, const std::string_view spelling, const bool allow_prefix) noexcept {
    if (symbol.size() > 1U && symbol.front() == 'k' && symbol[1U] >= 'A' && symbol[1U] <= 'Z') { symbol.remove_prefix(1U); }
    std::size_t symbol_index = 0U;
    std::size_t spelling_index = 0U;
    while (symbol_index < symbol.size() && spelling_index < spelling.size()) {
        while (symbol_index < symbol.size() && (symbol[symbol_index] == '-' || symbol[symbol_index] == '_')) { ++symbol_index; }
        while (spelling_index < spelling.size() && (spelling[spelling_index] == '-' || spelling[spelling_index] == '_')) { ++spelling_index; }
        if (symbol_index == symbol.size() || spelling_index == spelling.size()) break;
        if (ascii_lower(symbol[symbol_index++]) != ascii_lower(spelling[spelling_index++])) return false;
    }
    while (symbol_index < symbol.size() && (symbol[symbol_index] == '-' || symbol[symbol_index] == '_')) ++symbol_index;
    while (spelling_index < spelling.size() && (spelling[spelling_index] == '-' || spelling[spelling_index] == '_')) { ++spelling_index; }
    return spelling_index == spelling.size() && (allow_prefix || symbol_index == symbol.size());
}
template <class Value>
[[nodiscard]] std::expected<Value, ParseError> parse_scalar(const std::string_view text) {
    if constexpr (OptionalValue<Value>::value) {
        using Inner = typename OptionalValue<Value>::type;
        if (text.empty()) return Value{};
        auto parsed = parse_scalar<Inner>(text);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        return Value{std::move(*parsed)};
    } else if constexpr (std::is_same_v<Value, std::string>) {
        return std::string(text);
    } else if constexpr (std::is_same_v<Value, std::filesystem::path>) {
        return std::filesystem::path(text);
    } else if constexpr (std::is_same_v<Value, bool>) {
        if (text == "true" || text == "1") return true;
        if (text == "false" || text == "0") return false;
        return std::unexpected(ParseError{ParseErrorCode::InvalidBoolean, text, "invalid boolean"});
    } else if constexpr (std::is_enum_v<Value>) {
        for (const auto entry : enum_entries<Value>()) {
            if constexpr (requires { cli_enum_spelling(entry.value); }) {
                if (cli_enum_spelling(entry.value) == text) return entry.value;
            } else if (enum_spelling_matches(entry.name, text, false)) {
                return entry.value;
            }
        }
        if constexpr (!requires(Value value) { cli_enum_spelling(value); }) {
            std::optional<Value> prefix_match;
            for (const auto entry : enum_entries<Value>()) {
                if (!enum_spelling_matches(entry.name, text, true)) continue;
                if (prefix_match.has_value()) { return std::unexpected(ParseError{ParseErrorCode::InvalidValue, text, "ambiguous enum value"}); }
                prefix_match = entry.value;
            }
            if (prefix_match.has_value()) return *prefix_match;
        }
        return std::unexpected(ParseError{ParseErrorCode::InvalidValue, text, "invalid enum value"});
    } else if constexpr (std::is_integral_v<Value>) {
        Value value{};
        const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || position != text.data() + text.size()) {
            return std::unexpected(ParseError{ParseErrorCode::InvalidInteger, text, "invalid integer"});
        }
        // CLEANUP-IGNORE: Integral and floating parsing report different public error categories and finite-number
        // policy.
        return value;
    } else if constexpr (std::is_floating_point_v<Value>) {
        Value value{};
        const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || position != text.data() + text.size() || !std::isfinite(value)) {
            return std::unexpected(ParseError{ParseErrorCode::InvalidFiniteNumber, text, "invalid finite number"});
        }
        return value;
    } else {
        static_assert(!sizeof(Value), "static CLI option requires an explicit scalar parser");
    }
}
template <class Value>
[[nodiscard]] std::expected<void, ParseError> validate_scalar(const Value& value, const FieldConstraint constraint, const std::string_view token) {
    if constexpr (OptionalValue<Value>::value) {
        if (value) return validate_scalar(*value, constraint, token);
    } else if constexpr (std::is_arithmetic_v<Value> && !std::is_same_v<Value, bool>) {
        if (!satisfies(value, constraint)) { return std::unexpected(ParseError{ParseErrorCode::InvalidValue, token, "value violates field policy"}); }
    }
    if constexpr (std::is_same_v<Value, std::string>) {
        if (constraint.maximum_bytes != 0U && value.size() > constraint.maximum_bytes) {
            return std::unexpected(ParseError{ParseErrorCode::InvalidValue, token, "value exceeds field policy"});
        }
    }
    if constexpr (std::is_same_v<Value, std::filesystem::path>) {
        if (constraint.maximum_bytes != 0U && value.native().size() > constraint.maximum_bytes) {
            return std::unexpected(ParseError{ParseErrorCode::InvalidValue, token, "path exceeds field policy"});
        }
    }
    return {};
}
template <class Request>
struct OptionDescriptor {
    std::string_view name;
    std::string_view alias;
    std::string_view negated_name;
    std::string_view group;
    std::string_view help;
    std::string_view environment;
    ReflectedMemberIdentity terminal_member;
    FieldConstraint constraint{};
    OptionKind kind = OptionKind::Value;
    bool repeatable = false;
    bool required = false;
    std::expected<void, ParseError> (*assign)(Request&, std::string_view, bool, const FieldConstraint);
    void (*emit)(std::vector<std::string>&, const Request&, std::string_view, std::string_view, OptionKind, bool);
    bool (*emission_enabled)(const Request&) noexcept = nullptr;
};
template <class Value>
inline constexpr bool kRepeatableValue =
    !std::is_same_v<Value, std::string> && !std::is_same_v<Value, std::filesystem::path> && requires(Value& value, typename Value::value_type item) {
        value.push_back(std::move(item));
        value.size();
    };
template <class Value>
using sequence_value_t = typename Value::value_type;
template <class Value>
[[nodiscard]] consteval auto descriptor_scalar_identity() {
    if constexpr (OptionalValue<Value>::value) {
        return std::type_identity<typename OptionalValue<Value>::type>{};
    } else if constexpr (kRepeatableValue<Value>) {
        return std::type_identity<sequence_value_t<Value>>{};
    } else {
        return std::type_identity<Value>{};
    }
}
template <class Value>
using descriptor_scalar_t = typename decltype(descriptor_scalar_identity<Value>())::type;
template <class Value>
void append_scalar(std::vector<std::string>& arguments, const Value& value) {
    if constexpr (std::is_same_v<Value, std::filesystem::path>) {
        arguments.emplace_back(value.string());
    } else if constexpr (std::is_same_v<Value, std::string>) {
        arguments.emplace_back(value);
    } else if constexpr (std::is_enum_v<Value>) {
        if constexpr (requires { cli_enum_spelling(value); }) {
            arguments.emplace_back(cli_enum_spelling(value));
            return;
        }
        std::string_view symbol = enum_name(value);
        if (symbol.size() > 1U && symbol.front() == 'k' && symbol[1U] >= 'A' && symbol[1U] <= 'Z') { symbol.remove_prefix(1U); }
        std::string spelling;
        spelling.reserve(symbol.size() + 4U);
        for (std::size_t index = 0U; index < symbol.size(); ++index) {
            const char character = symbol[index];
            if (character >= 'A' && character <= 'Z' && index != 0U && symbol[index - 1U] >= 'a' && symbol[index - 1U] <= 'z') { spelling.push_back('-'); }
            spelling.push_back(ascii_lower(character));
        }
        arguments.emplace_back(std::move(spelling));
    } else {
        arguments.emplace_back(std::format("{}", value));
    }
}
template <class Request, auto Access>
[[nodiscard]] std::expected<void, ParseError> assign_accessed(Request& request, const std::string_view text, const bool negated,
                                                              const FieldConstraint constraint) {
    using Value = accessor_value_t<Request, Access>;
    if constexpr (std::is_same_v<Value, bool>) {
        if (text.empty()) {
            access<Request, Access>(request) = !negated;
            return {};
        }
        auto parsed = parse_scalar<bool>(text);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        access<Request, Access>(request) = negated ? !*parsed : *parsed;
        return {};
    } else if constexpr (OptionalValue<Value>::value) {
        auto parsed = parse_scalar<Value>(text);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        if (auto valid = validate_scalar(*parsed, constraint, text); !valid) { return std::unexpected(std::move(valid.error())); }
        access<Request, Access>(request) = std::move(*parsed);
        return {};
    } else if constexpr (kRepeatableValue<Value>) {
        using Item = sequence_value_t<Value>;
        auto parsed = parse_scalar<Item>(text);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        if (auto valid = validate_scalar(*parsed, constraint, text); !valid) return std::unexpected(std::move(valid.error()));
        auto& destination = access<Request, Access>(request);
        if (constraint.maximum_items != 0U && destination.size() >= constraint.maximum_items) {
            return std::unexpected(ParseError{ParseErrorCode::InvalidValue, text, "too many option values"});
        }
        if constexpr (requires { destination.full(); }) {
            if (destination.full()) { return std::unexpected(ParseError{ParseErrorCode::InvalidValue, text, "option capacity exceeded"}); }
        }
        destination.push_back(std::move(*parsed));
        return {};
    } else {
        auto parsed = parse_scalar<Value>(text);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        if (auto valid = validate_scalar(*parsed, constraint, text); !valid) return std::unexpected(std::move(valid.error()));
        access<Request, Access>(request) = std::move(*parsed);
        return {};
    }
}
template <class Request, auto Access>
void emit_accessed(std::vector<std::string>& arguments, const Request& request, const std::string_view name, const std::string_view negated_name,
                   const OptionKind kind, const bool include_empty) {
    using Value = accessor_value_t<const Request, Access>;
    const Value& value = access<const Request, Access>(request);
    if constexpr (std::is_same_v<Value, bool>) {
        if (value) {
            arguments.emplace_back(name);
        } else if (!negated_name.empty()) {
            arguments.emplace_back(negated_name);
        }
    } else if constexpr (OptionalValue<Value>::value) {
        if (!value) return;
        if (kind != OptionKind::Positional) arguments.emplace_back(name);
        append_scalar(arguments, *value);
    } else if constexpr (kRepeatableValue<Value>) {
        for (const auto& item : value) {
            if (kind != OptionKind::Positional) arguments.emplace_back(name);
            append_scalar(arguments, item);
        }
    } else if constexpr (std::is_same_v<Value, std::string>) {
        if (include_empty || !value.empty()) {
            if (kind != OptionKind::Positional) arguments.emplace_back(name);
            arguments.emplace_back(value);
        }
    } else if constexpr (std::is_same_v<Value, std::filesystem::path>) {
        if (include_empty || !value.empty()) {
            if (kind != OptionKind::Positional) arguments.emplace_back(name);
            arguments.emplace_back(value.string());
        }
    } else if constexpr (std::is_enum_v<Value>) {
        if (kind != OptionKind::Positional) arguments.emplace_back(name);
        append_scalar(arguments, value);
    } else {
        if (kind != OptionKind::Positional) arguments.emplace_back(name);
        arguments.emplace_back(std::format("{}", value));
    }
}
namespace detail {
template <class Request, auto Access, bool (*EmissionEnabled)(const Request&) noexcept = nullptr>
[[nodiscard]] consteval OptionDescriptor<Request> option_from_reflected_policy(const std::string_view name, const std::string_view help,
                                                                               const std::string_view group, const std::string_view alias,
                                                                               const std::string_view negated_name, const bool required,
                                                                               const std::string_view environment, const FieldConstraint constraint) {
    using Value = accessor_value_t<Request, Access>;
    using Scalar = descriptor_scalar_t<Value>;
    using AccessType = std::remove_cvref_t<decltype(Access)>;
    if constexpr (std::is_member_object_pointer_v<AccessType>) {
        if (!member_default_satisfies<Access>()) { throw "CLI descriptor field default violates its reflected policy"; }
    } else if constexpr (requires { AccessType::terminal_member; }) {
        if (!member_default_satisfies<AccessType::terminal_member>()) { throw "CLI descriptor field default violates its reflected policy"; }
    }
    if constexpr (std::is_enum_v<Scalar>) {
        constexpr auto entries = enum_entries<Scalar>();
        if (entries.empty()) throw "CLI enum has no reflected symbols";
        for (std::size_t left = 0U; left < entries.size(); ++left) {
            if constexpr (requires(Scalar value) { cli_enum_spelling(value); }) {
                if (cli_enum_spelling(entries[left].value).empty()) { throw "CLI enum contains an empty presentation spelling"; }
            }
            for (std::size_t right = left + 1U; right < entries.size(); ++right) {
                if (entries[left].value == entries[right].value) throw "CLI enum contains aliased values";
                if constexpr (requires(Scalar value) { cli_enum_spelling(value); }) {
                    const std::string_view left_spelling = cli_enum_spelling(entries[left].value);
                    const std::string_view right_spelling = cli_enum_spelling(entries[right].value);
                    if (left_spelling == right_spelling) { throw "CLI enum contains duplicate presentation spellings"; }
                }
            }
        }
    }
    if (!negated_name.empty() && !std::is_same_v<Value, bool>) throw "only a boolean option can be negated";
    if (constraint.maximum_items != 0U && !kRepeatableValue<Value>) { throw "item capacity requires a repeatable option"; }
    if constexpr (kRepeatableValue<Value> && requires(Value& value) { value.full(); }) {
        if (constraint.maximum_items != 0U && constraint.maximum_items > Value{}.capacity()) { throw "item policy exceeds fixed-capacity option storage"; }
    }
    // CLEANUP-IGNORE: The custom reflected descriptor has distinct assignment policy and callback authority.
    return OptionDescriptor<Request>{
        .name = name,
        .alias = alias,
        .negated_name = negated_name,
        .group = group,
        .help = help,
        .environment = environment,
        .terminal_member = accessor_member_identity<Request, Access>(),
        .constraint = constraint,
        .kind = std::is_same_v<Value, bool> ? OptionKind::Flag : (kRepeatableValue<Value> ? OptionKind::RepeatableValue : OptionKind::Value),
        .repeatable = kRepeatableValue<Value>,
        .required = required,
        .assign = &assign_accessed<Request, Access>,
        .emit = &emit_accessed<Request, Access>,
        .emission_enabled = EmissionEnabled,
    };
}
}  // namespace detail
// Custom syntax (for example a comma-separated list) may replace scalar
// assignment and emission, while terminal identity and policy remain derived
// from the canonical reflected member.
template <class Request, auto CanonicalAccess, auto Assign, auto Emit, bool (*EmissionEnabled)(const Request&) noexcept = nullptr>
[[nodiscard]] consteval OptionDescriptor<Request> custom_option(const std::string_view name, const std::string_view help, const std::string_view group = {},
                                                                const std::string_view alias = {}, const std::string_view negated_name = {},
                                                                const OptionKind kind = OptionKind::Value, const bool repeatable = false,
                                                                const bool required = false, const std::string_view environment = {}) {
    using AccessType = std::remove_cvref_t<decltype(CanonicalAccess)>;
    static_assert(
        std::is_member_object_pointer_v<AccessType> || requires { AccessType::terminal_member; },
        "custom CLI option requires a canonical reflected member path");
    static_assert(std::is_convertible_v<decltype(Assign), decltype(OptionDescriptor<Request>::assign)>);
    static_assert(std::is_convertible_v<decltype(Emit), decltype(OptionDescriptor<Request>::emit)>);
    constexpr auto terminal = [] {
        if constexpr (std::is_member_object_pointer_v<AccessType>)
            return CanonicalAccess;
        else
            return AccessType::terminal_member;
    }();
    if (!member_default_satisfies<terminal>()) { throw "custom CLI descriptor field default violates its reflected policy"; }
    return OptionDescriptor<Request>{
        .name = name,
        .alias = alias,
        .negated_name = negated_name,
        .group = group,
        .help = help,
        .environment = environment,
        .terminal_member = accessor_member_identity<Request, CanonicalAccess>(),
        .constraint = accessor_policy<CanonicalAccess>(),
        .kind = kind,
        .repeatable = repeatable,
        .required = required,
        .assign = Assign,
        .emit = Emit,
        .emission_enabled = EmissionEnabled,
    };
}
// CLEANUP-IGNORE: Ordinary reflected options and custom-syntax options are distinct developer entry points.
template <class Request, auto Access, bool (*EmissionEnabled)(const Request&) noexcept = nullptr>
[[nodiscard]] consteval OptionDescriptor<Request> option(const std::string_view name, const std::string_view help, const std::string_view group = {},
                                                         const std::string_view alias = {}, const std::string_view negated_name = {},
                                                         const bool required = false, const std::string_view environment = {}) {
    return detail::option_from_reflected_policy<Request, Access, EmissionEnabled>(name, help, group, alias, negated_name, required, environment,
                                                                                  accessor_policy<Access>());
}
// A negative flag projects the same canonical boolean with reversed CLI polarity.
template <class Request, auto Access>
[[nodiscard]] consteval OptionDescriptor<Request> negative_flag(const std::string_view name, const std::string_view help, const std::string_view group = {}) {
    static_assert(std::is_same_v<accessor_value_t<Request, Access>, bool>);
    auto descriptor = option<Request, Access>(name, help, group);
    descriptor.assign = +[](Request& request, std::string_view text, bool negated, FieldConstraint constraint) {
        return assign_accessed<Request, Access>(request, text, !negated, constraint);
    };
    descriptor.emit = +[](std::vector<std::string>& arguments, const Request& request, std::string_view spelling, std::string_view, OptionKind, bool) {
        if (!access<const Request, Access>(request)) arguments.emplace_back(spelling);
    };
    return descriptor;
}
// A repeatable CLI projection may store an element type that differs from its
// canonical request element.  Both limits still derive from reflected native
// members; the descriptor cannot introduce an independent hard policy.
// CLEANUP-IGNORE: Item-policy options deliberately extend the ordinary option entry point with one canonical item
// member.
template <class Request, auto Access, auto CanonicalItemMember, bool (*EmissionEnabled)(const Request&) noexcept = nullptr>
[[nodiscard]] consteval OptionDescriptor<Request> option_with_item_policy(const std::string_view name, const std::string_view help,
                                                                          const std::string_view group = {}, const std::string_view alias = {},
                                                                          const std::string_view negated_name = {}, const bool required = false,
                                                                          const std::string_view environment = {}) {
    using Value = accessor_value_t<Request, Access>;
    static_assert(kRepeatableValue<Value>, "item policy requires a repeatable CLI projection");
    using ItemOwner = typename MemberPointerOwner<std::remove_cvref_t<decltype(CanonicalItemMember)>>::type;
    using CanonicalItem = std::remove_cvref_t<decltype(std::declval<ItemOwner&>().*CanonicalItemMember)>;
    static_assert(std::is_same_v<sequence_value_t<Value>, CanonicalItem>, "CLI projection item must match its canonical reflected member");
    constexpr FieldConstraint constraint = merge(accessor_policy<Access>(), policy_of_member<CanonicalItemMember>());
    return detail::option_from_reflected_policy<Request, Access, EmissionEnabled>(name, help, group, alias, negated_name, required, environment, constraint);
}
template <class Request, auto Access, bool (*EmissionEnabled)(const Request&) noexcept = nullptr>
[[nodiscard]] consteval OptionDescriptor<Request> positional(const std::string_view name, const std::string_view help, const bool required = false) {
    auto descriptor = option<Request, Access, EmissionEnabled>(name, help, "Positionals", {}, {}, required);
    descriptor.kind = OptionKind::Positional;
    return descriptor;
}
template <class Request>
struct ParsedCommand {
    Request request;
    PresenceSet presence;
};
template <class Request, std::size_t Count>
[[nodiscard]] consteval std::size_t unique_descriptor_index(const std::array<OptionDescriptor<Request>, Count>& descriptors,
                                                            const ReflectedMemberIdentity identity) {
    std::size_t result = Count;
    for (std::size_t index = 0U; index < Count; ++index) {
        if (descriptors[index].terminal_member != identity) continue;
        if (result != Count) throw "multiple descriptors have the requested reflected identity";
        result = index;
    }
    if (result == Count) throw "no descriptor has the requested reflected identity";
    return result;
}
template <class Request>
[[nodiscard]] std::expected<ParsedCommand<Request>, ParseError> parse(const std::span<const std::string_view> arguments,
                                                                      const std::span<const OptionDescriptor<Request>> descriptors) {
    if (descriptors.size() > kMaximumCommandOptions) {
        return std::unexpected(ParseError{ParseErrorCode::InvalidValue, {}, "command descriptor capacity exceeded"});
    }
    ParsedCommand<Request> result{};
    PresenceSet environment_presence;
    for (std::size_t descriptor_index = 0U; descriptor_index < descriptors.size(); ++descriptor_index) {
        const auto& descriptor = descriptors[descriptor_index];
        if (descriptor.environment.empty()) continue;
        if (const char* value = std::getenv(descriptor.environment.data()); value != nullptr && value[0] != '\0') {
            if (auto assigned = descriptor.assign(result.request, value, false, descriptor.constraint); !assigned) {
                return std::unexpected(std::move(assigned.error()));
            }
            environment_presence.set(descriptor_index);
        }
    }
    bool positional_only = false;
    std::size_t positional_index = 0U;
    for (std::size_t index = 0U; index < arguments.size(); ++index) {
        const std::string_view token = arguments[index];
        if (token == "--") {
            positional_only = true;
            continue;
        }
        std::string_view spelling = token;
        std::string_view inline_value;
        const std::size_t equals = positional_only ? std::string_view::npos : token.find('=');
        const bool has_inline_value = equals != std::string_view::npos;
        if (has_inline_value) {
            spelling = token.substr(0U, equals);
            inline_value = token.substr(equals + 1U);
        }
        const OptionDescriptor<Request>* descriptor = nullptr;
        std::size_t descriptor_index = 0U;
        bool negated = false;
        if (!positional_only) {
            for (; descriptor_index < descriptors.size(); ++descriptor_index) {
                const auto& candidate = descriptors[descriptor_index];
                if (candidate.kind == OptionKind::Positional) continue;
                if (spelling == candidate.name || (!candidate.alias.empty() && spelling == candidate.alias)) {
                    descriptor = &candidate;
                    break;
                }
                if (!candidate.negated_name.empty() && spelling == candidate.negated_name) {
                    descriptor = &candidate;
                    negated = true;
                    break;
                }
            }
        }
        if (descriptor == nullptr && (positional_only || !spelling.starts_with('-'))) {
            std::size_t ordinal = 0U;
            for (descriptor_index = 0U; descriptor_index < descriptors.size(); ++descriptor_index) {
                const auto& candidate = descriptors[descriptor_index];
                if (candidate.kind != OptionKind::Positional) continue;
                if (ordinal++ < positional_index) continue;
                descriptor = &candidate;
                if (!candidate.repeatable) ++positional_index;
                break;
            }
        }
        if (descriptor == nullptr) { return std::unexpected(ParseError{ParseErrorCode::UnknownOption, spelling, "unknown option"}); }
        if (result.presence.test(descriptor_index) && !descriptor->repeatable) {
            return std::unexpected(ParseError{ParseErrorCode::DuplicateOption, spelling, "duplicate option"});
        }
        std::string_view value = descriptor->kind == OptionKind::Positional ? token : inline_value;
        if (descriptor->kind == OptionKind::Flag) {
            if (has_inline_value) {
                auto parsed = parse_scalar<bool>(value);
                if (!parsed) return std::unexpected(std::move(parsed.error()));
            }
        } else if (descriptor->kind != OptionKind::Positional && !has_inline_value) {
            if (++index == arguments.size() || arguments[index] == "--") {
                return std::unexpected(ParseError{ParseErrorCode::MissingValue, spelling, "missing option value"});
            }
            value = arguments[index];
        }
        if (auto assigned = descriptor->assign(result.request, value, negated, descriptor->constraint); !assigned) {
            const ParseError& error = assigned.error();
            return std::unexpected(ParseError{error.code, spelling, std::format("{}: {}", spelling, error.what())});
        }
        result.presence.set(descriptor_index);
    }
    for (std::size_t index = 0U; index < descriptors.size(); ++index) {
        if (descriptors[index].required && !result.presence.test(index) && !environment_presence.test(index)) {
            return std::unexpected(ParseError{ParseErrorCode::MissingRequired, descriptors[index].name, "required option is missing"});
        }
    }
    return result;
}
template <class Request>
void emit(std::vector<std::string>& arguments, const Request& request, const std::span<const OptionDescriptor<Request>> descriptors,
          const bool include_empty = false) {
    bool positional_separator_emitted = false;
    for (const auto& descriptor : descriptors) {
        if (descriptor.emit == nullptr || (descriptor.emission_enabled != nullptr && !descriptor.emission_enabled(request))) { continue; }
        if (descriptor.kind == OptionKind::Positional && !positional_separator_emitted) {
            arguments.emplace_back("--");
            positional_separator_emitted = true;
        }
        descriptor.emit(arguments, request, descriptor.name, descriptor.negated_name, descriptor.kind, include_empty);
    }
}
template <class Request, std::size_t Count>
void emit(std::vector<std::string>& arguments, const Request& request, const std::array<OptionDescriptor<Request>, Count>& descriptors,
          const bool include_empty = false) {
    emit(arguments, request, std::span<const OptionDescriptor<Request>>{descriptors}, include_empty);
}
template <class Request>
[[nodiscard]] std::string help(const std::string_view usage, const std::string_view description, const std::span<const OptionDescriptor<Request>> descriptors) {
    std::string result(description);
    result += "\nUsage: ";
    result += usage;
    std::string_view previous_group;
    for (const OptionDescriptor<Request>& descriptor : descriptors) {
        if (descriptor.group != previous_group) {
            result += "\n";
            result += descriptor.group.empty() ? "Options:" : descriptor.group;
            previous_group = descriptor.group;
        }
        result += "\n  ";
        result += descriptor.name;
        if (!descriptor.alias.empty()) {
            result += ", ";
            result += descriptor.alias;
        }
        if (!descriptor.negated_name.empty()) {
            result += ", ";
            result += descriptor.negated_name;
        }
        result += "  ";
        result += descriptor.help;
    }
    return result;
}
template <class Request, std::size_t Count>
[[nodiscard]] std::string help(const std::string_view usage, const std::string_view description,
                               const std::array<OptionDescriptor<Request>, Count>& descriptors) {
    return help<Request>(usage, description, std::span<const OptionDescriptor<Request>>{descriptors});
}
struct UnexposedMember {
    ReflectedMemberIdentity identity;
    std::string_view reason;
};
template <class Request, auto Access>
[[nodiscard]] consteval UnexposedMember unexposed(const std::string_view reason) {
    if (reason.empty()) throw "unexposed CLI member requires an ownership reason";
    return {accessor_member_identity<Request, Access>(), reason};
}
namespace detail {
[[nodiscard]] consteval bool has_cli_policy(const FieldConstraint policy) noexcept {
    return policy.has_minimum || policy.has_maximum || policy.finite || policy.maximum_bytes != 0U || policy.maximum_items != 0U;
}
template <class Value>
inline constexpr bool kReflectedCliAggregate =
    std::is_class_v<Value> && std::is_aggregate_v<Value> && !std::is_same_v<Value, std::string> && !std::is_same_v<Value, std::filesystem::path> &&
    !kOpaqueRelationStorage<Value> && !requires { typename Value::value_type; };
template <class Root, class Current, auto... Prefix, class Visitor>
consteval void visit_cli_policy_members(Visitor& visitor) {
    visit_materialized_bases<Current>([&]<class Base>() { visit_cli_policy_members<Root, Base, Prefix...>(visitor); });
    visit_materialized_members<Current>([&]<class Declaration>(const auto&) {
        if constexpr (requires { Declaration::pointer; }) {
            constexpr auto pointer = Declaration::pointer;
            constexpr FieldConstraint policy = policy_of_member<pointer>();
            using Member = OptionalValueT<decltype(std::declval<Current&>().*pointer)>;
            if constexpr (has_cli_policy(policy)) { visitor(ReflectedMemberIdentity::from_path<Root, Prefix..., pointer>()); }
            if constexpr (kReflectedCliAggregate<Member>) { visit_cli_policy_members<Root, Member, Prefix..., pointer>(visitor); }
        }
    });
}
}  // namespace detail
template <class Request, std::size_t Count, std::size_t ExcludedCount>
consteval void audit_descriptors(const std::array<OptionDescriptor<Request>, Count>& descriptors, const std::array<UnexposedMember, ExcludedCount>& excluded) {
    static_assert(Count <= kMaximumCommandOptions, "command exceeds fixed presence capacity");
    for (std::size_t left = 0U; left < Count; ++left) {
        if (descriptors[left].name.empty() || descriptors[left].assign == nullptr || descriptors[left].emit == nullptr) { throw "invalid command descriptor"; }
        if (!descriptors[left].terminal_member.valid()) { throw "command descriptor has no reflected terminal identity"; }
        if ((!descriptors[left].alias.empty() && descriptors[left].alias == descriptors[left].name) ||
            (!descriptors[left].negated_name.empty() &&
             (descriptors[left].negated_name == descriptors[left].name || descriptors[left].negated_name == descriptors[left].alias))) {
            throw "duplicate spelling within command descriptor";
        }
        if (descriptors[left].kind == OptionKind::Positional && !descriptors[left].alias.empty()) { throw "positional descriptor cannot have an alias"; }
        if (descriptors[left].constraint.has_minimum && descriptors[left].constraint.has_maximum &&
            descriptors[left].constraint.minimum > descriptors[left].constraint.maximum) {
            throw "invalid command field policy";
        }
        for (std::size_t right = left + 1U; right < Count; ++right) {
            if (descriptors[left].terminal_member == descriptors[right].terminal_member) {
                throw "multiple CLI descriptors own the same reflected member path";
            }
            const auto collides = [](const std::string_view first, const std::string_view second) { return !first.empty() && first == second; };
            if (collides(descriptors[left].name, descriptors[right].name) || collides(descriptors[left].name, descriptors[right].alias) ||
                collides(descriptors[left].name, descriptors[right].negated_name) || collides(descriptors[left].alias, descriptors[right].name) ||
                collides(descriptors[left].alias, descriptors[right].alias) || collides(descriptors[left].alias, descriptors[right].negated_name) ||
                collides(descriptors[left].negated_name, descriptors[right].name) || collides(descriptors[left].negated_name, descriptors[right].alias) ||
                collides(descriptors[left].negated_name, descriptors[right].negated_name)) {
                throw "duplicate command spelling";
            }
        }
    }
    std::array<bool, ExcludedCount> matched_exclusions{};
    for (std::size_t left = 0U; left < ExcludedCount; ++left) {
        if (!excluded[left].identity.valid() || excluded[left].reason.empty()) { throw "invalid unexposed CLI member decision"; }
        for (const auto& descriptor : descriptors) {
            if (descriptor.terminal_member == excluded[left].identity) { throw "CLI member cannot be both exposed and unexposed"; }
        }
        for (std::size_t right = left + 1U; right < ExcludedCount; ++right) {
            if (excluded[left].identity == excluded[right].identity) { throw "duplicate unexposed CLI member decision"; }
        }
    }
    auto verify_ownership = [&](const ReflectedMemberIdentity identity) consteval {
        std::size_t owners = 0U;
        for (const auto& descriptor : descriptors) owners += descriptor.terminal_member == identity ? 1U : 0U;
        for (std::size_t index = 0U; index < ExcludedCount; ++index) {
            if (excluded[index].identity == identity) {
                ++owners;
                matched_exclusions[index] = true;
            }
        }
        if (owners != 1U) throw "policy-bearing CLI member lacks one exposure decision";
    };
    detail::visit_cli_policy_members<Request, Request>(verify_ownership);
    for (const bool matched : matched_exclusions) {
        if (!matched) throw "unexposed CLI member decision does not name a policy-bearing member";
    }
}
template <class Request, std::size_t Count>
consteval void audit_descriptors(const std::array<OptionDescriptor<Request>, Count>& descriptors) {
    audit_descriptors(descriptors, std::array<UnexposedMember, 0U>{});
}
}  // namespace mmltk::frameworks::reflection
