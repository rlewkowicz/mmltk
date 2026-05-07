#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mmltk::browser::contract {

inline constexpr std::uint32_t kProtocolVersion = 2U;

struct WorkflowSpec {
    std::string_view id;
    std::string_view label;
};

inline constexpr std::array<WorkflowSpec, 6U> kWorkflows{{
    {"train", "Train"},
    {"validate", "Validate"},
    {"predict", "Predict"},
    {"annotate", "Annotate"},
    {"export", "Export"},
    {"live", "Live"},
}};

struct PresetSpec {
    std::string_view preset_name;
    std::string_view display_name;
};

inline constexpr std::array<PresetSpec, 10U> kPresets{{
    {"rf-detr-nano", "RF-DETR Nano"},
    {"rf-detr-small", "RF-DETR Small"},
    {"rf-detr-medium", "RF-DETR Medium"},
    {"rf-detr-large", "RF-DETR Large"},
    {"rf-detr-seg-nano", "RF-DETR Seg Nano"},
    {"rf-detr-seg-small", "RF-DETR Seg Small"},
    {"rf-detr-seg-medium", "RF-DETR Seg Medium"},
    {"rf-detr-seg-large", "RF-DETR Seg Large"},
    {"rf-detr-seg-xlarge", "RF-DETR Seg XLarge"},
    {"rf-detr-seg-xxlarge", "RF-DETR Seg XXLarge"},
}};

[[nodiscard]] consteval bool metadata_is_valid() {
    for (std::size_t i = 0U; i < kWorkflows.size(); ++i) {
        for (std::size_t j = i + 1U; j < kWorkflows.size(); ++j) {
            if (kWorkflows[i].id == kWorkflows[j].id) {
                return false;
            }
        }
    }
    for (std::size_t i = 0U; i < kPresets.size(); ++i) {
        for (std::size_t j = i + 1U; j < kPresets.size(); ++j) {
            if (kPresets[i].preset_name == kPresets[j].preset_name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(metadata_is_valid(), "native browser contract metadata contains duplicates or invalid references");

}  // namespace mmltk::browser::contract
