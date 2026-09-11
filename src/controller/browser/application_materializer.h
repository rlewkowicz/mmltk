#pragma once

#include <array>
#include <cstddef>
#include <concepts>
#include <expected>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "src/controller/browser/application_schema.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/frameworks/serialization/serialization.h"

namespace mmltk::controller::browser {
namespace application_materializer_detail {

template <class Value>
[[nodiscard]] std::expected<wire::Value, wire::ErrorCode> reflected_value(const Value& value) {
    auto result = mmltk::frameworks::serialization::reflected_value(value);
    if (!result) return std::unexpected(result.error().code);
    return std::move(*result);
}

template <class Schema, class Endpoint>
[[nodiscard]] std::expected<typename Endpoint::request_type, ApplicationErrorRecord> materialize_request(
    const std::vector<IntentField>& fields) {
    using Request = typename Endpoint::request_type;
    wire::Value::Object object;
    object.reserve(fields.size());
    std::size_t matched = 0U;
    bool duplicate = false;
    Schema::template VisitRequestFields<Endpoint>([&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& fact) {
        const auto first = std::ranges::find(fields, fact.stable_id, &IntentField::field_id);
        if (first == fields.end()) return;
        if (std::ranges::find(std::ranges::subrange(std::next(first), fields.end()), fact.stable_id, &IntentField::field_id) !=
            fields.end()) {
            duplicate = true;
            return;
        }
        object.emplace_back(std::string(fact.name), first->value);
        ++matched;
    });
    if (duplicate || matched != fields.size()) {
        return std::unexpected(ApplicationErrorRecord{.category = mmltk::controller::contracts::ApplicationErrorCategory::InvalidIntent,
                                                      .detail = "intent contains an unknown or duplicate field"});
    }
    Request request{};
    if (!mmltk::frameworks::serialization::decode_into(request, wire::Value(std::move(object)))) {
        return std::unexpected(ApplicationErrorRecord{.category = mmltk::controller::contracts::ApplicationErrorCategory::InvalidIntent,
                                                      .detail = "intent fields do not satisfy the reflected request declaration"});
    }
    return request;
}

template <class Result>
[[nodiscard]] wire::Value encode_result(Result&& result) {
    auto encoded = reflected_value(result);
    if (!encoded) throw std::runtime_error("intent result cannot be encoded");
    return std::move(*encoded);
}

[[nodiscard]] inline wire::Value void_result() { return wire::Value(wire::Value::Object{}); }

}  // namespace application_materializer_detail

template <class Composition>
[[nodiscard]] IntentReply dispatch_intent(Composition& systems, Intent intent) noexcept {
    IntentReply reply{.correlation = intent.correlation, .result = {}, .error = {}};
    try {
        bool found = false;
        ApplicationSchema<Composition>::VisitEndpoints([&]<class Endpoint>() {
            if constexpr (!Endpoint::interaction) {
                if (found || intent.endpoint_id != Endpoint::stable_id) return;
                found = true;
                auto* system = systems.*Endpoint::system_cell::pointer;
                if (system == nullptr) throw mmltk::controller::contracts::UnavailableError("application system is unavailable");
                if constexpr (Endpoint::signature::has_request) {
                    auto request =
                        application_materializer_detail::materialize_request<ApplicationSchema<Composition>, Endpoint>(intent.fields);
                    if (!request) throw mmltk::controller::contracts::InvalidIntentError(request.error().detail);
                    if constexpr (std::is_void_v<typename Endpoint::result_type>) {
                        std::invoke(Endpoint::method, *system, std::move(*request));
                        reply.result = application_materializer_detail::void_result();
                    } else {
                        reply.result =
                            application_materializer_detail::encode_result(std::invoke(Endpoint::method, *system, std::move(*request)));
                    }
                } else {
                    if (!intent.fields.empty())
                        throw mmltk::controller::contracts::InvalidIntentError("parameterless intent contains fields");
                    if constexpr (std::is_void_v<typename Endpoint::result_type>) {
                        std::invoke(Endpoint::method, *system);
                        reply.result = application_materializer_detail::void_result();
                    } else {
                        reply.result = application_materializer_detail::encode_result(std::invoke(Endpoint::method, *system));
                    }
                }
            }
        });
        if (!found) throw mmltk::controller::contracts::InvalidIntentError("unknown intent endpoint");
    } catch (...) {
        reply.result.reset();
        reply.error = map_current_exception();
    }
    return reply;
}

enum class InteractionDispatchDisposition : std::uint8_t {
    Accepted,
    ProtocolInvalid,
    ApplicationRejected,
};

struct InteractionDispatchResult final {
    InteractionDispatchDisposition disposition = InteractionDispatchDisposition::ProtocolInvalid;
    std::uint64_t endpoint_id = 0U;
    std::string_view endpoint_name{};
    std::uint64_t generation = 0U;
    std::optional<ApplicationErrorRecord> error{};
};

template <class Composition>
[[nodiscard]] InteractionDispatchResult dispatch_interaction(Composition& systems, const InteractionView interaction) noexcept {
    InteractionDispatchResult result{.endpoint_id = interaction.Get<&Interaction::endpoint_id>()};
    try {
        bool found = false;
        ApplicationSchema<Composition>::VisitEndpoints([&]<class Endpoint>() {
            if constexpr (Endpoint::interaction) {
                if (found || interaction.Get<&Interaction::endpoint_id>() != Endpoint::stable_id) return;
                found = true;
                result.endpoint_name = Endpoint::name;
                typename Endpoint::request_type request{};
                if (!mmltk::frameworks::serialization::decode_compact_into(
                        request, interaction.Get<&Interaction::value>(),
                        {.max_bytes = kMaxIntentValueBytes, .max_items = kMaxIntentValueItems, .max_depth = kMaxIntentValueDepth})) {
                    return;
                }
                try {
                    auto* system = systems.*Endpoint::system_cell::pointer;
                    if (system == nullptr) throw mmltk::controller::contracts::UnavailableError("application system is unavailable");
                    std::invoke(Endpoint::method, *system, std::move(request));
                    if constexpr (requires { system->LastInteractionGeneration(); })
                        result.generation = system->LastInteractionGeneration();
                    result.disposition = InteractionDispatchDisposition::Accepted;
                } catch (...) {
                    result.disposition = InteractionDispatchDisposition::ApplicationRejected;
                    result.error = map_current_exception();
                }
            }
        });
        if (!found) return result;
    } catch (...) { return result; }
    return result;
}

template <class Composition>
[[nodiscard]] InteractionDispatchResult dispatch_interaction(Composition& systems, const Interaction& interaction) noexcept {
    return dispatch_interaction(systems, InteractionView{interaction});
}

template <class Composition>
[[nodiscard]] std::vector<SystemSnapshot> encode_application_snapshots(const Composition& systems) {
    std::vector<SystemSnapshot> snapshots;
    snapshots.reserve(kMaxSnapshotCount);
    ApplicationSchema<Composition>::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        auto* system = systems.*SystemCell::pointer;
        if (system == nullptr) return;
        auto value = application_materializer_detail::reflected_value(std::invoke(&[:Snapshot:], *system));
        if (!value) throw std::runtime_error("system snapshot cannot be encoded");
        snapshots.push_back({.system_id = SystemCell::stable_id, .value = std::move(*value)});
    });
    return snapshots;
}

