#pragma once
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/backend/data/compiled/compiled_format_limits.h"
#include "src/common/types/workspace_buffer.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/common/math/checked_arithmetic.h"
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::detail {
class CompiledLabelWorkspace final {
 using Memory = common::system::NumaMemory;
 static std::size_t aligned(std::size_t n) { return common::math::checked_add(n,std::size_t{63},"label workspace overflow")/64*64; }
 CompileAllowance custody_;
 std::shared_ptr<Memory> owned_;
 bool prepared_ = false;
 dataset::MaskResizeScratch scratch_;
public:
 common::types::WorkspaceBuffer<PackedInstance> labels;
 common::types::WorkspaceBuffer<RLEPair> runs;
 struct OutputLease {
  CompileAllowance custody;
  std::shared_ptr<const Memory> owned;
  std::span<const PackedInstance> labels;
  std::span<const RLEPair> runs;
  OutputLease() = default;
  OutputLease(OutputLease&&) noexcept = default;
  OutputLease& operator=(OutputLease&&) noexcept = default;
  OutputLease(const OutputLease&) = delete;
  OutputLease& operator=(const OutputLease&) = delete;
 };
 [[nodiscard]] static std::size_t required_bytes(std::uint32_t source_width) {
  const auto intervals = common::math::checked_multiply<std::size_t>((std::uint64_t{source_width}+1)/2,sizeof(std::pair<std::uint32_t,std::uint32_t>),"mask interval workspace overflow");
  return common::math::checked_add(aligned(COMPILED_LABEL_CAPACITY/sizeof(PackedInstance)*sizeof(PackedInstance)),
   common::math::checked_add(aligned(COMPILED_LABEL_CAPACITY/sizeof(RLEPair)*sizeof(RLEPair)),aligned(intervals),"label workspace overflow"),"label workspace overflow");
 }
 void prepare(std::uint32_t width, CompileAllowance custody = {}) {
  const auto bytes = required_bytes(width);
  std::span<std::byte> storage;
  if (custody) {
   storage = custody.workspace();
   if (storage.size() < bytes) throw std::length_error("compiled labels exceed supplied arena");
   custody_ = std::move(custody);
  } else {
   if (!owned_ || owned_.use_count()!=1) owned_ = std::make_shared<Memory>(common::system::NumaTopology::Capture().permitted_nodes.front(),Memory::Lock::Pages);
   owned_->ensure_bytes(bytes);
   storage = {static_cast<std::byte*>(owned_->data()),owned_->capacity_bytes()};
   custody_ = {};
  }
  const auto bind = [&]<class T>(common::types::WorkspaceBuffer<T>& target,std::size_t count) {
   target.bind({reinterpret_cast<T*>(storage.data()),count}); storage = storage.subspan(aligned(count*sizeof(T)));
  };
  bind(labels,COMPILED_LABEL_CAPACITY/sizeof(PackedInstance)); bind(runs,COMPILED_LABEL_CAPACITY/sizeof(RLEPair));
  bind(scratch_.intervals,(std::uint64_t{width}+1)/2);
  prepared_ = true;
 }
 [[nodiscard]] dataset::MaskResizeScratch& scratch() noexcept { return scratch_; }
 [[nodiscard]] std::size_t retained_bytes() const noexcept { return custody_ ? custody_.bytes() : owned_ ? owned_->capacity_bytes() : 0; }
 void release_borrow() noexcept { custody_={}; prepared_=false; }
 [[nodiscard]] OutputLease release_output() {
  if (!prepared_) throw std::logic_error("compiled labels have no prepared output");
  OutputLease result;
  result.custody = std::move(custody_); result.owned = owned_; result.labels = {labels.data(),labels.size()}; result.runs = {runs.data(),runs.size()};
  prepared_ = false;
  return result;
 }
};
}
