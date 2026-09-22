module;
#include <compare>
#include <cstddef>
#include <cstdint>
export module mmltk.backend.ml.cuda.gpu_quiescence;
export namespace mmltk::backend::ml::cuda {
struct GpuBackendGeneration {
 std::uint64_t value = 0U;
 [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0U; }
 auto operator<=>(const GpuBackendGeneration&) const = default;
};
enum class GpuBackendQuiescenceStrategy : std::uint8_t {
 TransitiveFence = 0U,
 TargetDeviceBarrier = 1U,
 Count = 2U,
};
struct GpuBackendQuiescenceRequirement {
 GpuBackendGeneration generation;
 std::int32_t device_id = -1;
 GpuBackendQuiescenceStrategy strategy = GpuBackendQuiescenceStrategy::TargetDeviceBarrier;
 bool user_compute_stream_owned = false;
 bool provider_copy_streams_ordered = false;
 bool auxiliary_streams_ordered = false;
 [[nodiscard]] bool valid() const noexcept {
  if (!generation || device_id < 0) { return false; }
  switch (strategy) {
   case GpuBackendQuiescenceStrategy::TransitiveFence: return user_compute_stream_owned && provider_copy_streams_ordered && auxiliary_streams_ordered;
   case GpuBackendQuiescenceStrategy::TargetDeviceBarrier: return true;
   case GpuBackendQuiescenceStrategy::Count: return false;
  }
  return false;
 }
};
[[nodiscard]] constexpr GpuBackendQuiescenceRequirement target_device_barrier_quiescence(const GpuBackendGeneration generation, const std::int32_t device_id) noexcept {
 return {
  .generation = generation,
  .device_id = device_id,
  .strategy = GpuBackendQuiescenceStrategy::TargetDeviceBarrier,
  .user_compute_stream_owned = true,
  .provider_copy_streams_ordered = true,
  .auxiliary_streams_ordered = false,
 };
}
[[nodiscard]] GpuBackendGeneration next_gpu_backend_generation() noexcept;
static_assert(static_cast<std::size_t>(GpuBackendQuiescenceStrategy::Count) == 2U);
}  // namespace mmltk::backend::ml::cuda
