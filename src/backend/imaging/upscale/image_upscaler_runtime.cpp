module;
#include <cuda_runtime.h>

#include <array>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/image_upscaler_cuda.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/common/system/runtime_paths.h"
#include "src/frameworks/gpu/cuda_error.h"

module mmltk.backend.imaging.upscale.image_upscaler;

import mmltk.backend.ml.cuda.gpu_quiescence;

#include "detail/image_upscaler_internal.h"

namespace mmltk::backend::imaging::upscale {

using mmltk::backend::ml::cuda::GpuBackendQuiescenceStrategy;
using mmltk::backend::ml::cuda::next_gpu_backend_generation;
using mmltk::frameworks::gpu::ensure_cuda_ok;

namespace {

constexpr std::array<ImageUpscalerDescriptor, 2U> kDescriptors{{
    {
        .kind = ImageUpscalerKind::ShiftLUT,
        .filename = "ShiftLUT_fp32.onnx",
        .input_name = "image",
        .output_name = "upscaled",
        .sha256 = "111ec75e71638aa37fd0728d513d01b277a8708e09422c546b094c0547255444",
        .cache_name = "shiftlut-fp32",
        .label = "ShiftLUT",
        .halo = 32U,
        .tensor_rt_enabled = false,
        .allow_fp16 = false,
        .allow_tf32 = false,
    },
    {
        .kind = ImageUpscalerKind::RealPLKSR,
        .filename = "RealPLKSR_fp16.onnx",
        .input_name = "input",
        .output_name = "output",
        .sha256 = "294d117eb8e7093417cc972d41c7c5b84f3d452193c952e58bc7c94e2143692d",
        .cache_name = "realplksr-fp16",
        .label = "RealPLKSR",
        .halo = 16U,
        .tensor_rt_enabled = true,
        .allow_fp16 = true,
        .allow_tf32 = true,
    },
}};

}

const ImageUpscalerDescriptor& image_upscaler_descriptor(const ImageUpscalerKind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= kDescriptors.size()) { std::terminate(); }
    return kDescriptors[index];
}

std::filesystem::path image_upscaler_model_path(const ImageUpscalerDescriptor& descriptor) {
    std::filesystem::path repository = mmltk::common::system::runtime_paths::repository_root() / "src" / "backend" / "imaging" / "upscale" /
                                       "assets" / descriptor.filename;
    if (std::filesystem::is_regular_file(repository)) { return repository; }
    std::filesystem::path installed = mmltk::common::system::runtime_paths::install_prefix() / "models" / descriptor.filename;
    if (std::filesystem::is_regular_file(installed)) { return installed; }
    throw std::runtime_error("missing Image upscaler model " + std::string(descriptor.filename));
}

std::size_t checked_upscaler_elements(const std::uint32_t width, const std::uint32_t height, const std::size_t channels) {
    if (width == 0U || height == 0U || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / width) {
        throw std::overflow_error("Image upscaler dimensions overflow");
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (channels == 0U || pixels > std::numeric_limits<std::size_t>::max() / channels) {
        throw std::overflow_error("Image upscaler tensor size overflow");
    }
    return pixels * channels;
}

UpscalerFloatBuffer::~UpscalerFloatBuffer() { reset(); }

void UpscalerFloatBuffer::ensure(const std::size_t elements, const char* context) {
    if (elements <= capacity_) { return; }
    if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::overflow_error(std::string(context) + " byte size overflow");
    }
    float* replacement = nullptr;
    ensure_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&replacement), elements * sizeof(float)), context);
    if (data_ != nullptr) { ensure_cuda_ok(cudaFree(data_), "cudaFree while growing Image upscaler buffer"); }
    data_ = replacement;
    capacity_ = elements;
}

void UpscalerFloatBuffer::reset() noexcept { static_cast<void>(Release()); }

cudaError_t UpscalerFloatBuffer::Release() noexcept {
    if (data_ != nullptr) {
        const cudaError_t failure = cudaFree(data_);
        if (failure != cudaSuccess) return failure;
        data_ = nullptr;
        capacity_ = 0U;
    }
    return cudaSuccess;
}

TiledImageUpscalerRuntimeState::TiledImageUpscalerRuntimeState(ImageUpscalerDescriptor descriptor, const int device_id)
    : descriptor_(descriptor), device_id_(device_id), generation_(next_gpu_backend_generation()) {
    if (!generation_) { throw std::runtime_error("Image upscaler backend generation identity exhausted"); }
    ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for Image upscaler runtime");
    ensure_cuda_ok(cudaEventCreateWithFlags(&consumer_done_, cudaEventDisableTiming),
                   "cudaEventCreate for Image upscaler output consumption");
    const cudaError_t cleanup_stream_status = cudaStreamCreateWithFlags(&cleanup_stream_, cudaStreamNonBlocking);
    if (cleanup_stream_status != cudaSuccess) {
        static_cast<void>(cudaEventDestroy(consumer_done_));
        consumer_done_ = nullptr;
        ensure_cuda_ok(cleanup_stream_status, "cudaStreamCreate for Image upscaler cleanup");
    }
}

