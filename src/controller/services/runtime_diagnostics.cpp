#include "src/controller/services/runtime_diagnostics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace mmltk::controller::services {
namespace {

[[nodiscard]] std::string_view owner_name(const contracts::DiagnosticOwner owner) noexcept {
    switch (owner) {
        case contracts::DiagnosticOwner::BrowserRuntime:
            return "browser_runtime";
        case contracts::DiagnosticOwner::BrowserServer:
            return "browser_server";
        case contracts::DiagnosticOwner::FirefoxProcess:
            return "firefox_process";
        case contracts::DiagnosticOwner::Explore:
            return "explore";
        case contracts::DiagnosticOwner::Annotation:
            return "annotation";
        case contracts::DiagnosticOwner::Upscale:
            return "upscale";
        case contracts::DiagnosticOwner::Live:
            return "live";
        case contracts::DiagnosticOwner::Presentation:
            return "presentation";
        case contracts::DiagnosticOwner::AnnotationResource:
            return "annotation_resource";
    }
    return {};
}

[[nodiscard]] bool valid_event_name(const std::string_view event) noexcept {
    if (event.empty() || event.size() > 96U) return false;
    for (const unsigned char character : event) {
        const bool valid = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '.' || character == '_';
        if (!valid) return false;
    }
    return true;
}

namespace wire = mmltk::frameworks::serialization::wire;

class BoundedJsonWriter;

struct JsonValueVisitor final {
    BoundedJsonWriter& writer;

    [[nodiscard]] bool operator()(std::monostate value) const noexcept;
    [[nodiscard]] bool operator()(bool value) const noexcept;
    [[nodiscard]] bool operator()(std::int64_t value) const noexcept;
    [[nodiscard]] bool operator()(std::uint64_t value) const noexcept;
    [[nodiscard]] bool operator()(double value) const noexcept;
    [[nodiscard]] bool operator()(const std::string& value) const noexcept;
    [[nodiscard]] bool operator()(const wire::Value::Bytes& value) const noexcept;
    [[nodiscard]] bool operator()(const wire::Value::Array& value) const noexcept;
    [[nodiscard]] bool operator()(const wire::Value::Object& value) const noexcept;
};

class BoundedJsonWriter final {
   public:
    explicit BoundedJsonWriter(const std::span<char> destination) noexcept : destination_(destination) {}

    [[nodiscard]] bool runtime_event(const RuntimeDiagnosticFact& fact, const std::int64_t steady_ns) noexcept {
        return append("{\"kind\":\"gui_runtime\",\"steady_ns\":") && integer(steady_ns) && reflected_fields(fact) && append("}");
    }
    [[nodiscard]] bool browser_event(const std::string_view event, const wire::Value::Object& fields) noexcept {
        if (!append("{\"event\":\"browser.telemetry\",\"fields\":{")) { return false; }

        bool wrote_field = false;
        for (const auto& [name, value] : fields) {
            if (name == "kind" || name == "name") continue;
            if (!field_prefix(wrote_field) || !string(name) || !append(":") || !wire_value(value)) { return false; }
            wrote_field = true;
        }

        if (!field_prefix(wrote_field) || !append("\"kind\":\"event\"")) { return false; }
        wrote_field = true;
        if (!field_prefix(wrote_field) || !append("\"name\":") || !string(event)) { return false; }
        return append("}}");
    }

    [[nodiscard]] bool benchmark_event(const std::string_view event, const std::string_view json_fields) noexcept {
        return append("{\"kind\":\"benchmark_dataset\",\"name\":") && string(event) && append(",\"fields\":") && append(json_fields) &&
               append("}");
    }

    [[nodiscard]] std::string_view view() const noexcept { return {destination_.data(), size_}; }

