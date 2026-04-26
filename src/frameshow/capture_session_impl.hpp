#pragma once

#include "frameshow/capture_session.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <cuda_runtime_api.h>
#include <linux/videodev2.h>

namespace frameshow {

namespace capture_internal {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kBgr3V4l2PixelFormat = V4L2_PIX_FMT_BGR24;
constexpr std::size_t kBgr3BytesPerPixel = 3;
constexpr std::uint32_t kPackedRegionFieldLimit = 0xFFFFU;

enum class InferenceState : std::uint8_t {
    kFree = 0,
    kWriting = 1,
    kPublished = 2,
    kAcquired = 3,
};

enum class PreviewState : std::uint8_t {
    kFree = 0,
    kPublished = 1,
    kDisplaying = 2,
};

template <typename StateEnum>
inline std::uint32_t ToStateValue(StateEnum state) {
    return static_cast<std::uint32_t>(state);
}

struct HostBuffer {
    void* data = nullptr;
    std::size_t bytes = 0;
    bool pinned = false;
};

std::uint64_t NowNs();
Status MakeStatus(StatusCode code, std::string message);
Status MakeErrnoStatus(StatusCode code, const char* label);
Status MakeCudaStatus(cudaError_t code, const char* label);
int Xioctl(int fd, unsigned long request, void* arg);
std::size_t RoundUpToPage(std::size_t bytes, std::size_t page_size);
Status AllocateHostBuffer(std::size_t bytes, bool pinned, HostBuffer* out);
void FreeHostBuffer(HostBuffer* buffer);
std::uint64_t PackRegion(const CaptureRegion& region);
CaptureRegion UnpackRegion(std::uint64_t packed);

}  // namespace capture_internal

struct CaptureSession::Impl {
    struct RequeueContext {
        Impl* owner = nullptr;
        std::uint32_t slot_index = 0;
    };

    struct HostSlotRuntime {
        std::uint32_t slot_index = 0;
        capture_internal::HostBuffer capture_buffer;
        RequeueContext requeue_context;
    };

    struct InferenceSlotRuntime {
        std::uint32_t slot_index = 0;
        std::uint8_t* device_ptr = nullptr;
        std::size_t pitch_bytes = 0;
        cudaStream_t copy_stream = nullptr;
        cudaEvent_t ready_event = nullptr;
        std::atomic<std::uint32_t> state{capture_internal::ToStateValue(capture_internal::InferenceState::kFree)};
        std::uint64_t frame_id = 0;
        std::uint64_t capture_ns = 0;
        std::uint64_t ready_ns = 0;
        bool short_frame = false;
        CaptureRegion region{};
    };

    struct PreviewSlotRuntime {
        std::uint32_t slot_index = 0;
        std::uint8_t* device_ptr = nullptr;
        std::size_t pitch_bytes = 0;
        cudaStream_t copy_stream = nullptr;
        cudaEvent_t ready_event = nullptr;
        cudaEvent_t displayed_event = nullptr;
        std::atomic<bool> displayed_event_pending{false};
        std::atomic<std::uint32_t> state{capture_internal::ToStateValue(capture_internal::PreviewState::kFree)};
        std::uint64_t frame_id = 0;
        std::uint64_t capture_ns = 0;
        std::uint64_t ready_ns = 0;
        bool short_frame = false;
        CaptureRegion region{};
    };

    explicit Impl(CaptureConfig config_in);
    ~Impl();

    Status start();
    Status stop();
    Status set_capture_region(CaptureRegion region);
    CaptureRegion snapshot_capture_region() const;
    CaptureFormatInfo snapshot_format() const;
    Status try_acquire_latest_inference_frame(InferenceFrameView* out_view);
    Status wait_acquire_latest_inference_frame(InferenceFrameView* out_view, const std::atomic<bool>& stop_requested);
    void notify_inference_waiters();
    Status release_inference_frame(std::uint32_t buffer_index);
    Status try_acquire_latest_preview(PreviewFrameView* out_view);
    Status mark_preview_displayed(std::uint32_t buffer_index, cudaStream_t stream);
    Status release_preview(std::uint32_t buffer_index);
    CaptureStats snapshot_stats() const;
    std::string last_error() const;

