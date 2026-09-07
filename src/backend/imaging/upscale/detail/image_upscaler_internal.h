#pragma once

namespace mmltk::backend::imaging::upscale {

using mmltk::backend::ml::cuda::GpuBackendGeneration;
using mmltk::backend::ml::cuda::GpuBackendQuiescenceRequirement;
using mmltk::backend::ml::runtime::TensorRtEngine;

inline constexpr std::uint32_t kImageUpscalerInputExtent = 256U;
inline constexpr std::uint32_t kImageUpscalerOutputExtent = kImageUpscalerInputExtent * 4U;

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
    void reset() noexcept;
    [[nodiscard]] cudaError_t Release() noexcept;
    [[nodiscard]] inline float* data() noexcept { return data_; }
    [[nodiscard]] inline const float* data() const noexcept { return data_; }

   private:
    float* data_ = nullptr;
    std::size_t capacity_ = 0U;
};

struct ImageUpscalerRuntimeOutput {
    const float* device_pixels = nullptr;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

class ImageUpscalerRuntime {
   public:
    inline virtual ~ImageUpscalerRuntime() = default;
    [[nodiscard]] virtual ImageUpscalerBackend backend() const noexcept = 0;
    [[nodiscard]] virtual GpuBackendQuiescenceRequirement quiescence_requirement() const noexcept = 0;
    [[nodiscard]] virtual ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, cudaStream_t consumer_stream) = 0;
    virtual void mark_consumed(cudaStream_t stream) = 0;
    virtual void abandon_consumer() noexcept = 0;
    [[nodiscard]] virtual cudaError_t Stop() noexcept = 0;
};

using ImageUpscalerSubmitTiles = bool (*)(void*, const ImageUpscalerRequest&, cudaStream_t, std::uint32_t, std::uint32_t);
using ImageUpscalerReleaseBackend = cudaError_t (*)(void*) noexcept;

class TiledImageUpscalerRuntimeState {
   public:
    TiledImageUpscalerRuntimeState(ImageUpscalerDescriptor descriptor, int device_id);
    ~TiledImageUpscalerRuntimeState();

    [[nodiscard]] ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, cudaStream_t consumer_stream, void* backend,
                                                     ImageUpscalerSubmitTiles submit_tiles);
    void mark_consumed(cudaStream_t stream);
    void abandon_consumer() noexcept;
    [[nodiscard]] cudaError_t Stop(void* backend, ImageUpscalerReleaseBackend release_backend) noexcept;
    [[nodiscard]] GpuBackendQuiescenceRequirement quiescence_requirement() const noexcept;
    [[nodiscard]] inline const ImageUpscalerDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] inline int device_id() const noexcept { return device_id_; }
    [[nodiscard]] inline float* restored_pixels() noexcept { return restored_.data(); }

   private:
    ImageUpscalerDescriptor descriptor_;
    int device_id_ = 0;
    GpuBackendGeneration generation_;
    UpscalerFloatBuffer restored_;
    cudaEvent_t consumer_done_ = nullptr;
    bool consumer_pending_ = false;
    bool awaiting_consumer_ = false;
    bool consumer_fatal_ = false;
    cudaStream_t cleanup_stream_ = nullptr;
    bool stopped_ = false;
    std::mutex mutex_;
};

template <typename BackendOwner, ImageUpscalerBackend Backend>
class TiledImageUpscalerRuntimeAdapter : public ImageUpscalerRuntime {
   public:
    TiledImageUpscalerRuntimeAdapter(ImageUpscalerDescriptor descriptor, const int device_id) : state_(descriptor, device_id) {}

    [[nodiscard]] ImageUpscalerBackend backend() const noexcept final { return Backend; }
    [[nodiscard]] GpuBackendQuiescenceRequirement quiescence_requirement() const noexcept final { return state_.quiescence_requirement(); }
    [[nodiscard]] ImageUpscalerRuntimeOutput enqueue(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream) final {
        return state_.enqueue(
            request, consumer_stream, &owner(),
            [](void* context, const ImageUpscalerRequest& submitted, const cudaStream_t stream, const std::uint32_t width,
               const std::uint32_t height) { return static_cast<BackendOwner*>(context)->submit_tiles(submitted, stream, width, height); });
    }
    void mark_consumed(const cudaStream_t stream) final { state_.mark_consumed(stream); }
    void abandon_consumer() noexcept final { state_.abandon_consumer(); }
    [[nodiscard]] cudaError_t Stop() noexcept final {
        return state_.Stop(&owner(), [](void* context) noexcept { return static_cast<BackendOwner*>(context)->ReleaseBackend(); });
    }

   protected:
    [[nodiscard]] const ImageUpscalerDescriptor& descriptor() const noexcept { return state_.descriptor(); }
    [[nodiscard]] int device_id() const noexcept { return state_.device_id(); }
    [[nodiscard]] float* restored_pixels() noexcept { return state_.restored_pixels(); }

   private:
    [[nodiscard]] BackendOwner& owner() noexcept { return static_cast<BackendOwner&>(*this); }

    TiledImageUpscalerRuntimeState state_;
};

[[nodiscard]] std::shared_ptr<ImageUpscalerRuntime> make_onnx_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
                                                                               const std::filesystem::path& model_path, int device_id);
[[nodiscard]] std::shared_ptr<ImageUpscalerRuntime> make_tensorrt_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
                                                                                   std::unique_ptr<TensorRtEngine> engine, int device_id);

}  // namespace mmltk::backend::imaging::upscale
