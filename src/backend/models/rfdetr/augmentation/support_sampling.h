#pragma once
#include <cuda_runtime.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::models::rfdetr::augment_math {
[[nodiscard]] __host__ __device__ inline std::int64_t support_pixel_index(float coordinate, std::int64_t extent) noexcept {
    return static_cast<std::int64_t>(fminf(static_cast<float>(extent - 1), fmaxf(0.0F, nearbyintf(coordinate * static_cast<float>(extent) - 0.5F))));
}
template <typename Run>
[[nodiscard]] __host__ __device__ bool rle_support_contains(const Run* runs, std::size_t count, std::uint64_t pixel) noexcept {
    std::size_t low = 0, high = count;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (runs[middle].start <= pixel)
            low = middle + 1;
        else
            high = middle;
    }
    return low != 0 && pixel - runs[low - 1].start < runs[low - 1].length;
}
}  // namespace mmltk::backend::models::rfdetr::augment_math
