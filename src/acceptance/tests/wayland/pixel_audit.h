#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include "surface_audit.h"
namespace mmltk::acceptance::wayland {
struct PixelBoundaryAudit final {
    explicit PixelBoundaryAudit(const bool pixel_enabled = true) : enabled(pixel_enabled) {}
    bool enabled = true;
    using Key = std::pair<std::string, std::uint64_t>;
    struct Sample final {
        std::uint64_t x = 0U, y = 0U;
        std::uint32_t rgba = 0U;
        bool operator==(const Sample&) const = default;
    };
    struct Boundary final {
        nlohmann::json identity;
        nlohmann::json publication_fact;
        std::array<std::optional<Sample>, 25> values{};
        [[nodiscard]] bool complete() const {
            return std::ranges::all_of(values, [](const auto& value) { return value.has_value(); });
        }
    };
    struct Publication final {
        // Reoffers retain the Presentation revision but own different timeline
        // transfers. The Firefox physical receipt selects its native attempt.
        std::map<std::pair<std::string, std::uint64_t>, Boundary> native;
        std::array<Boundary, 4> receivers;
        nlohmann::json forwarded;
        std::array<bool, 3> counted{};
        bool direct_counted = false;
        bool viewer = false;
        [[nodiscard]] bool direct() const { return !forwarded.empty() && forwarded.value("direct_sampling", false); }
        [[nodiscard]] bool complete() const {
            return direct() ? direct_counted : std::ranges::all_of(counted, [](const bool value) { return value; });
        }
    };
    std::map<Key, Publication> samples;
    std::vector<nlohmann::json> probe_failures;
    std::set<std::string> failed_probe_retirements;
    struct ProbeFailureEvidence final {
        bool forwarded = false;
        bool recovered = false;
        [[nodiscard]] bool complete() const noexcept { return forwarded && recovered; }
    };
    [[nodiscard]] ProbeFailureEvidence probe_failure_evidence(std::string_view expected) const;
    [[nodiscard]] bool copy_probe_omission_proven() const;
    [[nodiscard]] bool probe_failure_complete(std::string_view expected) const;
    std::array<std::size_t, 3> joined{};
    std::size_t direct_joined = 0U;
    struct Composition final {
        nlohmann::json identity;
        std::map<std::uint64_t, std::array<std::optional<nlohmann::json>, 4>> cards;
        std::set<std::uint64_t> expected;
        bool summary = false;
        [[nodiscard]] bool complete() const {
            return summary && !expected.empty() && cards.size() == expected.size() && std::ranges::all_of(cards, [&](const auto& card) {
                       return expected.contains(card.first) && std::ranges::all_of(card.second, [](const auto& value) { return value.has_value(); });
                   });
        }
    };
    using CompositionKey = std::tuple<std::uint64_t, std::string, std::uint64_t>;
    std::map<CompositionKey, Composition> compositions;
    std::string failure;
    bool retained_logical_content = false;
    bool upscale_growth = false;
    bool canvas_seen = false;
    std::size_t viewer_canvas_joins = 0U;
    std::set<std::uint64_t> cached_methods;
    std::vector<std::uint64_t> stop_observations;
    std::uint64_t departure_begin = 0U;
    std::uint64_t departure_end = 0U;
    bool settings_preserved = false;
    bool basic_reentry = false;
    bool reconnected = false;
    unsigned navigation_stage = 0U;
    bool route_persistence = false;
    bool authoritative_route = false;
    std::uint64_t route_revision = 0U;
    std::array<std::uint64_t, 3> reentry_product{};
    std::optional<std::pair<std::uint64_t, std::uint64_t>> successful_viewer;
    void consume_continuity(const nlohmann::json& record, std::string_view event);
    [[nodiscard]] bool raw_complete() const;
    [[nodiscard]] bool viewer_nonblack_complete() const;
    static bool colored(std::uint32_t value);
    static bool black(std::uint32_t value);
    void reject(std::string_view reason);
    static nlohmann::json identity_of(const nlohmann::json& record, std::size_t boundary);
    void reconcile(Publication& publication);
    void consume(const nlohmann::json& record);
    [[nodiscard]] bool composition_complete() const;
    [[nodiscard]] bool continuity_complete(bool require_gallery = true) const;
};
}  // namespace mmltk::acceptance::wayland
