#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <sys/mman.h>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutStaging::Block {
 detail::StagedArtifact artifact;
 common::io::MappedByteRegion mapping;
 std::size_t used = 0;
 Block(detail::StorageReservationPool& storage, const std::filesystem::path& directory, std::size_t bytes)
  : artifact(detail::StagedArtifact::create(storage, directory / ".coconut-block", bytes, "COCONut immutable ranges")) {
  artifact.preallocate(bytes);
  auto* address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, artifact.file().get(), 0);
  if (address == MAP_FAILED) throw common::io::errno_error("map COCONut staging block", artifact.path().string());
  mapping.adopt(address, bytes);
  artifact.close();
 }
};
CoconutStaging::CoconutStaging(const std::filesystem::path& directory, detail::StorageReservationPool* storage, std::size_t block_bytes)
 : directory_(directory.empty() ? std::filesystem::temp_directory_path() : directory), storage_(directory_, {}, storage), block_bytes_(block_bytes) {
 if (!block_bytes_) throw std::invalid_argument("empty COCONut staging block");
 std::filesystem::create_directories(directory_);
}
void CoconutStaging::retire() noexcept { const std::lock_guard lock(mutex_); active_.reset(); }
CoconutStaging::Bytes CoconutStaging::append(std::size_t bytes, std::size_t alignment, const std::function<void(std::span<std::byte>)>& fill) {
 if (!alignment || !std::has_single_bit(alignment) || alignment > 4096) throw std::invalid_argument("invalid COCONut staging alignment");
 if (!bytes) return {};
 std::shared_ptr<Block> owner;
 std::span<std::byte> target;
 {
  const std::lock_guard lock(mutex_);
  const auto aligned = [&](std::size_t offset) { return common::math::checked_add(offset, alignment - 1, "COCONut staging alignment overflow") & ~(alignment - 1); };
  auto offset = active_ ? aligned(active_->used) : 0;
  if (!active_ || offset > active_->mapping.mapped_bytes() || bytes > active_->mapping.mapped_bytes() - offset) {
   active_ = std::make_shared<Block>(storage_, directory_, std::max(block_bytes_, aligned(bytes)));
   offset = 0;
  }
  owner = active_;
  owner->used = common::math::checked_add(offset, bytes, "COCONut staging range overflow");
  target = {static_cast<std::byte*>(owner->mapping.address()) + offset, bytes};
 }
 // Disjoint claims fill independently. A failed fill leaves an unpublished hole.
 fill(target);
 return {std::move(owner), target};
}
}
