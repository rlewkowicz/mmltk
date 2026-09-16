module;
#include <atomic>
#include <cstdint>
#include <limits>
module mmltk.backend.ml.cuda.gpu_quiescence;
namespace mmltk::backend::ml::cuda {
GpuBackendGeneration next_gpu_backend_generation() noexcept {
    static std::atomic<std::uint64_t> next_generation{1U};
    std::uint64_t candidate = next_generation.load(std::memory_order_relaxed);
    while (candidate != 0U && candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next_generation.compare_exchange_weak(candidate, candidate + 1U, std::memory_order_relaxed, std::memory_order_relaxed))
            return GpuBackendGeneration{candidate};
    }
    return {};
}
}  // namespace mmltk::backend::ml::cuda
