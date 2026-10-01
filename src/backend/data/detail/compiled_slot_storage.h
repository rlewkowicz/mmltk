#pragma once
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
namespace mmltk::backend::data::detail {
// The staged inode is borrowed. Slots are logical facts until a bounded file
// operation prepares them; only immutable mapping handles escape to CPU writers.
class CompiledSlotStorage final {
 struct Slot;
 struct SlotBlock;

public:
 class PixelLease final {
 public:
  PixelLease() = default;
  PixelLease(PixelLease&&) noexcept;
  PixelLease& operator=(PixelLease&&) noexcept;
  ~PixelLease();
  PixelLease(const PixelLease&) = delete;
  PixelLease& operator=(const PixelLease&) = delete;
  [[nodiscard]] float* data() const noexcept { return data_; }

 private:
  friend class CompiledSlotStorage;
  PixelLease(CompiledSlotStorage*, Slot*, float*);
  void release() noexcept;
  CompiledSlotStorage* owner_ = nullptr;
  Slot* slot_ = nullptr;
  float* data_ = nullptr;
 };
 class LabelLease final {
 public:
  LabelLease(LabelLease&&) noexcept = default;
  LabelLease& operator=(LabelLease&&) noexcept = default;
  LabelLease(const LabelLease&) = delete;
  LabelLease& operator=(const LabelLease&) = delete;
  void write(const ImageEntry&, CompiledLabelWorkspace::OutputLease, std::uint64_t ordinal_base = 0);
  void settle_ordinals(std::size_t count, std::uint64_t base, bool incomplete);

 private:
  friend class CompiledSlotStorage;
  LabelLease(CompiledSlotStorage&, std::uint32_t, PixelLease, std::unique_lock<std::mutex>);
  CompiledSlotStorage* owner_;
  std::uint32_t index_;
  PixelLease custody_;
  std::unique_lock<std::mutex> lock_;
 };
 CompiledSlotStorage(StagedArtifact&, const FileLayout&);
 ~CompiledSlotStorage();
 CompiledSlotStorage(const CompiledSlotStorage&) = delete;
 CompiledSlotStorage& operator=(const CompiledSlotStorage&) = delete;
 // Metadata-only: safe beneath a caller's short source/facts lock.
 [[nodiscard]] std::uint32_t reserve_slot();
 [[nodiscard]] FileLayout layout() const;
 // File-service operations, never called beneath source/facts/admission locks.
 void prepare_pixels(std::uint32_t);
 [[nodiscard]] PixelLease pixels(std::uint32_t);
 [[nodiscard]] LabelLease labels(std::uint32_t);
 // No new borrowers after this boundary; joins all physical writers then unmaps.
 void quiesce();

private:
 [[nodiscard]] Slot& slot(std::uint32_t);
 [[nodiscard]] Slot& slot_locked(std::uint32_t);
 [[nodiscard]] PixelLease operation(std::uint32_t);
 void prepare(std::uint32_t, std::size_t label_bytes, bool pixels);
 StagedArtifact& artifact_;
 mutable std::mutex mutex_;
 std::condition_variable changed_;
 FileLayout layout_;
 std::vector<std::unique_ptr<SlotBlock>> slots_;
 std::vector<common::io::MappedByteRegion> blocks_;
 std::mutex preparation_;
 std::size_t prepared_length_ = 0;
 std::size_t borrowers_ = 0;
 bool closed_ = false;
};
}  // namespace mmltk::backend::data::detail
