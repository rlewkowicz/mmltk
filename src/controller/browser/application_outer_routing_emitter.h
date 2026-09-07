#pragma once

#include <cstddef>
#include <meta>
#include <ostream>
#include <string>
#include <string_view>

#include "src/controller/browser/application_schema.h"

namespace mmltk::controller::browser {

template <class Writer>
concept ApplicationOuterRoutingWriter = requires(Writer& writer, std::string_view text) {
    { writer.output() } -> std::same_as<std::ostream&>;
    writer.reserve(text, text, text);
    { writer.identifier(text, true) } -> std::same_as<std::string>;
    { writer.template rust_type<void>() } -> std::same_as<std::string>;
    { writer.template native_source<void>() } -> std::same_as<std::string>;
};

template <class Composition, ApplicationOuterRoutingWriter Writer>
void emit_application_outer_routing(Writer& writer) {
    using Schema = ApplicationSchema<Composition>;
    auto& output = writer.output();
    const auto variant = [&writer](const std::string_view system, const std::string_view member = {}) {
        auto value = writer.identifier(system, true);
        if (!member.empty()) value += writer.identifier(member, true);
        return value;
    };
    const auto system_trait = [&writer](const std::string_view system) {
        return writer.identifier(system, true) + "ApplicationProjection";
    };

    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        const auto symbol = "SYSTEM_" + writer.identifier(SystemCell::name, true);
        writer.reserve("module", symbol, "system " + std::string(SystemCell::name));
        output << "pub const " << symbol << ": u64 = " << SystemCell::stable_id << ";\n";
    });
    Schema::VisitEndpoints([&]<class Endpoint>() {
        const auto symbol =
            "ENDPOINT_" + writer.identifier(Endpoint::system_cell::name, true) + '_' + writer.identifier(Endpoint::name, true);
        writer.reserve("module", symbol, "endpoint " + std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name));
        output << "pub const " << symbol << ": u64 = " << Endpoint::stable_id << ";\n";
    });
    Schema::VisitEvents([&]<class Identity, class Event>(const auto&) {
        const auto event_name = mmltk::frameworks::serialization::reflected_schema_type_name<Event>();
        const auto symbol = "EVENT_" + writer.identifier(Identity::system_cell::name, true) + '_' + writer.identifier(event_name, true);
        writer.reserve("module", symbol,
                       "event " + std::string(Identity::system_cell::name) + "." + writer.template native_source<Event>());
        output << "pub const " << symbol << ": u64 = " << Identity::event_id << ";\n";
    });

    for (const std::string_view symbol : {"ApplicationSystem",
                                          "ApplicationEndpoint",
                                          "ApplicationIntentEndpoint",
                                          "EncodedApplicationIntent",
                                          "application_system_stable_id",
                                          "application_system_from_stable_id",
                                          "application_endpoint_stable_id",
                                          "application_intent_endpoint_stable_id",
                                          "decode_application_intent_endpoint",
                                          "application_intent_system",
                                          "application_reply_endpoint",
                                          "ApplicationSnapshot",
                                          "ApplicationEvent",
                                          "ApplicationReply",
                                          "APPLICATION_SNAPSHOT_COUNT",
                                          "application_snapshot_kind",
                                          "application_snapshot_system",
                                          "application_event_system",
                                          "application_bootstrap_complete",
                                          "decode_application_snapshot",
                                          "decode_application_event",
                                          "application_event_delivery",
                                          "decode_application_reply",
                                          "dispatch_application_snapshot",
                                          "dispatch_application_event",
                                          "dispatch_application_reply",
                                          "ApplicationProjection"}) {
        writer.reserve("module", symbol, "generated application outer routing");
    }

    output << "\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum ApplicationSystem {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() { output << "    " << variant(SystemCell::name) << ",\n"; });
    output << "}\npub const fn application_system_stable_id(system: ApplicationSystem) -> u64 { match system {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    ApplicationSystem::" << variant(SystemCell::name) << " => " << SystemCell::stable_id << ",\n";
    });
    output << "} }\npub const fn application_system_from_stable_id(stable_id: u64) -> Option<ApplicationSystem> { "
              "match stable_id {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    " << SystemCell::stable_id << " => Some(ApplicationSystem::" << variant(SystemCell::name) << "),\n";
    });
    output << "    _ => None, } }\n"
              "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum ApplicationEndpoint {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() { output << "    " << variant(Endpoint::system_cell::name, Endpoint::name) << ",\n"; });
    output << "}\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum ApplicationIntentEndpoint {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction) output << "    " << variant(Endpoint::system_cell::name, Endpoint::name) << ",\n";
    });
    output << "}\npub const fn application_endpoint_stable_id(endpoint: ApplicationEndpoint) -> u64 { match endpoint {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        output << "    ApplicationEndpoint::" << variant(Endpoint::system_cell::name, Endpoint::name) << " => " << Endpoint::stable_id
               << ",\n";
    });
    output << "} }\npub const fn decode_application_intent_endpoint(stable_id: u64) "
              "-> Option<ApplicationIntentEndpoint> { match stable_id {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction)
            output << "    " << Endpoint::stable_id
                   << " => Some(ApplicationIntentEndpoint::" << variant(Endpoint::system_cell::name, Endpoint::name) << "),\n";
    });
    output << "    _ => None, } }\npub const fn application_intent_endpoint_stable_id("
              "endpoint: ApplicationIntentEndpoint) -> u64 { match endpoint {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction)
            output << "    ApplicationIntentEndpoint::" << variant(Endpoint::system_cell::name, Endpoint::name) << " => "
                   << Endpoint::stable_id << ",\n";
    });
    output << "} }\npub const fn application_intent_system(endpoint: ApplicationIntentEndpoint) "
              "-> ApplicationSystem { match endpoint {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction)
            output << "    ApplicationIntentEndpoint::" << variant(Endpoint::system_cell::name, Endpoint::name)
                   << " => ApplicationSystem::" << variant(Endpoint::system_cell::name) << ",\n";
    });
    output << "} }\n#[derive(Debug, Clone, PartialEq)]\npub struct EncodedApplicationIntent { "
              "pub endpoint: ApplicationIntentEndpoint, pub record: Intent }\n";

    output << "\n#[derive(Debug, Clone, PartialEq)]\npub enum ApplicationSnapshot {\n";
    std::size_t snapshot_count = 0U;
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
        const auto name = variant(SystemCell::name);
        writer.reserve("enum ApplicationSnapshot", name, "system " + std::string(SystemCell::name));
        output << "    " << name << '(' << writer.template rust_type<typename Signature::result_type>() << "),\n";
        ++snapshot_count;
    });
    output << "}\npub const APPLICATION_SNAPSHOT_COUNT: usize = " << snapshot_count
           << ";\npub const fn application_snapshot_kind(value: &ApplicationSnapshot) -> usize { match value {\n";
    std::size_t snapshot_kind = 0U;
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    ApplicationSnapshot::" << variant(SystemCell::name) << "(_) => " << snapshot_kind++ << ",\n";
    });
    output << "} }\npub const fn application_snapshot_system(value: &ApplicationSnapshot) -> ApplicationSystem { "
              "match value {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    ApplicationSnapshot::" << variant(SystemCell::name) << "(_) => ApplicationSystem::" << variant(SystemCell::name)
               << ",\n";
    });
    output << "} }\npub fn application_bootstrap_complete(values: &[ApplicationSnapshot]) -> bool {\n"
              "    if values.len() != APPLICATION_SNAPSHOT_COUNT { return false; }\n"
              "    let mut seen = [false; APPLICATION_SNAPSHOT_COUNT];\n"
              "    for value in values { let kind = application_snapshot_kind(value); "
              "if std::mem::replace(&mut seen[kind], true) { return false; } }\n"
              "    seen.into_iter().all(|present| present)\n"
              "}\n#[derive(Debug, Clone, PartialEq)]\npub enum ApplicationEvent {\n";
    Schema::VisitEvents([&]<class Identity, class Event>(const auto&) {
        const auto name = variant(Identity::system_cell::name, mmltk::frameworks::serialization::reflected_schema_type_name<Event>());
        writer.reserve("enum ApplicationEvent", name,
                       "event " + std::string(Identity::system_cell::name) + "." + writer.template native_source<Event>());
        output << "    " << name << '(' << writer.template rust_type<Event>() << "),\n";
    });
    output << "}\npub const fn application_event_system(value: &ApplicationEvent) -> ApplicationSystem { "
              "match value {\n";
    Schema::VisitEvents([&]<class Identity, class Event>(const auto&) {
        output << "    ApplicationEvent::"
               << variant(Identity::system_cell::name, mmltk::frameworks::serialization::reflected_schema_type_name<Event>())
               << "(_) => ApplicationSystem::" << variant(Identity::system_cell::name) << ",\n";
    });
    output << "} }\n#[derive(Debug, Clone, PartialEq)]\npub enum ApplicationReply {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction) {
            const auto name = variant(Endpoint::system_cell::name, Endpoint::name);
            writer.reserve("enum ApplicationReply", name,
                           "endpoint " + std::string(Endpoint::system_cell::name) + "." + std::string(Endpoint::name));
            output << "    " << name << '(' << writer.template rust_type<typename Endpoint::result_type>() << "),\n";
        }
    });
    output << "}\npub const fn application_reply_endpoint(reply: &ApplicationReply) -> ApplicationIntentEndpoint { "
              "match reply {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction)
            output << "    ApplicationReply::" << variant(Endpoint::system_cell::name, Endpoint::name)
                   << "(_) => ApplicationIntentEndpoint::" << variant(Endpoint::system_cell::name, Endpoint::name) << ",\n";
    });
    output << "} }\n\npub fn decode_application_snapshot(system_id: u64, value: Value) "
              "-> Result<ApplicationSnapshot, String> { match system_id {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    " << SystemCell::stable_id << " => Ok(ApplicationSnapshot::" << variant(SystemCell::name)
               << "(FromApplicationValue::from_application_value(value)?)),\n";
    });
    output << "    _ => Err(\"unknown application snapshot\".into()), } }\n"
              "pub fn decode_application_event(system_id: u64, event_id: u64, value: Value) "
              "-> Result<ApplicationEvent, String> { match (system_id, event_id) {\n";
    Schema::VisitEvents([&]<class Identity, class Event>(const auto&) {
        output << "    (" << Identity::system_id << ", " << Identity::event_id << ") => Ok(ApplicationEvent::"
               << variant(Identity::system_cell::name, mmltk::frameworks::serialization::reflected_schema_type_name<Event>())
               << "(FromApplicationValue::from_application_value(value)?)),\n";
    });
    output << "    _ => Err(\"unknown application event\".into()), } }\n"
              "pub fn application_event_delivery(system_id: u64, event_id: u64) -> Option<EventDelivery> { "
              "match (system_id, event_id) {\n";
    Schema::VisitEvents([&]<class Identity, class Event>(const mmltk::controller::contracts::reflection::Event metadata) {
        output << "    (" << Identity::system_id << ", " << Identity::event_id
               << ") => Some(EventDelivery::" << mmltk::frameworks::reflection::enum_name(metadata.delivery) << "),\n";
    });
    output << "    _ => None, } }\npub fn decode_application_reply(endpoint_id: u64, value: Value) "
              "-> Result<ApplicationReply, String> { match endpoint_id {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction)
            output << "    " << Endpoint::stable_id << " => Ok(ApplicationReply::" << variant(Endpoint::system_cell::name, Endpoint::name)
                   << "(FromApplicationValue::from_application_value(value)?)),\n";
    });
    output << "    _ => Err(\"unknown application reply\".into()), } }\n\n";

    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        using Signature = SystemMethodSignature<decltype(&[:Snapshot:])>;
        const auto trait = system_trait(SystemCell::name);
        writer.reserve("module", trait, "system " + std::string(SystemCell::name) + " static projection");
        output << "pub trait " << trait << "<Error> {\n"
               << "    fn project_" << writer.identifier(SystemCell::name, false)
               << "_snapshot(&mut self, value: " << writer.template rust_type<typename Signature::result_type>()
               << ") -> Result<(), Error>;\n"
               << "    fn project_" << writer.identifier(SystemCell::name, false) << "_event(&mut self, event: ApplicationEvent);\n"
               << "    fn project_" << writer.identifier(SystemCell::name, false)
               << "_reply(&mut self, correlation: u64, reply: ApplicationReply);\n"
               << "}\n";
    });
    output << "pub trait ApplicationProjection<Error>: ";
    bool first = true;
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        if (!first) output << " + ";
        first = false;
        output << system_trait(SystemCell::name) << "<Error>";
    });
    output << " {}\nimpl<T, Error> ApplicationProjection<Error> for T where T: ";
    first = true;
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        if (!first) output << " + ";
        first = false;
        output << system_trait(SystemCell::name) << "<Error>";
    });
    output << " {}\npub fn dispatch_application_snapshot<T, Error>(target: &mut T, snapshot: ApplicationSnapshot) "
              "-> Result<(), Error> where T: ApplicationProjection<Error> { match snapshot {\n";
    Schema::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        output << "    ApplicationSnapshot::" << variant(SystemCell::name) << "(value) => target.project_"
               << writer.identifier(SystemCell::name, false) << "_snapshot(value),\n";
    });
    output << "} }\npub fn dispatch_application_event<T, Error>(target: &mut T, event: ApplicationEvent) "
              "where T: ApplicationProjection<Error> { match event {\n";
    Schema::VisitEvents([&]<class Identity, class Event>(const auto&) {
        const auto name = variant(Identity::system_cell::name, mmltk::frameworks::serialization::reflected_schema_type_name<Event>());
        output << "    ApplicationEvent::" << name << "(value) => target.project_" << writer.identifier(Identity::system_cell::name, false)
               << "_event(ApplicationEvent::" << name << "(value)),\n";
    });
    output << "} }\npub fn dispatch_application_reply<T, Error>(target: &mut T, correlation: u64, "
              "reply: ApplicationReply) where T: ApplicationProjection<Error> { match reply {\n";
    Schema::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction) {
            const auto name = variant(Endpoint::system_cell::name, Endpoint::name);
            output << "    ApplicationReply::" << name << "(value) => target.project_"
                   << writer.identifier(Endpoint::system_cell::name, false) << "_reply(correlation, ApplicationReply::" << name
                   << "(value)),\n";
        }
    });
    output << "} }\n";
}

}  // namespace mmltk::controller::browser
