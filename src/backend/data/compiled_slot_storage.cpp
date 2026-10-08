#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/math/checked_arithmetic.h"
#include <sys/mman.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::detail {
struct CompiledSlotStorage::Slot {
 std::mutex labels;
 float* pixels = nullptr;
 bool pixel_ready = false, pixel_borrowed = false;
 std::uint8_t label_pages = 0;
};
struct CompiledSlotStorage::SlotBlock {
 std::array<Slot, COMPILED_BLOCK_SLOTS> slots;
};
CompiledSlotStorage::CompiledSlotStorage(StagedArtifact& artifact, const FileLayout& layout) : artifact_(artifact), layout_(layout) {
 slots_.reserve(layout.block_count());
 for (std::size_t i = 0; i < layout.block_count(); ++i) slots_.push_back(std::make_unique<SlotBlock>());
}
CompiledSlotStorage::~CompiledSlotStorage() { quiesce(); }
std::uint32_t CompiledSlotStorage::reserve_slot() {
 const std::lock_guard lock(mutex_);
 if (closed_) throw std::logic_error("compiled slot admission after seal");
 const auto slot = layout_.num_slots;
 const auto next = compute_file_layout(common::math::checked_add(slot, 1U, "compiled slot overflow"), layout_.image_stride, layout_.label_capacity);
 if (next.block_count() > slots_.size()) slots_.push_back(std::make_unique<SlotBlock>());
 layout_ = next;
 return slot;
}
FileLayout CompiledSlotStorage::layout() const {
 const std::lock_guard lock(mutex_);
 return layout_;
}
CompiledSlotStorage::Slot& CompiledSlotStorage::slot(std::uint32_t index) {
 const std::lock_guard lock(mutex_);
 // The caller already owns operation custody. A concurrent seal closes new
 // admission while allowing these admitted preparations/writes to complete.
 return slot_locked(index);
}
CompiledSlotStorage::Slot& CompiledSlotStorage::slot_locked(std::uint32_t index) {
 if (index >= layout_.num_slots) throw std::out_of_range("compiled storage slot");
 return slots_[index / COMPILED_BLOCK_SLOTS]->slots[index % COMPILED_BLOCK_SLOTS];
}
void CompiledSlotStorage::prepare(std::uint32_t index, std::size_t label_bytes, bool pixel) {
 // Only this file's preparation is serialized. Existing pixel stores and
 // independent source claims/completions never take the mutation lock.
 const std::lock_guard preparation(preparation_);
 auto& value = slot(index);
 const auto current = layout();
 if (label_bytes > current.label_capacity) throw std::length_error("compiled labels exceed slot capacity");
 if (prepared_length_ < current.index_offset) {
  artifact_.resize_sparse(current.index_offset);
  prepared_length_ = current.index_offset;
 }
 if (pixel && !value.pixel_ready) {
  const auto count = current.block_index(index) + 1;
  if (blocks_.capacity() < count) blocks_.reserve(std::max(count, std::max(std::size_t{1}, blocks_.capacity() * 2)));
  if (blocks_.size() < count) blocks_.resize(count);
  auto& region = blocks_[current.block_index(index)];
  if (!region.address()) {
   auto* address = ::mmap(nullptr, current.pixel_block_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, artifact_.file().get(),
    common::math::checked_cast<off_t>(current.block_offset(current.block_index(index)), "compiled mapping offset overflow"));
   if (address == MAP_FAILED) throw common::io::errno_error("compiled pixel block mmap failed");
   region.adopt(address, current.pixel_block_bytes);
   (void)::madvise(address, current.pixel_block_bytes, MADV_HUGEPAGE);
  }
  artifact_.allocate_range(current.pixel_offset(index), current.pixel_pitch);
  const std::lock_guard lock(mutex_);
  value.pixels = reinterpret_cast<float*>(static_cast<std::byte*>(blocks_[current.block_index(index)].address()) + current.pixel_offset(index) - current.block_offset(current.block_index(index)));
  value.pixel_ready = true;
 }
 const auto pages = compiled_align_up(label_bytes, PAGE_SIZE) / PAGE_SIZE;
 if (pages > value.label_pages) {
  artifact_.allocate_range(current.label_offset(index) + value.label_pages * PAGE_SIZE, (pages - value.label_pages) * PAGE_SIZE);
  value.label_pages = common::math::checked_cast<std::uint8_t>(pages, "compiled label page count overflow");
 }
}
void CompiledSlotStorage::prepare_pixels(std::uint32_t index) {
 auto custody = operation(index);
 prepare(index, 0, true);
}
CompiledSlotStorage::PixelLease::PixelLease(CompiledSlotStorage* owner, Slot* slot, float* data) : owner_(owner), slot_(slot), data_(data) {}
CompiledSlotStorage::PixelLease::PixelLease(PixelLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), slot_(std::exchange(other.slot_, nullptr)), data_(std::exchange(other.data_, nullptr)) {}
CompiledSlotStorage::PixelLease& CompiledSlotStorage::PixelLease::operator=(PixelLease&& other) noexcept {
 if (this != &other) {
  // CLEANUP-IGNORE: Pixel-storage and terminal-retirement leases release different owners and transfer different identity fields.
  release();
  owner_ = std::exchange(other.owner_, nullptr);
  slot_ = std::exchange(other.slot_, nullptr);
  data_ = std::exchange(other.data_, nullptr);
 }
 return *this;
}
CompiledSlotStorage::PixelLease::~PixelLease() { release(); }
void CompiledSlotStorage::PixelLease::release() noexcept {
 if (!owner_) return;
 {
  const std::lock_guard lock(owner_->mutex_);
  if (slot_) slot_->pixel_borrowed = false;
  --owner_->borrowers_;
 }
 owner_->changed_.notify_all();
 owner_ = nullptr;
}
CompiledSlotStorage::PixelLease CompiledSlotStorage::pixels(std::uint32_t index) {
 const std::lock_guard lock(mutex_);
 auto& value = slot_locked(index);
 if (closed_ || !value.pixel_ready || value.pixel_borrowed) throw std::logic_error("compiled pixels lack exclusive prepared custody");
 value.pixel_borrowed = true;
 ++borrowers_;
 return PixelLease(this, &value, value.pixels);
}
CompiledSlotStorage::PixelLease CompiledSlotStorage::operation(std::uint32_t index) {
 const std::lock_guard lock(mutex_);
 if (closed_) throw std::logic_error("compiled slot operation after seal");
 (void)slot_locked(index);
 ++borrowers_;
 return PixelLease(this, nullptr, nullptr);
}
CompiledSlotStorage::LabelLease::LabelLease(CompiledSlotStorage& owner, std::uint32_t index, PixelLease custody, std::unique_lock<std::mutex> lock)
    : owner_(&owner), index_(index), custody_(std::move(custody)), lock_(std::move(lock)) {}
CompiledSlotStorage::LabelLease CompiledSlotStorage::labels(std::uint32_t index) {
 auto custody = operation(index);
 auto& value = slot(index);
 return LabelLease(*this, index, std::move(custody), std::unique_lock(value.labels));
}
void CompiledSlotStorage::LabelLease::write(const ImageEntry& entry, CompiledLabelWorkspace::OutputLease output, std::uint64_t ordinal_base) {
 owner_->prepare(index_, entry.label_bytes, false);
 write_compiled_annotations(owner_->artifact_.file(), owner_->layout(), index_, entry, {output.labels, output.runs}, ordinal_base);
}
void CompiledSlotStorage::LabelLease::settle_ordinals(std::size_t count, std::uint64_t base, bool incomplete) {
 write_compiled_label_ordinals(owner_->artifact_.file(), owner_->layout().label_offset(index_), count, {}, base, incomplete);
}
void CompiledSlotStorage::quiesce() {
 std::unique_lock lock(mutex_);
 closed_ = true;
 changed_.wait(lock, [&] { return borrowers_ == 0; });
 lock.unlock();
 const std::lock_guard preparation(preparation_);
 blocks_.clear();
}
}  // namespace mmltk::backend::data::detail
