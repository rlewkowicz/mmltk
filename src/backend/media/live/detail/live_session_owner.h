#pragma once
#include "src/backend/media/live/live_session_controller.h"
#include "src/backend/media/live/manual_overlay_document.h"
#include "src/backend/media/live/live_types.h"
#include "src/backend/media/capture/capture_session.h"
#include "src/backend/media/capture/status.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
#include "live_analyzer_worker.h"
#include "live_device_types.h"
#include "live_frame_fanout.h"
#include "live_manual_overlay_worker.h"
#include "live_video_ingress.h"
#include "workspace_frame_signal.h"
#include <stop_token>
#include <cuda_runtime_api.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include "live_compositor_owner.h"
#include "live_output_callback_lifetime.h"
#include "live_physical_retirement.h"
namespace mmltk::backend::media::live {
class LiveMediaDataPlane::Impl final {
   public:
    explicit Impl(LiveDataPlaneConfig config);
    ~Impl();
    // CLEANUP-IGNORE: The private implementation repeats the sealed module facade exactly at its single pimpl boundary.
    [[nodiscard]] mmltk::backend::media::capture::CaptureSessionStartResult start(LiveAnalysisProviderFactory provider_factory);
    // CLEANUP-IGNORE: The private implementation mirrors the public stop-through-observer facade at this pimpl
    // boundary.
    [[nodiscard]] mmltk::backend::media::capture::Status stop() noexcept;
    // CLEANUP-IGNORE: Remaining declarations mirror the one sealed public facade at its private pimpl boundary.
    [[nodiscard]] std::shared_ptr<const LivePhysicalTerminal> admitted_run_terminal() const noexcept;
    [[nodiscard]] mmltk::backend::media::capture::Status first_failure() const;
    [[nodiscard]] std::optional<PhysicalFrameRevision> newest_revision() const noexcept;
    [[nodiscard]] bool try_acquire_output(PhysicalFrameRevision revision, LiveCompositeOutputLease* output);
    [[nodiscard]] bool request_raw_readback(LiveRawFrameReadback request);
    void set_revision_listener(LiveRevisionListener listener);
    void set_admitted_run_terminal_listener(LiveAdmittedRunTerminalListener listener);
    void publish_manual_overlay(ManualOverlayDocumentSnapshot snapshot);
    void clear_manual_overlay(std::uint32_t width, std::uint32_t height);

   private:
    enum class PhysicalPhase : std::uint8_t { Idle, Starting, Running, Closing, Terminal };
    enum class RawReadbackPhase : std::uint8_t { Idle, Pending, Active, Delivered };
    struct RawReadbackState final {
        RawReadbackPhase phase = RawReadbackPhase::Idle;
        std::optional<LiveRawFrameReadback> request;
    };
    struct RawReadbackDelivery final {
        LiveRawFrameReadback request{};
        LiveRawFrameReadbackResult result{};
        [[nodiscard]] inline bool valid() const noexcept { return request.valid() && result.valid() && request.frame == result.frame; }
    };
    struct PhysicalResources final {
        PhysicalResources(Impl& owner, const LiveDataPlaneConfig& config);
        ~PhysicalResources();
        PhysicalResources(const PhysicalResources&) = delete;
        PhysicalResources& operator=(const PhysicalResources&) = delete;
        [[nodiscard]] mmltk::backend::media::capture::CaptureSessionStartResult start(LiveAnalysisProviderFactory provider_factory);
        void close_admission() noexcept;
        void settle() noexcept;
        [[nodiscard]] bool drain();
        [[nodiscard]] bool settled() const noexcept;
        Impl& owner;
        mmltk::frameworks::gpu::ResourceOwnerCommandAuthority commands;
        LivePhysicalCudaContext cuda;
        LiveVideoIngress ingress;
        LiveFrameFanout fanout;
        LiveAnalyzerWorker analyzer;
        LiveManualOverlayWorker manual_overlay;
        LiveCompositor compositor;
        cudaStream_t analysis_stream = nullptr;
        bool started = false;
        bool resources_settled = false;
    };
    static LiveDataPlaneConfig Validate(LiveDataPlaneConfig config);
    static void RecordCudaFailure(void* context, cudaError_t failure) noexcept;
    static void CompleteOutput(void* context, PhysicalFrameRevision revision) noexcept;
    static void AbandonOutput(void* context, PhysicalFrameRevision revision) noexcept;
    static void WakeOutputCallback(void* context) noexcept;
    void complete_output(PhysicalFrameRevision revision) noexcept;
    void abandon_output(PhysicalFrameRevision revision) noexcept;
    void record_cuda_failure(cudaError_t failure) noexcept;
    void record_failure(mmltk::backend::media::capture::Status failure) noexcept;
    void record_failure_locked(mmltk::backend::media::capture::Status failure) noexcept;
    void wake() noexcept;
    [[nodiscard]] std::optional<RawReadbackDelivery> finish_raw_readback(LiveRawFrameReadbackResult result) noexcept;
    void deliver(std::optional<RawReadbackDelivery> raw) noexcept;
    void run(std::stop_token stop) noexcept;
    void finish_admitted_run(std::stop_token stop, bool admission_closing) noexcept;
    void publish_start(mmltk::backend::media::capture::CaptureSessionStartResult result) noexcept;
    void publish_revision() noexcept;
    void publish_terminal(std::shared_ptr<const mmltk::backend::media::capture::CaptureStopTerminal> capture_terminal) noexcept;
    const LiveDataPlaneConfig config_;
    LiveOutputCallbackLifetime output_callbacks_;
    mutable std::mutex lifecycle_;
    std::condition_variable_any wake_condition_;
    LiveCompletedFramePublication completed_frames_;
    ManualOverlayDocument manual_document_;
    std::optional<PhysicalResources> resources_;
    std::jthread ingestion_thread_;
    LiveAnalysisProviderFactory pending_provider_factory_{};
    mmltk::backend::media::capture::CaptureSessionStartResult start_result_{};
    std::optional<LivePhysicalTerminal> terminal_;
    LiveRevisionListener revision_listener_{};
    LiveAdmittedRunTerminalListener admitted_run_terminal_listener_{};
    mmltk::backend::media::capture::Status first_failure_{};
    PhysicalPhase phase_ = PhysicalPhase::Idle;
    bool startup_ready_ = false;
    bool wake_pending_ = false;
    bool stop_requested_ = false;
    bool close_issued_ = false;
    bool failure_reported_ = false;
    RawReadbackState raw_readback_{};
};
}  // namespace mmltk::backend::media::live
