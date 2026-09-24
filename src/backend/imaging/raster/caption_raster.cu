#include "detail/caption_raster_cuda.h"
#include "native_caption_metrics.h"
#include <cuda_runtime.h>
namespace mmltk::backend::imaging::raster::detail {
namespace {
__global__ void composite(std::uint8_t* clean, std::size_t pitch, const std::uint8_t* semantic, std::size_t semantic_pitch, unsigned width, unsigned height) {
 const unsigned x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
 if (x >= width || y >= height) return;
 const auto column = static_cast<std::size_t>(x) * 4U;
 auto* output = clean + y * pitch + column;
 const auto* overlay = semantic + y * semantic_pitch + column;
 const unsigned alpha = overlay[3];
 for (unsigned channel = 0; channel < 3; ++channel) output[channel] = static_cast<std::uint8_t>((unsigned(output[channel]) * (255U - alpha) + unsigned(overlay[channel]) * alpha + 127U) / 255U);
 output[3] = 255U;
}
// Each block visits only one caption's bounded rectangle. Atomic priority
// selects the final opaque painter; the second pass shades only its pixels.
// Work is O(image pixels + sum of caption areas), not image × labels.
template <bool Paint>
__global__ void captions(std::uint8_t* pixels, std::size_t pitch, unsigned width, unsigned height, const std::uint8_t* atlas, std::size_t atlas_pitch, const CaptionCommand* commands,
 std::uint32_t* winners, std::size_t winner_pitch) {
 const auto item = commands[blockIdx.x];
 constexpr unsigned rows = kNativeCaptionHeight;
 for (unsigned offset = threadIdx.x; offset < item.width * rows; offset += blockDim.x) {
  const unsigned x = offset % item.width, y = offset / item.width;
  const auto px = static_cast<long long>(item.x) + x, py = static_cast<long long>(item.y) + y;
  if (px < 0 || py < 0 || px >= width || py >= height) continue;
  auto* winner = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(winners) + py * winner_pitch) + px;
  if constexpr (!Paint)
   atomicMax(winner, blockIdx.x + 1U);
  else {
   if (*winner != blockIdx.x + 1U) continue;
   auto* output = pixels + py * pitch + px * 4U;
   const unsigned r = item.color & 255U, g = (item.color >> 8U) & 255U, b = (item.color >> 16U) & 255U;
   const unsigned foreground = r * 2126U + g * 7152U + b * 722U > 1402500U ? 0U : 255U;
   unsigned alpha = x < item.name_width ? atlas[(item.name * rows + y) * atlas_pitch + static_cast<std::size_t>(x) * 4U] : 0U;
   if (item.suffix_count && x >= item.name_width - 4U) {
    alpha = 0U;
    // At most 32 sorted glyph intervals, independent of catalog/history size.
    unsigned low = 0U, high = item.suffix_count;
    while (low < high) {
     const unsigned middle = (low + high) / 2U;
     if (x >= item.suffix[middle].end)
      low = middle + 1U;
     else
      high = middle;
    }
    if (low < item.suffix_count) {
     const auto glyph = item.suffix[low];
     if (x >= glyph.begin) alpha = atlas[(glyph.row * rows + y) * atlas_pitch + static_cast<std::size_t>(x - glyph.begin + 4U) * 4U];
    }
   }
   for (unsigned channel = 0; channel < 3U; ++channel) {
    const unsigned background = (item.color >> (channel * 8U)) & 255U;
    output[channel] = static_cast<std::uint8_t>((background * (255U - alpha) + foreground * alpha + 127U) / 255U);
   }
   output[3] = 255U;
  }
 }
}
}  // namespace
int composite_caption_source(std::uint8_t* clean, std::size_t pitch, const std::uint8_t* semantic, std::size_t semantic_pitch, unsigned width, unsigned height, cudaStream_t stream) {
 composite<<<dim3((width + 15U) / 16U, (height + 15U) / 16U), dim3(16, 16), 0, stream>>>(clean, pitch, semantic, semantic_pitch, width, height);
 return cudaGetLastError();
}
int paint_captions(std::uint8_t* pixels, std::size_t pitch, unsigned width, unsigned height, const std::uint8_t* atlas, std::size_t atlas_pitch, const CaptionCommand* commands, unsigned count,
 std::uint32_t* winners, std::size_t winner_pitch, cudaStream_t stream) {
 auto status = cudaMemset2DAsync(winners, winner_pitch, 0, static_cast<std::size_t>(width) * sizeof(std::uint32_t), height, stream);
 if (status != cudaSuccess) return status;
 captions<false><<<count, 256, 0, stream>>>(pixels, pitch, width, height, atlas, atlas_pitch, commands, winners, winner_pitch);
 status = cudaGetLastError();
 if (status != cudaSuccess) return status;
 captions<true><<<count, 256, 0, stream>>>(pixels, pitch, width, height, atlas, atlas_pitch, commands, winners, winner_pitch);
 return cudaGetLastError();
}
}  // namespace mmltk::backend::imaging::raster::detail
