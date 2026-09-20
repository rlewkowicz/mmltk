#pragma once
#include <catch2/catch_test_macros.hpp>
#include <cuda_runtime_api.h>
#include <expected>
#include <cstdio>
#include <cstdlib>
#include <string>
namespace mmltk::testsupport {
class ScopedTestStream final {
   public:
    ScopedTestStream() { REQUIRE(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) == cudaSuccess); }
    ~ScopedTestStream() { static_cast<void>(cudaStreamDestroy(stream_)); }
    ScopedTestStream(const ScopedTestStream&) = delete;
    ScopedTestStream& operator=(const ScopedTestStream&) = delete;
    [[nodiscard]] cudaStream_t get() const noexcept { return stream_; }

   private:
    cudaStream_t stream_ = nullptr;
};
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
[[nodiscard]] inline std::string select_cuda_test_device(const int ordinal) {
    REQUIRE(cudaSetDevice(ordinal) == cudaSuccess);
    cudaDeviceProp device{};
    REQUIRE(cudaGetDeviceProperties(&device, ordinal) == cudaSuccess);
    return "CUDA device " + std::to_string(ordinal) + ": " + device.name + ", CC " + std::to_string(device.major) + '.' + std::to_string(device.minor);
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
