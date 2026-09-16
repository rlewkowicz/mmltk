module;
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include "detail/cuda_utils_cuda.h"
module mmltk.backend.ml.cuda.cuda_utils;
namespace mmltk::backend::ml::cuda {
const char* validate_bgr_split_to_planar_float_args(const std::size_t src_pitch_bytes, const std::uint32_t src_width, const std::uint32_t src_height,
                                                    const std::uint32_t dst_width, const std::uint32_t dst_height, const std::uintptr_t stream) {
    return detail::validate_bgr_split_to_planar_float_args(src_pitch_bytes, src_width, src_height, dst_width, dst_height,
                                                           reinterpret_cast<cudaStream_t>(stream));
}
std::int32_t launch_bgr_split_to_planar_float(const std::uint8_t* src, const std::size_t src_pitch_bytes, const std::uint32_t src_width,
                                              const std::uint32_t src_height, float* dst, const std::uint32_t dst_width, const std::uint32_t dst_height,
                                              const std::uintptr_t stream) {
    return static_cast<std::int32_t>(detail::launch_bgr_split_to_planar_float(src, src_pitch_bytes, src_width, src_height, dst, dst_width, dst_height,
                                                                              reinterpret_cast<cudaStream_t>(stream)));
}
std::int32_t launch_bgr_vertical_flip_in_place_pitched(std::uint8_t* buffer, const std::size_t pitch_bytes, const std::uint32_t width,
                                                       const std::uint32_t height, const std::uintptr_t stream) {
    return static_cast<std::int32_t>(
        detail::launch_bgr_vertical_flip_in_place_pitched(buffer, pitch_bytes, width, height, reinterpret_cast<cudaStream_t>(stream)));
}
}  // namespace mmltk::backend::ml::cuda