   private:
    template <class Record>
    [[nodiscard]] bool reflected_fields(const Record& record) noexcept {
        bool valid = true;
        mmltk::frameworks::reflection::visit_materialized_members<Record>([&]<class Declaration>(const auto& field) {
            if (!valid) return;
            const auto& value = record.*Declaration::pointer;
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_arithmetic_v<Value> || std::is_enum_v<Value> || std::is_same_v<Value, std::string_view> ||
                          std::is_same_v<Value, contracts::DiagnosticTraceId> || std::is_same_v<Value, contracts::DiagnosticSpanId>) {
                valid = append(",") && string(field.member_name) && append(":");
                if (!valid) return;
                if constexpr (std::is_same_v<Value, contracts::DiagnosticOwner>)
                    valid = string(owner_name(value));
                else if constexpr (std::is_same_v<Value, std::string_view>)
                    valid = string(value);
                else if constexpr (std::is_enum_v<Value>)
                    valid = integer_value(static_cast<std::underlying_type_t<Value>>(value));
                else if constexpr (std::is_same_v<Value, bool>)
                    valid = append(value ? "true" : "false");
                else if constexpr (std::is_floating_point_v<Value>)
                    valid = floating(value);
                else if constexpr (std::is_integral_v<Value>)
                    valid = integer_value(value);
                else
                    valid = integer_value(value.value());
            } else {
                valid = reflected_fields(value);
            }
        });
        return valid;
    }
    [[nodiscard]] bool append(const std::string_view value) noexcept {
        if (value.size() > destination_.size() - size_) return false;
        std::copy(value.begin(), value.end(), destination_.begin() + static_cast<std::ptrdiff_t>(size_));
        size_ += value.size();
        return true;
    }

    [[nodiscard]] bool character(const char value) noexcept {
        if (size_ == destination_.size()) return false;
        destination_[size_++] = value;
        return true;
    }

    [[nodiscard]] bool field_prefix(const bool wrote_field) noexcept { return !wrote_field || append(","); }