template <auto Member, class Event, class Composition = mmltk::controller::ApplicationSystems>
[[nodiscard]] SystemEvent encode_system_event(const Event& event) {
    using Descriptor = ApplicationEventDescriptor<Composition, Member, Event>;
    auto value = application_materializer_detail::reflected_value(event);
    if (!value) throw std::runtime_error("system event cannot be encoded");
    return {.system_id = Descriptor::system_id,
            .event_id = Descriptor::event_id,
            .delivery = Descriptor::delivery,
            .state_revision = Descriptor::StateRevision(event),
            .value = std::move(*value)};
}

template <class Composition>
[[nodiscard]] auto materialize_visual_source_readers(const Composition& systems) {
    using Schema = ApplicationSchema<Composition>;
    std::array<VisualSourceReader, Schema::VisualSourceCount()> readers{};
    std::size_t index = 0U;
    Schema::VisitVisualSources([&]<class Cell, std::meta::info Snapshot, class Projection>() {
        auto* system = systems.*Cell::pointer;
        if (system == nullptr) throw std::logic_error("visual source is unavailable during application construction");
        readers[index++] = {
            .source = {Projection::kind, 1U},
            .observe =
                [system] {
                    if constexpr (requires {
                                      { system->ObserveSource() } -> std::same_as<VisualSourceObservation>;
                                  })
                        return system->ObserveSource();
                    else
                        return Projection::Observe(std::invoke(&[:Snapshot:], *system));
                },
            .borrow = [system] { return system->BorrowFrame(); },
            .observe_workspace = [system] { return system->ObserveWorkspace(); },
            .borrow_workspace = [system] { return system->BorrowWorkspace(); },
            .request_workspace = [system](VisualWorkspaceRequest request) { system->RequestWorkspace(std::move(request)); },
        };
    });
    return readers;
}

template <class Composition>
[[nodiscard]] Bootstrap materialize_bootstrap(const Composition& systems) {
    return {.schema_fingerprint = application_schema_fingerprint<Composition>().words, .snapshots = encode_application_snapshots(systems)};
}

}  // namespace mmltk::controller::browser
