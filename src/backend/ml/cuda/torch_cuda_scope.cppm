module;
#include <cstdint>

export module mmltk.backend.ml.cuda.torch_scope;

export namespace mmltk::backend::ml::cuda {

using TorchCudaWork = void (*)(void*);

enum class TorchCudaPrecision : std::uint8_t {
    Float32,
    Float16,
    BFloat16,
};

struct TorchCudaExecutionOptions final {
    std::int32_t device = -1;
    // CUDA's zero stream handle selects the device's default stream.
    std::uintptr_t stream = 0U;
    bool inference_mode = false;
    bool autocast = false;
    TorchCudaPrecision precision = TorchCudaPrecision::Float32;
};

void run_on_torch_cuda_stream(std::int32_t device, std::uintptr_t stream, void* context, TorchCudaWork work);

void run_with_torch_cuda_scope(const TorchCudaExecutionOptions& options, void* context, TorchCudaWork work);

[[nodiscard]] std::uintptr_t current_torch_cuda_stream(std::int32_t device);

[[nodiscard]] TorchCudaPrecision preferred_torch_cuda_precision(std::int32_t device);

}  // namespace mmltk::backend::ml::cuda
