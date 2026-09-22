#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <type_traits>
#include <utility>
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/imaging/explore/detail/explore_render_cuda_abi.h"
#include "src/controller/subsystems/explore/explore_system.h"
namespace mmltk::controller::explore_detail {
// Track the standard library's actual control-block allocation without
// depending on its private layout. The result pointer is used only during
// synchronous construction; deallocation never accesses it.
template <class T>
struct GallerySharedAllocator final {
 using value_type = T;
 std::size_t* result = nullptr;
 explicit GallerySharedAllocator(std::size_t& bytes) noexcept : result(&bytes) {}
 template <class U>
 GallerySharedAllocator(const GallerySharedAllocator<U>& source) noexcept : result(source.result) {}
 [[nodiscard]] T* allocate(std::size_t count) {
  auto* allocation = std::allocator<T>{}.allocate(count);
  *result += count * sizeof(T);
  return allocation;
 }
 void deallocate(T* allocation, std::size_t count) noexcept { std::allocator<T>{}.deallocate(allocation, count); }
 bool operator==(const GallerySharedAllocator&) const = default;
};
template <class T>
struct GallerySharedDelete final {
 std::size_t bytes = sizeof(T);
 void operator()(const T* value) const noexcept { delete value; }
};
template <class T, class... Arguments>
[[nodiscard]] std::shared_ptr<T> MakeGalleryShared(Arguments&&... arguments) {
 using Value = std::remove_const_t<T>;
 std::size_t control_bytes = 0U;
 std::shared_ptr<T> result{new Value(std::forward<Arguments>(arguments)...), GallerySharedDelete<Value>{}, GallerySharedAllocator<std::byte>{control_bytes}};
 std::get_deleter<GallerySharedDelete<Value>>(result)->bytes += control_bytes;
 return result;
}
template <class T>
[[nodiscard]] std::size_t GallerySharedBytes(const std::shared_ptr<T>& value) noexcept {
 if (!value) return 0U;
 const auto* allocation = std::get_deleter<GallerySharedDelete<std::remove_const_t<T>>>(value);
 return allocation ? allocation->bytes : sizeof(T);
}
struct GalleryTileMeaning final {
 mmltk::backend::imaging::explore::detail::ExploreRenderCardDescriptorAbi card{};
 std::vector<mmltk::backend::imaging::explore::detail::ExploreRenderAnnotationDescriptorAbi> annotations;
 std::vector<mmltk::backend::imaging::explore::detail::ExploreRenderRlePairAbi> runs;
};
// Pixel identities occupy retained physical slots. Demand positions only map to
// those slots; reorder and viewport size are not pixel identity. The product
// retains the artifact behind incarnation throughout every physical use.
class GalleryThumbnailCache final {
public:
 static constexpr std::uint32_t kNeighborRows = 4U;
 static constexpr std::size_t kMaximumCards = (1U + 2U * kNeighborRows) * kExploreVisibleItemCapacity;
 struct Identity final {
  const mmltk::backend::data::CompiledDataset* incarnation = nullptr;
  std::uint64_t dataset = 0U;
  std::uint64_t seed = 0U;
  mmltk::backend::models::rfdetr::GpuAugmentationConfig augmentation{};
  std::uint32_t extent = 0U;
  bool augmented = false;
  bool operator==(const Identity&) const noexcept = default;
  // A changed preview replaces pixels, but completed tiles from the same
  // artifact and raster extent remain drawable until replacement.
  [[nodiscard]] bool SameSource(const Identity& other) const noexcept { return incarnation == other.incarnation && dataset == other.dataset && extent == other.extent; }
 };
 struct Entry final {
  std::size_t position = std::numeric_limits<std::size_t>::max();
  std::uint32_t compiled_index = 0U;
  std::uint8_t bank = 0U;
  std::uint8_t semantic_bank = 0U;
  bool refresh_pending = false;
  std::shared_ptr<const GalleryTileMeaning> meaning{};
  std::uint64_t semantic_identity = 0U;
 };
 [[nodiscard]] static std::size_t WindowCount(std::size_t matching, const ExploreViewport&) noexcept;
 [[nodiscard]] static std::size_t WindowFirst(std::size_t matching, const ExploreViewport&) noexcept;
 void Configure(std::size_t count, Identity);
 void Reserve(std::size_t count);
 void BeginUpdate();
 void CommitUpdate() noexcept;
 void RollbackUpdate() noexcept;
 [[nodiscard]] const Entry& Protected(std::size_t slot) const noexcept;
 [[nodiscard]] std::uint8_t WritableBank(std::size_t position, const GalleryThumbnailCache* incumbent = nullptr, bool semantic = false) const;
 void Admit(std::span<const std::uint32_t>, std::size_t first);
 [[nodiscard]] std::size_t Position(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] std::size_t MetadataBytes() const noexcept;
 void Clear() noexcept;
 // Physical slots and their retained metadata capacity; neither is demand size.
 [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
 [[nodiscard]] std::size_t capacity() const noexcept { return entries_.capacity(); }
 [[nodiscard]] const Identity& identity() const noexcept { return identity_; }
 [[nodiscard]] std::size_t Slot(std::size_t position) const noexcept { return demand_slots_[position - demand_first_]; }
 // Requires a valid physical slot and bank 0 or 1; returns rows, not bytes.
 [[nodiscard]] std::size_t PhysicalRow(std::size_t slot, std::uint8_t bank) const noexcept { return (static_cast<std::size_t>(bank) * entries_.size() + slot) * identity_.extent; }
 [[nodiscard]] const Entry* Find(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] const Entry* Retained(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] const Entry& Physical(std::size_t slot) const { return entries_.at(slot); }
 void Restore(std::size_t slot, Entry entry) noexcept;
 void Complete(std::size_t position, std::uint32_t compiled_index, std::shared_ptr<const GalleryTileMeaning>, std::uint64_t semantic_identity, std::uint8_t bank = 0U, std::uint8_t semantic_bank = 0U);
 void UpdateSemantics(std::size_t position, std::uint64_t semantic_identity, std::uint8_t semantic_bank);
 [[nodiscard]] std::size_t MeaningBytes(const GalleryThumbnailCache* other = nullptr, std::span<const std::shared_ptr<const GalleryTileMeaning>> additional_meanings = {}) const;

private:
 Identity identity_{};
 static constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();
 [[nodiscard]] std::size_t Lookup(std::uint32_t) const noexcept;
 [[nodiscard]] std::size_t Assign(std::uint32_t);
 void Reindex();
 void Save(std::size_t);
 void Erase(std::size_t) noexcept;
 std::vector<Entry> entries_;
 std::vector<std::size_t> buckets_, next_, demand_slots_;
 std::vector<bool> pinned_;
 std::size_t demand_first_ = 0U;
 std::size_t eviction_ = 0U;
 struct Undo final {
  std::size_t slot;
  Entry entry;
 };
 std::vector<Undo> undo_;
 std::vector<std::size_t> undo_indices_, prior_demand_;
 std::size_t prior_first_ = 0U;
 bool updating_ = false;
};
}  // namespace mmltk::controller::explore_detail
