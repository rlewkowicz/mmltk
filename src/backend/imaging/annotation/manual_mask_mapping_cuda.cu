#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include "detail/manual_mask_mapping_cuda_abi.h"
namespace mmltk::backend::imaging::annotation::detail {
namespace {
[[nodiscard]] __device__ int global_thread_x() noexcept { return static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); }
[[nodiscard]] __device__ int global_thread_y() noexcept { return static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y); }
__device__ void store_rgba(const MutableRgbaSurfaceAbi& surface, const int x, const int y, const RgbaColorAbi color) noexcept {
    std::uint8_t* pixel = surface.pixels + static_cast<std::size_t>(y) * surface.pitch_bytes + static_cast<std::size_t>(x) * 4U;
    pixel[0] = color.red;
    pixel[1] = color.green;
    pixel[2] = color.blue;
    pixel[3] = color.alpha;
}
__global__ void deferred_manual_mask_runs_kernel(const DeferredManualMaskRunsLaunchAbi launch) {
    const int x = global_thread_x();
    const int y = global_thread_y();
    if (x >= launch.overlay_region.width || y >= launch.overlay_region.height) { return; }
    const ManualMaskMappingAbi& mapping = launch.mapping;
    const std::uint32_t output_x = mapping.view_x + launch.region_capture_x + static_cast<std::uint32_t>(x);
    const std::uint32_t output_y = mapping.view_y + launch.region_capture_y + static_cast<std::uint32_t>(y);
    const std::uint32_t source_x = mapping.source_crop_x + project_crop_coordinate(output_x, mapping.source_crop_width, mapping.output_width);
    const std::uint32_t source_y = mapping.source_crop_y + project_crop_coordinate(output_y, mapping.source_crop_height, mapping.output_height);
    const std::uint64_t source_pixel = static_cast<std::uint64_t>(source_y) * mapping.source_width + source_x;
    std::uint32_t low = 0U;
    std::uint32_t high = launch.run_count;
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2U;
        const std::size_t pair_index = static_cast<std::size_t>(middle) * 2U;
        const std::uint64_t start = launch.run_pairs[pair_index];
        const std::uint64_t end = start + launch.run_pairs[pair_index + 1U];
        if (source_pixel < start) {
            high = middle;
        } else if (source_pixel >= end) {
            low = middle + 1U;
        } else {
            store_rgba(launch.overlay_region, x, y, launch.color);
            return;
        }
    }
}
}  // namespace
cudaError_t launch_deferred_manual_mask_runs_cuda(const DeferredManualMaskRunsLaunchAbi& launch) noexcept {
    const ManualMaskMappingAbi& mapping = launch.mapping;
    if (launch.stream == nullptr || launch.overlay_region.pixels == nullptr || launch.overlay_region.width <= 0 || launch.overlay_region.height <= 0 ||
        launch.overlay_region.pitch_bytes < static_cast<std::size_t>(launch.overlay_region.width) * 4U || launch.run_pairs == nullptr ||
        launch.run_count == 0U || mapping.source_width == 0U || mapping.source_height == 0U || mapping.source_crop_width == 0U ||
        mapping.source_crop_height == 0U || mapping.output_width == 0U || mapping.output_height == 0U ||
        static_cast<std::uint64_t>(mapping.source_crop_x) + mapping.source_crop_width > mapping.source_width ||
        static_cast<std::uint64_t>(mapping.source_crop_y) + mapping.source_crop_height > mapping.source_height ||
        static_cast<std::uint64_t>(mapping.view_x) + launch.region_capture_x + static_cast<std::uint32_t>(launch.overlay_region.width) > mapping.output_width ||
        static_cast<std::uint64_t>(mapping.view_y) + launch.region_capture_y + static_cast<std::uint32_t>(launch.overlay_region.height) >
            mapping.output_height) {
        return cudaErrorInvalidValue;
    }
    const dim3 block{16U, 16U, 1U};
    const dim3 grid{
        (static_cast<unsigned int>(launch.overlay_region.width) + block.x - 1U) / block.x,
        (static_cast<unsigned int>(launch.overlay_region.height) + block.y - 1U) / block.y,
        1U,
    };
    deferred_manual_mask_runs_kernel<<<grid, block, 0U, launch.stream>>>(launch);
    return cudaPeekAtLastError();
}
}  // namespace mmltk::backend::imaging::annotation::detail
