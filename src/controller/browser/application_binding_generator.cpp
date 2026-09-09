#include "src/controller/contracts/application_systems.h"
#include "src/controller/browser/application_outer_routing_emitter.h"
#include "src/controller/browser/application_visual_projection_emitter.h"
#include "src/controller/browser/application_schema.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>

#include "src/frameworks/serialization/serialization.h"

namespace {

using mmltk::controller::ApplicationSystems;
namespace schema = mmltk::controller::browser::application_schema_detail;

[[nodiscard]] std::string browser_protocol_marker() {
    return "MMLTK_HOST_API_PROTOCOL_" + std::to_string(mmltk::controller::browser::kBrowserProtocolVersion);
}

void emit_rust_value(std::ostream& output, const mmltk::controller::browser::wire::Value& value) {
    using Value = mmltk::controller::browser::wire::Value;
    std::visit(
        [&]<class Storage>(const Storage& storage) {
            using Type = std::remove_cvref_t<Storage>;
            if constexpr (std::same_as<Type, std::monostate>) {
                output << "Value::Null";
            } else if constexpr (std::same_as<Type, bool>) {
                output << (storage ? "Value::Bool(true)" : "Value::Bool(false)");
            } else if constexpr (std::same_as<Type, std::int64_t>) {
                output << "Value::Signed(" << storage << ')';
            } else if constexpr (std::same_as<Type, std::uint64_t>) {
                output << "Value::Unsigned(" << storage << ')';
            } else if constexpr (std::same_as<Type, double>) {
                output << "Value::Float(" << std::showpoint << storage << std::noshowpoint << ')';
            } else if constexpr (std::same_as<Type, std::string>) {
                output << "Value::Text(" << std::quoted(storage) << ".into())";
            } else if constexpr (std::same_as<Type, Value::Bytes>) {
                output << "Value::Bytes(vec![";
                for (const std::byte byte : storage)
                    output << static_cast<unsigned int>(std::to_integer<std::uint8_t>(byte)) << ',';
                output << "])";
            } else if constexpr (std::same_as<Type, Value::Array>) {
                output << "Value::Array(vec![";
                for (const auto& item : storage) {
                    emit_rust_value(output, item);
                    output << ',';
                }
                output << "])";
            } else {
                output << "Value::Object(vec![";
                for (const auto& [name, item] : storage) {
                    output << '(' << std::quoted(name) << ".into(),";
                    emit_rust_value(output, item);
                    output << "),";
                }
                output << "])";
            }
        },
        value.storage);
}

[[nodiscard]] std::string rust_identifier(std::string_view source, const bool upper) {
    if (const auto separator = source.rfind("::"); separator != std::string_view::npos) source.remove_prefix(separator + 2U);
    std::string result;
    bool capitalize = upper;
    for (const unsigned char byte : source) {
        if (!std::isalnum(byte)) {
            capitalize = upper;
            continue;
        }
        char character = static_cast<char>(byte);
        if (capitalize) character = static_cast<char>(std::toupper(byte));
        result.push_back(character);
        capitalize = false;
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) result.insert(result.begin(), '_');
    static constexpr std::array keywords{
        "as",     "async", "await", "become",   "box",    "break",  "const",   "continue", "crate",   "do",    "dyn",    "else",   "enum",
        "extern", "false", "final", "fn",       "for",    "gen",    "if",      "impl",     "in",      "let",   "loop",   "macro",  "match",
        "mod",    "move",  "mut",   "override", "priv",   "pub",    "ref",     "return",   "self",    "Self",  "static", "struct", "super",
        "trait",  "true",  "try",   "type",     "typeof", "unsafe", "unsized", "use",      "virtual", "where", "while",  "yield",
    };
    if (std::ranges::find(keywords, result) != keywords.end()) result.push_back('_');
    return result;
}

[[nodiscard]] std::string rust_constant_identifier(std::string_view source) {
    if (const auto separator = source.rfind("::"); separator != std::string_view::npos) source.remove_prefix(separator + 2U);
    std::string result;
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const unsigned char byte = source[index];
        if (!std::isalnum(byte)) {
            if (!result.empty() && result.back() != '_') result.push_back('_');
            continue;
        }
        if (std::isupper(byte) && !result.empty() && result.back() != '_' && index != 0U &&
            std::islower(static_cast<unsigned char>(source[index - 1U])))
            result.push_back('_');
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) result.insert(result.begin(), '_');
    return result;
}

void emit_rust_field_access(std::ostream& output, std::string_view path) {
    while (!path.empty()) {
        const std::size_t separator = path.find('.');
        output << '.' << rust_identifier(path.substr(0U, separator), false);
        if (separator == std::string_view::npos) return;
        path.remove_prefix(separator + 1U);
    }
}

class ProjectedSymbolRegistry final {
   public:
    bool Reserve(const std::string_view name_space, const std::string_view symbol, const std::string_view source) {
        const std::string key = std::string(name_space) + '\n' + std::string(symbol);
        const auto [entry, inserted] = symbols_.emplace(key, std::string(source));
        if (inserted) return true;
        if (entry->second == source) return false;
        throw std::logic_error("Rust symbol collision in " + std::string(name_space) + " for `" + std::string(symbol) + "` between `" +
                               entry->second + "` and `" + std::string(source) + "`");
    }

   private:
    std::map<std::string, std::string> symbols_;
};

template <class Type>
concept NamedVariant =
    requires(Type value) {
        typename Type::variant_type;
        value.value;
    } && schema::Variant<typename Type::variant_type>::value &&
    std::same_as<std::remove_cvref_t<decltype(std::declval<Type>().value)>, typename Type::variant_type>;

template <class Value>
[[nodiscard]] std::string rust_type() {
    using Type = std::remove_cvref_t<Value>;
    static_assert(schema::runtime_boundary_projectable<Type>() || schema::catalog_boundary_projectable<Type>(),
                  "Rust spelling requested for a type outside the canonical application schema categories");
    if constexpr (std::same_as<Type, void>)
        return "()";
    else if constexpr (std::same_as<Type, bool>)
        return "bool";
    else if constexpr (std::same_as<Type, float>)
        return "f32";
    else if constexpr (std::same_as<Type, double>)
        return "f64";
    else if constexpr (std::same_as<Type, std::string> || std::same_as<Type, std::filesystem::path>)
        return "String";
    else if constexpr (std::same_as<Type, std::string_view>)
        return "Cow<'static, str>";
    else if constexpr (std::same_as<Type, std::byte> || std::same_as<Type, std::uint8_t>)
        return "u8";
    else if constexpr (std::same_as<Type, std::uint16_t>)
        return "u16";
    else if constexpr (std::same_as<Type, std::uint32_t>)
        return "u32";
    else if constexpr (std::same_as<Type, std::uint64_t>)
        return "u64";
    else if constexpr (std::same_as<Type, std::int8_t> || std::same_as<Type, char>)
        return "i8";
    else if constexpr (std::same_as<Type, std::int16_t>)
        return "i16";
    else if constexpr (std::same_as<Type, std::int32_t>)
        return "i32";
    else if constexpr (std::same_as<Type, std::int64_t>)
        return "i64";
    else if constexpr (std::same_as<Type, mmltk::controller::browser::wire::Value> ||
                       std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue>)
        return "Value";
    else if constexpr (schema::Optional<Type>::value)
        return "Option<" + rust_type<typename schema::Optional<Type>::value_type>() + ">";
    else if constexpr (schema::ByteSequence<Type>::value) {
        if constexpr (schema::ByteSequence<Type>::extent == 0U)
            return "crate::application_codec::ByteBuffer";
        else
            return "crate::application_codec::ByteArray<" + std::to_string(schema::ByteSequence<Type>::extent) + ">";
    } else if constexpr (schema::Sequence<Type>::value) {
        using Element = typename schema::Sequence<Type>::value_type;
        if constexpr (schema::Sequence<Type>::extent != 0U && requires { std::tuple_size<Type>::value; })
            return "[" + rust_type<Element>() + "; " + std::to_string(schema::Sequence<Type>::extent) + "]";
        else
            return "Vec<" + rust_type<Element>() + ">";
    } else if constexpr (NamedVariant<Type>) {
        return rust_identifier(mmltk::frameworks::serialization::reflected_schema_type_name<Type>(), true);
    } else if constexpr (schema::Variant<Type>::value) {
        std::string result;
        schema::Variant<Type>::Visit([&]<class Alternative>() {
            if (!result.empty()) result += "Or";
            result += rust_type<Alternative>();
        });
        return result + "Variant";
    } else
        return rust_identifier(mmltk::frameworks::serialization::reflected_schema_type_name<Type>(), true);
}

class BindingOuterRoutingWriter final {
   public:
    BindingOuterRoutingWriter(std::ostream& output, ProjectedSymbolRegistry& symbols) : output_(output), symbols_(symbols) {}

    [[nodiscard]] std::ostream& output() const noexcept { return output_; }

    void reserve(const std::string_view name_space, const std::string_view symbol, const std::string_view source) {
        symbols_.Reserve(name_space, symbol, source);
    }

    [[nodiscard]] std::string identifier(const std::string_view source, const bool upper) const { return rust_identifier(source, upper); }

    template <class Type>
    [[nodiscard]] std::string rust_type() const {
        return ::rust_type<Type>();
    }

    template <class Type>
    [[nodiscard]] std::string native_source() const {
        return std::string(std::meta::display_string_of(^^Type));
    }

   private:
    std::ostream& output_;
    ProjectedSymbolRegistry& symbols_;
};

