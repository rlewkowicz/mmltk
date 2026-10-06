#include "src/backend/imaging/resample/detail/torch_resize.h"
#include "src/common/math/checked_arithmetic.h"
#include <stdexcept>
#if defined(MMLTK_TORCH_RGB_RESIZE)
#include <ATen/Config.h>
#include <ATen/Parallel.h>
#include <ATen/ops/from_blob.h>
#include <ATen/ops/upsample_bilinear2d.h>
#include <ATen/ops/_upsample_bilinear2d_aa.h>
#include <c10/core/InferenceMode.h>
#if AT_PARALLEL_OPENMP && defined(_OPENMP)
#include <omp.h>
#endif
#endif
namespace mmltk::backend::imaging::resample::detail {
void require_torch_resize() {
#if !defined(MMLTK_TORCH_RGB_RESIZE)
 throw std::runtime_error("Torch RGB resampling requires a build with the pinned Torch payload");
#elif !AT_PARALLEL_OPENMP || !defined(_OPENMP)
 throw std::runtime_error("Torch RGB resampling requires the pinned OpenMP CPU payload for bounded image-worker execution");
#endif
}
TorchResizeStorage torch_resize_storage(std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, bool antialias) {
 require_torch_resize();
 if (!sw || !sh || !dw || !dh) throw std::invalid_argument("Torch resize dimensions must be positive");
 using common::math::checked_add;
 using common::math::checked_multiply;
 const auto add = [](std::size_t a, std::size_t b) { return checked_add(a, b, "Torch resize storage overflow"); };
 const auto mul = [](std::size_t a, std::size_t b) { return checked_multiply(a, b, "Torch resize storage overflow"); };
 const auto input = mul(mul(sw, sh), 3 * sizeof(float));
 const auto output = mul(mul(dw, dh), 3 * sizeof(float));
 // Source basis: aten/src/ATen/native/cpu/UpSampleKernel.cpp, upstream PyTorch.
 // These conservative bounds cover its generic and separable implementations;
 // installed-payload numerical/storage acceptance remains a validation gate.
 // ATen CPU upsampling may materialize channels-last input/output even from
 // contiguous C3 planes. The AA separable route additionally retains H x outW.
 // Charge both axis orders and two complete layout copies conservatively.
 // Per-axis coefficient/index tensors are O(out * (2*ceil(in/out)+3)); the
 // 32-byte tap allowance covers float weights and multiple int64 index arrays.
 // These bounds include alternatives, not just the selected fast path; the
 // fixed tail covers bounded tensor/TensorIterator control allocations.
 const auto axis = [&](std::size_t in, std::size_t out) {
  const auto taps = antialias ? add(mul(2, add(in, out - 1) / out), 3) : std::size_t{2};
  return mul(out, add(128, mul(32, taps)));
 };
 auto opaque = add(mul(2, add(input, output)), add(axis(sw, dw), axis(sh, dh)));
 if (antialias) opaque = add(opaque, mul(add(mul(sh, dw), mul(sw, dh)), 3 * sizeof(float)));
 return {add(input, output), add(opaque, 64U << 10)};
}
void torch_resize(const std::uint8_t* rgb, std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, bool antialias, std::span<std::byte> storage, RgbResizeWork* observed) {
 const auto required = torch_resize_storage(sw, sh, dw, dh, antialias);
 if (storage.size() < required.supplied_bytes) throw std::length_error("Torch resize supplied workspace exhausted");
#if defined(MMLTK_TORCH_RGB_RESIZE) && AT_PARALLEL_OPENMP && defined(_OPENMP)
 // OpenMP 5.1 section 3.2.1: nthreads-var is local to this calling task, unlike
 // at::set_num_threads, which changes Torch's process-wide configuration.
 // Initialize ATen's per-thread state before installing this scoped limit.
 (void)at::get_num_threads();
 struct CpuScope {
  int previous = omp_get_max_threads();
  CpuScope() { omp_set_num_threads(1); }
  ~CpuScope() { omp_set_num_threads(previous); }
 } cpu_scope;
 if (at::get_num_threads() != 1) throw std::runtime_error("Torch resize CPU payload did not honor the local worker limit");
 const c10::InferenceMode inference;
 auto* input_data = reinterpret_cast<float*>(storage.data());
 rgb_hwc_u8_to_nchw_f32(rgb, input_data, sw, sh);
 const auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCPU);
 const auto input = at::from_blob(input_data, {1, 3, sh, sw}, options);
 auto output = at::from_blob(input_data + std::size_t(sw) * sh * 3, {1, 3, dh, dw}, options);
 if (antialias)
  at::_upsample_bilinear2d_aa_out(output, input, {dh, dw}, false);
 else
  at::upsample_bilinear2d_out(output, input, {dh, dw}, false);
 // Opaque storage is reported by the immutable plan, not as fictitious
 // allocator events in RgbResizeWork's supplied-workspace counters.
 if (observed) ++observed->preparations;
#else
 (void)rgb;
 (void)observed;
#endif
}
}  // namespace mmltk::backend::imaging::resample::detail