   private:
    static void CUDART_CB RequeueCapturedBufferThunk(void* user_data);

    Status ValidateConfig() const;
    void ResetRuntimeState();
    CaptureRegion NormalizeRegion(CaptureRegion region) const;
    void TeardownSession();
    Status InitializeCuda();
    Status OpenDevice();
    void CloseDevice();
    Status ConfigureDevice();
    Status ConfigureCaptureFormat();
    Status ConfigureFrameRateAndBuffers();
    Status StartStreaming();
    void StopStreaming();
    Status AllocateHostSlots();
    void DestroyHostSlots();
    Status AllocateInferenceSlots();
    void DestroyInferenceSlots();
    Status AllocatePreviewSlots();
    void DestroyPreviewSlots();
    Status QueueAllV4l2Buffers();
    Status QueueV4l2Buffer(std::uint32_t slot_index, bool record_requeue_failure);
    bool WaitForCaptureReady();
    bool TryDequeueBuffer(v4l2_buffer* out);
    void UpdateSequenceStats(std::uint32_t sequence);
    std::optional<std::uint32_t> ResolveHostSlotIndex(const v4l2_buffer& buf) const;
    void CaptureLoop();
    void RecycleDisplayedPreviewSlots();
    void ReclaimStaleInferenceSlots();
    std::optional<std::uint32_t> ReserveInferenceSlot();
    std::optional<std::uint32_t> ReservePreviewSlot();
    void HandleDequeuedBuffer(const v4l2_buffer& buf);
    void ZeroFillShortFrame(HostSlotRuntime& host_slot, std::size_t valid_bytes) const;
    Status ScheduleInferenceCopy(HostSlotRuntime& host_slot, InferenceSlotRuntime& inference_slot,
                                 const CaptureRegion& region, std::uint64_t capture_ns, bool short_frame);
    Status TrySchedulePreviewTap(const InferenceSlotRuntime& inference_slot);
    void OnPreviewCopyComplete(std::uint32_t host_slot_index);
    void NotifyInferenceFramePublished();
    void ResetStats();
    void ClearLastError();
    void SetLastError(const std::string& message);

    CaptureConfig config;
    int fd_ = -1;
    bool streaming_ = false;
    std::uint32_t actual_v4l2_buffer_count_ = 0;
    std::size_t bytes_per_line_ = 0;
    std::size_t size_image_ = 0;

    std::vector<std::unique_ptr<HostSlotRuntime>> host_slots_;
    std::vector<std::unique_ptr<InferenceSlotRuntime>> inference_slots_;
    std::vector<std::unique_ptr<PreviewSlotRuntime>> preview_slots_;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::thread capture_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<std::uint64_t> packed_region_{0};
    std::atomic<int> latest_inference_index_{-1};
    std::atomic<int> latest_preview_index_{-1};
    mutable std::mutex inference_publish_mutex_;
    std::condition_variable inference_publish_cv_;
    std::uint64_t inference_publish_generation_ = 0;

    std::atomic<std::uint64_t> queued_v4l2_buffers_{0};
    std::atomic<std::uint64_t> dequeued_v4l2_buffers_{0};
    std::atomic<std::uint64_t> bytes_captured_{0};
    std::atomic<std::uint64_t> inference_frames_published_{0};
    std::atomic<std::uint64_t> preview_frames_published_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> empty_frames_dropped_{0};
    std::atomic<std::uint64_t> inference_backpressure_drops_{0};
    std::atomic<std::uint64_t> preview_backpressure_drops_{0};
    std::atomic<std::uint64_t> short_frames_{0};
    std::atomic<std::uint64_t> sequence_gaps_{0};
    std::atomic<std::uint64_t> requeue_failures_{0};
    std::optional<std::uint32_t> last_sequence_;
    std::uint64_t next_frame_id_ = 1;
    std::size_t next_inference_slot_ = 0;
    std::size_t next_preview_slot_ = 0;
};

}  // namespace frameshow
