#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cuda_runtime_api.h>
#include <expected>

namespace mmltk::testsupport {

[[nodiscard]] inline std::expected<int, cudaError_t> classify_cuda_device_count(const cudaError_t status, const int count) {
    if (status == cudaErrorNoDevice || status == cudaErrorInsufficientDriver) { return 0; }
    if (status != cudaSuccess || count < 0) { return std::unexpected(status); }
    return count;
}

[[nodiscard]] inline int checked_cuda_device_count() {
    int count = 0;
    const auto status = cudaGetDeviceCount(&count);
    const auto result = classify_cuda_device_count(status, count);
    INFO("cudaGetDeviceCount status=" << static_cast<int>(status) << ", count=" << count);
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace mmltk::testsupport
