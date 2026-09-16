#include "src/controller/subsystems/explore/detail/gallery_host_allocations.h"
#include <algorithm>
#include <cuda_runtime_api.h>
#include <stdexcept>
#include "src/controller/subsystems/explore/native_explore_storage.h"
namespace mmltk::controller::explore_detail {
void NativeExploreStorage::Bind(mmltk::backend::imaging::explore::ExploreCudaAllocationApi api) noexcept { storage_.Bind(api); }
auto NativeExploreStorage::ResetChecked() noexcept -> Release { return storage_.ResetChecked(); }
bool NativeExploreStorage::OwnsAllocation() const noexcept { return storage_.OwnsAllocation(); }
}
namespace mmltk::controller::explore_detail {
namespace explore = mmltk::backend::imaging::explore;
namespace {
[[nodiscard]] explore::ExploreStorageStatus allocate_device(void*, void** destination, const std::size_t bytes) noexcept {
    return static_cast<explore::ExploreStorageStatus>(cudaMalloc(destination, std::max<std::size_t>(bytes, 1U)));
}
[[nodiscard]] explore::ExploreStorageStatus release_device(void*, void* allocation) noexcept {
    return static_cast<explore::ExploreStorageStatus>(cudaFree(allocation));
}
}
    explore::ExploreCudaAllocationApi ExploreHostAllocations::api() noexcept {
        return {.context = this, .allocate_device = allocate_device, .release_device = release_device, .allocate_pinned = Allocate, .release_pinned = Release};
    }
    explore::ExploreStorageStatus ExploreHostAllocations::Allocate(void* owner, void** destination, std::size_t bytes) noexcept {
        try {
            auto storage = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
            storage->ensure_bytes(std::max<std::size_t>(bytes, 1));
            auto* data = storage->data();
            static_cast<ExploreHostAllocations*>(owner)->allocations_.emplace(data, std::move(storage));
            *destination = data;
            return explore::kExploreStorageSuccess;
        } catch (...) { return static_cast<explore::ExploreStorageStatus>(cudaErrorMemoryAllocation); }
    }
    explore::ExploreStorageStatus ExploreHostAllocations::Release(void* owner, void* data) noexcept {
        auto& allocations = static_cast<ExploreHostAllocations*>(owner)->allocations_;
        const auto found = allocations.find(data);
        if (found == allocations.end()) return static_cast<explore::ExploreStorageStatus>(cudaErrorInvalidValue);
        if (found->second->ReleaseSettled() != CUDA_SUCCESS) return static_cast<explore::ExploreStorageStatus>(cudaErrorUnknown);
        allocations.erase(found);
        return explore::kExploreStorageSuccess;
    }
}

namespace mmltk::controller::explore_detail {
void ensure_gallery_buffer(explore::ExploreHighWaterBuffer& buffer, const std::size_t bytes, const char* const detail, VisualDiagnosticSink diagnostics_, int device_, std::uint64_t generation) {
    const bool observed = diagnostics_.valid();
    const auto previous = observed ? buffer.capacity_bytes() : 0U;
    if (!buffer.ensure_bytes(bytes)) throw std::runtime_error(detail);
    if (observed && buffer.capacity_bytes() != previous)
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreStorageGrown,
                                        .device = device_,
                                        .generation = generation,
                                        .value = previous,
                                        .detail = buffer.capacity_bytes()};
        });
}
}
