#pragma once
#include <catch2/catch_test_macros.hpp>
#include <cuda_runtime_api.h>
#include <expected>
#include <cstdio>
#include <cstdlib>
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
// These checks may run on worker threads, where Catch assertions are disabled.
// Preserve the operation text and the existing process-abort failure policy.
inline void cuda_require(cudaError_t err, const char* expr) {
    if (err == cudaSuccess) { return; }
    std::fprintf(stderr, "CUDA call failed: %s: %s\n", expr, cudaGetErrorString(err));
    std::abort();
}
}  // namespace mmltk::testsupport
#define CUDA_ASSERT_OK(call) ::mmltk::testsupport::cuda_require((call), #call)
