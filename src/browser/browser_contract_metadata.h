#pragma once

#include "mmltk/rfdetr/preset_catalog.h"

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

inline constexpr const auto& kPresets = mmltk::rfdetr::kPresetCatalog;

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

}  
