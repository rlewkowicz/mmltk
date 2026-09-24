#include "video_frame_convert.h"
#include <cuda_runtime.h>
#include <algorithm>
namespace mmltk::backend::media::video {
namespace {
__device__ float sample(VideoPlane plane, unsigned x, unsigned y) {
 const auto* address = plane.data + y * plane.pitch + static_cast<std::size_t>(x) * plane.step + plane.offset;
 const auto value = plane.depth + plane.shift > 8 ? *reinterpret_cast<const std::uint16_t*>(address) : *address;
 return static_cast<float>(value >> plane.shift) / static_cast<float>((1U << plane.depth) - 1U);
}
__global__ void convert(VideoColorConversion input, unsigned width, unsigned height, float* target) {
 const auto x = blockIdx.x * blockDim.x + threadIdx.x;
 if (x >= width) return;
 // The capped grid covers tall admitted frames without a second launch.
 // One final increment stays below UINT_MAX for the source's int heights.
 const auto row_stride = blockDim.y * gridDim.y;
 for (auto y = blockIdx.y * blockDim.y + threadIdx.y; y < height; y += row_stride) {
  float first = sample(input.planes[0], x, y);
  float second = sample(input.planes[1], x >> input.chroma_x, y >> input.chroma_y);
  float third = sample(input.planes[2], x >> input.chroma_x, y >> input.chroma_y);
  if (!input.rgb) {
   const float scale = static_cast<float>(1U << (input.planes[0].depth - 8));
   const float maximum = static_cast<float>((1U << input.planes[0].depth) - 1U);
   const float yy = input.full_range ? first : (first * maximum - 16.0F * scale) / (219.0F * scale);
   const float cb = (second * maximum - 128.0F * scale) / ((input.full_range ? maximum : 224.0F * scale));
   const float cr = (third * maximum - 128.0F * scale) / ((input.full_range ? maximum : 224.0F * scale));
   first = yy + 2.0F * (1.0F - input.kr) * cr;
   third = yy + 2.0F * (1.0F - input.kb) * cb;
   second = (yy - input.kr * first - input.kb * third) / (1.0F - input.kr - input.kb);
  }
  unsigned target_x = x, target_y = y, target_width = width;
  switch (input.clockwise_quarters) {
   case 1U:
    target_x = height - 1U - y;
    target_y = x;
    target_width = height;
    break;
   case 2U:
    target_x = width - 1U - x;
    target_y = height - 1U - y;
    break;
   case 3U:
    target_x = y;
    target_y = width - 1U - x;
    target_width = height;
    break;
   default: break;
  }
  const auto offset = static_cast<std::size_t>(target_y) * target_width + target_x;
  const auto plane = static_cast<std::size_t>(width) * height;
  target[offset] = fminf(1.0F, fmaxf(0.0F, first));
  target[plane + offset] = fminf(1.0F, fmaxf(0.0F, second));
  target[2U * plane + offset] = fminf(1.0F, fmaxf(0.0F, third));
 }
}
}  // namespace
int convert_video_chw(VideoColorConversion input, std::uint32_t width, std::uint32_t height, float* target, cudaStream_t stream) noexcept {
 convert<<<dim3((width + 15U) / 16U, std::min((height + 15U) / 16U, 65535U)), dim3(16, 16), 0, stream>>>(input, width, height, target);
 return cudaGetLastError();
}
}  // namespace mmltk::backend::media::video
namespace mmltk::backend::media::video {
namespace {
__device__ float output_component(const std::uint8_t* rgba, std::size_t pitch, unsigned width, unsigned height, unsigned x, unsigned y, unsigned channel) {
 return rgba[static_cast<std::size_t>(min(y, height - 1U)) * pitch + static_cast<std::size_t>(min(x, width - 1U)) * 4U + channel];
}
__device__ std::uint8_t output_byte(float value) { return static_cast<std::uint8_t>(fminf(255.0F, fmaxf(0.0F, roundf(value)))); }
__global__ void encode_nv12(const std::uint8_t* rgba, std::size_t pitch, unsigned width, unsigned height, std::uint8_t* luma, std::size_t y_pitch, std::uint8_t* chroma, std::size_t uv_pitch) {
 const unsigned x = (blockIdx.x * blockDim.x + threadIdx.x) * 2U, y = (blockIdx.y * blockDim.y + threadIdx.y) * 2U;
 if (x >= width || y >= height) return;
 float red = 0, green = 0, blue = 0;
 for (unsigned dy = 0; dy < 2U; ++dy)
  for (unsigned dx = 0; dx < 2U; ++dx) {
   const float r = output_component(rgba, pitch, width, height, x + dx, y + dy, 0);
   const float g = output_component(rgba, pitch, width, height, x + dx, y + dy, 1);
   const float b = output_component(rgba, pitch, width, height, x + dx, y + dy, 2);
   luma[(y + dy) * y_pitch + x + dx] = output_byte(16.0F + 0.182586F * r + 0.614231F * g + 0.062007F * b);
   red += r;
   green += g;
   blue += b;
  }
 chroma[(y / 2U) * uv_pitch + x] = output_byte(128.0F + (-0.100644F * red - 0.338572F * green + 0.439216F * blue) * 0.25F);
 chroma[(y / 2U) * uv_pitch + x + 1U] = output_byte(128.0F + (0.439216F * red - 0.398942F * green - 0.040274F * blue) * 0.25F);
}
}  // namespace
int convert_video_nv12(
 const std::uint8_t* rgba, std::size_t pitch, unsigned width, unsigned height, std::uint8_t* y, std::size_t y_pitch, std::uint8_t* uv, std::size_t uv_pitch, cudaStream_t stream) noexcept {
 encode_nv12<<<dim3((width + 31U) / 32U, (height + 31U) / 32U), dim3(16, 16), 0, stream>>>(rgba, pitch, width, height, y, y_pitch, uv, uv_pitch);
 return cudaGetLastError();
}
}  // namespace mmltk::backend::media::video
