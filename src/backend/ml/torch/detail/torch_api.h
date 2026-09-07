#pragma once
#include <ATen/CPUGeneratorImpl.h>
#include <ATen/TensorAccessor.h>
#include <ATen/TensorIndexing.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/ops/_fused_adamw.h>
#include <c10/core/InferenceMode.h>
#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <torch/csrc/autograd/autograd.h>
#include <torch/cuda.h>
#include <torch/nn/functional/vision.h>
#include <torch/nn/utils/clip_grad.h>
#include <torch/serialize.h>
#include <torch/torch.h>
#include <torch/version.h>

#include <cstdint>
#include <string_view>

namespace mmltk::backend::ml::torch_api {

using ::at::_fused_adamw_;
using ::at::IntArrayRef;
using ::at::kBFloat16;
using ::at::kBool;
using ::at::kFloat;
using ::at::kHalf;
using ::at::kInt;
using ::at::kLong;
using ::at::ScalarType;
using ::at::cuda::getCurrentCUDAStream;
using ::at::cuda::getDeviceProperties;
using ::c10::DeviceIndex;
using ::c10::InferenceMode;
using ::c10::intrusive_ptr;
using ::c10::IValue;
using ::c10::make_intrusive;
using ::torch::_foreach_add_;
using ::torch::_foreach_addcdiv_;
using ::torch::_foreach_addcmul_;
using ::torch::_foreach_div_;
using ::torch::_foreach_maximum_;
using ::torch::_foreach_mul_;
using ::torch::_foreach_sqrt;
using ::torch::allclose;
using ::torch::arange;
using ::torch::cat;
using ::torch::clamp_min;
using ::torch::Device;
using ::torch::einsum;
using ::torch::empty;
using ::torch::empty_like;
using ::torch::equal;
using ::torch::floor_divide;
using ::torch::from_blob;
using ::torch::full;
using ::torch::full_like;
using ::torch::is_complex;
using ::torch::isfinite;
using ::torch::kBFloat16;
// CLEANUP-IGNORE: Explicit Torch symbol imports form one compile-time API surface; wildcard indirection would hide
// dependencies.
using ::torch::kBilinear;
// CLEANUP-IGNORE: Scalar and device aliases remain explicit names in the same owned Torch facade.
using ::torch::kBool;
using ::torch::kBorder;
using ::torch::kCPU;
using ::torch::kCUDA;
using ::torch::kFloat16;
using ::torch::kFloat32;
using ::torch::kInt32;
using ::torch::kInt64;
using ::torch::kNearest;
using ::torch::kUInt8;
using ::torch::linspace;
using ::torch::manual_seed;
using ::torch::matmul;
using ::torch::maximum;
using ::torch::NoGradGuard;
using ::torch::ones;
using ::torch::ones_like;
using ::torch::rand;
using ::torch::randint;
using ::torch::randn;
using ::torch::randn_like;
using ::torch::stack;
using ::torch::Tensor;
using ::torch::tensor;
using ::torch::TensorOptions;
using ::torch::zeros;
using ::torch::zeros_like;
using ::torch::nn::functional::interpolate;
using ::torch::nn::functional::InterpolateFuncOptions;
using CudaStream = ::cudaStream_t;

namespace autograd {
using ::torch::autograd::grad;
using ::torch::autograd::Variable;
}  // namespace autograd

namespace cuda {
using ::c10::cuda::CUDAGuard;
using ::c10::cuda::CUDAStreamGuard;
using ::c10::cuda::getStreamFromPool;
using ::torch::cuda::device_count;
using ::torch::cuda::is_available;
}  // namespace cuda

namespace indexing {
using ::torch::indexing::Slice;
}

namespace nn {
using ::torch::nn::Module;
namespace functional {
using ::torch::nn::functional::grid_sample;
using ::torch::nn::functional::GridSampleFuncOptions;
}  // namespace functional
namespace utils {
using ::torch::nn::utils::clip_grad_norm_;
}
}  // namespace nn

namespace serialize {
using ::torch::serialize::InputArchive;
using ::torch::serialize::OutputArchive;
}  // namespace serialize

using autograd::grad;
using nn::Module;
using nn::utils::clip_grad_norm_;
using serialize::InputArchive;
using serialize::OutputArchive;

[[nodiscard]] inline auto create_cpu_generator(const std::uint64_t seed) { return ::at::detail::createCPUGenerator(seed); }

inline constexpr std::string_view kVersion{TORCH_VERSION};

}  // namespace mmltk::backend::ml::torch_api
