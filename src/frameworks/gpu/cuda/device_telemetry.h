#pragma once
#include <memory>
#include <optional>
#include "src/frameworks/reflection/reflected_declarations.h"
namespace mmltk::frameworks::gpu {
// Device-wide counters, independent of CUDA work or allocator accounting.
// Missing/unsupported counters remain absent, never fabricated as zero.
struct CudaDeviceTelemetry {
 std::optional<double> gpu_utilization_percent;
 std::optional<double> gpu_memory_used_bytes;
 std::optional<double> gpu_memory_total_bytes;
 std::optional<double> gpu_temperature_celsius;
 bool operator==(const CudaDeviceTelemetry&) const = default;
};
MMLTK_REFLECT_FIELDS(CudaDeviceTelemetry)
// One retained NVML session/physical-device handle, resolved by CUDA PCI identity.
// Call from an existing background owner. No threads, CUDA context, stream wait,
// or subprocess; samples are cached for one second across bursty publications.
class CudaDeviceMonitor final {
public:
 explicit CudaDeviceMonitor(int cuda_ordinal) noexcept;
 ~CudaDeviceMonitor() noexcept;
 CudaDeviceMonitor(const CudaDeviceMonitor&) = delete;
 CudaDeviceMonitor& operator=(const CudaDeviceMonitor&) = delete;
 [[nodiscard]] CudaDeviceTelemetry Sample() noexcept;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::frameworks::gpu
