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
LiveFrameFanout::LiveFrameFanout(LiveVideoIngress& ingress, const std::uint32_t count, const std::uint32_t width, const std::uint32_t height, LivePhysicalCudaContext cuda)
    : ingress_(ingress),
      cuda_(std::move(cuda)),
      raw_cache_(count, width, height, cuda_),
      analysis_(count == 0U ? nullptr : std::make_unique<FanoutSlot[]>(count)),
      composite_(count == 0U ? nullptr : std::make_unique<FanoutSlot[]>(count)),
      releases_(std::make_unique<SourceRelease[]>(ingress.slot_count())),
      slot_count_(count),
      width_(width),
      height_(height) {
 if (count == 0U || width == 0U || height == 0U || !cuda_.valid()) throw std::invalid_argument("Live fanout requires fixed CUDA storage");
 try {
  auto scope = cuda_.scope();
  if (!scope) throw std::runtime_error("enter Live fanout CUDA scope");
  const auto initialize = [&](FanoutSlot* slots) {
   for (std::uint32_t index = 0; index < count; ++index) {
    FanoutSlot& slot = slots[index];
    slot.index = index;
    if (scope.Record(cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking)) != cudaSuccess) throw std::runtime_error("create Live fanout stream");
    if (scope.Record(cudaEventCreateWithFlags(&slot.ready, cudaEventDisableTiming)) != cudaSuccess) throw std::runtime_error("create Live fanout event");
    if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slot.pixels), &slot.pitch, static_cast<std::size_t>(width) * 3U, height)) != cudaSuccess)
     throw std::runtime_error("allocate Live fanout slot");
   }
  };
  initialize(analysis_.get());
  initialize(composite_.get());
  if (scope.Record(cudaStreamCreateWithFlags(&release_stream_, cudaStreamNonBlocking)) != cudaSuccess) throw std::runtime_error("create Live fanout release stream");
 } catch (...) {
  destroy();
  throw;
 }
 for (std::uint32_t index = 0; index < ingress.slot_count(); ++index) {
  releases_[index].owner = this;
  releases_[index].ingress_slot = index;
 }
}
LiveFrameFanout::~LiveFrameFanout() { destroy(); }
void LiveFrameFanout::start() noexcept { running_.store(true, std::memory_order_release); }
void LiveFrameFanout::ScrubProduct(FanoutSlot& slot) noexcept { slot.metadata = {}; }
void LiveFrameFanout::publish_slot(FanoutSlot* slots, FanoutSlot& slot, const SlotState published) noexcept {
 auto& latest = slots == analysis_.get() ? latest_analysis_ : latest_composite_;
 publish_latest_live_owner_slot(latest, slot.index, slot.state, published, [&slot] noexcept { ScrubProduct(slot); });
}
void LiveFrameFanout::settle_slots(FanoutSlot* slots) noexcept {
 auto scope = cuda_.scope();
 for (std::uint32_t index = 0U; index < slot_count_; ++index) {
  FanoutSlot& slot = slots[index];
  if (const auto published = retire_live_slot(scope, slot.state, slot.stream)) publish_slot(slots, slot, *published);
 }
}
void LiveFrameFanout::close_admission() noexcept { running_.store(false, std::memory_order_release); }
void LiveFrameFanout::stop() noexcept {
 close_admission();
 latest_analysis_.store(-1, std::memory_order_release);
 latest_composite_.store(-1, std::memory_order_release);
 auto scope = cuda_.scope();
 if (scope && release_stream_ != nullptr) static_cast<void>(scope.Record(cudaStreamSynchronize(release_stream_)));
 settle_slots(analysis_.get());
 settle_slots(composite_.get());
 raw_cache_.clear();
}
LiveFrameFanout::FanoutSlot* LiveFrameFanout::reserve(FanoutSlot* slots, std::atomic<int>& latest) noexcept {
 const auto reservation = reserve_live_slot(slots, slot_count_, &latest);
 if (reservation.replaced) replaced_.fetch_add(1U, std::memory_order_relaxed);
 return reservation.slot;
}
bool LiveFrameFanout::process_latest() {
 if (!running_.load(std::memory_order_acquire)) return false;
 DeviceFrameView source{};
 if (!ingress_.try_acquire_latest(&source)) return false;
 FanoutSlot* analysis = reserve(analysis_.get(), latest_analysis_);
 FanoutSlot* composite = reserve(composite_.get(), latest_composite_);
 // Analysis has no consumer without the exact matching composite branch.
 // Return that unstarted reservation immediately instead of retaining a
 // provider result which can never reach its last GPU reader.
 if (composite == nullptr && analysis != nullptr) {
  publish_slot(analysis_.get(), *analysis, SlotState::Free);
  analysis = nullptr;
 }
 auto scope = cuda_.scope();
 if (!scope) {
  if (analysis != nullptr) { publish_slot(analysis_.get(), *analysis, SlotState::Terminal); }
  if (composite != nullptr) { publish_slot(composite_.get(), *composite, SlotState::Terminal); }
  ingress_.release(source.slot);
  return false;
 }
 const cudaEvent_t raw_ready = raw_cache_.store(source);
 if (analysis == nullptr && composite == nullptr && raw_ready == nullptr) {
  dropped_.fetch_add(1U, std::memory_order_relaxed);
  ingress_.release(source.slot);
  return true;
 }
 const auto copy = [&](FanoutSlot* target, std::atomic<int>& latest) {
  if (target == nullptr) return true;
  const cudaError_t status = copy_device_frame(scope, source, {.pixels = target->pixels, .pitch_bytes = target->pitch, .ready = target->ready, .stream = target->stream});
  if (status != cudaSuccess) return false;
  target->metadata = DeviceFrameMetadata::From(source);
  publish_live_slot_state(target->state, SlotState::Published);
  latest.store(static_cast<int>(target->index), std::memory_order_release);
  return true;
 };
 const bool analysis_ok = copy(analysis, latest_analysis_);
 const bool composite_ok = copy(composite, latest_composite_);
 if (!analysis_ok || !composite_ok) {
  const bool settled = settle_source_reads(analysis, composite, raw_ready);
  if (analysis != nullptr) { publish_slot(analysis_.get(), *analysis, settled ? SlotState::Free : SlotState::Terminal); }
  if (composite != nullptr) { publish_slot(composite_.get(), *composite, settled ? SlotState::Free : SlotState::Terminal); }
  if (settled)
   ingress_.release(source.slot);
  else
   ingress_.terminalize(source.slot);
  dropped_.fetch_add(1U, std::memory_order_relaxed);
  return false;
 }
 SourceRelease& release = releases_[source.slot];
 cudaError_t release_status = cudaSuccess;
 if (analysis != nullptr) release_status = scope.Record(cudaStreamWaitEvent(release_stream_, analysis->ready, 0U));
 if (release_status == cudaSuccess && composite != nullptr) release_status = scope.Record(cudaStreamWaitEvent(release_stream_, composite->ready, 0U));
 if (release_status == cudaSuccess && raw_ready != nullptr) release_status = scope.Record(cudaStreamWaitEvent(release_stream_, raw_ready, 0U));
 if (release_status == cudaSuccess) release_status = scope.Record(cudaLaunchHostFunc(release_stream_, ReleaseSource, &release));
 if (release_status != cudaSuccess) {
  if (settle_source_reads(analysis, composite, raw_ready))
   ingress_.release(source.slot);
  else
   ingress_.terminalize(source.slot);
  return false;
 }
 fanned_.fetch_add(1U, std::memory_order_relaxed);
 ready_.notify();
 return true;
}
void CUDART_CB LiveFrameFanout::ReleaseSource(void* context) noexcept {
 auto& release = *static_cast<SourceRelease*>(context);
 release.owner->ingress_.release(release.ingress_slot);
}
bool LiveFrameFanout::acquire(FanoutSlot* slots, std::atomic<int>& latest, DeviceFrameView* output) noexcept {
 if (output == nullptr) return false;
 const int index = latest.load(std::memory_order_acquire);
 if (index < 0 || index >= static_cast<int>(slot_count_)) return false;
 FanoutSlot& slot = slots[index];
 if (!transition_slot_state(slot.state, SlotState::Published, SlotState::Acquired)) return false;
 *output = slot.metadata.View(slot.index, slot.pixels, slot.pitch, slot.ready, slot.stream);
 return true;
}
bool LiveFrameFanout::try_acquire_analysis(DeviceFrameView* output) { return acquire(analysis_.get(), latest_analysis_, output); }
bool LiveFrameFanout::try_acquire_composite(DeviceFrameView* output) { return acquire(composite_.get(), latest_composite_, output); }
void LiveFrameFanout::release(FanoutSlot* slots, const std::uint32_t index, cudaEvent_t completion, cudaStream_t stream) {
 if (index >= slot_count_) throw std::out_of_range("Live fanout slot");
 FanoutSlot& slot = slots[index];
 if (!claim_live_slot(slot.state, SlotState::Acquired)) {
  cuda_.Record(cudaErrorUnknown);
  return;
 }
 if ((completion == nullptr) != (stream == nullptr)) {
  cuda_.Record(cudaErrorInvalidResourceHandle);
  publish_slot(slots, slot, SlotState::Terminal);
  return;
 }
 if (completion != nullptr && stream != nullptr) {
  auto scope = cuda_.scope();
  if (!scope || scope.Record(cudaStreamWaitEvent(slot.stream, completion, 0U)) != cudaSuccess) {
   publish_slot(slots, slot, SlotState::Terminal);
   return;
  }
 }
 publish_slot(slots, slot, SlotState::Free);
}
void LiveFrameFanout::release_analysis(const std::uint32_t slot, cudaEvent_t completion, cudaStream_t stream) { release(analysis_.get(), slot, completion, stream); }
void LiveFrameFanout::release_composite(const std::uint32_t slot, cudaEvent_t completion, cudaStream_t stream) { release(composite_.get(), slot, completion, stream); }
bool LiveFrameFanout::settle_source_reads(FanoutSlot* const analysis, FanoutSlot* const composite, const cudaEvent_t raw_ready) noexcept {
 auto scope = cuda_.scope();
 if (!scope) return false;
 bool settled = true;
 if (analysis != nullptr && analysis->stream != nullptr) settled = scope.Record(cudaStreamSynchronize(analysis->stream)) == cudaSuccess && settled;
 if (composite != nullptr && composite->stream != nullptr) settled = scope.Record(cudaStreamSynchronize(composite->stream)) == cudaSuccess && settled;
 if (raw_ready != nullptr) settled = scope.Record(cudaEventSynchronize(raw_ready)) == cudaSuccess && settled;
 if (release_stream_ != nullptr) settled = scope.Record(cudaStreamSynchronize(release_stream_)) == cudaSuccess && settled;
 return settled;
}
bool LiveFrameFanout::begin_raw_readback(LiveRawFrameReadbackWork work) { return raw_cache_.begin_readback(work); }
std::optional<LiveRawFrameReadbackResult> LiveFrameFanout::take_raw_readback_result() noexcept { return raw_cache_.take_readback_result(); }
std::optional<LiveRawFrameReadbackResult> LiveFrameFanout::settle_raw_readback() noexcept { return raw_cache_.settle_readback(); }
void LiveFrameFanout::set_raw_readback_listener(std::function<void()> listener) { raw_cache_.set_ready_listener(std::move(listener)); }
void LiveFrameFanout::set_ready_listener(std::function<void()> listener) { ready_.set_listener(std::move(listener)); }
LiveFrameFanout::Status LiveFrameFanout::status() const noexcept {
 return {running_.load(std::memory_order_acquire), fanned_.load(std::memory_order_relaxed), dropped_.load(std::memory_order_relaxed), replaced_.load(std::memory_order_relaxed)};
}
void LiveFrameFanout::destroy() noexcept {
 if (analysis_ == nullptr && composite_ == nullptr) return;
 stop();
 auto scope = cuda_.scope();
 if (scope) synchronize_live_cuda_stream(scope, release_stream_);
 const auto release = [&](FanoutSlot* slots) noexcept {
  if (slots == nullptr) return;
  for (std::uint32_t index = 0; index < slot_count_; ++index) {
   if (scope) {
    synchronize_live_cuda_stream(scope, slots[index].stream);
    destroy_live_cuda_event(scope, slots[index].ready);
    destroy_live_cuda_stream(scope, slots[index].stream);
    free_live_cuda_allocation(scope, slots[index].pixels);
   }
   slots[index].ready = nullptr;
   slots[index].stream = nullptr;
   slots[index].pixels = 0U;
   slots[index].pitch = 0U;
   slots[index].metadata = {};
  }
 };
 release(analysis_.get());
 release(composite_.get());
 if (scope) destroy_live_cuda_stream(scope, release_stream_);
 release_stream_ = nullptr;
 analysis_.reset();
 composite_.reset();
 releases_.reset();
}
}  // namespace mmltk::backend::media::live
