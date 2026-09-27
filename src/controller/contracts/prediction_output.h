#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <cstdint>
namespace mmltk::controller::contracts {
enum class PredictionCompiledSampling : std::uint8_t { Percent, Total };
enum class PredictionVideoSaving : std::uint8_t { Samples, Full };
MMLTK_REFLECT_ENUM(PredictionCompiledSampling)
MMLTK_REFLECT_ENUM(PredictionVideoSaving)
struct PredictionOutputSettings final {
 bool single_enabled = true;
 bool compiled_enabled = true;
 PredictionCompiledSampling compiled_mode = PredictionCompiledSampling::Percent;
 MMLTK_MINIMUM(unsigned, 1U) MMLTK_MAXIMUM(unsigned, 100U) unsigned compiled_percent = 10U;
 MMLTK_MINIMUM(std::uint64_t, 1U) std::uint64_t compiled_total = 6U;
 bool video_enabled = true;
 PredictionVideoSaving video_mode = PredictionVideoSaving::Full;
 MMLTK_MINIMUM(std::uint64_t, 1U) std::uint64_t video_samples = 6U;
 bool operator==(const PredictionOutputSettings&) const = default;
};
struct PredictionRunPreview final {
 bool labels = true, boxes = true, masks = true;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_MAXIMUM(float, 1.0F) MMLTK_FINITE float confidence_threshold = 0.25F;
 bool operator==(const PredictionRunPreview&) const = default;
};
MMLTK_REFLECT_FIELDS(PredictionOutputSettings)
MMLTK_REFLECT_FIELDS(PredictionRunPreview)
}  // namespace mmltk::controller::contracts
