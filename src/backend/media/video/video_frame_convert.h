#pragma once
#include <cstddef>
#include <cstdint>
#include <cuda_runtime_api.h>
namespace mmltk::backend::media::video {
struct VideoPlane final {
 const std::uint8_t* data = nullptr;
 std::size_t pitch = 0U;
 int step = 1;
 int offset = 0;
 int shift = 0;
 int depth = 8;
};
struct VideoColorConversion final {
 VideoPlane planes[3]{};
 unsigned clockwise_quarters = 0U;
 int chroma_x = 1;
 int chroma_y = 1;
 bool rgb = false;
 bool full_range = false;
 float kr = 0.299F;
 float kb = 0.114F;
};
[[nodiscard]] int convert_video_chw(VideoColorConversion, std::uint32_t width, std::uint32_t height, float* target, cudaStream_t stream) noexcept;
}  // namespace mmltk::backend::media::video