template <class Value>
void emit_catalog_value(std::ostream& output, const Value& value) {
    using Type = std::remove_cvref_t<Value>;
    static_assert(schema::catalog_boundary_projectable<Type>(), "catalog value is outside the canonical static catalog category");
    if constexpr (std::same_as<Type, bool>) {
        output << (value ? "true" : "false");
    } else if constexpr (std::is_enum_v<Type>) {
        output << rust_type<Type>() << "::" << rust_identifier(mmltk::frameworks::reflection::enum_name(value), true);
    } else if constexpr (std::same_as<Type, std::string_view>) {
        output << "Cow::Borrowed(" << std::quoted(value) << ')';
    } else if constexpr (std::is_floating_point_v<Type>) {
        output << std::showpoint << value << std::noshowpoint;
        if constexpr (std::same_as<Type, float>) output << "f32";
    } else if constexpr (std::is_integral_v<Type>) {
        if constexpr (std::is_signed_v<Type>)
            output << static_cast<std::int64_t>(value);
        else
            output << static_cast<std::uint64_t>(value);
    } else if constexpr (schema::Optional<Type>::value) {
        if (value) {
            output << "Some(";
            emit_catalog_value(output, *value);
            output << ')';
        } else {
            output << "None";
        }
    } else if constexpr (schema::Sequence<Type>::value) {
        output << '[';
        for (const auto& item : value) {
            emit_catalog_value(output, item);
            output << ',';
        }
        output << ']';
    } else if constexpr (schema::ReflectedObject<Type>) {
        output << rust_type<Type>() << " { ";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitFields<Type>(
            [&]<class Owner, class Declaration>(const auto& fact) {
                output << rust_identifier(fact.member_name, false) << ": ";
                emit_catalog_value(output, value.*Declaration::pointer);
                output << ", ";
            });
        output << '}';
    } else {
        static_assert(!sizeof(Type), "unsupported reflected catalog row field type");
    }
}

class BindingEmitter final {
    using Schema = mmltk::controller::browser::ApplicationSchema<ApplicationSystems>;
    using Settings = typename Schema::settings_type;

   public:
    explicit BindingEmitter(std::ostream& output) : output_(output) {
        for (const std::string_view symbol :
             {"Cow", "Value", "Intent", "IntentField", "Interaction", "IntoApplicationValue", "FromApplicationValue", "object",
              "take_field", "take_optional_field", "application_value_within_limits"})
            symbols_.Reserve("module", symbol, "codec foundation");
    }

    void Emit() {
        namespace cbor = mmltk::frameworks::serialization;
        using namespace mmltk::controller::browser;
        constexpr auto bootstrap_bound = cbor::reflected_structural_cbor_bytes<std::variant<Bootstrap>>(Schema::BootstrapPayloadBudget());
        static_assert(bootstrap_bound <= kMaxRecordWireBytes, "bootstrap exceeds aggregate record admission ceiling");
        const auto fingerprint = mmltk::controller::browser::application_schema_fingerprint<ApplicationSystems>().words;
        symbols_.Reserve("module", "BROWSER_PROTOCOL_VERSION", "canonical protocol version");
        symbols_.Reserve("module", "SCHEMA_FINGERPRINT", "canonical application fingerprint");
        output_ << "// Generated from the canonical reflected C++ application schema. Do not edit.\n"
                   "use std::borrow::Cow;\n"
                   "use crate::application_codec::{application_value_within_limits, object, take_field, "
                   "take_optional_field, FromApplicationValue, IntoApplicationValue, Value};\n"
                   "use crate::protocol::client_records::{Intent, IntentField, Interaction};\n\n"
                   "pub const BROWSER_PROTOCOL_VERSION: u64 = "
                << mmltk::controller::browser::kBrowserProtocolVersion
                << ";\npub const MAX_RECORD_WIRE_BYTES: usize = " << mmltk::controller::browser::kMaxRecordWireBytes
                << ";\npub const MAX_OUTPUT_VALUE_BYTES: usize = " << kMaxOutputValueBytes
                << ";\npub const MAX_OUTPUT_VALUE_ITEMS: usize = " << kMaxOutputValueItems
                << ";\npub const MAX_BOOTSTRAP_WIRE_BYTES: usize = " << bootstrap_bound
                << ";\npub const SYSTEM_EVENT_FIELD_COUNT: usize = " << cbor::reflected_cbor_member_count<SystemEvent>()
                << ";\npub const MAX_INTENT_VALUE_BYTES: usize = " << mmltk::controller::browser::kMaxIntentValueBytes
                << ";\npub const MAX_INTENT_VALUE_ITEMS: usize = " << mmltk::controller::browser::kMaxIntentValueItems
                << ";\npub const MAX_INTENT_VALUE_DEPTH: usize = " << mmltk::controller::browser::kMaxIntentValueDepth
                << ";\npub const MAX_SNAPSHOT_COUNT: usize = " << mmltk::controller::browser::kMaxSnapshotCount
                << ";\npub const MAX_ERROR_DETAIL_BYTES: usize = " << mmltk::controller::browser::kMaxErrorDetailBytes
                << ";\npub const MAX_INTENT_FIELDS: usize = " << mmltk::controller::browser::kMaxIntentFields
                << ";\n#[used]\n#[unsafe(no_mangle)]\n"
                   "pub static MMLTK_HOST_API_PROTOCOL_MARKER: &[u8] = b"
                << std::quoted(browser_protocol_marker()) << ";\npub const SCHEMA_FINGERPRINT: [u64; 2] = [" << fingerprint[0] << ", "
                << fingerprint[1] << "];\n\n";
        symbols_.Reserve("module", "EXPLORE_VISIBLE_ITEM_CAPACITY", "canonical Explore visible constraint");
        output_ << "pub const EXPLORE_VISIBLE_ITEM_CAPACITY: u32 = "
                << mmltk::frameworks::reflection::policy_of_member<&mmltk::controller::ExploreOrderFacts::visible_indices>().maximum_items
                << ";\n";
        EmitType<mmltk::controller::contracts::ApplicationErrorCategory>();
        EmitType<mmltk::controller::contracts::reflection::EventDelivery>();
        VisitBoundaryTypes();
        EmitMetadata();
        EmitIdentitiesAndApplicationEnums();
        EmitSnapshotDefaults();
        Schema::VisitEndpoints([&]<class Endpoint>() {
            if constexpr (Endpoint::interaction) EmitCompactType<typename Endpoint::request_type>();
        });
        symbols_.Reserve("module", "ANNOTATION_INPUT_BATCH_CAPACITY", "native annotation batch capacity");
        symbols_.Reserve("module", "ANNOTATION_INPUT_ADMISSION_SLOTS", "native annotation admission slots");
        output_ << "pub const ANNOTATION_INPUT_BATCH_CAPACITY: usize = " << mmltk::controller::kAnnotationInputBatchCapacity
                << ";\npub const ANNOTATION_INPUT_ADMISSION_SLOTS: usize = " << mmltk::controller::kAnnotationInputAdmissionSlots << ";\n";
        symbols_.Reserve("module", "ANNOTATION_INPUT_ENCODED_CAPACITY", "native annotation input wire bound");
        constexpr auto input_wire_bound = cbor::reflected_maximum_cbor_bytes<mmltk::controller::AnnotationInputBatch>();
        constexpr auto interaction_overhead = cbor::reflected_maximum_cbor_bytes<std::variant<Interaction>>() - kMaxIntentValueBytes;
        static_assert(input_wire_bound <= kMaxIntentValueBytes);
        output_ << "pub const ANNOTATION_INPUT_ENCODED_CAPACITY: usize = " << input_wire_bound + interaction_overhead << ";\n";
        EmitEndpoints();
        EmitRequestDefaults();
        EmitDefaults();
        EmitSettingsHelpers();
        EmitSettingsRelations();
        EmitCatalogs();
    }

