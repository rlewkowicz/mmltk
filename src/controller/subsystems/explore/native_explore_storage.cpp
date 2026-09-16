#include "src/controller/subsystems/explore/native_explore_storage.h"
namespace mmltk::controller::explore_detail {
namespace explore = mmltk::backend::imaging::explore;
void NativeExploreStorage::Bind(const explore::ExploreCudaAllocationApi api) noexcept {
    traversal().Visit(buffers_, [api](auto& buffer) { buffer.bind(api); });
}
auto NativeExploreStorage::ResetChecked() noexcept -> Release {
    Release result;
    traversal().Visit(buffers_, [&result](auto& buffer) {
        const auto status = buffer.reset();
        if (result.failure == explore::kExploreStorageSuccess) result.failure = status;
    });
    result.all_released = !OwnsAllocation();
    return result;
}
bool NativeExploreStorage::OwnsAllocation() const noexcept {
    bool owned = false;
    traversal().Visit(buffers_, [&owned](const auto& buffer) { owned |= buffer.owns_allocation(); });
    return owned;
}
}  // namespace mmltk::controller::explore_detail
