#include "src/controller/subsystems/live/live_system.h"
#include "src/frameworks/gpu/system_image_worker.h"

#include <cuda_runtime_api.h>

#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "src/controller/subsystems/live/live_receiver_copy.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"

import mmltk.backend.media.capture.capture_session;
import mmltk.backend.media.live.live_session_controller;
import mmltk.backend.media.live.live_types;

namespace mmltk::controller {
namespace {

namespace capture = mmltk::backend::media::capture;
namespace media = mmltk::backend::media::live;
namespace gpu = mmltk::frameworks::gpu;

class NativeLiveAlgorithm final : public LiveAlgorithm {
   public:
    NativeLiveAlgorithm(const VisualDeviceSettings settings, LiveNativeConfiguration configuration, gpu::DeviceExecution execution)
        : settings_(settings),
          execution_(std::move(execution)),
          configuration_(std::move(configuration)),
          owner_thread_(std::this_thread::get_id()) {}

    ~NativeLiveAlgorithm() override { Stop(); }

    void Start(const LiveStart& request) override {
        if (plane_) throw std::logic_error("Live data plane is already active");
        media::LiveDataPlaneConfig config{};
        config.capture.device_path = configuration_.capture_device;
        config.capture.cuda_device_index = settings_.device;
        config.capture.execution = execution_;
        config.capture.width = request.extent.width;
        config.capture.height = request.extent.height;
        config.capture.fps = request.frames_per_second;
        config.capture.v4l2_buffer_count = configuration_.capture_buffers;
        config.resource_worker =
            gpu::ResourceOwnerWorkerCapability{this, 1U, &NativeLiveAlgorithm::IsCurrent, &NativeLiveAlgorithm::FailCurrent};
        config.ingress_slots = configuration_.ingress_slots;
        config.fanout_slots = configuration_.fanout_slots;
        config.analysis_slots = configuration_.analysis_slots;
        config.composite_slots = configuration_.composite_slots;
        plane_ = std::make_unique<media::LiveMediaDataPlane>(config);
        revisions_.Reset();
        plane_->set_revision_listener({
            .context = this,
            .notify = &NativeLiveAlgorithm::RevisionReady,
        });
        plane_->set_admitted_run_terminal_listener({
            .context = this,
            .notify = &NativeLiveAlgorithm::TerminalReady,
        });
        const auto started = plane_->start();
        if (!started.running()) {
            static_cast<void>(plane_->stop());
            plane_.reset();
            throw std::runtime_error("Live capture session failed to start");
        }
    }

    bool Capture(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream_value,
                 const std::stop_token stop) override {
        const auto observed = revisions_.Wait(stop, plane_.get(), &NativeLiveAlgorithm::NewestRevision);
        if (stop.stop_requested()) return false;
        if (!observed || !plane_) throw std::runtime_error("Live media data plane failed");
        media::LiveCompositeOutputLease lease;
        if (!media::try_acquire_live_output(plane_.get(), *observed, &NativeLiveAlgorithm::AcquireOutput, &lease)) {
            if (revisions_.failed()) throw std::runtime_error("Live media data plane failed");
            return false;
        }
        const auto source = lease.view();
        if (source.width != target.descriptor.width || source.height != target.descriptor.height)
            throw std::runtime_error("Live composite extent changed unexpectedly");
        const cudaError_t status = detail::copy_live_receiver_frame(target, source.pixels, source.pitch_bytes, source.ready_event,
                                                                    stream_value, detail::native_live_receiver_copy_operations());
        if (status != cudaSuccess) throw std::runtime_error("Live composite receiver copy failed");
        std::move(lease).Complete();
        return true;
    }

    void Stop() noexcept override {
        if (!plane_) return;
        static_cast<void>(plane_->stop());
        plane_.reset();
        revisions_.Fail();
    }

   private:
    static bool IsCurrent(const void* context, std::uintptr_t) noexcept {
        return static_cast<const NativeLiveAlgorithm*>(context)->owner_thread_ == std::this_thread::get_id();
    }
    static bool FailCurrent(const void* context, std::uintptr_t) noexcept {
        auto& owner = *const_cast<NativeLiveAlgorithm*>(static_cast<const NativeLiveAlgorithm*>(context));
        owner.revisions_.Fail();
        return true;
    }
    static void RevisionReady(void* context) noexcept { static_cast<NativeLiveAlgorithm*>(context)->revisions_.RevisionReady(); }
    static void TerminalReady(void* context, const media::LivePhysicalTerminal&) noexcept {
        static_cast<NativeLiveAlgorithm*>(context)->revisions_.Fail();
    }
    static std::optional<media::PhysicalFrameRevision> NewestRevision(void* context) noexcept {
        return context == nullptr ? std::nullopt : static_cast<media::LiveMediaDataPlane*>(context)->newest_revision();
    }
    static bool AcquireOutput(void* context, const media::PhysicalFrameRevision revision, media::LiveCompositeOutputLease* output) {
        return context != nullptr && static_cast<media::LiveMediaDataPlane*>(context)->try_acquire_output(revision, output);
    }
    VisualDeviceSettings settings_;
    gpu::DeviceExecution execution_;
    LiveNativeConfiguration configuration_;
    std::thread::id owner_thread_;
    std::unique_ptr<media::LiveMediaDataPlane> plane_;
    media::LiveRevisionWait revisions_;
};

}  // namespace

VisualRuntimeFactory make_native_live_runtime_factory(const VisualDeviceSettings settings, LiveNativeConfiguration configuration) {
    if (!settings.valid()) throw contracts::InvalidIntentError("Live device settings are invalid");
    return [settings, execution = resolve_visual_device_execution(settings), configuration = std::move(configuration)] {
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
            .device = settings.device,
            .model = std::make_unique<NativeLiveAlgorithm>(settings, configuration, execution),
            .numa_node = settings.numa_node,
            .execution = execution,
        });
    };
}

}  // namespace mmltk::controller