   private:
    template <class Value>
    void EmitType() {
        using Type = std::remove_cvref_t<Value>;
        if constexpr (schema::Optional<Type>::value) {
            EmitType<typename schema::Optional<Type>::value_type>();
        } else if constexpr (schema::ByteSequence<Type>::value) {
            return;
        } else if constexpr (schema::Sequence<Type>::value) {
            EmitType<typename schema::Sequence<Type>::value_type>();
        } else if constexpr (std::is_enum_v<Type>) {
            const auto name = rust_type<Type>();
            if (!ReserveType<Type>(name)) return;
            output_ << "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum " << name << " {\n";
            for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>()) {
                const auto variant = rust_identifier(entry.name, true);
                symbols_.Reserve("enum " + name, variant, NativeSource<Type>() + "::" + std::string(entry.name));
                output_ << "    " << variant << ",\n";
            }
            output_ << "}\nimpl IntoApplicationValue for " << name
                    << " { fn into_application_value(self) -> Value { Value::Text(match self {\n";
            for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>())
                output_ << "    Self::" << rust_identifier(entry.name, true) << " => \"" << entry.name << "\",\n";
            output_ << "}.into()) } }\nimpl FromApplicationValue for " << name
                    << " { fn from_application_value(value: Value) -> Result<Self, String> { match value {\n";
            for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>())
                output_ << "    Value::Text(value) if value == \"" << entry.name << "\" => Ok(Self::" << rust_identifier(entry.name, true)
                        << "),\n";
            output_ << "    _ => Err(\"invalid enum symbol\".into()), } } }\n\n";
            const auto inventory = rust_constant_identifier(name) + "_VALUES";
            symbols_.Reserve("module", inventory, NativeSource<Type>() + " enum inventory");
            output_ << "pub const " << inventory << ": &[" << name << "] = &[\n";
            for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>())
                output_ << "    " << name << "::" << rust_identifier(entry.name, true) << ",\n";
            output_ << "];\n\n";
        } else if constexpr (NamedVariant<Type>) {
            EmitVariant<Type, typename Type::variant_type>(true);
        } else if constexpr (schema::Variant<Type>::value) {
            EmitVariant<Type, Type>(false);
        } else if constexpr (schema::ReflectedObject<Type> && !std::same_as<Type, std::string> &&
                             !std::same_as<Type, std::filesystem::path>) {
            static_assert(schema::valid_fixed_text_shape<Type>(), "fixed-text boundary declaration is invalid");
            const auto name = rust_type<Type>();
            if (!ReserveType<Type>(name)) return;
            VisitRustFields<Type>([&]<class Field, class>(const auto&, const std::string&) { EmitType<Field>(); });
            if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<Type>) {
                std::size_t field_count = 0U;
                std::string member_name;
                std::string storage_type;
                std::uint64_t maximum = 0U;
                VisitRustFields<Type>([&]<class Field, class>(const auto& fact, const std::string&) {
                    static_assert(std::unsigned_integral<Field>, "opaque relation storage must use an unsigned integral field");
                    ++field_count;
                    member_name = fact.member_name;
                    storage_type = rust_type<Field>();
                    if (!fact.constraint.has_maximum ||
                        fact.constraint.maximum < static_cast<long double>(std::numeric_limits<Field>::min()) ||
                        fact.constraint.maximum > static_cast<long double>(std::numeric_limits<Field>::max()))
                        throw std::logic_error("opaque relation storage requires an integral maximum");
                    maximum = static_cast<std::uint64_t>(fact.constraint.maximum);
                });
                if (field_count != 1U) throw std::logic_error("opaque relation storage must contain exactly one field");
                output_ << "#[derive(Debug, Clone, PartialEq)]\npub struct " << name << "(" << storage_type
                        << ");\nimpl IntoApplicationValue for " << name
                        << " { fn into_application_value(self) -> Value { Value::Object(vec!["
                           "(\""
                        << member_name
                        << "\".into(), self.0.into_application_value())]) } }\n"
                           "impl FromApplicationValue for "
                        << name
                        << " { fn from_application_value(value: Value) -> Result<Self, String> { "
                           "let mut fields = object(value)?; let storage: "
                        << storage_type << " = FromApplicationValue::from_application_value(take_field(&mut fields, \"" << member_name
                        << "\")?)?; if storage > " << maximum
                        << " { return Err(\"field above maximum\".into()) } "
                           "if !fields.is_empty() { return Err(\"unknown object field\".into()) } "
                           "Ok(Self(storage)) } }\n\n";
                CollectMetadata<Type>(name);
                return;
            }
            output_ << "#[derive(Debug, Clone, PartialEq)]\npub struct " << name << " {\n";
            VisitRustFields<Type>([&]<class Field, class>(const auto& fact, const std::string& member) {
                symbols_.Reserve("struct " + name, member, NativeSource<Type>() + "." + std::string(fact.member_name));
                output_ << "    pub " << member << ": " << rust_type<Field>() << ",\n";
            });
            output_ << "}\n";
            if constexpr (mmltk::frameworks::reflection::fixed_text_annotation_count<Type>() == 1U) {
                constexpr auto policy = mmltk::frameworks::reflection::fixed_text_policy_of<Type>();
                output_ << "impl TryFrom<&str> for " << name
                        << " { type Error = String; fn try_from(value: &str) -> Result<Self, Self::Error> {\n"
                        << "if value.is_empty() { return Err(\"fixed text must not be empty\".into()) }\n"
                        << "if value.len() > " << policy.capacity << " { return Err(\"fixed text capacity exceeded\".into()) }\n";
                if constexpr (policy.characters == mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii)
                    output_ << "if !value.bytes().all(|byte| (0x20..0x7f).contains(&byte)) "
                               "{ return Err(\"fixed text contains a non-printable character\".into()) }\n";
                output_ << "let mut bytes = [0_u8; " << policy.capacity
                        << "]; bytes[..value.len()].copy_from_slice(value.as_bytes());\n"
                           "let size = value.len().try_into().map_err(|_| \"fixed text size overflow\".to_owned())?;\n"
                           "Ok(Self { bytes: crate::application_codec::ByteArray(bytes), size }) } }\n";
            }
            output_ << "impl IntoApplicationValue for " << name << " { fn into_application_value(self) -> Value { Value::Object(vec![\n";
            VisitRustFields<Type>([&]<class, class>(const auto& fact, const std::string& member) {
                output_ << "    (\"" << fact.member_name << "\".into(), self." << member << ".into_application_value()),\n";
            });
            output_ << "]) } }\nimpl FromApplicationValue for " << name
                    << " { fn from_application_value(value: Value) -> Result<Self, String> { "
                       "let mut fields = object(value)?;\n";
            VisitRustFields<Type>([&]<class Field, class>(const auto& fact, const std::string& member) {
                output_ << "let " << member << ": " << rust_type<Field>() << " = FromApplicationValue::from_application_value(";
                if constexpr (schema::Optional<Field>::value)
                    output_ << "take_optional_field(&mut fields, \"" << fact.member_name << "\")";
                else
                    output_ << "take_field(&mut fields, \"" << fact.member_name << "\")?";
                output_ << ")?;\n";
                EmitConstraint<Field>(member, fact.constraint);
            });
            output_ << "if !fields.is_empty() { return Err(\"unknown object field\".into()) }\n";
            if constexpr (mmltk::frameworks::reflection::fixed_text_annotation_count<Type>() == 1U) {
                constexpr auto policy = mmltk::frameworks::reflection::fixed_text_policy_of<Type>();
                output_ << "let fixed_size: usize = size.try_into().map_err(|_| \"fixed text size overflow\")?;\n"
                           "if fixed_size == 0 || fixed_size > "
                        << policy.capacity
                        << " { return Err(\"invalid fixed text size\".into()) }\n"
                           "if bytes.0[fixed_size..].iter().any(|byte| *byte != 0) "
                           "{ return Err(\"fixed text tail is not canonical\".into()) }\n";
                if constexpr (policy.characters == mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii)
                    output_ << "if bytes.0[..fixed_size].iter().any(|byte| !(0x20..0x7f).contains(byte)) "
                               "{ return Err(\"invalid fixed text character\".into()) }\n";
            }
            output_ << "Ok(Self {\n";
            VisitRustFields<Type>([&]<class, class>(const auto&, const std::string& member) { output_ << "    " << member << ",\n"; });
            output_ << "}) } }\n\n";
            CollectMetadata<Type>(name);
        } else if constexpr (!Builtin<Type>) {
            throw std::logic_error("unsupported reachable application boundary type at `" + NativeSource<Type>() + "`");
        }
    }

    template <class Type, class Variant>
    void EmitVariant(const bool wrapped) {
        const auto name = rust_type<Type>();
        if (!ReserveType<Type>(name)) return;
        schema::Variant<Variant>::Visit([&]<class Alternative>() { EmitType<Alternative>(); });
        output_ << "#[derive(Debug, Clone, PartialEq)]\npub enum " << name << " {\n";
        schema::Variant<Variant>::Visit([&]<class Alternative>() {
            const auto source = mmltk::frameworks::serialization::reflected_schema_type_name<Alternative>();
            const auto variant = rust_identifier(source, true);
            symbols_.Reserve("enum " + name, variant, NativeSource<Alternative>());
            output_ << "    " << variant << '(' << rust_type<Alternative>() << "),\n";
        });
        output_ << "}\nimpl IntoApplicationValue for " << name << " { fn into_application_value(self) -> Value { ";
        if (wrapped) output_ << "let value = ";
        output_ << "match self {\n";
        schema::Variant<Variant>::Visit([&]<class Alternative>() {
            const auto source = mmltk::frameworks::serialization::reflected_schema_type_name<Alternative>();
            output_ << "    Self::" << rust_identifier(source, true) << "(value) => Value::Object(vec![(\"kind\".into(), Value::Text(\""
                    << source << "\".into())), (\"payload\".into(), value.into_application_value())]),\n";
        });
        output_ << '}';
        if (wrapped) output_ << "; Value::Object(vec![(\"value\".into(), value)])";
        output_ << " } }\nimpl FromApplicationValue for " << name
                << " { fn from_application_value(value: Value) -> Result<Self, String> { ";
        if (wrapped)
            output_ << "let mut wrapper = object(value)?; "
                       "let mut fields = object(take_field(&mut wrapper, \"value\")?)?; "
                       "if !wrapper.is_empty() { return Err(\"unknown named variant field\".into()) } ";
        else
            output_ << "let mut fields = object(value)?; ";
        output_ << "let kind = take_field(&mut fields, \"kind\")?; "
                   "let Value::Text(name) = kind else { return Err(\"variant kind must be text\".into()) }; "
                   "let value = take_field(&mut fields, \"payload\")?; "
                   "if !fields.is_empty() { return Err(\"unknown variant field\".into()) } "
                   "match name.as_str() {\n";
        schema::Variant<Variant>::Visit([&]<class Alternative>() {
            const auto source = mmltk::frameworks::serialization::reflected_schema_type_name<Alternative>();
            output_ << "    \"" << source << "\" => Ok(Self::" << rust_identifier(source, true)
                    << "(FromApplicationValue::from_application_value(value)?)),\n";
        });
        output_ << "    _ => Err(\"unknown variant\".into()), } } }\n\n";
    }

    template <class Type>
    static constexpr bool Builtin = schema::runtime_scalar_projectable<Type>() || std::same_as<Type, std::string_view>;

    template <class Type>
    [[nodiscard]] static std::string NativeSource() {
        return std::string(std::meta::display_string_of(^^Type));
    }

    template <class Type>
    bool ReserveType(const std::string& name) {
        return symbols_.Reserve("module", name, NativeSource<Type>());
    }

    template <class Type, class Visitor>
    void VisitRustFields(Visitor&& visitor) {
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitFields<Type>(
            [&]<class Owner, class Declaration>(const auto& fact) {
                using Field = typename Declaration::member_type;
                visitor.template operator()<Field, Declaration>(fact, rust_identifier(fact.member_name, false));
            });
    }

    void ReserveGeneratedStruct(const std::string_view name, const std::string_view source,
                                const std::initializer_list<std::string_view> fields) {
        symbols_.Reserve("module", name, source);
        for (const auto field : fields)
            symbols_.Reserve("struct " + std::string(name), field, std::string(source) + "." + std::string(field));
    }

    static void EmitRustFloatLiteral(std::ostream& output, const long double value) {
        const double projected = static_cast<double>(value);
        if (!std::isfinite(projected)) throw std::logic_error("generated Rust floating constraint must be finite");
        const std::ios_base::fmtflags prior_flags = output.flags();
        const std::streamsize prior_precision = output.precision();
        output << std::setprecision(std::numeric_limits<double>::max_digits10) << std::showpoint << projected << std::noshowpoint;
        output.flags(prior_flags);
        output.precision(prior_precision);
    }

    static void EmitOptionalFloat(std::ostream& output, const bool present, const long double value) {
        if (!present) {
            output << "None";
            return;
        }
        output << "Some(";
        EmitRustFloatLiteral(output, value);
        output << ')';
    }

    static void EmitConstraintBounds(std::ostream& output, const mmltk::frameworks::reflection::FieldConstraint& constraint) {
        output << "finite: " << (constraint.finite ? "true" : "false") << ", minimum: ";
        EmitOptionalFloat(output, constraint.has_minimum, constraint.minimum);
        output << ", maximum: ";
        EmitOptionalFloat(output, constraint.has_maximum, constraint.maximum);
    }

    static void EmitCompactConstraintFields(std::ostream& output, const mmltk::frameworks::reflection::FieldConstraint& constraint) {
        EmitConstraintBounds(output, constraint);
        output << ", min_bytes: " << constraint.minimum_bytes << ", max_bytes: " << constraint.maximum_bytes
               << ", max_items: " << constraint.maximum_items;
    }

    void EmitConstraintFields(const mmltk::frameworks::reflection::FieldConstraint& constraint) {
        EmitConstraintBounds(output_, constraint);
        output_ << ", minimum_bytes: " << constraint.minimum_bytes << ", maximum_bytes: " << constraint.maximum_bytes
                << ", maximum_items: " << constraint.maximum_items;
    }

    template <class Field>
    void EmitConstraint(const std::string& member, const mmltk::frameworks::reflection::FieldConstraint constraint) {
        using Type = std::remove_cvref_t<Field>;
        if (constraint.finite) output_ << "if !" << member << ".is_finite() { return Err(\"non-finite field " << member << "\".into()) }\n";
        if (constraint.has_minimum) {
            output_ << "if " << member << " < ";
            EmitRustFloatLiteral(output_, constraint.minimum);
            output_ << " as _ { return Err(\"field below minimum\".into()) }\n";
        }
        if (constraint.has_maximum) {
            output_ << "if " << member << " > ";
            EmitRustFloatLiteral(output_, constraint.maximum);
            output_ << " as _ { return Err(\"field above maximum\".into()) }\n";
        }
        if constexpr (std::same_as<Type, mmltk::frameworks::serialization::wire::FlatValue> ||
                      std::same_as<Type, mmltk::frameworks::serialization::wire::Value>) {
            if (constraint.maximum_bytes != 0U || constraint.maximum_items != 0U)
                output_ << "if !application_value_within_limits(&" << member << ", " << constraint.maximum_bytes << ", "
                        << constraint.maximum_items
                        << ", MAX_INTENT_VALUE_DEPTH, 0) { return Err(\"dynamic field capacity exceeded\".into()) }\n";
        } else if constexpr (std::same_as<Type, std::string> || std::same_as<Type, std::filesystem::path>) {
            if (constraint.minimum_bytes != 0U)
                output_ << "if " << member << ".len() < " << constraint.minimum_bytes
                        << " { return Err(\"field byte minimum not met\".into()) }\n";
            if (constraint.maximum_bytes != 0U)
                output_ << "if " << member << ".len() > " << constraint.maximum_bytes
                        << " { return Err(\"field byte capacity exceeded\".into()) }\n";
        } else if constexpr (schema::Sequence<Type>::value) {
            if (constraint.maximum_items != 0U)
                output_ << "if " << member << ".len() > " << constraint.maximum_items
                        << " { return Err(\"field item capacity exceeded\".into()) }\n";
        }
    }

    void VisitBoundaryTypes() {
        EmitType<mmltk::controller::VisualCleanContentIdentity>();
        EmitType<mmltk::controller::AnnotationInputProgress>();
        EmitType<mmltk::controller::contracts::FeatureId>();
        EmitType<mmltk::controller::contracts::reflection::OperationStateSemantic>();
        EmitType<mmltk::controller::contracts::reflection::ProgressFieldSemantic>();
        EmitType<mmltk::controller::contracts::FileDialogMode>();
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
            using Signature = mmltk::controller::browser::SystemMethodSignature<decltype(&[:Snapshot:])>;
            EmitType<typename Signature::result_type>();
        });
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            if constexpr (Endpoint::signature::has_request) EmitType<typename Endpoint::request_type>();
            if constexpr (!std::is_void_v<typename Endpoint::result_type>) EmitType<typename Endpoint::result_type>();
        });
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEvents(
            [&]<class Identity, class Event>(const auto&) { EmitType<Event>(); });
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto&) { EmitCatalogDependencies<Row>(); });
    }

    template <class Type>
    void CollectMetadata(const std::string& name) {
        std::string feature_scope;
        std::string progress_work = "None";
        template for (constexpr auto reflected : mmltk::frameworks::reflection::reflected_annotations<^^Type>()) {
            using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(reflected):]>;
            const Annotation annotation = std::meta::extract<Annotation>(reflected);
            if constexpr (mmltk::controller::contracts::reflection::is_feature_scope_annotation<Annotation>) {
                if (!feature_scope.empty()) throw std::logic_error("reachable type has duplicate feature scopes");
                std::set<mmltk::controller::contracts::FeatureId> unique;
                for (const auto feature : annotation.values) {
                    if (!mmltk::controller::contracts::valid_feature(feature) || !unique.insert(feature).second)
                        throw std::logic_error("reachable type has invalid feature scope");
                    if (!feature_scope.empty()) feature_scope += ", ";
                    feature_scope += "FeatureId::" + rust_identifier(mmltk::frameworks::reflection::enum_name(feature), true);
                }
            } else if constexpr (mmltk::controller::contracts::reflection::is_operation_progress_annotation<Annotation>) {
                using Work = typename Annotation::work_type;
                progress_work = "Some(\"" + std::string(mmltk::frameworks::serialization::reflected_schema_type_name<Work>()) + "\")";
            }
        }
        std::ostringstream type_row;
        type_row << "ReflectedTypeFact { name: \"" << name << "\", feature_scope: &[" << feature_scope
                 << "], progress_work: " << progress_work << " },\n";
        type_facts_.push_back(std::move(type_row).str());

        VisitRustFields<Type>([&]<class, class Declaration>(const auto& fact, const std::string&) {
            std::string operation_state = "None";
            std::string progress = "None";
            std::string catalog = "None";
            Declaration::VisitAnnotations([&]<class Annotation>(const Annotation& annotation) {
                using A = std::remove_cvref_t<Annotation>;
                if constexpr (std::same_as<A, mmltk::controller::contracts::reflection::OperationStateField>) {
                    operation_state = "Some(OperationStateSemantic::" +
                                      rust_identifier(mmltk::frameworks::reflection::enum_name(annotation.semantic), true) + ")";
                } else if constexpr (std::same_as<A, mmltk::controller::contracts::reflection::ProgressField>) {
                    progress = "Some(ProgressFieldSemantic::" +
                               rust_identifier(mmltk::frameworks::reflection::enum_name(annotation.semantic), true) + ")";
                } else if constexpr (mmltk::frameworks::reflection::is_catalog_provider_annotation<A>) {
                    using Provider = typename A::provider_type;
                    if (catalog != "None") throw std::logic_error("reachable field has duplicate catalog providers");
                    catalog = "Some(\"" + std::string(mmltk::frameworks::reflection::type_name<Provider>()) + "\")";
                }
            });
            std::ostringstream row;
            row << "ReflectedFieldFact { owner: \"" << name << "\", name: \"" << fact.member_name << "\", ";
            EmitCompactConstraintFields(row, fact.constraint);
            row << ", presentation: \"" << mmltk::frameworks::reflection::enum_name(fact.presentation)
                << "\", operation_state: " << operation_state << ", progress: " << progress << ", catalog_provider: " << catalog << " },\n";
            field_facts_.push_back(std::move(row).str());
        });
    }

    void EmitMetadata() {
        ReserveGeneratedStruct("ReflectedTypeFact", "generated reflected type metadata", {"name", "feature_scope", "progress_work"});
        symbols_.Reserve("module", "REFLECTED_TYPE_FACTS", "generated reflected type metadata");
        ReserveGeneratedStruct("ReflectedFieldFact", "generated reflected field metadata",
                               {"owner", "name", "finite", "minimum", "maximum", "min_bytes", "max_bytes", "max_items", "presentation",
                                "operation_state", "progress", "catalog_provider"});
        symbols_.Reserve("module", "REFLECTED_FIELD_FACTS", "generated reflected field metadata");
        output_ << "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n"
                   "pub struct ReflectedTypeFact { pub name: &'static str, "
                   "pub feature_scope: &'static [FeatureId], "
                   "pub progress_work: Option<&'static str> }\n"
                   "pub static REFLECTED_TYPE_FACTS: &[ReflectedTypeFact] = &[\n";
        for (const auto& row : type_facts_)
            output_ << row;
        output_ << "];\n#[derive(Debug, Clone, Copy, PartialEq)]\n"
                   "pub struct ReflectedFieldFact { pub owner: &'static str, "
                   "pub name: &'static str, pub finite: bool, "
                   "pub minimum: Option<f64>, pub maximum: Option<f64>, "
                   "pub min_bytes: usize, pub max_bytes: usize, pub max_items: usize, "
                   "pub presentation: &'static str, "
                   "pub operation_state: Option<OperationStateSemantic>, "
                   "pub progress: Option<ProgressFieldSemantic>, "
                   "pub catalog_provider: Option<&'static str> }\n"
                   "pub static REFLECTED_FIELD_FACTS: &[ReflectedFieldFact] = &[\n";
        for (const auto& row : field_facts_)
            output_ << row;
        output_ << "];\n";
        ReserveGeneratedStruct("ModelArtifactDialogFact", "canonical model artifact dialogs",
                               {"target", "stable_field_id", "source_field_id", "input_field_id", "preset_field_id", "resolution_field_id",
                                "predicate_field_id", "field_path", "title", "filter", "pattern"});
        symbols_.Reserve("module", "MODEL_ARTIFACT_DIALOGS", "canonical model artifact dialogs");
        output_ << "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub struct ModelArtifactDialogFact { pub target: ModelArtifactTarget, "
                   "pub stable_field_id: u64, pub source_field_id: u64, pub input_field_id: u64, "
                   "pub preset_field_id: u64, pub resolution_field_id: u64, "
                   "pub predicate_field_id: Option<u64>, "
                   "pub field_path: &'static str, pub title: &'static str, "
                   "pub filter: &'static str, pub pattern: &'static str }\n"
                   "pub static MODEL_ARTIFACT_DIALOGS: &[ModelArtifactDialogFact] = &[\n";
        mmltk::controller::contracts::ModelSelectionRelation::VisitRows([&]<class Relation>(const auto& row) {
            constexpr auto path = mmltk::frameworks::reflection::reflected_member_path<Settings, Relation::artifact>();
            const auto stable_id = mmltk::controller::browser::application_settings_field_stable_id(path.view());
            constexpr auto source_path = mmltk::frameworks::reflection::reflected_member_path<Settings, Relation::source>();
            constexpr auto input_path = mmltk::frameworks::reflection::reflected_member_path<Settings, Relation::input>();
            constexpr auto preset_path = mmltk::frameworks::reflection::reflected_member_path<Settings, Relation::preset>();
            constexpr auto resolution_path = mmltk::frameworks::reflection::reflected_member_path<Settings, Relation::resolution>();
            output_ << "ModelArtifactDialogFact { target: ModelArtifactTarget { stableid: " << stable_id
                    << ", workflow: FeatureId::" << rust_identifier(mmltk::frameworks::reflection::enum_name(row.workflow), true)
                    << ", input: ModelArtifactInputKind::" << rust_identifier(mmltk::frameworks::reflection::enum_name(row.input), true)
                    << " }, stable_field_id: " << stable_id
                    << ", source_field_id: " << mmltk::controller::browser::application_settings_field_stable_id(source_path.view())
                    << ", input_field_id: " << mmltk::controller::browser::application_settings_field_stable_id(input_path.view())
                    << ", preset_field_id: " << mmltk::controller::browser::application_settings_field_stable_id(preset_path.view())
                    << ", resolution_field_id: " << mmltk::controller::browser::application_settings_field_stable_id(resolution_path.view())
                    << ", predicate_field_id: ";
            if constexpr (std::tuple_size_v<decltype(Relation::predicate)> == 0U) {
                output_ << "None";
            } else {
                constexpr auto predicate_path =
                    mmltk::frameworks::reflection::reflected_member_path<Settings, std::get<0>(Relation::predicate)>();
                output_ << "Some(" << mmltk::controller::browser::application_settings_field_stable_id(predicate_path.view()) << ')';
            }
            output_ << ", field_path: " << std::quoted(path.view()) << ", title: " << std::quoted(row.dialog_title)
                    << ", filter: " << std::quoted(row.dialog_filter) << ", pattern: " << std::quoted(row.dialog_pattern) << " },\n";
        });
        output_ << "];\n";
    }

    void EmitIdentitiesAndApplicationEnums() {
        BindingOuterRoutingWriter writer(output_, symbols_);
        mmltk::controller::browser::emit_application_outer_routing<ApplicationSystems>(writer);
        mmltk::controller::browser::emit_application_visual_projection<ApplicationSystems>(writer);
    }

    void EmitEndpoints() {
        ReserveGeneratedStruct("FieldFact", "generated request-field metadata",
                               {"endpoint_id", "field_id", "name", "finite", "minimum", "maximum", "min_bytes", "max_bytes", "max_items",
                                "presentation", "file_dialog_identity", "settings_update_values"});
        symbols_.Reserve("module", "APPLICATION_REQUEST_FIELDS", "generated request-field metadata");
        output_ << "#[derive(Debug, Clone, Copy, PartialEq)]\npub struct FieldFact { "
                   "pub endpoint_id: u64, pub field_id: u64, pub name: &'static str, "
                   "pub finite: bool, pub minimum: Option<f64>, pub maximum: Option<f64>, "
                   "pub min_bytes: usize, pub max_bytes: usize, pub max_items: usize, pub presentation: &'static str, "
                   "pub file_dialog_identity: bool, pub settings_update_values: bool }\n"
                   "pub static APPLICATION_REQUEST_FIELDS: &[FieldFact] = &[\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            if constexpr (Endpoint::signature::has_request) {
                mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitRequestFields<Endpoint>(
                    [&]<class Owner, class Declaration>(const auto& fact) {
                        output_ << "FieldFact { endpoint_id: " << Endpoint::stable_id << ", field_id: " << fact.stable_id << ", name: \""
                                << fact.name << "\", ";
                        EmitCompactConstraintFields(output_, fact.constraint);
                        output_ << ", presentation: \"" << mmltk::frameworks::reflection::enum_name(fact.presentation)
                                << "\", file_dialog_identity: " << (fact.file_dialog_identity ? "true" : "false")
                                << ", settings_update_values: " << (fact.settings_update_values ? "true" : "false") << " },\n";
                    });
            }
        });
        output_ << "];\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            const auto function =
                "encode_" + rust_identifier(Endpoint::system_cell::name, false) + "_" + rust_identifier(Endpoint::name, false);
            symbols_.Reserve("module", function,
                             "endpoint encoder " + std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name));
            if constexpr (Endpoint::interaction) {
                output_ << "pub fn " << function << "(request: " << rust_type<typename Endpoint::request_type>()
                        << ") -> Result<Interaction, crate::protocol::ProtocolError> { Ok(Interaction { endpoint_id: " << Endpoint::stable_id
                        << ", replaceable: " << (Endpoint::replaceable ? "true" : "false")
                        << ", value: crate::protocol::client_records::compact_bytes(&request)? }) }\n";
                symbols_.Reserve("module", function + "_into", "retained compact endpoint encoder " + std::string(Endpoint::name));
                output_ << "pub fn " << function << "_into(request: &" << rust_type<typename Endpoint::request_type>()
                        << ", scratch: &mut Vec<u8>, output: &mut Vec<u8>) -> Result<(), crate::protocol::ProtocolError> { "
                           "crate::protocol::client_records::encode_compact_interaction(" << Endpoint::stable_id
                        << ", request, scratch, output) }\n";
            } else {
                output_ << "pub fn " << function << "(correlation: u64";
                if constexpr (Endpoint::signature::has_request) output_ << ", request: " << rust_type<typename Endpoint::request_type>();
                output_ << ") -> EncodedApplicationIntent { EncodedApplicationIntent { endpoint: "
                        << "ApplicationIntentEndpoint::" << rust_identifier(Endpoint::system_cell::name, true)
                        << rust_identifier(Endpoint::name, true) << ", record: Intent { correlation, endpoint_id: " << Endpoint::stable_id
                        << ", fields: vec![\n";
                if constexpr (Endpoint::signature::has_request) {
                    mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitRequestFields<Endpoint>(
                        [&]<class Owner, class Declaration>(const auto& fact) {
                            output_ << "IntentField { field_id: " << fact.stable_id << ", value: request."
                                    << rust_identifier(fact.name, false) << ".into_application_value() },\n";
                        });
                }
                output_ << "] } } }\n";
            }
        });
    }

    template <class Value>
    void EmitCompactType() {
        using Type = std::remove_cvref_t<Value>;
        if constexpr (schema::Optional<Type>::value) {
            EmitCompactType<typename schema::Optional<Type>::value_type>();
        } else if constexpr (schema::Sequence<Type>::value) {
            EmitCompactType<typename schema::Sequence<Type>::value_type>();
        } else if constexpr (std::is_enum_v<Type>) {
            if (!compact_types_.insert(NativeSource<Type>()).second) return;
            output_ << "impl crate::protocol::client_records::Compact for " << rust_type<Type>()
                    << " { fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), crate::protocol::ProtocolError> { let value: "
                    << (std::is_signed_v<std::underlying_type_t<Type>> ? "i64" : "u64") << " = match self {\n";
            for (const auto entry : mmltk::frameworks::reflection::enum_entries<Type>()) {
                output_ << "Self::" << rust_identifier(entry.name, true) << " => ";
                if constexpr (std::is_signed_v<std::underlying_type_t<Type>>) output_ << static_cast<std::int64_t>(entry.value);
                else output_ << static_cast<std::uint64_t>(entry.value);
                output_ << ",\n";
            }
            output_ << "}; crate::protocol::client_records::Compact::compact(&value, bytes) } }\n";
        } else if constexpr (schema::ReflectedObject<Type> && !Builtin<Type>) {
            if (!compact_types_.insert(NativeSource<Type>()).second) return;
            std::size_t count = 0U;
            VisitRustFields<Type>([&]<class Field, class>(const auto&, const std::string&) { ++count; EmitCompactType<Field>(); });
            output_ << "impl crate::protocol::client_records::Compact for " << rust_type<Type>()
                    << " { fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), crate::protocol::ProtocolError> { crate::protocol::client_records::compact_head(4, " << count << ", bytes)?;\n";
            VisitRustFields<Type>([&]<class, class>(const auto&, const std::string& member) {
                output_ << "crate::protocol::client_records::Compact::compact(&self." << member << ", bytes)?;\n";
            });
            output_ << "Ok(()) } }\n";
        }
    }
    std::set<std::string> compact_types_;

    void EmitSnapshotDefaults() {
        ReserveGeneratedStruct("SnapshotDefaultFact", "canonical snapshot defaults", {"system_id", "value"});
        symbols_.Reserve("module", "application_snapshot_defaults", "canonical snapshot defaults");
        output_ << "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub struct SnapshotDefaultFact { pub system_id: u64, "
                   "pub value: ApplicationSnapshot }\n"
                   "pub fn application_snapshot_defaults() "
                   "-> Result<Vec<SnapshotDefaultFact>, String> { Ok(vec![\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitSnapshotDefaults(
            [&]<class SystemCell, class Value>(const Value& value) {
                auto encoded = mmltk::frameworks::serialization::reflected_value(value);
                if (!encoded) throw std::logic_error("unsupported snapshot default for `" + std::string(SystemCell::name) + "`");
                output_ << "SnapshotDefaultFact { system_id: " << SystemCell::stable_id
                        << ", value: ApplicationSnapshot::" << rust_identifier(SystemCell::name, true)
                        << "(FromApplicationValue::from_application_value(";
                emit_rust_value(output_, *encoded);
                output_ << ")?) },\n";
            });
        output_ << "]) }\n";
    }

    void EmitSettingsHelpers() {
        ReserveGeneratedStruct("SettingsLeafFact", "generated settings-leaf metadata",
                               {"stable_field_id", "path", "mutable_leaf", "workflows", "catalog_provider", "has_file_dialog", "finite",
                                "minimum", "maximum", "minimum_bytes", "maximum_bytes", "maximum_items"});
        ReserveGeneratedStruct("SettingsLeafConstraint", "generated typed settings constraint",
                               {"stable_field_id", "finite", "minimum", "maximum", "minimum_bytes", "maximum_bytes", "maximum_items"});
        symbols_.Reserve("module", "SETTINGS_LEAVES", "generated settings-leaf metadata");
        symbols_.Reserve("module", "SETTINGS_UPDATE_CAPACITY", "generated settings update capacity");
        output_ << "pub const SETTINGS_UPDATE_CAPACITY: usize = " << mmltk::controller::contracts::kMaxSettingsUpdates << ";\n";
        symbols_.Reserve("module", "SETTINGS_EDIT_CAPACITY", "generated settings edit capacity");
        output_ << "pub const SETTINGS_EDIT_CAPACITY: usize = "
                << mmltk::controller::contracts::settings_vocabulary::mutable_leaf_count<Settings>() << ";\n";
        output_ << "#[derive(Debug, Clone, Copy, PartialEq)]\n"
                   "pub struct SettingsLeafFact { pub stable_field_id: u64, "
                   "pub path: &'static str, pub mutable_leaf: bool, "
                   "pub workflows: &'static [FeatureId], "
                   "pub catalog_provider: Option<&'static str>, "
                   "pub has_file_dialog: bool, pub finite: bool, "
                   "pub minimum: Option<f64>, pub maximum: Option<f64>, "
                   "pub minimum_bytes: usize, pub maximum_bytes: usize, "
                   "pub maximum_items: usize }\n"
                   "#[derive(Debug, Clone, Copy, PartialEq)]\n"
                   "pub struct SettingsLeafConstraint { pub stable_field_id: u64, "
                   "pub finite: bool, pub minimum: Option<f64>, "
                   "pub maximum: Option<f64>, pub minimum_bytes: usize, "
                   "pub maximum_bytes: usize, pub maximum_items: usize }\n"
                   "pub static SETTINGS_LEAVES: &[SettingsLeafFact] = &[\n";
        Schema::VisitApplicationSettingsLeaves(
            [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsLeafFact& fact) {
                output_ << "SettingsLeafFact { stable_field_id: " << fact.stable_id << ", path: \"" << fact.path
                        << "\", mutable_leaf: " << (fact.mutable_leaf ? "true" : "false") << ", workflows: &[";
                for (std::size_t index = 0U; index < fact.workflows.count; ++index) {
                    if (index != 0U) output_ << ", ";
                    output_ << "FeatureId::"
                            << rust_identifier(mmltk::frameworks::reflection::enum_name(fact.workflows.workflows[index]), true);
                }
                output_ << "], catalog_provider: ";
                if (fact.catalog_provider.empty())
                    output_ << "None";
                else
                    output_ << "Some(\"" << fact.catalog_provider << "\")";
                output_ << ", has_file_dialog: " << (fact.file_dialog ? "true" : "false") << ", ";
                EmitConstraintFields(fact.constraint);
                output_ << " },\n";
            });
        output_ << "];\n";
        symbols_.Reserve("module", "SettingsFieldValue", "generated settings identity value");
        symbols_.Reserve("module", "SettingsFieldApplyError", "generated settings identity error");
        symbols_.Reserve("module", "read_settings_field", "generated settings identity read");
        symbols_.Reserve("module", "apply_settings_field", "generated settings identity apply");
        std::map<std::string, std::string> settings_value_variants;
        Schema::VisitApplicationSettingsLeaves([&]<class Owner, class Declaration, class Member>(const auto&) {
            EmitType<Member>();
            const auto type = rust_type<Member>();
            settings_value_variants.try_emplace(type, rust_identifier(type, true));
        });
        output_ << "#[derive(Debug, Clone, PartialEq)]\npub enum SettingsFieldValue {\n";
        for (const auto& [type, variant] : settings_value_variants)
            output_ << "    " << variant << "(" << type << "),\n";
        const auto settings_type = rust_type<Settings>();
        output_ << "}\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n"
                   "pub enum SettingsFieldApplyError { UnknownField, ImmutableField, TypeMismatch }\n"
                << "pub fn read_settings_field(state: &" << settings_type
                << ", stable_field_id: u64) "
                   "-> Result<SettingsFieldValue, SettingsFieldApplyError> { match stable_field_id {\n";
        Schema::VisitApplicationSettingsLeaves([&]<class Owner, class Declaration, class Member>(const auto& fact) {
            const auto& variant = settings_value_variants.at(rust_type<Member>());
            output_ << fact.stable_id << " => Ok(SettingsFieldValue::" << variant << "(state";
            emit_rust_field_access(output_, fact.path);
            output_ << ".clone())),\n";
        });
        output_ << "_ => Err(SettingsFieldApplyError::UnknownField), } }\n"
                << "pub fn apply_settings_field(state: &mut " << settings_type
                << ", stable_field_id: u64, "
                   "value: SettingsFieldValue) -> Result<SettingsValueUpdate, SettingsFieldApplyError> {\n"
                   "match stable_field_id {\n";
        Schema::VisitApplicationSettingsLeaves([&]<class Owner, class Declaration, class Member>(const auto& fact) {
            output_ << fact.stable_id << " => ";
            if (!fact.mutable_leaf) {
                output_ << "Err(SettingsFieldApplyError::ImmutableField),\n";
                return;
            }
            const auto& variant = settings_value_variants.at(rust_type<Member>());
            output_ << "match value { SettingsFieldValue::" << variant << "(typed) => { state";
            emit_rust_field_access(output_, fact.path);
            output_ << " = typed.clone(); Ok(SettingsValueUpdate { path: " << std::quoted(fact.path)
                    << ".into(), value: typed.into_application_value() }) }, "
                       "_ => Err(SettingsFieldApplyError::TypeMismatch) },\n";
        });
        output_ << "_ => Err(SettingsFieldApplyError::UnknownField), } }\n";
        Schema::VisitApplicationSettingsLeaves(
            [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsLeafFact& fact) {
                if (!fact.mutable_leaf) return;
                const auto function = "update_" + rust_identifier(fact.path, false);
                symbols_.Reserve("module", function, "settings leaf " + std::string(fact.path));
                output_ << "pub fn " << function << "(value: " << rust_type<Member>()
                        << ") -> SettingsValueUpdate { "
                           "SettingsValueUpdate { path: \""
                        << fact.path
                        << "\".into(), value: "
                           "value.into_application_value() } }\n";
                const auto edit_function = "edit_" + rust_identifier(fact.path, false);
                symbols_.Reserve("module", edit_function, "settings draft leaf " + std::string(fact.path));
                output_ << "pub fn " << edit_function << "(state: &mut " << settings_type << ", value: " << rust_type<Member>()
                        << ") -> SettingsValueUpdate { state";
                emit_rust_field_access(output_, fact.path);
                output_ << " = value.clone(); " << function << "(value) }\n";
                const auto constraint_function = "constraint_" + rust_identifier(fact.path, false);
                symbols_.Reserve("module", constraint_function, "settings constraint " + std::string(fact.path));
                output_ << "pub const fn " << constraint_function
                        << "() -> SettingsLeafConstraint { "
                           "SettingsLeafConstraint { stable_field_id: "
                        << fact.stable_id << ", ";
                EmitConstraintFields(fact.constraint);
                output_ << " } }\n";
            });
    }

    void EmitRequestDefaults() {
        ReserveGeneratedStruct("RequestDefaultFact", "canonical request defaults", {"endpoint_id", "field_id", "path", "value"});
        symbols_.Reserve("module", "RequestDefault", "canonical request defaults");
        symbols_.Reserve("module", "application_request_defaults", "canonical request defaults");
        output_ << "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub enum RequestDefault {\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitRequestDefaults<Endpoint>(
                [&]<class Owner, class Declaration, class Member>(const auto& fact, const Member&) {
                    EmitType<Member>();
                    const auto variant = rust_identifier(
                        std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name) + "." + std::string(fact.name), true);
                    symbols_.Reserve("enum RequestDefault", variant,
                                     "request default " + std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name) +
                                         "." + std::string(fact.name));
                    output_ << "    " << variant << "(" << rust_type<Member>() << "),\n";
                });
        });
        output_ << "}\n#[derive(Debug, Clone, PartialEq)]\n"
                   "pub struct RequestDefaultFact { pub endpoint_id: u64, "
                   "pub field_id: u64, pub path: &'static str, "
                   "pub value: RequestDefault }\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitRequestDefaults<Endpoint>(
                [&]<class Owner, class Declaration, class Member>(const auto& fact, const Member& value) {
                    auto encoded = mmltk::frameworks::serialization::reflected_value(value);
                    const std::string path =
                        std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name) + "." + std::string(fact.name);
                    if (!encoded) throw std::logic_error("unsupported request default at `" + path + "`");
                    const auto function = "default_request_" + rust_identifier(path, false);
                    symbols_.Reserve("module", function, "request default " + path);
                    output_ << "pub fn " << function << "() -> Result<" << rust_type<Member>()
                            << ", String> { "
                               "FromApplicationValue::from_application_value(";
                    emit_rust_value(output_, *encoded);
                    output_ << ") }\n";
                });
        });
        output_ << "pub fn application_request_defaults() "
                   "-> Result<Vec<RequestDefaultFact>, String> { Ok(vec![\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
            mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitRequestDefaults<Endpoint>(
                [&]<class Owner, class Declaration, class Member>(const auto& fact, const Member&) {
                    const std::string path =
                        std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name) + "." + std::string(fact.name);
                    output_ << "RequestDefaultFact { endpoint_id: " << fact.endpoint_id << ", field_id: " << fact.stable_id
                            << ", path: " << std::quoted(path) << ", value: RequestDefault::" << rust_identifier(path, true)
                            << "(default_request_" << rust_identifier(path, false) << "()?) },\n";
                });
        });
        output_ << "]) }\n";
    }

    void EmitDefaults() {
        ReserveGeneratedStruct("SettingsDefaultFact", "canonical settings defaults", {"stable_field_id", "path", "value"});
        symbols_.Reserve("module", "SettingsDefault", "canonical settings defaults");
        symbols_.Reserve("module", "application_settings_defaults", "canonical settings defaults");
        Schema::VisitApplicationSettingsDefaults(
            [&]<class Owner, class Declaration, class Member>(const auto&, const Member&) { EmitType<Member>(); });
        output_ << "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub enum SettingsDefault {\n";
        Schema::VisitApplicationSettingsDefaults(
            [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsDefaultFact& fact,
                                                              const Member&) {
                const auto variant = rust_identifier(fact.path, true);
                symbols_.Reserve("enum SettingsDefault", variant, "settings default " + std::string(fact.path));
                output_ << "    " << variant << "(" << rust_type<Member>() << "),\n";
            });
        output_ << "}\n#[derive(Debug, Clone, PartialEq)]\n"
                   "pub struct SettingsDefaultFact { pub stable_field_id: u64, "
                   "pub path: &'static str, pub value: SettingsDefault }\n";
        Schema::VisitApplicationSettingsDefaults(
            [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsDefaultFact& fact,
                                                              const Member& value) {
                auto encoded = mmltk::frameworks::serialization::reflected_value(value);
                if (!encoded) throw std::logic_error("unsupported settings default at `" + std::string(fact.path) + "`");
                const auto function = "default_" + rust_identifier(fact.path, false);
                symbols_.Reserve("module", function, "settings default " + std::string(fact.path));
                output_ << "pub fn " << function << "() -> Result<" << rust_type<Member>()
                        << ", String> { "
                           "FromApplicationValue::from_application_value(";
                emit_rust_value(output_, *encoded);
                output_ << ") }\n";
            });
        output_ << "pub fn application_settings_defaults() "
                   "-> Result<Vec<SettingsDefaultFact>, String> { Ok(vec![\n";
        Schema::VisitApplicationSettingsDefaults(
            [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsDefaultFact& fact,
                                                              const Member&) {
                output_ << "SettingsDefaultFact { stable_field_id: " << fact.stable_id << ", path: " << std::quoted(fact.path)
                        << ", value: SettingsDefault::" << rust_identifier(fact.path, true) << "(default_"
                        << rust_identifier(fact.path, false) << "()?) },\n";
            });
        output_ << "]) }\n";
    }

    template <class Value>
    void EmitCatalogDependencies() {
        using Type = std::remove_cvref_t<Value>;
        static_assert(schema::catalog_boundary_projectable<Type>(), "catalog dependency is outside the canonical static catalog category");
        if constexpr (schema::Optional<Type>::value) {
            EmitCatalogDependencies<typename schema::Optional<Type>::value_type>();
        } else if constexpr (schema::Sequence<Type>::value) {
            EmitCatalogDependencies<typename schema::Sequence<Type>::value_type>();
        } else if constexpr (std::is_enum_v<Type>) {
            EmitType<Type>();
        } else if constexpr (schema::ReflectedObject<Type> && !std::same_as<Type, std::string> &&
                             !std::same_as<Type, std::filesystem::path>) {
            EmitType<Type>();
        }
    }

    void EmitSettingsRelations() {
        ReserveGeneratedStruct("SettingsRelationFact", "canonical typed settings relations",
                               {"stable_field_id", "source_path", "destination_path"});
        output_ << "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n"
                   "pub struct SettingsRelationFact { pub stable_field_id: u64, "
                   "pub source_path: &'static str, pub destination_path: &'static str }\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitSettingsRelations<Settings>(
            [&]<class Provider, class Relation, auto Selector>() {
                const std::string provider_name = rust_identifier(mmltk::frameworks::reflection::type_name<Provider>(), true);
                const std::string field_type = provider_name + "RelationField";
                const std::string facts_name = rust_constant_identifier(provider_name) + "_RELATION";
                const std::string rows_name = rust_constant_identifier(mmltk::frameworks::reflection::type_name<Provider>());
                symbols_.Reserve("module", field_type, NativeSource<Provider>() + " settings relation");
                symbols_.Reserve("module", facts_name, NativeSource<Provider>() + " settings relation facts");
                output_ << "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum " << field_type << " {\n";
                Relation::VisitMembers([&]<class Entry>() {
                    constexpr auto terminal = std::remove_cvref_t<decltype(Entry::destination)>::terminal_member;
                    output_ << rust_identifier(mmltk::frameworks::reflection::materialized_member_name<terminal>(), true) << ",\n";
                });
                const auto emit_relation_bit_arms = [&] {
                    std::size_t ordinal = 0U;
                    Relation::VisitMembers([&]<class Entry>() {
                        constexpr auto terminal = std::remove_cvref_t<decltype(Entry::destination)>::terminal_member;
                        output_ << field_type
                                << "::" << rust_identifier(mmltk::frameworks::reflection::materialized_member_name<terminal>(), true)
                                << " => " << (std::uint16_t{1U} << ordinal++) << "u16,\n";
                    });
                };
                output_ << "}\nimpl " << rust_type<typename Relation::override_state_type>() << " {\n"
                        << "pub fn overridden(&self, field: " << field_type << ") -> bool { let bit = match field {\n";
                emit_relation_bit_arms();
                output_ << "}; self.0 & bit != 0 }\n"
                           "fn set_override(&mut self, field: "
                        << field_type << ") { self.0 |= match field {\n";
                emit_relation_bit_arms();
                output_ << "}; }\nfn clear_override(&mut self, field: " << field_type << ") { self.0 &= !(match field {\n";
                emit_relation_bit_arms();
                output_ << "}); }\n}\npub static " << facts_name << ": &[SettingsRelationFact] = &[\n";
                constexpr auto selector_path = mmltk::frameworks::reflection::reflected_member_path<Settings, Selector>();
                constexpr auto override_path =
                    mmltk::frameworks::reflection::rebase_member_path<Settings, typename Relation::destination_type>(
                        Selector, Relation::destination_override_state);
                constexpr auto rendered_override_path = mmltk::frameworks::reflection::reflected_member_path<Settings, override_path>();
                const auto visit_relation_fields = [&]<class Visitor>(Visitor&& visitor) {
                    Relation::VisitMembers([&]<class Entry>() {
                        constexpr auto destination =
                            mmltk::frameworks::reflection::rebase_member_path<Settings, typename Relation::destination_type>(
                                Selector, Entry::destination);
                        constexpr auto destination_path = mmltk::frameworks::reflection::reflected_member_path<Settings, destination>();
                        constexpr auto source_path =
                            mmltk::frameworks::reflection::reflected_member_path<typename Relation::source_type, Entry::source>();
                        visitor.template operator()<Entry>(destination_path, source_path);
                    });
                };
                visit_relation_fields([&]<class Entry>(const auto& destination_path, const auto& source_path) {
                    output_ << "SettingsRelationFact { stable_field_id: "
                            << mmltk::controller::browser::application_settings_field_stable_id(destination_path.view())
                            << ", source_path: " << std::quoted(source_path.view())
                            << ", destination_path: " << std::quoted(destination_path.view()) << " },\n";
                });
                output_ << "];\n";
                visit_relation_fields([&]<class Entry>(const auto& destination_path, const auto& source_path) {
                    using Field = mmltk::frameworks::reflection::accessor_value_t<typename Relation::destination_type, Entry::destination>;
                    constexpr auto source_selector_path =
                        mmltk::frameworks::reflection::reflected_member_path<typename Relation::source_type, Relation::source_selector>();
                    constexpr auto terminal = std::remove_cvref_t<decltype(Entry::destination)>::terminal_member;
                    const std::string variant = rust_identifier(mmltk::frameworks::reflection::materialized_member_name<terminal>(), true);
                    const std::string suffix = rust_identifier(destination_path.view(), false);
                    constexpr auto relative_override_path =
                        mmltk::frameworks::reflection::reflected_member_path<typename Relation::destination_type,
                                                                             Relation::destination_override_state>();
                    constexpr auto relative_destination_path =
                        mmltk::frameworks::reflection::reflected_member_path<typename Relation::destination_type, Entry::destination>();
                    constexpr auto relative_selector_path =
                        mmltk::frameworks::reflection::reflected_member_path<typename Relation::destination_type,
                                                                             Relation::destination_selector>();
                    const auto emit_effective_value = [&](const std::string_view state_override_path,
                                                          const std::string_view state_destination_path,
                                                          const std::string_view state_selector_path) {
                        output_ << "if state";
                        emit_rust_field_access(output_, state_override_path);
                        output_ << ".overridden(" << field_type << "::" << variant << ") { state";
                        emit_rust_field_access(output_, state_destination_path);
                        output_ << ".clone() } else { " << rows_name << ".iter().find(|row| row";
                        emit_rust_field_access(output_, source_selector_path.view());
                        output_ << " == state";
                        emit_rust_field_access(output_, state_selector_path);
                        output_ << ").unwrap_or(&" << rows_name << "[0])";
                        emit_rust_field_access(output_, source_path.view());
                        output_ << ".clone() }";
                    };
                    output_ << "pub fn effective_" << suffix << "(state: &" << rust_type<typename Relation::destination_type>() << ") -> "
                            << rust_type<Field>() << " { ";
                    emit_effective_value(relative_override_path.view(), relative_destination_path.view(), relative_selector_path.view());
                    output_ << " }\n";
                    output_ << "pub fn edit_relation_" << suffix << "(state: &mut " << rust_type<Settings>()
                            << ", value: " << rust_type<Field>() << ") -> SettingsValueUpdate { state";
                    emit_rust_field_access(output_, destination_path.view());
                    output_ << " = value.clone(); state";
                    emit_rust_field_access(output_, rendered_override_path.view());
                    output_ << ".set_override(" << field_type << "::" << variant << "); update_" << suffix << "(value) }\n";
                    output_ << "pub fn clear_relation_" << suffix << "(state: &mut " << rust_type<Settings>()
                            << ") -> SettingsValueUpdate { let value = ";
                    emit_effective_value(rendered_override_path.view(), destination_path.view(), selector_path.view());
                    output_ << "; state";
                    emit_rust_field_access(output_, rendered_override_path.view());
                    output_ << ".clear_override(" << field_type << "::" << variant << "); state";
                    emit_rust_field_access(output_, destination_path.view());
                    output_ << " = value; SettingsValueUpdate { path: " << std::quoted(destination_path.view())
                            << ".into(), value: Value::Null } }\n";
                });
            });
    }

    void EmitCatalogs() {
        ReserveGeneratedStruct("CatalogProviderFact", "canonical catalog providers",
                               {"stable_id", "identity", "name", "row_type", "row_count"});
        ReserveGeneratedStruct("CatalogRowFact", "canonical catalog rows", {"provider_id", "stable_id", "key", "index"});
        ReserveGeneratedStruct("CatalogValueFact", "canonical typed catalog rows", {"provider_id", "stable_id", "key", "value"});
        symbols_.Reserve("module", "ApplicationCatalogRow", "canonical typed catalog rows");
        symbols_.Reserve("module", "application_catalog_rows", "canonical typed catalog rows");
        symbols_.Reserve("module", "decode_application_catalog_row", "canonical typed catalog rows");
        symbols_.Reserve("module", "CATALOG_PROVIDERS", "canonical catalog providers");
        symbols_.Reserve("module", "CATALOG_ROWS", "canonical catalog rows");
        ReserveGeneratedStruct("FileDialogFact", "canonical file-dialog projection",
                               {"stable_field_id", "title", "filter", "pattern", "field_path", "workflows", "mode", "artifact"});
        symbols_.Reserve("module", "FILE_DIALOGS", "canonical file-dialog projection");
        output_ << "\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n"
                   "pub struct CatalogProviderFact { pub stable_id: u64, "
                   "pub identity: &'static str, pub name: &'static str, "
                   "pub row_type: &'static str, pub row_count: usize }\n"
                   "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n"
                   "pub struct CatalogRowFact { pub provider_id: u64, "
                   "pub stable_id: u64, pub key: &'static str, "
                   "pub index: usize }\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const mmltk::controller::browser::ApplicationCatalogProviderFact& provider) {
                EmitCatalogDependencies<Row>();
                const std::string static_name = rust_constant_identifier(provider.name);
                symbols_.Reserve("module", static_name, "catalog provider " + std::string(provider.name));
                output_ << "pub static " << static_name << ": &[" << rust_type<Row>() << "] = &[\n";
                mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitCatalogRows<Provider>(
                    [&]<class ActualProvider, class ActualRow>(const auto&, const ActualRow& row) {
                        emit_catalog_value(output_, row);
                        output_ << ",\n";
                    });
                output_ << "];\n";
            });
        output_ << "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub enum ApplicationCatalogRow {\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto& provider) {
                const auto variant = rust_identifier(provider.name, true);
                symbols_.Reserve("enum ApplicationCatalogRow", variant, "catalog provider " + std::string(provider.name));
                output_ << "    " << variant << "(" << rust_type<Row>() << "),\n";
            });
        output_ << "}\nimpl IntoApplicationValue for ApplicationCatalogRow { "
                   "fn into_application_value(self) -> Value { match self {\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto& provider) {
                const auto variant = rust_identifier(provider.name, true);
                output_ << "Self::" << variant << "(value) => value.into_application_value(),\n";
            });
        output_ << "} } }\npub fn decode_application_catalog_row(provider_id: u64, value: Value) "
                   "-> Result<ApplicationCatalogRow, String> { match provider_id {\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto& provider) {
                output_ << provider.stable_id << " => Ok(ApplicationCatalogRow::" << rust_identifier(provider.name, true)
                        << "(FromApplicationValue::from_application_value(value)?)),\n";
            });
        output_ << "_ => Err(\"unknown catalog provider\".into()), } }\n"
                   "#[derive(Debug, Clone, PartialEq)]\n"
                   "pub struct CatalogValueFact { pub provider_id: u64, "
                   "pub stable_id: u64, pub key: &'static str, "
                   "pub value: ApplicationCatalogRow }\n"
                   "pub fn application_catalog_rows() -> Vec<CatalogValueFact> { vec![\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto& provider) {
                const std::string static_name = rust_constant_identifier(provider.name);
                mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitCatalogRows<Provider>(
                    [&]<class ActualProvider, class ActualRow>(const auto& row, const ActualRow&) {
                        output_ << "CatalogValueFact { provider_id: " << row.provider_id << ", stable_id: " << row.stable_id
                                << ", key: " << std::quoted(row.key)
                                << ", value: ApplicationCatalogRow::" << rust_identifier(provider.name, true) << "(" << static_name << "["
                                << row.index << "].clone()) },\n";
                    });
            });
        output_ << "] }\n";
        output_ << "pub static CATALOG_PROVIDERS: &[CatalogProviderFact] = &[\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const mmltk::controller::browser::ApplicationCatalogProviderFact& provider) {
                std::size_t row_count = 0U;
                mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitCatalogRows<Provider>(
                    [&]<class ActualProvider, class ActualRow>(const auto&, const ActualRow&) { ++row_count; });
                output_ << "CatalogProviderFact { stable_id: " << provider.stable_id << ", identity: " << std::quoted(provider.identity)
                        << ", name: " << std::quoted(provider.name) << ", row_type: " << std::quoted(provider.row_type)
                        << ", row_count: " << row_count << " },\n";
            });
        output_ << "];\npub static CATALOG_ROWS: &[CatalogRowFact] = &[\n";
        mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::VisitCatalogProviders(
            [&]<class Provider, class Row>(const auto&) {
                mmltk::controller::browser::ApplicationSchema<ApplicationSystems>::template VisitCatalogRows<Provider>(
                    [&]<class ActualProvider, class ActualRow>(const mmltk::controller::browser::ApplicationCatalogRowFact& fact,
                                                               const ActualRow&) {
                        output_ << "CatalogRowFact { provider_id: " << fact.provider_id << ", stable_id: " << fact.stable_id
                                << ", key: " << std::quoted(fact.key) << ", index: " << fact.index << " },\n";
                    });
            });
        output_ << "];\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub struct FileDialogFact { "
                   "pub stable_field_id: u64, pub title: &'static str, pub filter: &'static str, "
                   "pub pattern: &'static str, pub field_path: &'static str, "
                   "pub workflows: &'static [FeatureId], "
                   "pub mode: FileDialogMode }\n"
                   "pub static FILE_DIALOGS: &[FileDialogFact] = &[\n";
        Schema::VisitApplicationSettingsLeaves([&]<class Owner, class Declaration, class Member>(
                                                   const mmltk::controller::browser::ApplicationSettingsLeafFact& field) {
            if (!field.file_dialog) return;
            const auto& dialog = *field.file_dialog;
            output_ << "FileDialogFact { stable_field_id: " << field.stable_id << ", title: \"" << dialog.title << "\", filter: \""
                    << dialog.filter << "\", pattern: \"" << dialog.pattern << "\", field_path: \"" << field.path << "\", workflows: &[";
            for (std::size_t index = 0U; index < field.workflows.count; ++index) {
                if (index != 0U) output_ << ", ";
                output_ << "FeatureId::"
                        << rust_identifier(mmltk::frameworks::reflection::enum_name(field.workflows.workflows[index]), true);
            }
            output_ << "], mode: FileDialogMode::" << rust_identifier(mmltk::frameworks::reflection::enum_name(dialog.mode), true)
                    << " },\n";
        });
        output_ << "];\n";
    }

    std::ostream& output_;
    ProjectedSymbolRegistry symbols_;
    std::vector<std::string> type_facts_;
    std::vector<std::string> field_facts_;
};

