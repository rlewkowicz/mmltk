#pragma once
#include <string>
#include <string_view>
#include "src/controller/browser/application_outer_routing_emitter.h"
#include "src/controller/presentation/visual_source_projection.h"
namespace mmltk::controller::browser {
template <class Root, auto Access, ApplicationOuterRoutingWriter Writer>
[[nodiscard]] std::string visual_projection_path(Writer& writer) {
    constexpr auto reflected = mmltk::frameworks::reflection::reflected_member_path<Root, Access>();
    std::string result;
    auto remaining = reflected.view();
    while (!remaining.empty()) {
        const auto end = remaining.find('.');
        if (!result.empty()) result += '.';
        result += writer.identifier(remaining.substr(0U, end), false);
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1U);
    }
    return result;
}
template <class Composition, ApplicationOuterRoutingWriter Writer>
void emit_application_visual_projection(Writer& writer) {
    using Schema = ApplicationSchema<Composition>;
    constexpr auto count = Schema::VisualSourceCount();
    auto& output = writer.output();
    for (const auto symbol : {"ApplicationVisualSnapshots", "ApplicationVisualObservation", "presentation_source_session", "visual_clean_content_identity"})
        writer.reserve("module", symbol, "canonical visual source projections");
    const auto kind_type = writer.template rust_type<PresentationSourceKind>();
    output << "pub const fn presentation_source_session(kind: " << kind_type << ") -> u64 { match kind {\n";
    for (const auto source : presentation_source_metadata)
        output << kind_type << "::" << writer.identifier(mmltk::frameworks::reflection::enum_name(source.kind), true) << " => " << source.session << ",\n";
    output << "} }\n";
    output << "pub struct ApplicationVisualObservation<'a> {\n";
    Schema::template VisitFields<VisualSourceObservation>([&]<class, class Declaration>(const auto& fact) {
        using Member = typename Declaration::member_type;
        const auto field = writer.identifier(fact.member_name, false);
        writer.reserve("struct ApplicationVisualObservation", field, "native observation " + std::string(fact.member_name));
        output << "pub " << field << ": " << (std::same_as<Member, VisualFrame> ? "&'a " : "") << writer.template rust_type<Member>() << ",\n";
    });
    output << "}\n";
    output << "pub struct ApplicationVisualSnapshots" << (count == 0U ? "" : "<'a>") << " {\n";
    Schema::VisitVisualSources([&]<class Cell, std::meta::info, class Projection>() {
        const auto field = writer.identifier(Cell::name, false);
        writer.reserve("struct ApplicationVisualSnapshots", field, "visual source " + std::string(Cell::name));
        output << "pub " << field << ": Option<&'a " << writer.template rust_type<typename Projection::snapshot_type>() << ">,\n";
    });
    output << "}\nimpl" << (count == 0U ? "" : "<'a>") << " ApplicationVisualSnapshots" << (count == 0U ? "" : "<'a>")
           << " { pub fn observe(&self, kind: " << kind_type << ") -> Option<ApplicationVisualObservation<'" << (count == 0U ? "static" : "a")
           << ">> { match kind {\n";
    for (const auto source : presentation_source_metadata) {
        output << kind_type << "::" << writer.identifier(mmltk::frameworks::reflection::enum_name(source.kind), true) << " => ";
        bool found = false;
        Schema::VisitVisualSources([&]<class Cell, std::meta::info, class Projection>() {
            if (source.kind != Projection::kind) return;
            found = true;
            output << "self." << writer.identifier(Cell::name, false) << ".map(|snapshot| ApplicationVisualObservation { ";
            Projection::relation::VisitMembers([&]<class Entry>() {
                constexpr bool frame =
                    std::same_as<mmltk::frameworks::reflection::accessor_value_t<typename Projection::snapshot_type, Entry::source>, VisualFrame>;
                output << visual_projection_path<VisualSourceObservation, Entry::destination>(writer) << ": " << (frame ? "&" : "") << "snapshot."
                       << visual_projection_path<typename Projection::snapshot_type, Entry::source>(writer) << ", ";
            });
            output << "})";
        });
        if (!found) output << "None";
        output << ",\n";
    }
    output << "} } }\n";
    // This is a structural relation with an explicit zero fallback, not native function translation.
    output << "pub fn visual_clean_content_identity(frame: &" << writer.template rust_type<VisualFrame>() << ") -> "
           << writer.template rust_type<VisualCleanContentIdentity>() << " { let mut identity = " << writer.template rust_type<VisualCleanContentIdentity>()
           << " {\n";
    VisualCleanContentRelation::VisitMembers([&]<class Entry>() {
        output << visual_projection_path<VisualCleanContentIdentity, Entry::destination>(writer) << ": frame."
               << visual_projection_path<VisualFrame, Entry::source>(writer) << ".clone(),\n";
    });
    const auto destination = visual_projection_path<VisualCleanContentIdentity, VisualCleanContentRelation::zero_fallback_destination>(writer);
    output << "}; if identity." << destination << " == 0 { identity." << destination << " = frame."
           << visual_projection_path<VisualFrame, VisualCleanContentRelation::zero_fallback_source>(writer) << "; } identity }\n";
}
}  // namespace mmltk::controller::browser
