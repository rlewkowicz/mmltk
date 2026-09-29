#include "detail/writable_pixel_range.h"
#include <sys/mman.h>
#include <sys/types.h>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::detail {
WritablePixelRange::WritablePixelRange(int descriptor, const FileLayout& layout) : descriptor_(descriptor) { grow(layout); }
void WritablePixelRange::grow(const FileLayout& layout) {
 const std::lock_guard lock(mutex_);
 if (layout_.image_stride &&
     (layout_.image_stride != layout.image_stride || layout.num_slots < layout_.num_slots || layout_.label_pitch != layout.label_pitch || layout_.block_bytes != layout.block_bytes))
  throw std::logic_error("writable compiled layout changed");
 const auto count = layout.block_count();
 blocks_.reserve(count);
 while (blocks_.size() < count) {
  const auto bytes = layout.pixel_block_bytes;
  void* mapping =
   ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, mmltk::common::math::checked_cast<off_t>(layout.block_offset(blocks_.size()), "compiled block mmap offset overflow"));
  if (mapping == MAP_FAILED) throw mmltk::common::io::errno_error("compiled pixel block mmap failed");
  mmltk::common::io::MappedByteRegion region;
  region.adopt(mapping, bytes);
  (void)::madvise(mapping, bytes, MADV_HUGEPAGE);
  blocks_.push_back(std::move(region));
 }
 layout_ = layout;
}
float* WritablePixelRange::image(std::uint32_t slot) const {
 const std::lock_guard lock(mutex_);
 if (slot >= layout_.num_slots) throw std::out_of_range("writable compiled pixel slot");
 const auto block = layout_.block_index(slot);
 return reinterpret_cast<float*>(static_cast<std::byte*>(blocks_[block].address()) + layout_.pixel_offset(slot) - layout_.block_offset(block));
}
}  // namespace mmltk::backend::data::detail