class TemporaryOutput final {
   public:
    explicit TemporaryOutput(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryOutput() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void Released() noexcept { path_.clear(); }

   private:
    std::filesystem::path path_;
};

}  // namespace

int main(const int argument_count, char* const* const arguments) {
    if (argument_count != 3 || arguments[1] == nullptr || std::string_view(arguments[1]).empty() || arguments[2] == nullptr ||
        std::string_view(arguments[2]).empty())
        return EXIT_FAILURE;
    try {
        const std::filesystem::path destination(arguments[1]);
        const std::filesystem::path protocol_marker_destination(arguments[2]);
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::create_directories(protocol_marker_destination.parent_path());
        TemporaryOutput temporary(destination.string() + ".tmp." + std::to_string(::getpid()));
        TemporaryOutput protocol_marker_temporary(protocol_marker_destination.string() + ".tmp." + std::to_string(::getpid()));
        {
            std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open temporary Rust binding output");
            BindingEmitter(output).Emit();
            output.flush();
            if (!output) throw std::runtime_error("cannot write generated Rust bindings");
        }
        {
            std::ofstream output(protocol_marker_temporary.path(), std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open temporary protocol marker output");
            output << browser_protocol_marker();
            output.flush();
            if (!output) throw std::runtime_error("cannot write generated protocol marker");
        }
        std::filesystem::rename(temporary.path(), destination);
        temporary.Released();
        std::filesystem::rename(protocol_marker_temporary.path(), protocol_marker_destination);
        protocol_marker_temporary.Released();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "application binding generation failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
