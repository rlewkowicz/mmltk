#include "explore_render_storage.h"
#include <cuda_runtime_api.h>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
namespace mmltk::backend::imaging::explore {
namespace {
[[nodiscard]] cudaError_t cuda_status(const ExploreStorageStatus status) noexcept { return static_cast<cudaError_t>(status); }
[[nodiscard]] ExploreStorageStatus storage_status(const cudaError_t status) noexcept { return static_cast<ExploreStorageStatus>(status); }
}  // namespace
struct ExploreHighWaterBuffer::Owner final {
    explicit Owner(const ExploreBufferMemory selected_memory) noexcept : memory(selected_memory) {}
    ~Owner() = default;
    void bind(const ExploreCudaAllocationApi selected_api) noexcept {
        if (!allocation.empty()) std::terminate();
        api = selected_api;
    }
    [[nodiscard]] bool ensure_bytes(const std::size_t required) noexcept {
        if (!api.valid()) {
            last_failure = cudaErrorInitializationError;
            return false;
        }
        const auto retired = allocation.RetryPending([this](void* value) noexcept { return release(value); });
        if (!retired.released()) {
            last_failure = retired.failure;
            return false;
        }
        if (!allocation.replacement_available()) {
            last_failure = cudaErrorNotReady;
            return false;
        }
        if (required <= capacity_bytes) {
            last_failure = cudaSuccess;
            return true;
        }
        const std::size_t next = memory == ExploreBufferMemory::PinnedHost ? required : growth_capacity(capacity_bytes, required);
        const auto allocated = allocation.AllocateCandidate([this, next](void*& replacement) noexcept { return allocate(&replacement, next); });
        if (!allocated.released()) {
            last_failure = allocated.failure;
            return false;
        }
        const auto promoted = allocation.PromoteCandidate([this](void* value) noexcept { return release(value); });
        if (!promoted.released()) {
            last_failure = promoted.failure;
            return false;
        }
        capacity_bytes = next;
        last_failure = cudaSuccess;
        return true;
    }
    [[nodiscard]] cudaError_t reset() noexcept {
        const auto receipt = allocation.ReleaseAll([this](void* value) noexcept { return release(value); });
        last_failure = receipt.failure;
        if (receipt.released()) capacity_bytes = 0U;
        return last_failure;
    }
    [[nodiscard]] static std::size_t growth_capacity(const std::size_t current, const std::size_t required) noexcept {
        if (current == 0U || current > std::numeric_limits<std::size_t>::max() - current / 2U) return required;
        return std::max(required, current + current / 2U);
    }
    [[nodiscard]] cudaError_t allocate(void** destination, const std::size_t bytes) noexcept {
        return cuda_status(memory == ExploreBufferMemory::PinnedHost ? api.allocate_pinned(api.context, destination, bytes)
                                                                     : api.allocate_device(api.context, destination, bytes));
    }
    [[nodiscard]] cudaError_t release(void* value) noexcept {
        return cuda_status(memory == ExploreBufferMemory::PinnedHost ? api.release_pinned(api.context, value) : api.release_device(api.context, value));
    }
    ExploreCudaAllocationApi api{};
    ExploreBufferMemory memory = ExploreBufferMemory::Device;
    mmltk::frameworks::gpu::CudaHighWaterAllocation<void*> allocation{};
    std::size_t capacity_bytes = 0U;
    cudaError_t last_failure = cudaSuccess;
};
ExploreHighWaterBuffer::ExploreHighWaterBuffer(const ExploreBufferMemory memory) : owner_(std::make_unique<Owner>(memory)) {}
ExploreHighWaterBuffer::~ExploreHighWaterBuffer() = default;
void ExploreHighWaterBuffer::bind(const ExploreCudaAllocationApi api) noexcept { owner_->bind(api); }
bool ExploreHighWaterBuffer::ensure_bytes(const std::size_t required) noexcept { return owner_->ensure_bytes(required); }
ExploreStorageStatus ExploreHighWaterBuffer::reset() noexcept { return storage_status(owner_->reset()); }
void* ExploreHighWaterBuffer::data() noexcept { return owner_->allocation.active(); }
const void* ExploreHighWaterBuffer::data() const noexcept { return owner_->allocation.active(); }
std::size_t ExploreHighWaterBuffer::capacity_bytes() const noexcept { return owner_->capacity_bytes; }
ExploreStorageStatus ExploreHighWaterBuffer::last_failure() const noexcept { return storage_status(owner_->last_failure); }
bool ExploreHighWaterBuffer::owns_allocation() const noexcept { return !owner_->allocation.empty(); }
}  // namespace mmltk::backend::imaging::explore
