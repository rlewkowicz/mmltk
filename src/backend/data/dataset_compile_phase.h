#pragma once
#include <cstdint>
#include <string_view>
#include <utility>
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::data {
enum class DatasetCompilePhase : std::uint8_t {
    Idle,
    Planning,
    Downloading,
    Indexing,
    Extracting,
    Labels,
    Pixels,
    Syncing,
    Publishing,
};
[[nodiscard]] inline constexpr std::string_view dataset_compile_phase_label(const DatasetCompilePhase phase) noexcept {
    switch (phase) {
        case DatasetCompilePhase::Idle: return "idle";
        case DatasetCompilePhase::Planning: return "planning";
        case DatasetCompilePhase::Downloading: return "downloading";
        case DatasetCompilePhase::Indexing: return "indexing";
        case DatasetCompilePhase::Extracting: return "extracting";
        case DatasetCompilePhase::Labels: return "labels";
        case DatasetCompilePhase::Pixels: return "pixels";
        case DatasetCompilePhase::Syncing: return "syncing";
        case DatasetCompilePhase::Publishing: return "publishing";
    }
    std::unreachable();
}
MMLTK_REFLECT_ENUM(DatasetCompilePhase)
}  // namespace mmltk::backend::data
