#pragma once
#include "src/backend/media/capture/capture_session.h"
#include "src/backend/media/capture/capture_types.h"
#include "src/backend/media/capture/status.h"
#include "live_slot_state.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "live_device_types.h"
#include "live_state_signal.h"
namespace mmltk::backend::media::live {
namespace capture = mmltk::backend::media::capture;
class LiveVideoIngress final {
public:
 using FailureListener = std::function<void(capture::Status)>;
 LiveVideoIngress(capture::CaptureConfig config, std::uint32_t slot_count, LivePhysicalCudaContext cuda);
 ~LiveVideoIngress();
 LiveVideoIngress(const LiveVideoIngress&) = delete;
 LiveVideoIngress& operator=(const LiveVideoIngress&) = delete;
 [[nodiscard]] capture::CaptureSessionStartResult start();
 [[nodiscard]] capture::Status request_stop() noexcept;
 [[nodiscard]] std::shared_ptr<const capture::CaptureStopTerminal> try_take_terminal() noexcept;
 [[nodiscard]] capture::Status finalize_terminal(const std::shared_ptr<const capture::CaptureStopTerminal>& terminal);
 [[nodiscard]] capture::Status report_failure(capture::Status failure) noexcept;
 void settle() noexcept;
 [[nodiscard]] bool ingest_next();
 [[nodiscard]] bool try_acquire_latest(DeviceFrameView* output);
 void release(std::uint32_t slot) noexcept;
 void terminalize(std::uint32_t slot) noexcept;
 void set_ready_listener(std::function<void()> listener);
 void set_ingestion_wake(std::function<void()> listener);
 void set_failure_listener(FailureListener listener);
 [[nodiscard]] bool running() const noexcept;
 [[nodiscard]] capture::CaptureStats snapshot_stats() const;
 [[nodiscard]] inline std::uint32_t slot_count() const noexcept { return slot_count_; }

private:
 struct DeviceSlot final {
  std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
  capture::FilledCaptureSlotLease lease{};
  CUdeviceptr pixels = 0U;
  std::size_t pitch_bytes = 0U;
  cudaStream_t stream = nullptr;
  cudaEvent_t ready = nullptr;
  DeviceFrameMetadata metadata{};
  LiveVideoIngress* owner = nullptr;
  std::uint32_t index = 0U;
 };
 static void CUDART_CB CompleteSlot(void* context) noexcept;
 static void ScrubProduct(DeviceSlot& slot) noexcept;
 void publish_slot(DeviceSlot& slot, SlotState published) noexcept;
 [[nodiscard]] DeviceSlot* reserve() noexcept;
 void complete(DeviceSlot& slot) noexcept;
 void fail_upload(DeviceSlot& slot, cudaError_t failure, capture::Status capture_failure = capture::Status::Ok()) noexcept;
 void publish_failure(capture::Status failure) const noexcept;
 void release_resources() noexcept;
 capture::CaptureConfig config_{};
 LivePhysicalCudaContext cuda_{};
 capture::CaptureSession capture_;
 std::unique_ptr<DeviceSlot[]> slots_;
 std::uint32_t slot_count_ = 0U;
 std::atomic<int> latest_{-1};
 std::atomic<bool> running_{false};
 capture::CaptureSessionIdentity identity_{};
 mutable std::mutex lifecycle_;
 LiveStateSignal ready_signal_;
 LiveStateSignal ingestion_signal_;
 std::atomic<std::shared_ptr<const FailureListener>> failure_listener_{};
};
}  // namespace mmltk::backend::media::live
