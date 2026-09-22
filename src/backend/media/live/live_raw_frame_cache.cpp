#include "detail/live_frame_fanout.h"
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::media::live {
LiveRawFrameCache::LiveRawFrameCache(const std::uint32_t count, const std::uint32_t width, const std::uint32_t height, LivePhysicalCudaContext cuda)
    : cuda_(std::move(cuda)),
      slots_(count == 0U ? nullptr : std::make_unique<Slot[]>(count)),
      slot_count_(count),
      width_(width),
      height_(height),
      pinned_bytes_(static_cast<std::size_t>(width) * height * 3U) {
 if (count == 0U || width == 0U || height == 0U || !cuda_.valid()) throw std::invalid_argument("Live raw cache requires fixed CUDA storage");
 try {
  auto scope = cuda_.scope();
  if (!scope) throw std::runtime_error("enter Live raw-cache CUDA scope");
  pinned_storage_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
  pinned_storage_->ensure_bytes(pinned_bytes_);
  pinned_ = static_cast<std::uint8_t*>(pinned_storage_->data());
  if (scope.Record(cudaStreamCreateWithFlags(&store_stream_, cudaStreamNonBlocking)) != cudaSuccess || scope.Record(cudaStreamCreateWithFlags(&readback_stream_, cudaStreamNonBlocking)) != cudaSuccess)
   throw std::runtime_error("create Live raw-cache streams");
  for (std::uint32_t index = 0; index < count; ++index) {
   slots_[index].index = index;
   if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slots_[index].pixels), &slots_[index].pitch, static_cast<std::size_t>(width) * 3U, height)) != cudaSuccess)
    throw std::runtime_error("allocate Live raw-cache slot");
   if (scope.Record(cudaEventCreateWithFlags(&slots_[index].ready, cudaEventDisableTiming)) != cudaSuccess) throw std::runtime_error("create Live raw-cache event");
  }
 } catch (...) {
  destroy();
  throw;
 }
}
LiveRawFrameCache::~LiveRawFrameCache() { destroy(); }
void LiveRawFrameCache::ScrubProduct(Slot& slot) noexcept {
 slot.frame = {};
 slot.region = {};
}
void LiveRawFrameCache::publish_slot(Slot& slot, const SlotState published) noexcept {
 publish_live_owner_slot(slot.state, published, [&slot] noexcept { ScrubProduct(slot); });
}
LiveRawFrameCache::Slot* LiveRawFrameCache::reserve() noexcept { return reserve_live_slot(slots_.get(), slot_count_).slot; }
cudaEvent_t LiveRawFrameCache::store(const DeviceFrameView& source) {
 Slot* const slot = reserve();
 if (slot == nullptr) return nullptr;
 auto scope = cuda_.scope();
 if (!scope) {
  publish_slot(*slot, SlotState::Terminal);
  return nullptr;
 }
 const cudaError_t status = copy_device_frame(scope, source, {.pixels = slot->pixels, .pitch_bytes = slot->pitch, .ready = slot->ready, .stream = store_stream_});
 if (status != cudaSuccess) {
  const bool synchronized = scope.Record(cudaStreamSynchronize(store_stream_)) == cudaSuccess;
  publish_slot(*slot, synchronized ? SlotState::Free : SlotState::Terminal);
  return nullptr;
 }
 slot->frame = source.frame;
 slot->region = source.region;
 publish_live_slot_state(slot->state, SlotState::Published);
 return slot->ready;
}
bool LiveRawFrameCache::begin_readback(const LiveRawFrameReadbackWork work) {
 if (!work.valid() || readback_work_.valid()) return false;
 Slot* slot = nullptr;
 for (std::uint32_t index = 0U; index < slot_count_; ++index) {
  if (transition_slot_state(slots_[index].state, SlotState::Published, SlotState::Acquired)) {
   if (slots_[index].frame != work.frame) {
    publish_live_slot_state(slots_[index].state, SlotState::Published);
    continue;
   }
   slot = &slots_[index];
   break;
  }
 }
 if (slot == nullptr) return false;
 const std::size_t row_bytes = static_cast<std::size_t>(slot->region.width) * 3U;
 const std::size_t bytes = row_bytes * slot->region.height;
 if (bytes > pinned_bytes_ || work.destination_bytes < bytes) {
  publish_live_slot_state(slot->state, SlotState::Published);
  return false;
 }
 auto scope = cuda_.scope();
 if (!scope) {
  publish_live_slot_state(slot->state, SlotState::Published);
  return false;
 }
 cudaError_t status = scope.Record(cudaStreamWaitEvent(readback_stream_, slot->ready, 0U));
 if (status == cudaSuccess)
  status = scope.Record(cudaMemcpy2DAsync(pinned_, row_bytes, reinterpret_cast<const void*>(slot->pixels), slot->pitch, row_bytes, slot->region.height, cudaMemcpyDeviceToHost, readback_stream_));
 readback_work_ = work;
 readback_slot_ = slot->index;
 readback_bytes_ = bytes;
 readback_complete_.store(false, std::memory_order_release);
 if (status == cudaSuccess) status = scope.Record(cudaLaunchHostFunc(readback_stream_, ReadbackComplete, this));
 if (status != cudaSuccess) {
  static_cast<void>(scope.Record(cudaStreamSynchronize(readback_stream_)));
  readback_work_ = {};
  readback_bytes_ = 0U;
  publish_live_slot_state(slot->state, SlotState::Published);
  return false;
 }
 return true;
}
void CUDART_CB LiveRawFrameCache::ReadbackComplete(void* context) noexcept {
 auto& cache = *static_cast<LiveRawFrameCache*>(context);
 cache.readback_complete_.store(true, std::memory_order_release);
 cache.ready_.notify();
}
std::optional<LiveRawFrameReadbackResult> LiveRawFrameCache::finish_readback(const bool completed) noexcept {
 if (!readback_work_.valid()) return std::nullopt;
 const LiveFrameId frame = readback_work_.frame;
 readback_work_ = {};
 const std::size_t bytes = completed ? readback_bytes_ : 0U;
 readback_bytes_ = 0U;
 if (readback_slot_ < slot_count_) publish_live_slot_state(slots_[readback_slot_].state, SlotState::Published);
 return LiveRawFrameReadbackResult{frame, completed ? pinned_ : nullptr, bytes, completed};
}
std::optional<LiveRawFrameReadbackResult> LiveRawFrameCache::take_readback_result() noexcept {
 if (!readback_complete_.exchange(false, std::memory_order_acq_rel)) return std::nullopt;
 return finish_readback(true);
}
std::optional<LiveRawFrameReadbackResult> LiveRawFrameCache::settle_readback() noexcept {
 if (!readback_work_.valid()) return std::nullopt;
 auto scope = cuda_.scope();
 const bool completed = scope && scope.Record(cudaStreamSynchronize(readback_stream_)) == cudaSuccess;
 readback_complete_.store(false, std::memory_order_release);
 return finish_readback(completed);
}
void LiveRawFrameCache::set_ready_listener(std::function<void()> listener) { ready_.set_listener(std::move(listener)); }
void LiveRawFrameCache::clear() noexcept {
 auto scope = cuda_.scope();
 bool synchronized = static_cast<bool>(scope);
 if (scope && store_stream_ != nullptr) synchronized = scope.Record(cudaStreamSynchronize(store_stream_)) == cudaSuccess;
 for (std::uint32_t index = 0U; index < slot_count_; ++index) {
  Slot& slot = slots_[index];
  const bool owned = claim_live_slot_retirement(slot.state);
  if (owned) publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
 }
}
void LiveRawFrameCache::destroy() noexcept {
 if (slots_ == nullptr) return;
 auto scope = cuda_.scope();
 if (scope) {
  synchronize_live_cuda_stream(scope, readback_stream_);
  synchronize_live_cuda_stream(scope, store_stream_);
 }
 readback_work_ = {};
 readback_bytes_ = 0U;
 readback_complete_.store(false, std::memory_order_release);
 for (std::uint32_t index = 0U; index < slot_count_; ++index) {
  if (scope) {
   destroy_live_cuda_event(scope, slots_[index].ready);
   free_live_cuda_allocation(scope, slots_[index].pixels);
  }
  slots_[index].ready = nullptr;
  slots_[index].pixels = 0U;
  slots_[index].pitch = 0U;
  slots_[index].frame = {};
  slots_[index].region = {};
 }
 if (scope) {
  destroy_live_cuda_stream(scope, readback_stream_);
  destroy_live_cuda_stream(scope, store_stream_);
 }
 if (pinned_ != nullptr) {
  pinned_storage_.reset();
  pinned_ = nullptr;
 }
 readback_stream_ = nullptr;
 store_stream_ = nullptr;
 slots_.reset();
}
}  // namespace mmltk::backend::media::live
