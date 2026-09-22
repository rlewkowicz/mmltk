#include "detail/shiftlut_lookup_cuda.h"
#include "detail/shiftlut_model_format.h"
#include <cuda_runtime.h>
namespace mmltk::backend::imaging::upscale::shiftlut {
namespace {
struct Position {
 int y;
 int x;
};
// Coordinates in rot90(input, rotation) -> coordinates in input.
__device__ Position unrotate(int y, int x, int height, int width, int rotation) {
 switch (rotation) {
  case 1: return {x, width - 1 - y};
  case 2: return {height - 1 - y, width - 1 - x};
  case 3: return {height - 1 - x, y};
  default: return {y, x};
 }
}
__device__ float centered(const float* input, int channel, int y, int x, int height, int width, int rotation) {
 const auto source = unrotate(y, x, height, width, rotation);
 return fminf(127.0F, fmaxf(-128.0F, input[(channel * height + source.y) * width + source.x] - 128.0F));
}
__device__ float high(float value) { return floorf(value / 4.0F); }
__device__ float low(float value) { return truncf(value - floorf(value / 4.0F) * 4.0F); }
__device__ float clamp_high(float value) { return fminf(31.0F, fmaxf(-32.0F, value)); }
// Upstream Query is followed by Python sum in input order and torch.round.
// Explicit additions prohibit reassociation. PyTorch's CUDA scalar division
// multiplies by its FP32 reciprocal (BinaryDivTrueKernel.cu), then round uses
// ties-to-even; retain that rounding boundary rather than reassociating it.
__device__ float rounded_average(float sum, float reciprocal) { return nearbyintf(__fmul_rn(sum, reciprocal)); }
template <TableFamily Family>
__device__ float lookup(const float* table, int output, int input, int value) {
 constexpr auto shape = dimensions(Family);
 return table[(output * static_cast<int>(shape.inner) + input) * static_cast<int>(shape.domain) + value];
}
__device__ int shifted_pixel(int y, int x, int height, int width, const float* shifts, int channel) {
 return min(height - 1, max(0, y + static_cast<int>(shifts[channel]))) * width + min(width - 1, max(0, x + static_cast<int>(shifts[kChannels + channel])));
}
struct RotatedThread final {
 int plane = 0;
 int batch = 0;
 int channel = 0;
 int width = 0;
 int height = 0;
 int y = 0;
 int x = 0;
};
__device__ bool locate_rotated_thread(const int index, const int height, const int width, RotatedThread& thread) {
 thread.plane = height * width;
 if (index >= kRotatedBatch * kChannels * thread.plane) return false;
 thread.batch = index / (kChannels * thread.plane);
 thread.channel = index / thread.plane % kChannels;
 thread.width = thread.batch / kRgbChannels % 2 ? height : width;
 thread.height = thread.batch / kRgbChannels % 2 ? width : height;
 thread.y = index % thread.plane / thread.width;
 thread.x = index % thread.width;
 return true;
}
__global__ void record_shifted_decisions(const std::int8_t* source, const float* shifts, std::int8_t* target, int height, int width) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 RotatedThread thread;
 if (!locate_rotated_thread(index, height, width, thread)) return;
 const int pixel = shifted_pixel(thread.y, thread.x, thread.height, thread.width, shifts, thread.channel);
 target[index] = source[(thread.batch * kChannels + thread.channel) * thread.plane + pixel];
}
__global__ void initial_depthwise(const float* input, const float* tables, std::int8_t* target, int height, int width) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 RotatedThread thread;
 if (!locate_rotated_thread(index, height, width, thread)) return;
 const int rotation = thread.batch / kRgbChannels;
 float msb = 0;
 float lsb = 0;
 for (int tap = 0; tap < kSpatialTaps; ++tap) {
  const auto sample =
   centered(input, thread.batch % kRgbChannels, min(thread.height - 1, max(0, thread.y + tap / 3 - 1)), min(thread.width - 1, max(0, thread.x + tap % 3 - 1)), height, width, rotation);
  constexpr auto high_offset = table_offset(TableFamily::Depthwise);
  msb = __fadd_rn(msb, lookup<TableFamily::Depthwise>(tables + high_offset, thread.channel, tap, static_cast<int>(high(sample)) + 32));
  lsb = __fadd_rn(lsb, lookup<TableFamily::Low>(tables, thread.channel, tap, static_cast<int>(low(sample))));
 }
 const auto residual = centered(input, thread.batch % kRgbChannels, thread.y, thread.x, height, width, rotation);
 const float high_result = rounded_average(msb, 1.0F / 9.0F) + high(residual);
 const float low_result = fminf(3.0F, fmaxf(0.0F, rounded_average(lsb, 1.0F / 9.0F) + low(residual)));
 target[index] = static_cast<std::int8_t>(clamp_high(high_result + low_result));
}
__global__ void depthwise(const std::int8_t* source, const float* table, std::int8_t* target, int height, int width) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 RotatedThread thread;
 if (!locate_rotated_thread(index, height, width, thread)) return;
 const auto image_offset = (static_cast<std::size_t>(thread.batch) * static_cast<std::size_t>(kChannels) + static_cast<std::size_t>(thread.channel)) * static_cast<std::size_t>(thread.plane);
 const auto* image = source + image_offset;
 float sum = 0;
 for (int tap = 0; tap < kSpatialTaps; ++tap) {
  const int sy = min(thread.height - 1, max(0, thread.y + tap / 3 - 1));
  const int sx = min(thread.width - 1, max(0, thread.x + tap % 3 - 1));
  sum = __fadd_rn(sum, lookup<TableFamily::Depthwise>(table, thread.channel, tap, static_cast<int>(image[sy * thread.width + sx]) + 32));
 }
 target[index] = static_cast<std::int8_t>(clamp_high(rounded_average(sum, 1.0F / 9.0F) + static_cast<float>(source[index])));
}
__global__ void shifted_pointwise(const std::int8_t* source, const float* table, const float* shifts, std::int8_t* target, int height, int width) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 RotatedThread thread;
 if (!locate_rotated_thread(index, height, width, thread)) return;
 float sum = 0;
 float residual = 0;
 for (int in = 0; in < kChannels; ++in) {
  const int pixel = shifted_pixel(thread.y, thread.x, thread.height, thread.width, shifts, in);
  const float value = source[(thread.batch * kChannels + in) * thread.plane + pixel];
  if (in == thread.channel) residual = value;
  sum = __fadd_rn(sum, lookup<TableFamily::Pointwise>(table, thread.channel, in, static_cast<int>(value) + 32));
 }
 target[index] = static_cast<std::int8_t>(clamp_high(rounded_average(sum, 1.0F / 16.0F) + residual));
}
__global__ void restore(const std::int8_t* source, const float* table, float* output, int height, int width) {
 const int plane = height * width;
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 if (index >= kRgbChannels * kOutputPhases * plane) return;
 const int rgb = index / (kOutputPhases * plane);
 const int y = index % (kOutputPhases * plane) / (width * kScale);
 const int x = index % (width * kScale);
 float accumulated = 0;
 for (int rotation = 0; rotation < kRotations; ++rotation) {
  // Rotate an output coordinate into each branch before pixel shuffle.
  const int rotated_width = rotation % 2 ? height : width;
  const int rotated_height = rotation % 2 ? width : height;
  const auto position = unrotate(y, x, rotated_height * kScale, rotated_width * kScale, (kRotations - rotation) % kRotations);
  const int channel = position.y % kScale * kScale + position.x % kScale;
  const int pixel = position.y / kScale * rotated_width + position.x / kScale;
  const int batch = rotation * kRgbChannels + rgb;
  float sum = 0;
  for (int in = 0; in < kChannels; ++in) {
   const int value = static_cast<int>(source[(batch * kChannels + in) * plane + pixel]) + 32;
   sum = __fadd_rn(sum, lookup<TableFamily::Up>(table, channel, in, value));
  }
  const float restored = rounded_average(sum, 1.0F / 16.0F) + static_cast<float>(source[(batch * kChannels + channel) * plane + pixel]);
  accumulated += fminf(127.0F, fmaxf(-128.0F, restored));
 }
 output[index] = fminf(255.0F, fmaxf(0.0F, accumulated + 128.0F));
}
}  // namespace
cudaError_t enqueue(
 const float* input, const float* tables, std::int8_t* first, std::int8_t* second, float* output, std::uint32_t height, std::uint32_t width, cudaStream_t stream, std::int8_t* decisions) {
 const auto pixels = static_cast<std::size_t>(height) * width;
 const auto image_elements = scratch_elements(pixels);
 const auto blocks = (image_elements + 255U) / 256U;
 const auto kernel_height = static_cast<int>(height);
 const auto kernel_width = static_cast<int>(width);
 initial_depthwise<<<blocks, 256, 0, stream>>>(input, tables, first, kernel_height, kernel_width);
 for (std::size_t index = 0; index < kStages; ++index) {
  const auto stage = static_cast<Stage>(index);
  const auto* shifts = tables + table_offset(TableFamily::Shifts, stage);
  if (stage != Stage::First) depthwise<<<blocks, 256, 0, stream>>>(second, tables + table_offset(TableFamily::Depthwise, stage), first, kernel_height, kernel_width);
  if (decisions != nullptr) record_shifted_decisions<<<blocks, 256, 0, stream>>>(first, shifts, decisions + decision_offset(stage, Decision::Shifted, pixels), kernel_height, kernel_width);
  shifted_pointwise<<<blocks, 256, 0, stream>>>(first, tables + table_offset(TableFamily::Pointwise, stage), shifts, second, kernel_height, kernel_width);
  if (decisions != nullptr) {
   const auto copied = cudaMemcpyAsync(decisions + decision_offset(stage, Decision::Pointwise, pixels), second, image_elements * sizeof(std::int8_t), cudaMemcpyDeviceToDevice, stream);
   if (copied != cudaSuccess) return copied;
  }
 }
 const auto output_elements = static_cast<std::size_t>(kRgbChannels) * static_cast<std::size_t>(kOutputPhases) * pixels;
 restore<<<(output_elements + 255U) / 256U, 256, 0, stream>>>(second, tables + table_offset(TableFamily::Up), output, kernel_height, kernel_width);
 return cudaPeekAtLastError();
}
}  // namespace mmltk::backend::imaging::upscale::shiftlut
