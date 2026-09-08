#pragma once

#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include "src/backend/imaging/upscale/upscale_execution.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"

namespace mmltk::backend::imaging::upscale {

using mmltk::backend::ml::runtime::TensorRtEngine;

// Cleanup visits every independent obligation. Failed dependencies retain their
// resources, and the installed runtime retains the complete typed failure tree.
class UpscalerCleanup {
   public:
    inline bool Record(cudaError_t status, const char* operation) noexcept {
        if (status == cudaSuccess) return true;
        if (status_ == cudaSuccess) status_ = status;
        try {
            throw mmltk::frameworks::gpu::CudaError(status, operation);
        } catch (...) { failure_ = mmltk::frameworks::gpu::combine_image_failures(failure_, std::current_exception()); }
        return false;
    }
    inline void Checkpoint(const ImageUpscalerExecutionCheckpoint& checkpoint, ImageUpscalerExecutionStage stage) noexcept {
        if (!checkpoint) return;
        try {
            checkpoint(stage);
        } catch (...) {
            if (status_ == cudaSuccess) status_ = cudaErrorUnknown;
            failure_ = mmltk::frameworks::gpu::combine_image_failures(failure_, std::current_exception());
        }
    }
    [[nodiscard]] inline cudaError_t status() const noexcept { return status_; }
    [[nodiscard]] inline std::exception_ptr failure() const noexcept { return failure_; }

   private:
    cudaError_t status_ = cudaSuccess;
    std::exception_ptr failure_;
};

inline constexpr std::uint32_t kImageUpscalerInputExtent = 256U;
inline constexpr std::uint32_t kImageUpscalerOutputExtent = kImageUpscalerInputExtent * 4U;

[[nodiscard]] cudaError_t settle_upscaler_stream(cudaStream_t, cudaGraph_t& abandoned_capture) noexcept;

struct ImageUpscalerDescriptor {
    ImageUpscalerKind kind;
    std::string_view filename;
    const char* input_name;
    const char* output_name;
    std::string_view sha256;
    std::string_view cache_name;
    std::string_view label;
    std::uint32_t halo;
    bool tensor_rt_enabled;
    bool allow_fp16;
    bool allow_tf32;
};

[[nodiscard]] const ImageUpscalerDescriptor& image_upscaler_descriptor(ImageUpscalerKind kind) noexcept;
[[nodiscard]] std::filesystem::path image_upscaler_model_path(const ImageUpscalerDescriptor& descriptor);
[[nodiscard]] std::size_t checked_upscaler_elements(std::uint32_t width, std::uint32_t height, std::size_t channels);
class UpscalerFloatBuffer {
   public:
    UpscalerFloatBuffer() = default;
    ~UpscalerFloatBuffer();
    UpscalerFloatBuffer(const UpscalerFloatBuffer&) = delete;
    UpscalerFloatBuffer& operator=(const UpscalerFloatBuffer&) = delete;

    void ensure(std::size_t elements, const char* context);
    [[nodiscard]] cudaError_t Release(UpscalerCleanup* = nullptr) noexcept;
    [[nodiscard]] inline float* data() noexcept { return data_; }
    [[nodiscard]] inline const float* data() const noexcept { return data_; }

   private:
    float* data_ = nullptr;
    float* replacement_ = nullptr;
    std::size_t capacity_ = 0U;
};

struct ImageUpscalerRuntimeOutput {
    const std::uint8_t* device_pixels = nullptr;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    ImageUpscalerOutcome outcome = ImageUpscalerOutcome::Completed;
};

class ImageUpscalerRuntime {
   public:
    inline virtual ~ImageUpscalerRuntime() = default;
    [[nodiscard]] virtual ImageUpscalerOutcome Activate(ImageUpscalerCurrent) = 0;
    [[nodiscard]] virtual ImageUpscalerBackend backend() const noexcept = 0;
    [[nodiscard]] virtual bool graph_replay() const noexcept = 0;
    [[nodiscard]] virtual ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, cudaStream_t consumer_stream) = 0;
    virtual void mark_consumed(cudaStream_t stream) = 0;
    virtual void abandon_consumer() noexcept = 0;
    [[nodiscard]] virtual cudaError_t Settle() noexcept = 0;
    [[nodiscard]] virtual cudaError_t Stop() noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr cleanup_failure() const noexcept = 0;
};

using ImageUpscalerSubmitTiles = bool (*)(void*, const ImageUpscalerRequest&, cudaStream_t, std::uint32_t, std::uint32_t);
using ImageUpscalerReleaseBackend = cudaError_t (*)(void*) noexcept;

