#pragma once
#include "src/backend/media/live/live_types.h"
#include "live_device_types.h"
#include "live_frame_fanout.h"
#include "live_slot_state.h"
#include "live_state_signal.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include "live_analyzer_worker.h"
#include "live_manual_overlay_worker.h"
#include "workspace_frame_signal.h"
namespace mmltk::backend::media::live {
struct LiveCompositorTelemetry {
 bool running = false;
 std::uint64_t frames_composited = 0;
 std::uint64_t frames_dropped = 0;
 std::uint64_t front_revision = 0;
};
// CLEANUP-IGNORE: LiveCompositor has distinct physical compositor custody despite a conventional sealed-owner API.
class LiveCompositor final {
public:
 // CLEANUP-IGNORE: Its constructor and lifecycle are independent from manual-overlay worker ownership.
 LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer, LiveManualOverlayWorker* manual_overlay, LiveCompletedFramePublication& publication, std::uint32_t slot_count,
  std::uint32_t width,
  // CLEANUP-IGNORE: Physical compositor dimensions and CUDA context are explicit construction facts.
  std::uint32_t height, LivePhysicalCudaContext cuda);
 ~LiveCompositor();
 LiveCompositor(const LiveCompositor&) = delete;
 LiveCompositor& operator=(const LiveCompositor&) = delete;
 void start() noexcept;
 void close_admission() noexcept;
 void stop() noexcept;
 [[nodiscard]] bool process_latest();
 [[nodiscard]] bool drain_completions();
 [[nodiscard]] bool settled() const noexcept;
 [[nodiscard]] bool try_acquire(
  PhysicalFrameRevision revision, LiveCompositeOutputLease* output, void* callback_owner, LiveCompositeOutputLease::CompleteCallback complete, LiveCompositeOutputLease::AbandonCallback abandon);
 void complete_output(PhysicalFrameRevision frame_revision) noexcept;
 void abandon_output(PhysicalFrameRevision frame_revision) noexcept;
 void set_revision_listener(std::function<void()> listener);
 void set_completion_listener(std::function<void()> listener);
 [[nodiscard]] std::optional<PhysicalFrameRevision> newest_revision() const noexcept;
 [[nodiscard]] LiveCompositorTelemetry status() const noexcept;

private:
 struct CompositeSlot final {
  std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
  CUdeviceptr rgba = 0U;
  std::size_t rgba_pitch = 0U;
  CUdeviceptr overlay = 0U;
  std::size_t overlay_pitch = 0U;
  cudaStream_t stream = nullptr;
  cudaEvent_t ready = nullptr;
  DeviceFrameMetadata metadata{};
  std::uint64_t revision = 0U;
  std::uint32_t index = 0U;
  std::uint32_t source_slot = 0U;
  std::optional<std::uint32_t> analysis_slot;
  std::optional<std::uint32_t> manual_overlay_slot;
  std::atomic<bool> producer_complete{false};
  LiveCompositor* owner = nullptr;
 };
 static void CUDART_CB ProducerComplete(void* context) noexcept;
 static void ScrubLogicalProduct(CompositeSlot& slot) noexcept;
 void publish_slot(CompositeSlot& slot, SlotState published) noexcept;
 void finish_output(PhysicalFrameRevision, bool completed) noexcept;
 [[nodiscard]] CompositeSlot* reserve() noexcept;
 void complete(CompositeSlot& slot) noexcept;
 void destroy() noexcept;
 LiveFrameFanout& fanout_;
 LiveAnalyzerWorker* analyzer_ = nullptr;
 LiveManualOverlayWorker* manual_overlay_ = nullptr;
 LiveCompletedFramePublication& publication_;
 LivePhysicalCudaContext cuda_{};
 std::unique_ptr<CompositeSlot[]> slots_;
 std::uint32_t slot_count_ = 0U;
 std::uint32_t width_ = 0U;
 std::uint32_t height_ = 0U;
 std::atomic<bool> running_{false};
 std::atomic<std::uint64_t> revision_{0U};
 std::atomic<std::uint64_t> frames_{0U};
 std::atomic<std::uint64_t> dropped_{0U};
 LiveStateSignal revision_signal_;
 LiveStateSignal completion_signal_;
};
}  // namespace mmltk::backend::media::live
