#pragma once

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>
#include <cstdint>
#include <cstddef>

namespace mmltk::rfdetr {

inline void ensure_cuda_ok(cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

cudaError_t launch_bgr_split_to_planar_float(const std::uint8_t* src, std::size_t src_pitch_bytes,
                                             std::uint32_t src_width, std::uint32_t src_height, float* dst,
                                             std::uint32_t dst_width, std::uint32_t dst_height, cudaStream_t stream);

cudaError_t launch_bgr_vertical_flip_in_place_pitched(std::uint8_t* buffer, std::size_t pitch_bytes,
                                                      std::uint32_t width, std::uint32_t height, cudaStream_t stream);

const char* validate_bgr_split_to_planar_float_args(std::size_t src_pitch_bytes, std::uint32_t src_width,
                                                    std::uint32_t src_height, std::uint32_t dst_width,
                                                    std::uint32_t dst_height, cudaStream_t stream);

}  
