module;
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include "src/frameworks/gpu/device_execution.h"
#include <string>

export module mmltk.backend.media.capture.capture_session;

import mmltk.backend.media.capture.capture_types;
import mmltk.backend.media.capture.live_video_source;
import mmltk.backend.media.capture.status;

export namespace mmltk::backend::media::capture {

struct CaptureDeviceApi final {
    void* context = nullptr;
    int (*ioctl)(void*, int, unsigned long, void*) noexcept = nullptr;
    void (*host_storage_released)(void*, std::uint32_t) noexcept = nullptr;
};

struct CaptureConfig final : LiveVideoSourceOptions {
    int cuda_device_index = 0;
    CaptureDeviceApi device_api{};
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution;
};

enum class CaptureStopKind : std::uint8_t {
    kRequested = 0,
    kStartupFailed,
    kCameraError,
    kCameraHangup,
    kOwnerFailure,
    kCount,
};

struct CaptureStopTerminal {
    CaptureSessionIdentity identity{};
    CaptureStopKind kind = CaptureStopKind::kOwnerFailure;
    Status status{};
    CaptureStats final_stats{};
    bool streamoff_skipped = false;
};

enum class CaptureSessionStartPhase : std::uint8_t {
    NoCustody,
    Running,
    Closing,
};

struct CaptureSessionStartResult final {
    CaptureSessionStartPhase phase = CaptureSessionStartPhase::NoCustody;
    Status status{StatusCode::kInternalError, "capture admission was not produced"};
    CaptureSessionIdentity identity{};

    [[nodiscard]] bool valid() const noexcept {
        switch (phase) {
            case CaptureSessionStartPhase::NoCustody:
                return !identity.valid() && !status.ok();
            case CaptureSessionStartPhase::Running:
                return identity.valid() && status.ok();
            case CaptureSessionStartPhase::Closing:
                return identity.valid() && !status.ok();
        }
        return false;
    }
    [[nodiscard]] bool has_custody() const noexcept {
        switch (phase) {
            case CaptureSessionStartPhase::NoCustody:
                return false;
            case CaptureSessionStartPhase::Running:
            case CaptureSessionStartPhase::Closing:
                return true;
        }
        return false;
    }
    [[nodiscard]] bool running() const noexcept { return phase == CaptureSessionStartPhase::Running; }
    [[nodiscard]] bool closing() const noexcept { return phase == CaptureSessionStartPhase::Closing; }
};

class CaptureSession {
   public:
    explicit CaptureSession(CaptureConfig config = {});
    // An admitted run must be requested to stop or reach its own terminal
    // before destruction. Destruction never initiates or waits for teardown.
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    CaptureSession(CaptureSession&&) noexcept;
    CaptureSession& operator=(CaptureSession&&) noexcept;

    // start() admits one owner-thread run. Device setup, capture, and every
    // teardown path execute on that owner thread.
    [[nodiscard]] CaptureSessionStartResult start();
    // Stale identities are rejected without affecting the current run.
    [[nodiscard]] Status request_stop(CaptureSessionIdentity identity);
    // Borrowed nonblocking eventfd, stable until this CaptureSession is
    // released. Readiness is a hint; try_take_stop_terminal() is authoritative.
    [[nodiscard]] int stop_terminal_event_fd() const noexcept;
    [[nodiscard]] std::shared_ptr<const CaptureStopTerminal> try_take_stop_terminal() noexcept;
    // The exact terminal returned by try_take_stop_terminal() proves teardown
    // is complete. Only then may the owner thread be joined; no device work
    // remains behind that join.
    [[nodiscard]] Status finalize_stop(const std::shared_ptr<const CaptureStopTerminal>& terminal);
    [[nodiscard]] Status set_capture_region(CaptureRegion region);
    [[nodiscard]] CaptureRegion snapshot_capture_region() const;
    [[nodiscard]] CaptureFormatInfo snapshot_format() const;
    [[nodiscard]] Status try_take_filled(FilledCaptureSlotLease* out_lease);
    [[nodiscard]] Status mark_h2d_completion_pending(const FilledCaptureSlotLease& lease);
    [[nodiscard]] Status return_after_h2d(FilledCaptureSlotLease&& lease) noexcept;
    // Retains the first complete physical failure for this exact run and
    // requests owner-thread closure. Later reports cannot replace its cause.
    [[nodiscard]] Status report_failure(CaptureSessionIdentity identity, Status failure) noexcept;
    [[nodiscard]] CaptureStats snapshot_stats() const;
    [[nodiscard]] std::string last_error() const;
    void set_state_listener(std::function<void()> listener);
    void set_filled_frame_listener(std::function<void()> listener);

   private:
    [[nodiscard]] static FilledCaptureSlotLease MakeFilledSlotLease(CaptureSessionIdentity identity, std::uint32_t slot,
                                                                    std::uint64_t sequence, const std::uint8_t* data, std::size_t bytes,
                                                                    std::size_t stride_bytes, std::uint32_t pixel_format,
                                                                    CaptureRegion region, std::uint64_t capture_ns,
                                                                    bool short_frame) noexcept;
    static void ConsumeFilledSlotLease(FilledCaptureSlotLease& lease) noexcept;

    struct Impl;
    struct OwnerHandle;
    std::unique_ptr<OwnerHandle> owner_;
};

}  // namespace mmltk::backend::media::capture