class TiledImageUpscalerRuntimeState {
   public:
    TiledImageUpscalerRuntimeState(ImageUpscalerDescriptor descriptor, int device_id);
    ~TiledImageUpscalerRuntimeState();
    [[nodiscard]] ImageUpscalerOutcome Activate(const ImageUpscalerExecutionCheckpoint&, ImageUpscalerCurrent);

    [[nodiscard]] ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, cudaStream_t consumer_stream, void* backend,
                                                     ImageUpscalerSubmitTiles submit_tiles, const ImageUpscalerExecutionCheckpoint&);
    void mark_consumed(cudaStream_t stream);
    void abandon_consumer() noexcept;
    [[nodiscard]] cudaError_t Stop(void* backend, ImageUpscalerReleaseBackend release_backend,
                                   const ImageUpscalerExecutionCheckpoint&) noexcept;
    [[nodiscard]] inline std::exception_ptr cleanup_failure() const noexcept { return cleanup_.failure(); }
    [[nodiscard]] inline const ImageUpscalerDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] inline int device_id() const noexcept { return device_id_; }

   private:
    ImageUpscalerDescriptor descriptor_;
    int device_id_ = 0;
    cudaEvent_t consumer_done_ = nullptr;
    bool consumer_pending_ = false;
    bool awaiting_consumer_ = false;
    bool consumer_fatal_ = false;
    cudaStream_t cleanup_stream_ = nullptr;
    bool stopped_ = false;
    std::mutex mutex_;
    UpscalerCleanup cleanup_;
};

template <typename BackendOwner, ImageUpscalerBackend Backend>
class TiledImageUpscalerRuntimeAdapter : public ImageUpscalerRuntime {
   public:
    TiledImageUpscalerRuntimeAdapter(ImageUpscalerDescriptor descriptor, const int device_id, ImageUpscalerExecutionCheckpoint checkpoint)
        : state_(descriptor, device_id), checkpoint_(std::move(checkpoint)) {}

    [[nodiscard]] ImageUpscalerOutcome Activate(ImageUpscalerCurrent current) final {
        if (state_.Activate(checkpoint_, current) == ImageUpscalerOutcome::Cancelled) return ImageUpscalerOutcome::Cancelled;
        return owner().ActivateBackend(current);
    }
    [[nodiscard]] ImageUpscalerBackend backend() const noexcept final { return Backend; }
    [[nodiscard]] ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream) final {
        return state_.enqueue(
            request, consumer_stream, &owner(),
            [](void* context, const ImageUpscalerRequest& submitted, const cudaStream_t stream, const std::uint32_t width,
               const std::uint32_t height) { return static_cast<BackendOwner*>(context)->submit_tiles(submitted, stream, width, height); },
            checkpoint_);
    }
    void mark_consumed(const cudaStream_t stream) final { state_.mark_consumed(stream); }
    void abandon_consumer() noexcept final { state_.abandon_consumer(); }
    [[nodiscard]] cudaError_t Settle() noexcept final { return owner().SettleBackend(); }
    [[nodiscard]] cudaError_t Stop() noexcept final {
        return state_.Stop(
            &owner(), [](void* context) noexcept { return static_cast<BackendOwner*>(context)->ReleaseBackend(); }, checkpoint_);
    }
    [[nodiscard]] std::exception_ptr cleanup_failure() const noexcept final {
        return mmltk::frameworks::gpu::combine_image_failures(state_.cleanup_failure(), owner().cleanup_.failure());
    }

   protected:
    [[nodiscard]] const ImageUpscalerDescriptor& descriptor() const noexcept { return state_.descriptor(); }
    [[nodiscard]] int device_id() const noexcept { return state_.device_id(); }
    void Checkpoint(ImageUpscalerExecutionStage stage) const {
        if (checkpoint_) checkpoint_(stage);
    }
    void CleanupCheckpoint(UpscalerCleanup& cleanup, ImageUpscalerExecutionStage stage) const noexcept {
        cleanup.Checkpoint(checkpoint_, stage);
    }

   private:
    [[nodiscard]] BackendOwner& owner() noexcept { return static_cast<BackendOwner&>(*this); }
    [[nodiscard]] const BackendOwner& owner() const noexcept { return static_cast<const BackendOwner&>(*this); }

    TiledImageUpscalerRuntimeState state_;
    ImageUpscalerExecutionCheckpoint checkpoint_;
};

[[nodiscard]] std::shared_ptr<ImageUpscalerRuntime> make_onnx_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
                                                                               const std::filesystem::path& model_path, int device_id,
                                                                               const ImageUpscalerExecutionCheckpoint&);
[[nodiscard]] std::shared_ptr<ImageUpscalerRuntime> make_tensorrt_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
                                                                                   std::unique_ptr<TensorRtEngine> engine, int device_id,
                                                                                   const ImageUpscalerExecutionCheckpoint&);

}  // namespace mmltk::backend::imaging::upscale
