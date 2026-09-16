#pragma once
#include <cstdint>
namespace mmltk::backend::models::rfdetr {
// Final output-image erasure. Channel dropout deliberately has no spatial effect.
struct AugmentationSpatialErasure {
    std::uint64_t key = 0;
    float dropout_probability = 0.0F;
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    std::uint32_t rectangular = 0;
    constexpr bool operator==(const AugmentationSpatialErasure&) const noexcept = default;
};
// Effects are evaluated in unit RGB; only model consumers request normalization.
enum class GpuAugmentationOutputDomain : std::uint8_t {
    ModelNormalized,
    UnitRgb,
};
enum class GpuPreprocessOutputType : std::uint8_t {
    Float32,
    Float16,
    BFloat16,
};
}  // namespace mmltk::backend::models::rfdetr
