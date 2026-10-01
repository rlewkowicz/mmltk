#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/file_work_queue.h"
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
 Block(detail::StorageReservationPool& storage, const std::filesystem::path& directory, std::size_t bytes) {
  std::filesystem::create_directories(directory);
  artifact = detail::StagedArtifact::create(storage, directory / ".coconut-block", bytes, "COCONut immutable ranges");
  artifact.preallocate(bytes);
  auto* address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, artifact.file().get(), 0);
  if (address == MAP_FAILED) throw common::io::errno_error("map COCONut staging block", artifact.path().string());
  mapping.adopt(address, bytes);
  artifact.close();
 }
};
CoconutStaging::Request::Request(std::size_t bytes, std::size_t alignment) : bytes_(bytes), alignment_(alignment) {
 if (!alignment || !std::has_single_bit(alignment) || alignment > 4096) throw std::invalid_argument("invalid COCONut staging alignment");
}
std::span<std::byte> CoconutStaging::Request::target() const {
 if (!ready()) throw std::logic_error("COCONut staging range is not prepared");
 return target_;
}
CoconutStaging::CoconutStaging(const std::filesystem::path& directory, detail::StorageReservationPool* storage, std::size_t block_bytes)
    : directory_(directory.empty() ? std::filesystem::temp_directory_path() : directory), storage_(directory_, {}, storage), block_bytes_(block_bytes) {
 if (!block_bytes_) throw std::invalid_argument("empty COCONut staging block");
}
void CoconutStaging::retire() noexcept {
 std::shared_ptr<Block> retired;
 {
  const std::lock_guard lock(mutex_);
  retired = std::move(active_);
 }
}
bool CoconutStaging::try_claim(Request& request) {
 if (request.ready()) return true;
 const std::lock_guard lock(mutex_);
 if (!active_) return false;
 const auto offset = common::math::checked_add(active_->used, request.alignment_ - 1, "COCONut staging alignment overflow") & ~(request.alignment_ - 1);
 if (offset > active_->mapping.mapped_bytes() || request.bytes_ > active_->mapping.mapped_bytes() - offset) return false;
 active_->used = common::math::checked_add(offset, request.bytes_, "COCONut staging range overflow");
 request.owner_ = active_;
 request.target_ = {static_cast<std::byte*>(active_->mapping.address()) + offset, request.bytes_};
 return true;
}
void CoconutStaging::prepare(Request& request, const detail::CompileAllowance& descriptor) {
 // File dispatchers serialize block growth, not claims or mapped copies. A
 // second ordinary row rechecks the prepared block instead of allocating one.
 const std::lock_guard preparing(preparation_mutex_);
 if (try_claim(request)) return;
 if (descriptor && !descriptor.descriptors()) throw std::logic_error("COCONut staging preparation has no admitted descriptor");
 const auto bytes = common::math::checked_add(request.bytes_, request.alignment_ - 1, "COCONut staging extent overflow") & ~(request.alignment_ - 1);
 auto block = std::make_shared<Block>(storage_, directory_, std::max(block_bytes_, bytes));
 std::shared_ptr<Block> retired;
 {
  const std::lock_guard lock(mutex_);
  retired = std::exchange(active_, block);
  block->used = request.bytes_;
  request.owner_ = block;
  request.target_ = {static_cast<std::byte*>(block->mapping.address()), request.bytes_};
 }
}
void CoconutStaging::prepare(Request& request, detail::CompileExecution* execution, detail::FileWorkQueue* files, const detail::CompileAllowance& parent) {
 if (execution && (!files || execution->on_lane())) throw std::logic_error("COCONut staging preparation requires a controller and its file service");
 if (try_claim(request)) return;
 if (!execution) {
  prepare(request, detail::CompileAllowance{});
  return;
 }
 // The mapped block owns its retained controls; preparation needs one raw
 // descriptor and no additional transient payload or supplied workspace.
 const auto descriptor = execution->reserve({.descriptors = 1}, parent);
 files->submit({.work = [this, &request, descriptor] { prepare(request, descriptor); }}).wait();
}
}  // namespace mmltk::backend::data::benchmark_internal
