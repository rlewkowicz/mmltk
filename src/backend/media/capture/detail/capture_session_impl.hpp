#pragma once
#include "src/backend/media/capture/capture_session.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/common/system/numa_memory.h"
#include <cuda_runtime_api.h>
#include <linux/videodev2.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace mmltk::backend::media::capture {
namespace capture_internal {
using Clock = std::chrono::steady_clock;
enum class CaptureSlotPhase : std::uint8_t {
 kHardwareQueued = 0,
 kReplaceableFilled,
 kH2dActive,
 kCompletionPending,
 kRequeuePending,
 kOwnerRetained,
};
inline constexpr std::uint32_t kBgr3V4l2PixelFormat = V4L2_PIX_FMT_BGR24;
inline constexpr std::size_t kBgr3BytesPerPixel = 3;
inline constexpr std::uint32_t kPackedRegionFieldLimit = 0xFFFFU;
inline std::uint32_t CaptureSlotPhaseValue(const CaptureSlotPhase state) { return static_cast<std::uint32_t>(state); }
struct HostBuffer {
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> registered_storage;
 std::unique_ptr<mmltk::common::system::NumaMemory> storage;
 void* data = nullptr;
 std::size_t bytes = 0;
 bool pinned = false;
};
std::uint64_t NowNs();
Status MakeStatus(StatusCode code, std::string message);
Status MakeErrnoStatus(StatusCode code, const char* label);
Status MakeErrnoStatus(StatusCode code, const char* label, int error_number);
Status MakeCudaStatus(cudaError_t code, const char* label);
int Xioctl(int fd, unsigned long request, void* arg) noexcept;
std::size_t RoundUpToPage(std::size_t bytes, std::size_t page_size);
Status AllocateHostBuffer(std::size_t bytes, bool pinned, HostBuffer* out);
Status FreeHostBuffer(HostBuffer* buffer);
std::uint64_t PackRegion(const CaptureRegion& region);
CaptureRegion UnpackRegion(std::uint64_t packed);
}  // namespace capture_internal
struct CaptureSession::Impl {
 enum class CaptureReadyResult : std::uint8_t {
  kFrameReady = 0,
  kCompletionReady,
  kStopRequested,
  kCameraError,
  kCameraHangup,
 };
 enum class DequeueResult : std::uint8_t {
  kDequeued = 0,
  kNotReady,
  kCameraError,
 };
 enum class CaptureTeardownDisposition : std::uint8_t {
  kRequeueThenStreamOff,
  kOwnerRetainThenStreamOff,
  kNoStreamOrDeviceLost,
 };
 struct CaptureLoopResult {
  CaptureStopKind kind = CaptureStopKind::kRequested;
  Status status{};
  CaptureTeardownDisposition teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost;
 };
 struct HostSlotRuntime {
  std::uint32_t slot_index = 0;
  capture_internal::HostBuffer capture_buffer;
  std::atomic<std::uint32_t> phase{capture_internal::CaptureSlotPhaseValue(capture_internal::CaptureSlotPhase::kHardwareQueued)};
  std::uint64_t sequence = 0;
  std::uint64_t capture_ns = 0;
  bool short_frame = false;
  CaptureRegion region{};
 };
 explicit Impl(CaptureConfig config_in);
 ~Impl();
 CaptureSessionStartResult prepare_start();
 void cancel_prepared_start() noexcept;
 void capture_owner_main();
 Status request_stop(CaptureSessionIdentity identity);
 CaptureSessionIdentity active_identity() const noexcept;
 int stop_terminal_event_fd() const noexcept;
 std::shared_ptr<const CaptureStopTerminal> try_take_stop_terminal() noexcept;
 Status validate_finalize(const std::shared_ptr<const CaptureStopTerminal>& terminal) const;
 void mark_finalized() noexcept;
 bool owner_release_permitted() const noexcept;
 Status set_capture_region(CaptureRegion region);
 CaptureRegion snapshot_capture_region() const;
 CaptureFormatInfo snapshot_format() const;
 Status try_take_filled(FilledCaptureSlotLease* out_lease);
 Status mark_h2d_completion_pending(const FilledCaptureSlotLease& lease);
 Status return_after_h2d(FilledCaptureSlotLease&& lease) noexcept;
 Status report_failure(CaptureSessionIdentity identity, Status failure) noexcept;
 CaptureStats snapshot_stats() const;
 std::string last_error() const;
 void set_state_listener(std::function<void()> listener);
 void set_filled_frame_listener(std::function<void()> listener);

private:
 Status ValidateConfig() const;
 void ResetRuntimeState();
 CaptureRegion NormalizeRegion(CaptureRegion region) const;
 Status EnsureEventFds();
 Status TeardownSession(CaptureTeardownDisposition teardown);
 void PublishStopTerminal(CaptureLoopResult result);
 void CloseFilledAdmission() noexcept;
 void ReportCameraFault(const std::string& message);
 Status InitializeCuda();
 int DeviceIoctl(unsigned long request, void* argument) noexcept;
 Status OpenDevice();
 Status CloseDevice();
 Status ConfigureDevice();
 Status ConfigureCaptureFormat();
 Status ConfigureFrameRateAndBuffers();
 Status StartStreaming();
 Status StopStreaming();
 Status AllocateHostSlots();
 Status DestroyHostSlots();
 Status QueueAllV4l2Buffers();
 [[nodiscard]] int TryQueueV4l2Buffer(std::uint32_t slot_index) noexcept;
 Status QueueV4l2Buffer(std::uint32_t slot_index, bool record_requeue_failure);
 CaptureReadyResult WaitForCaptureReady();
 DequeueResult TryDequeueBuffer(v4l2_buffer* out, Status* error);
 void UpdateSequenceStats(std::uint32_t sequence);
 std::optional<std::uint32_t> ResolveHostSlotIndex(const v4l2_buffer& buf) const;
 CaptureLoopResult CaptureLoop();
 CaptureLoopResult StopRequestedResult() const;
 void RetainFirstFailure(CaptureSessionIdentity identity, Status failure) noexcept;
 [[nodiscard]] Status FirstFailure() const;
 Status RequeueCompletedSlots(CaptureTeardownDisposition* teardown);
 Status SettleSlotsBeforeTeardown(CaptureTeardownDisposition* teardown);
 Status HandleDequeuedBuffer(const v4l2_buffer& buf);
 void ZeroFillShortFrame(HostSlotRuntime& host_slot, std::size_t valid_bytes) const;
 void NotifyFilledFramePublished();
 void ResetStats();
 void ClearLastError();
 void SetLastError(const std::string& message);
 void NotifyState() const noexcept;
 CaptureConfig config;
 const std::uint64_t session_id_;
 int fd_ = -1;
 int stop_event_fd_ = -1;
 int terminal_event_fd_ = -1;
 std::atomic<bool> streaming_{false};
 std::uint32_t actual_v4l2_buffer_count_ = 0;
 std::size_t bytes_per_line_ = 0;
 std::size_t size_image_ = 0;
 std::atomic<std::uint32_t> published_width_{0};
 std::atomic<std::uint32_t> published_height_{0};
 std::atomic<std::uint32_t> published_bytes_per_line_{0};
 std::vector<std::unique_ptr<HostSlotRuntime>> host_slots_;
 mutable std::mutex error_mutex_;
 std::string last_error_;
 std::atomic<std::shared_ptr<const std::function<void()>>> state_listener_{};
 std::atomic<std::shared_ptr<const std::function<void()>>> filled_frame_listener_{};
 std::atomic<bool> running_{false};
 std::atomic<bool> shutdown_{false};
 std::atomic<bool> stop_requested_{false};
 std::atomic<bool> camera_fault_{false};
 std::atomic<bool> terminal_published_{false};
 std::atomic<bool> terminal_delivered_{false};
 std::atomic<bool> finalized_{true};
 std::atomic<std::uint64_t> active_generation_{0};
 std::atomic<std::shared_ptr<const CaptureStopTerminal>> terminal_{};
 struct PhysicalFailure final {
  CaptureSessionIdentity identity{};
  Status status{};
 };
 mutable std::mutex failure_mutex_;
 std::optional<PhysicalFailure> first_failure_;
 std::shared_ptr<CaptureStopTerminal> terminal_storage_;
 std::atomic<std::uint64_t> packed_region_{0};
 mutable std::mutex filled_mutex_;
 int replaceable_filled_index_ = -1;
 // CLEANUP-IGNORE: The capture completion descriptor and atomic telemetry are unrelated to Explore render-work vectors.
 int completion_event_fd_ = -1;
 // CLEANUP-IGNORE: Capture I/O telemetry is unrelated to Explore's typed render-work storage.
 std::atomic<std::uint64_t> queued_v4l2_buffers_{0};
 std::atomic<std::uint64_t> dequeued_v4l2_buffers_{0};
 std::atomic<std::uint64_t> bytes_captured_{0};
 std::atomic<std::uint64_t> filled_frames_published_{0};
 std::atomic<std::uint64_t> h2d_frames_admitted_{0};
 std::atomic<std::uint64_t> h2d_frames_completed_{0};
 std::atomic<std::uint64_t> frames_dropped_{0};
 // CLEANUP-IGNORE: Capture drop counters remain independent physical acquisition diagnostics.
 std::atomic<std::uint64_t> empty_frames_dropped_{0};
 std::atomic<std::uint64_t> replaced_filled_frames_{0};
 std::atomic<std::uint64_t> active_occupancy_drops_{0};
 std::atomic<std::uint64_t> short_frames_{0};
 std::atomic<std::uint64_t> sequence_gaps_{0};
 std::atomic<std::uint64_t> requeue_failures_{0};
 std::optional<std::uint32_t> last_sequence_;
 std::uint64_t next_frame_id_ = 1;
};
}  // namespace mmltk::backend::media::capture
