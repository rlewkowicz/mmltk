#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/presentation/tests/support/visual_runtime_fixture.h"
#include "src/controller/subsystems/live/tests/support/live_system_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/live/live_receiver_copy.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/frameworks/gpu/image_buffer.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cuda_runtime_api.h>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <variant>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct LiveReceiverCopyProbe final {
    static inline cudaError_t wait_status = cudaSuccess;
    static inline cudaError_t copy_status = cudaSuccess;
    static inline cudaError_t synchronize_status = cudaSuccess;
    static inline std::size_t waits = 0U;
    static inline std::size_t copies = 0U;
    static inline std::size_t synchronizations = 0U;
    static void Reset() noexcept {
        wait_status = cudaSuccess;
        copy_status = cudaSuccess;
        synchronize_status = cudaSuccess;
        waits = 0U;
        copies = 0U;
        synchronizations = 0U;
    }
    static cudaError_t Wait(std::uintptr_t, std::uintptr_t) noexcept {
        ++waits;
        return wait_status;
    }
    static cudaError_t Copy(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::size_t, std::uintptr_t) noexcept {
        ++copies;
        return copy_status;
    }
    static cudaError_t Synchronize(std::uintptr_t) noexcept {
        ++synchronizations;
        return synchronize_status;
    }
};
constexpr detail::LiveReceiverCopyOperations kLiveReceiverCopyProbe{
    .wait = &LiveReceiverCopyProbe::Wait,
    .copy = &LiveReceiverCopyProbe::Copy,
    .synchronize = &LiveReceiverCopyProbe::Synchronize,
};
[[nodiscard]] constexpr mmltk::frameworks::gpu::ImagePlaneView live_receiver_copy_target() noexcept {
    return {
        .data = 1U,
        .descriptor =
            {
                .kind = mmltk::frameworks::gpu::ImagePlaneKind::Clean,
                .format = mmltk::frameworks::gpu::ImageFormat::Rgba8,
                .width = 4U,
                .height = 4U,
                .pitch_bytes = 16U,
            },
    };
}
TEST_CASE("Live receiver copy completes only after synchronization") {
    LiveReceiverCopyProbe::Reset();
    const auto target = live_receiver_copy_target();
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaSuccess);
    CHECK(LiveReceiverCopyProbe::waits == 1U);
    CHECK(LiveReceiverCopyProbe::copies == 1U);
    CHECK(LiveReceiverCopyProbe::synchronizations == 1U);
}
TEST_CASE("Live receiver submission and synchronization failures stop progress") {
    const auto target = live_receiver_copy_target();
    LiveReceiverCopyProbe::Reset();
    LiveReceiverCopyProbe::copy_status = cudaErrorLaunchFailure;
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
    CHECK(LiveReceiverCopyProbe::synchronizations == 0U);
    LiveReceiverCopyProbe::Reset();
    LiveReceiverCopyProbe::synchronize_status = cudaErrorLaunchFailure;
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
    CHECK(LiveReceiverCopyProbe::synchronizations == 1U);
}
class FailingLiveAlgorithm final : public LiveAlgorithm {
   public:
    void Start(const LiveStart&) override {}
    void SetOutputAvailableSink(std::function<void()>) override {}
    bool AcquireOutput() override { return true; }
    bool Capture(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::stop_token) override {
        throw std::runtime_error("deterministic Live capture failure");
    }
    void Stop() noexcept override {}
};
TEST_CASE("Live queued discrete cancellation settles without running obsolete work") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> latest_entered;
    std::promise<void> release_latest;
    auto release = release_latest.get_future().share();
    std::promise<void> queued_cancelled;
    std::promise<void> active_cancelled;
    std::atomic_bool queued_ran = false;
    std::atomic_bool owner_failed = false;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures),
                                     [&owner_failed](std::exception_ptr) { owner_failed.store(true, std::memory_order_release); }};
    auto settle_owner = settle_visual_on_exit(owner, release_latest);
    owner.SubmitLatest([&latest_entered, &active_cancelled, release](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token stop) mutable {
        std::stop_callback observe_stop{stop, [&active_cancelled] { active_cancelled.set_value(); }};
        latest_entered.set_value();
        release.wait();
        return detail::VisualRuntimeOwner::Notification{};
    });
    mmltk::testsupport::await_test_promise(latest_entered, "latest_entered");
    REQUIRE(owner.SubmitDiscrete(
        [&queued_ran](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
            queued_ran.store(true, std::memory_order_release);
            return detail::VisualRuntimeOwner::Notification{};
        },
        [&queued_cancelled] { queued_cancelled.set_value(); }));
    owner.RequestActiveStop();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(active_cancelled, "active_cancelled", 2s));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(queued_cancelled, "queued_cancelled", 2s));
    CHECK_FALSE(queued_ran.load(std::memory_order_acquire));
    release_latest.set_value();
    owner.StopAndWait();
    CHECK_FALSE(owner_failed.load(std::memory_order_acquire));
}
TEST_CASE("Live failure publishes its newer settled snapshot") {
    auto backend = std::make_shared<FakeImageBackend>();
    std::promise<LiveFailed> failure;
    LiveSystem live{kDevice,
                    RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean, [] { return std::make_unique<FailingLiveAlgorithm>(); }),
                    [&failure](LiveSystem::event_type event) {
                        if (auto* failed = std::get_if<LiveFailed>(&event)) failure.set_value(std::move(*failed));
                    }};
    const auto admitted = live.Start({.extent = {80U, 45U}, .frames_per_second = 120U});
    const auto failed = failure.get_future().get();
    CHECK_FALSE(failed.snapshot.running);
    CHECK_FALSE(failed.snapshot.cancellation_requested);
    CHECK(failed.snapshot.revision > admitted.revision);
    CHECK(failed.snapshot.revision == live.snapshot().revision);
    CHECK(failed.snapshot.completed_frames == live.snapshot().completed_frames);
    CHECK_FALSE(failed.snapshot.frame.valid());
    CHECK_FALSE(live.BorrowFrame().valid());
}
}  // namespace
}  // namespace mmltk::controller