    [[nodiscard]] bool string(const std::string_view value) noexcept {
        static constexpr char hex[] = "0123456789abcdef";
        if (!character('"')) return false;
        unsigned remaining = 0U;
        std::uint32_t codepoint = 0U;
        std::uint32_t minimum = 0U;
        for (const unsigned char byte : value) {
            if (remaining != 0U) {
                if ((byte & 0xc0U) != 0x80U) return false;
                codepoint = (codepoint << 6U) | (byte & 0x3fU);
                if (--remaining == 0U && (codepoint < minimum || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)))
                    return false;
            } else if (byte >= 0x80U) {
                if (byte >= 0xc2U && byte <= 0xdfU) {
                    remaining = 1U;
                    codepoint = byte & 0x1fU;
                    minimum = 0x80U;
                } else if (byte >= 0xe0U && byte <= 0xefU) {
                    remaining = 2U;
                    codepoint = byte & 0x0fU;
                    minimum = 0x800U;
                } else if (byte >= 0xf0U && byte <= 0xf4U) {
                    remaining = 3U;
                    codepoint = byte & 0x07U;
                    minimum = 0x10000U;
                } else
                    return false;
            }
            switch (byte) {
                case '"':
                    if (!append("\\\"")) return false;
                    break;
                case '\\':
                    if (!append("\\\\")) return false;
                    break;
                case '\b':
                    if (!append("\\b")) return false;
                    break;
                case '\f':
                    if (!append("\\f")) return false;
                    break;
                case '\n':
                    if (!append("\\n")) return false;
                    break;
                case '\r':
                    if (!append("\\r")) return false;
                    break;
                case '\t':
                    if (!append("\\t")) return false;
                    break;
                default:
                    if (byte < 0x20U) {
                        if (!append("\\u00") || !character(hex[(byte >> 4U) & 0x0fU]) || !character(hex[byte & 0x0fU])) { return false; }
                    } else if (!character(static_cast<char>(byte))) {
                        return false;
                    }
                    break;
            }
        }
        return remaining == 0U && character('"');
    }

    template <class Integer>
    [[nodiscard]] bool integer_value(const Integer value) noexcept {
        constexpr std::size_t sign_bytes = std::numeric_limits<Integer>::is_signed ? 3U : 2U;
        std::array<char, std::numeric_limits<Integer>::digits10 + sign_bytes> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return result.ec == std::errc{} && append({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    [[nodiscard]] bool integer(const std::int64_t value) noexcept { return integer_value(value); }

    [[nodiscard]] bool integer(const std::uint64_t value) noexcept { return integer_value(value); }

    [[nodiscard]] bool floating(const double value) noexcept {
        if (!std::isfinite(value)) return append("null");
        std::array<char, 64U> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                                          std::numeric_limits<double>::max_digits10);
        return result.ec == std::errc{} && append({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    [[nodiscard]] bool bytes(const wire::Value::Bytes& value) noexcept {
        if (!character('[')) return false;
        bool wrote_value = false;
        for (const std::byte byte : value) {
            if (!field_prefix(wrote_value) || !integer(static_cast<std::uint64_t>(std::to_integer<unsigned int>(byte)))) { return false; }
            wrote_value = true;
        }
        return character(']');
    }

    [[nodiscard]] bool array(const wire::Value::Array& value) noexcept {
        if (!character('[')) return false;
        bool wrote_value = false;
        for (const wire::Value& item : value) {
            if (!field_prefix(wrote_value) || !wire_value(item)) return false;
            wrote_value = true;
        }
        return character(']');
    }

    [[nodiscard]] bool object(const wire::Value::Object& value) noexcept {
        if (!character('{')) return false;
        bool wrote_value = false;
        for (const auto& [name, item] : value) {
            if (!field_prefix(wrote_value) || !string(name) || !append(":") || !wire_value(item)) { return false; }
            wrote_value = true;
        }
        return character('}');
    }

    [[nodiscard]] bool wire_value(const wire::Value& value) noexcept { return std::visit(JsonValueVisitor{*this}, value.storage); }

    std::span<char> destination_;
    std::size_t size_ = 0U;

    friend struct JsonValueVisitor;
};

bool JsonValueVisitor::operator()(const std::monostate) const noexcept { return writer.append("null"); }
bool JsonValueVisitor::operator()(const bool value) const noexcept { return writer.append(value ? "true" : "false"); }
bool JsonValueVisitor::operator()(const std::int64_t value) const noexcept { return writer.integer(value); }
bool JsonValueVisitor::operator()(const std::uint64_t value) const noexcept { return writer.integer(value); }
bool JsonValueVisitor::operator()(const double value) const noexcept { return writer.floating(value); }
bool JsonValueVisitor::operator()(const std::string& value) const noexcept { return writer.string(value); }
bool JsonValueVisitor::operator()(const wire::Value::Bytes& value) const noexcept { return writer.bytes(value); }
bool JsonValueVisitor::operator()(const wire::Value::Array& value) const noexcept { return writer.array(value); }
bool JsonValueVisitor::operator()(const wire::Value::Object& value) const noexcept { return writer.object(value); }

}  // namespace

struct RuntimeDiagnosticTarget::State final {
    explicit State(DiagnosticsProducer producer_in, const bool probes) noexcept : producer(std::move(producer_in)), pixel_probes(probes) {}

    [[nodiscard]] bool enabled() const noexcept { return producer.enabled(); }

    void write(RuntimeDiagnosticFact fact) const noexcept;
    void write_browser_event(std::string_view event, const mmltk::frameworks::serialization::wire::Value& fields) const noexcept;
    void write_benchmark_trace(std::string_view event, std::string_view json_fields) const noexcept;

    DiagnosticsProducer producer;
    bool pixel_probes = false;
};

bool RuntimeDiagnosticTarget::valid() const noexcept { return state_ && state_->enabled(); }
bool RuntimeDiagnosticTarget::pixel_probes_enabled() const noexcept { return valid() && state_->pixel_probes; }

void RuntimeDiagnosticTarget::write(const RuntimeDiagnosticFact fact) const noexcept {
    if (state_) state_->write(fact);
}

void RuntimeDiagnosticTarget::write_browser_event(const std::string_view event,
                                                  const mmltk::frameworks::serialization::wire::Value& fields) const noexcept {
    if (state_) state_->write_browser_event(event, fields);
}

bool RuntimeDiagnosticTarget::benchmark_trace_enabled() const noexcept { return valid(); }

void RuntimeDiagnosticTarget::write_benchmark_trace(const std::string_view event, const std::string_view json_fields) const noexcept {
    if (state_) state_->write_benchmark_trace(event, json_fields);
}

RuntimeDiagnostics::RuntimeDiagnostics(DiagnosticsProducer producer, const bool pixel_probes) noexcept {
    if (!producer.enabled()) return;
    try {
        state_ = std::make_shared<RuntimeDiagnosticTarget::State>(std::move(producer), pixel_probes);
    } catch (...) {}
}

RuntimeDiagnosticTarget RuntimeDiagnostics::target() noexcept {
    return state_ && state_->enabled() ? RuntimeDiagnosticTarget{state_} : RuntimeDiagnosticTarget{};
}

void RuntimeDiagnostics::Submit(void* const context, const RuntimeDiagnosticFact fact) noexcept {
    if (context != nullptr) { static_cast<RuntimeDiagnostics*>(context)->write(fact); }
}

void RuntimeDiagnostics::SubmitBrowserEvent(void* const context, const std::string_view event,
                                            const mmltk::frameworks::serialization::wire::Value& fields) noexcept {
    if (context != nullptr) { static_cast<RuntimeDiagnostics*>(context)->write_browser_event(event, fields); }
}

void RuntimeDiagnostics::SubmitBenchmarkTrace(void* const context, const std::string_view event,
                                              const std::string_view json_fields) noexcept {
    if (context != nullptr) static_cast<RuntimeDiagnostics*>(context)->write_benchmark_trace(event, json_fields);
}

void RuntimeDiagnosticTarget::State::write(const RuntimeDiagnosticFact fact) const noexcept {
    const DiagnosticsProducer::Operation operation = producer.acquire();
    if (!operation.enabled()) return;
    const std::string_view owner = owner_name(fact.owner);
    if (owner.empty() || !valid_event_name(fact.event) || (!fact.participant.empty() && !valid_event_name(fact.participant))) return;
    if (fact.message.size() > 1024U) return;
    try {
        const auto steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::array<char, DiagnosticsClient::kRecordCapacity> record;
        BoundedJsonWriter writer{record};
        if (writer.runtime_event(fact, steady_ns)) static_cast<void>(operation.try_submit_encoded({.json = writer.view()}));
    } catch (...) {}
}

void RuntimeDiagnosticTarget::State::write_browser_event(const std::string_view event,
                                                         const mmltk::frameworks::serialization::wire::Value& fields) const noexcept {
    const DiagnosticsProducer::Operation operation = producer.acquire();
    if (!operation.enabled() || !valid_event_name(event)) return;
    const auto* const object = std::get_if<mmltk::frameworks::serialization::wire::Value::Object>(&fields.storage);
    if (object == nullptr) return;

    std::array<char, DiagnosticsClient::kRecordCapacity> record{};
    BoundedJsonWriter writer{record};
    if (!writer.browser_event(event, *object)) return;
    static_cast<void>(operation.try_submit_encoded({.json = writer.view()}));
}

void RuntimeDiagnosticTarget::State::write_benchmark_trace(const std::string_view event,
                                                           const std::string_view json_fields) const noexcept {
    const DiagnosticsProducer::Operation operation = producer.acquire();
    if (!operation.enabled() || !valid_event_name(event) || json_fields.empty() ||
        json_fields.size() > DiagnosticsClient::kRecordCapacity || json_fields.find_first_of("\r\n") != std::string_view::npos)
        return;
    try {
        const auto parsed = nlohmann::json::parse(json_fields, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return;
        std::array<char, DiagnosticsClient::kRecordCapacity> record;
        BoundedJsonWriter writer{record};
        if (!writer.benchmark_event(event, json_fields)) return;
        static_cast<void>(operation.try_submit_encoded({.json = writer.view()}));
    } catch (...) {}
}

void RuntimeDiagnostics::write(const RuntimeDiagnosticFact fact) noexcept {
    if (state_) state_->write(fact);
}

void RuntimeDiagnostics::write_browser_event(const std::string_view event,
                                             const mmltk::frameworks::serialization::wire::Value& fields) noexcept {
    if (state_) state_->write_browser_event(event, fields);
}

void RuntimeDiagnostics::write_benchmark_trace(const std::string_view event, const std::string_view json_fields) noexcept {
    if (state_) state_->write_benchmark_trace(event, json_fields);
}

}  // namespace mmltk::controller::services
