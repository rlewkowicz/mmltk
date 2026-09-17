#pragma once
#include <cstddef>
#include <cstdint>
#include "src/controller/presentation/visual_diagnostics.h"
#include <memory>
#include <unordered_map>
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/explore/explore_render_storage.h"
namespace mmltk::controller::explore_detail {
void ensure_gallery_buffer(mmltk::backend::imaging::explore::ExploreHighWaterBuffer&, std::size_t, const char*, VisualDiagnosticSink, int, std::uint64_t);
class ExploreHostAllocations final {
   public:
    [[nodiscard]] mmltk::backend::imaging::explore::ExploreCudaAllocationApi api() noexcept;

   private:
    static mmltk::backend::imaging::explore::ExploreStorageStatus Allocate(void*, void**, std::size_t) noexcept;
    static mmltk::backend::imaging::explore::ExploreStorageStatus Release(void*, void*) noexcept;
    std::unordered_map<void*, std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer>> allocations_;
};
}  // namespace mmltk::controller::explore_detail