GpuBackendQuiescenceRequirement TiledImageUpscalerRuntimeState::quiescence_requirement() const noexcept {
    return {
        .generation = generation_,
        .device_id = device_id_,
        .strategy = GpuBackendQuiescenceStrategy::TransitiveFence,
        .user_compute_stream_owned = true,
        .provider_copy_streams_ordered = true,
        .auxiliary_streams_ordered = true,
    };
}

TiledImageUpscalerRuntimeState::~TiledImageUpscalerRuntimeState() {
    std::lock_guard lock(mutex_);
    if ((!stopped_ && std::uncaught_exceptions() == 0) || awaiting_consumer_ || consumer_fatal_) std::terminate();
    if (consumer_done_ != nullptr) { (void)cudaEventDestroy(consumer_done_); }
    if (cleanup_stream_ != nullptr) (void)cudaStreamDestroy(cleanup_stream_);
}

ImageUpscalerRuntimeOutput TiledImageUpscalerRuntimeState::enqueue(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream,
                                                                   void* backend, const ImageUpscalerSubmitTiles submit_tiles) {
    std::lock_guard lock(mutex_);
    if (request.device_pixels == nullptr || consumer_stream == nullptr || request.crop_width == 0U || request.crop_height == 0U ||
        request.source_width == 0U || request.source_height == 0U || request.crop_x > request.source_width ||
        request.crop_width > request.source_width - request.crop_x || request.crop_y > request.source_height ||
        request.crop_height > request.source_height - request.crop_y) {
        throw std::invalid_argument("Image upscaler request has invalid source geometry");
    }
    if (awaiting_consumer_) { throw std::runtime_error("Image upscaler output was not consumed"); }
    ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for Image upscaler enqueue");
    if (consumer_pending_) {
        ensure_cuda_ok(cudaStreamWaitEvent(consumer_stream, consumer_done_, 0U),
                       "cudaStreamWaitEvent before reusing Image upscaler output");
        consumer_pending_ = false;
    }
    if (request.crop_width > std::numeric_limits<std::uint32_t>::max() / 4U ||
        request.crop_height > std::numeric_limits<std::uint32_t>::max() / 4U) {
        throw std::overflow_error("Image upscaler output dimensions overflow");
    }
    const std::uint32_t restored_width = request.crop_width * 4U;
    const std::uint32_t restored_height = request.crop_height * 4U;
    restored_.ensure(checked_upscaler_elements(restored_width, restored_height, 3U), "cudaMalloc for Image upscaler restored image");
    if (!submit_tiles(backend, request, consumer_stream, restored_width, restored_height)) { return {}; }
    awaiting_consumer_ = true;
    return {
        .device_pixels = restored_.data(),
        .width = restored_width,
        .height = restored_height,
    };
}

void TiledImageUpscalerRuntimeState::mark_consumed(const cudaStream_t stream) {
    std::lock_guard lock(mutex_);
    if (!awaiting_consumer_) { return; }
    ensure_cuda_ok(cudaEventRecord(consumer_done_, stream), "cudaEventRecord for Image upscaler output consumption");
    awaiting_consumer_ = false;
    consumer_pending_ = true;
}

void TiledImageUpscalerRuntimeState::abandon_consumer() noexcept {
    std::lock_guard lock(mutex_);
    if (!awaiting_consumer_) { return; }
    consumer_fatal_ = true;
}

cudaError_t TiledImageUpscalerRuntimeState::Stop(void* backend, const ImageUpscalerReleaseBackend release_backend) noexcept {
    std::lock_guard lock(mutex_);
    if (stopped_) return cudaSuccess;
    if (awaiting_consumer_ || consumer_fatal_ || cleanup_stream_ == nullptr) return cudaErrorNotReady;
    cudaError_t failure = cudaSetDevice(device_id_);
    if (failure == cudaSuccess && consumer_pending_) failure = cudaEventSynchronize(consumer_done_);
    if (failure == cudaSuccess) failure = cudaStreamSynchronize(cleanup_stream_);
    if (failure != cudaSuccess) return failure;
    failure = release_backend(backend);
    if (failure != cudaSuccess) return failure;
    failure = restored_.Release();
    if (failure != cudaSuccess) return failure;
    if (consumer_done_ != nullptr) {
        failure = cudaEventDestroy(consumer_done_);
        if (failure != cudaSuccess) return failure;
        consumer_done_ = nullptr;
    }
    failure = cudaStreamDestroy(cleanup_stream_);
    if (failure != cudaSuccess) return failure;
    cleanup_stream_ = nullptr;
    consumer_pending_ = false;
    stopped_ = true;
    return cudaSuccess;
}

}  // namespace mmltk::backend::imaging::upscale
