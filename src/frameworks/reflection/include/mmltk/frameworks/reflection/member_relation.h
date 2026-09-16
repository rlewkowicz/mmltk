#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>
#include "mmltk/frameworks/reflection/member_path.h"
namespace mmltk::frameworks::reflection {
struct ExactMemberTransform final {
    template <class Source, class Destination>
    [[nodiscard]] static consteval bool accepts() {
        return std::same_as<std::remove_cvref_t<Source>, std::remove_cvref_t<Destination>>;
    }
    template <class Destination, class Source>
    static constexpr void apply(Destination& destination, const Source& source) noexcept {
        static_assert(accepts<Source, Destination>());
        destination = source;
    }
};
template <auto Source, auto Destination, class Transform = ExactMemberTransform>
struct MemberRelationEntry final {
    static constexpr auto source = Source;
    static constexpr auto destination = Destination;
    using transform = Transform;
};
template <class Source, class Destination, std::size_t ExpectedCount, class... Entries>
struct StaticMemberRelation {
    using source_type = Source;
    using destination_type = Destination;
    static constexpr std::size_t member_count = sizeof...(Entries);
    template <class Visitor>
    static constexpr void VisitMembers(Visitor&& visitor) {
        (visitor.template operator()<Entries>(), ...);
    }
    [[nodiscard]] static consteval bool valid() {
        if constexpr (sizeof...(Entries) != ExpectedCount) return false;
        std::array<ReflectedMemberIdentity, sizeof...(Entries)> sources{};
        std::array<ReflectedMemberIdentity, sizeof...(Entries)> destinations{};
        std::size_t index = 0U;
        bool compatible = true;
        VisitMembers([&]<class Entry>() {
            if constexpr (accessor_is_applicable<Source, Entry::source>() && accessor_is_applicable<Destination, Entry::destination>()) {
                using SourceValue = accessor_value_t<Source, Entry::source>;
                using DestinationValue = accessor_value_t<Destination, Entry::destination>;
                sources[index] = accessor_member_identity<Source, Entry::source>();
                destinations[index] = accessor_member_identity<Destination, Entry::destination>();
                compatible =
                    compatible && Entry::transform::template accepts<SourceValue, DestinationValue>() && sources[index].valid() && destinations[index].valid();
            } else {
                compatible = false;
            }
            ++index;
        });
        if (!compatible) return false;
        for (std::size_t left = 0U; left < sources.size(); ++left) {
            for (std::size_t right = left + 1U; right < sources.size(); ++right) {
                if (sources[left] == sources[right] || destinations[left] == destinations[right]) return false;
            }
        }
        return true;
    }
    template <class SourceValue, class DestinationValue>
    static constexpr void Project(const SourceValue& source, DestinationValue& destination) {
        VisitMembers([&]<class Entry>() {
            Entry::transform::apply(access<DestinationValue, Entry::destination>(destination), access<const SourceValue, Entry::source>(source));
        });
    }
};
template <class Provider>
struct catalog_provider_relation;
template <class Provider>
concept HasCatalogProviderRelation = requires {
    typename catalog_provider_relation<Provider>::source_type;
    typename catalog_provider_relation<Provider>::destination_type;
    typename catalog_provider_relation<Provider>::override_state_type;
};
}  // namespace mmltk::frameworks::reflection
