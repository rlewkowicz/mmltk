#pragma once
#include <cuda_runtime.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace mmltk::frameworks::gpu::launch {
inline constexpr int kDefaultLinearThreads = 256;
inline int linear_blocks_for(const int item_count, const int threads = kDefaultLinearThreads) {
 const int safe_threads = std::max(1, threads);
 const int safe_items = std::max(0, item_count);
 return safe_items / safe_threads + static_cast<int>(safe_items % safe_threads != 0);
}
inline dim3 make_2d_grid(const int width, const int height, const dim3 block = dim3(16, 16, 1)) {
 const auto blocks_for = [](const int extent, const unsigned int threads) {
  const auto safe_extent = static_cast<unsigned int>(std::max(0, extent));
  const auto safe_threads = std::max(1U, threads);
  return safe_extent / safe_threads + static_cast<unsigned int>(safe_extent % safe_threads != 0U);
 };
 return dim3(blocks_for(width, block.x), blocks_for(height, block.y), 1);
}
}  // namespace mmltk::frameworks::gpu::launch
