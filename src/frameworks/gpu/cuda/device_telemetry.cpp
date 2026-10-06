#include "device_telemetry.h"
#include <cuda.h>
#include <nvml.h>
#include <dlfcn.h>
#include <array>
#include <chrono>
namespace mmltk::frameworks::gpu {
struct CudaDeviceMonitor::Impl final {
 void* library = nullptr;
 nvmlDevice_t device = nullptr;
 decltype(&nvmlShutdown) shutdown = nullptr;
 decltype(&nvmlDeviceGetUtilizationRates) utilization = nullptr;
 decltype(&nvmlDeviceGetMemoryInfo) memory = nullptr;
 decltype(&nvmlDeviceGetTemperatureV) temperature = nullptr;
 bool initialized = false;
 std::chrono::steady_clock::time_point next_sample{};
 CudaDeviceTelemetry sample;
 template <class Function>
 Function Resolve(const char* name) noexcept {
  return reinterpret_cast<Function>(::dlsym(library, name));
 }
 explicit Impl(int ordinal) noexcept {
  if (ordinal < 0) return;
  library = ::dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) return;
  const auto initialize = Resolve<decltype(&nvmlInit_v2)>("nvmlInit_v2");
  shutdown = Resolve<decltype(shutdown)>("nvmlShutdown");
  const auto handle = Resolve<decltype(&nvmlDeviceGetHandleByPciBusId_v2)>("nvmlDeviceGetHandleByPciBusId_v2");
  if (!initialize || !shutdown || !handle || initialize() != NVML_SUCCESS) return;
  initialized = true;
  CUdevice selected;
  std::array<char, 32U> pci{};
  if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&selected, ordinal) != CUDA_SUCCESS || cuDeviceGetPCIBusId(pci.data(), pci.size(), selected) != CUDA_SUCCESS ||
      handle(pci.data(), &device) != NVML_SUCCESS) {
   device = nullptr;
   return;
  }
  utilization = Resolve<decltype(utilization)>("nvmlDeviceGetUtilizationRates");
  memory = Resolve<decltype(memory)>("nvmlDeviceGetMemoryInfo");
  temperature = Resolve<decltype(temperature)>("nvmlDeviceGetTemperatureV");
 }
 ~Impl() noexcept {
  if (initialized) static_cast<void>(shutdown());
  if (library) static_cast<void>(::dlclose(library));
 }
 CudaDeviceTelemetry Sample() noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (!device || now < next_sample) return sample;
  next_sample = now + std::chrono::seconds(1);
  sample = {};
  nvmlUtilization_t activity{};
  if (utilization && utilization(device, &activity) == NVML_SUCCESS && activity.gpu <= 100U) sample.gpu_utilization_percent = activity.gpu;
  nvmlMemory_t bytes{};
  if (memory && memory(device, &bytes) == NVML_SUCCESS && bytes.total != 0U && bytes.used <= bytes.total) {
   sample.gpu_memory_used_bytes = static_cast<double>(bytes.used);
   sample.gpu_memory_total_bytes = static_cast<double>(bytes.total);
  }
  nvmlTemperature_t celsius{};
  celsius.version = nvmlTemperature_v1;
  celsius.sensorType = NVML_TEMPERATURE_GPU;
  if (temperature && temperature(device, &celsius) == NVML_SUCCESS) sample.gpu_temperature_celsius = celsius.temperature;
  return sample;
 }
};
CudaDeviceMonitor::CudaDeviceMonitor(int ordinal) noexcept {
 try {
  impl_ = std::make_unique<Impl>(ordinal);
 } catch (...) {}
}
CudaDeviceMonitor::~CudaDeviceMonitor() noexcept = default;
CudaDeviceTelemetry CudaDeviceMonitor::Sample() noexcept { return impl_ ? impl_->Sample() : CudaDeviceTelemetry{}; }
}  // namespace mmltk::frameworks::gpu
