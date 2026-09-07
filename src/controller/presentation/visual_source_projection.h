#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/controller/presentation/visual_system_types.h"

namespace mmltk::controller {

template <class Snapshot, PresentationSourceKind Kind, auto Frame, auto Revision>
struct VisualSourceProjection final {
    using snapshot_type = Snapshot;
    static constexpr auto kind = Kind;
    static constexpr auto frame = Frame;
    static constexpr auto revision = Revision;

    [[nodiscard]] static consteval bool valid() {
        using namespace mmltk::frameworks::reflection;
        if constexpr (!accessor_is_applicable<Snapshot, Frame>() || !accessor_is_applicable<Snapshot, Revision>()) {
            return false;
        } else if constexpr (!std::same_as<accessor_value_t<Snapshot, Frame>, VisualFrame> ||
                             !std::same_as<accessor_value_t<Snapshot, Revision>, std::uint64_t>) {
            return false;
        } else {
            return Kind != PresentationSourceKind::None && presentation_source_session(Kind) != 0U && relation::valid();
        }
    }

    using relation = mmltk::frameworks::reflection::StaticMemberRelation<
        Snapshot, VisualSourceObservation, 2U,
        mmltk::frameworks::reflection::MemberRelationEntry<Frame, &VisualSourceObservation::frame>,
        mmltk::frameworks::reflection::MemberRelationEntry<Revision, &VisualSourceObservation::snapshot_revision>>;

    [[nodiscard]] static constexpr VisualSourceObservation Observe(const Snapshot& snapshot) {
        static_assert(valid(), "invalid visual snapshot projection");
        VisualSourceObservation result;
        relation::Project(snapshot, result);
        return result;
    }
};

}  // namespace mmltk::controller
