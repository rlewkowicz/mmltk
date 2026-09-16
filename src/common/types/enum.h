#pragma once
#include <array>
#include <cstddef>
#include <utility>
namespace mmltk::common::types::detail {
template <std::size_t SourceSize, std::size_t DestinationSize>
[[nodiscard]] consteval bool claim_enum_mapping_member(std::array<bool, SourceSize>& sources, std::array<bool, DestinationSize>& destinations,
                                                       const std::size_t source_index, const std::size_t destination_index) noexcept {
    if (source_index >= SourceSize || destination_index >= DestinationSize || sources[source_index] || destinations[destination_index]) { return false; }
    sources[source_index] = true;
    destinations[destination_index] = true;
    return true;
}
template <std::size_t SourceSize, std::size_t DestinationSize>
[[nodiscard]] consteval bool enum_mapping_members_complete(const std::array<bool, SourceSize>& sources,
                                                           const std::array<bool, DestinationSize>& destinations) noexcept {
    for (const bool present : sources) {
        if (!present) { return false; }
    }
    for (const bool present : destinations) {
        if (!present) { return false; }
    }
    return true;
}
}  // namespace mmltk::common::types::detail
namespace mmltk::common::types {
template <typename Enum>
[[nodiscard]] constexpr bool valid_counted_enum(const Enum value) noexcept {
    return value >= static_cast<Enum>(0) && value < Enum::Count;
}
template <typename Source, typename Destination, std::size_t Size>
[[nodiscard]] consteval bool complete_counted_enum_mapping(const std::array<std::pair<Source, Destination>, Size>& mapping) noexcept {
    constexpr std::size_t source_count = static_cast<std::size_t>(Source::Count);
    constexpr std::size_t destination_count = static_cast<std::size_t>(Destination::Count);
    if constexpr (Size != source_count || Size != destination_count) {
        return false;
    } else {
        std::array<bool, source_count> sources{};
        std::array<bool, destination_count> destinations{};
        for (const auto& [source, destination] : mapping) {
            if (!valid_counted_enum(source) || !valid_counted_enum(destination)) { return false; }
            const std::size_t source_index = static_cast<std::size_t>(source);
            const std::size_t destination_index = static_cast<std::size_t>(destination);
            if (!detail::claim_enum_mapping_member(sources, destinations, source_index, destination_index)) { return false; }
        }
        return detail::enum_mapping_members_complete(sources, destinations);
    }
}
template <typename Source, typename Destination, std::size_t Size, typename DestinationEntry, std::size_t DestinationSize>
[[nodiscard]] consteval bool complete_counted_enum_mapping(const std::array<std::pair<Source, Destination>, Size>& mapping,
                                                           const std::array<DestinationEntry, DestinationSize>& destination_roster) noexcept {
    constexpr std::size_t source_count = static_cast<std::size_t>(Source::Count);
    if constexpr (Size != source_count || Size != DestinationSize) {
        return false;
    } else {
        std::array<bool, source_count> sources{};
        std::array<bool, DestinationSize> destinations{};
        for (std::size_t left = 0U; left < DestinationSize; ++left) {
            for (std::size_t right = left + 1U; right < DestinationSize; ++right) {
                if (destination_roster[left].value == destination_roster[right].value) { return false; }
            }
        }
        for (const auto& [source, destination] : mapping) {
            if (!valid_counted_enum(source)) { return false; }
            const std::size_t source_index = static_cast<std::size_t>(source);
            std::size_t destination_index = DestinationSize;
            for (std::size_t index = 0U; index < DestinationSize; ++index) {
                if (destination_roster[index].value == destination) {
                    destination_index = index;
                    break;
                }
            }
            if (!detail::claim_enum_mapping_member(sources, destinations, source_index, destination_index)) { return false; }
        }
        return detail::enum_mapping_members_complete(sources, destinations);
    }
}
template <typename Destination, typename Source, std::size_t Size>
[[nodiscard]] constexpr Destination map_counted_enum(const Source value, const std::array<std::pair<Source, Destination>, Size>& mapping,
                                                     const Destination fallback) noexcept {
    if (!valid_counted_enum(value)) { return fallback; }
    for (const auto& [source, destination] : mapping) {
        if (source == value) { return destination; }
    }
    return fallback;
}
}  // namespace mmltk::common::types
