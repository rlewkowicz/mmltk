module;
#include <cstddef>
#include <cstdint>

export module mmltk.backend.imaging.upscale.image_upscaler_types;

export namespace mmltk::backend::imaging::upscale {

enum class ImageUpscalerKind : std::uint8_t {
    ShiftLUT,
    RealPLKSR,
    Count,
};

struct ImageUpscalerRequest {
    const float* device_pixels = nullptr;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t crop_x = 0U;
    std::uint32_t crop_y = 0U;
    std::uint32_t crop_width = 0U;
    std::uint32_t crop_height = 0U;
};

static_assert(static_cast<std::size_t>(ImageUpscalerKind::Count) == 2U);

}  // namespace mmltk::backend::imaging::upscale
