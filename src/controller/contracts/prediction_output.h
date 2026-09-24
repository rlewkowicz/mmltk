#pragma once
#include <cstdint>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::controller::contracts {
enum class PredictionCompiledSampling : std::uint8_t { Percent, Total };
enum class PredictionVideoSaving : std::uint8_t { Samples, Full };
MMLTK_REFLECT_ENUM(PredictionCompiledSampling)
MMLTK_REFLECT_ENUM(PredictionVideoSaving)
struct PredictionOutputSettings final {
 bool single_enabled = true;
 bool compiled_enabled = true;
 PredictionCompiledSampling compiled_mode = PredictionCompiledSampling::Percent;
 [[= mmltk::frameworks::reflection::Minimum<unsigned>{1U}]][[= mmltk::frameworks::reflection::Maximum<unsigned>{100U}]] unsigned compiled_percent = 10U;
 [[= mmltk::frameworks::reflection::Minimum<std::uint64_t>{1U}]] std::uint64_t compiled_total = 6U;
 bool video_enabled = true;
 PredictionVideoSaving video_mode = PredictionVideoSaving::Full;
 [[= mmltk::frameworks::reflection::Minimum<std::uint64_t>{1U}]] std::uint64_t video_samples = 6U;
 bool operator==(const PredictionOutputSettings&) const = default;
};
struct PredictionRunPreview final {
 bool labels = true, boxes = true, masks = true;
 [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{1.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float confidence_threshold = 0.25F;
 bool operator==(const PredictionRunPreview&) const = default;
};
MMLTK_REFLECT_FIELDS(PredictionOutputSettings)
MMLTK_REFLECT_FIELDS(PredictionRunPreview)
}  // namespace mmltk::controller::contracts
