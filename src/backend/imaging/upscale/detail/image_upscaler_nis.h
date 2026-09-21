#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace mmltk::backend::imaging::upscale::image_upscaler_nis {
// CLEANUP-IGNORE: Configuration is the vendor-defined NIS ABI; a shared base with Explore's render plan would
// conflate independent crop, output, and execution contracts.
struct Configuration final {
 // CLEANUP-IGNORE: This NIS vendor ABI preserves independently versioned source and crop geometry.
 std::uint32_t source_width = 0U;
 std::uint32_t source_height = 0U;
 std::uint32_t crop_x = 0U;
 std::uint32_t crop_y = 0U;
 std::uint32_t crop_width = 0U;
 std::uint32_t crop_height = 0U;
 std::uint32_t output_width = 0U;
 std::uint32_t output_height = 0U;
};
struct ScratchRequirements final {
 std::size_t horizontal_bytes = 0U;
 std::size_t scaled_bytes = 0U;
};
[[nodiscard]] std::optional<ScratchRequirements> scratch_requirements(const Configuration& config) noexcept;
[[nodiscard]] cudaError_t launch_scale(const void* source, std::size_t source_pitch, void* horizontal, void* scaled, const Configuration& config,
                                       cudaStream_t stream) noexcept;
[[nodiscard]] cudaError_t launch_sharpen(const void* source, std::size_t source_pitch, const void* scaled, std::uint8_t* target, std::size_t target_pitch,
                                         const Configuration& config, cudaStream_t stream) noexcept;
}  // namespace mmltk::backend::imaging::upscale::image_upscaler_nis
