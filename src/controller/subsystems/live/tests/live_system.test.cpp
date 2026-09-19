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
#include <catch2/generators/catch_generators.hpp>
#include <mutex>
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
    LiveReceiverCopyProbe::wait_status = cudaErrorLaunchFailure;
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
    CHECK(LiveReceiverCopyProbe::copies == 0U);
    CHECK(LiveReceiverCopyProbe::synchronizations == 0U);
    LiveReceiverCopyProbe::Reset();
    LiveReceiverCopyProbe::copy_status = cudaErrorLaunchFailure;
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
    CHECK(LiveReceiverCopyProbe::synchronizations == 0U);
    LiveReceiverCopyProbe::Reset();
    LiveReceiverCopyProbe::synchronize_status = cudaErrorLaunchFailure;
    CHECK(detail::copy_live_receiver_frame(target, 2U, 16U, 3U, 4U, kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
    CHECK(LiveReceiverCopyProbe::synchronizations == 1U);
}
struct ControlledLiveCapture final {
    EventGate changed;
    std::mutex mutex;
    std::function<void()> wake;
    std::atomic_bool available{false};
    std::atomic_size_t acquisitions{0U}, captures{0U};
    bool result = true;
    std::function<void()> after_capture;
    void Offer() {
        std::function<void()> notify;
        {
            std::scoped_lock lock(mutex);
            available.store(true, std::memory_order_release);
            notify = wake;
        }
        if (notify) notify();
    }
};
class ControlledLiveAlgorithm final : public LiveAlgorithm {
   public:
    explicit ControlledLiveAlgorithm(std::shared_ptr<ControlledLiveCapture> state) : state_(std::move(state)) {}
    void Start(const LiveStart&) override {}
    void SetOutputAvailableSink(std::function<void()> wake) override {
        std::scoped_lock lock(state_->mutex);
        state_->wake = std::move(wake);
    }
    bool AcquireOutput() override {
        ++state_->acquisitions;
        const bool ready = state_->available.exchange(false, std::memory_order_acq_rel);
        state_->changed.Advance();
        return ready;
    }
    bool Capture(mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t, std::stop_token) override {
        const auto value = ++state_->captures;
        for (std::uint32_t y = 0U; y != target.descriptor.height; ++y)
            for (std::uint32_t x = 0U; x != target.descriptor.row_bytes(); ++x)
                *(reinterpret_cast<std::uint8_t*>(target.data) + y * target.descriptor.pitch_bytes + x) =
                    static_cast<std::uint8_t>(value * 31U + y * 7U + x);
        if (state_->after_capture) state_->after_capture();
        return state_->result;
    }
    void Stop() noexcept override {}
   private:
    std::shared_ptr<ControlledLiveCapture> state_;
};
TEST_CASE("Live complete frames alternate native output slots without preparation copies") {
    auto backend = std::make_shared<FakeImageBackend>();
    backend->pitch_padding_bytes = 17U;
    auto state = std::make_shared<ControlledLiveCapture>();
    EventGate events;
    LiveSystem live{kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean,
                                           [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U),
                    [&](LiveSystem::event_type) { events.Advance(); }};
    const auto check = [](const auto& read, const std::size_t value) {
        REQUIRE(read.valid());
        const auto plane = read.plane(0U).plane();
        for (std::uint32_t y = 0U; y != plane.descriptor.height; ++y)
            for (std::uint32_t x = 0U; x != plane.descriptor.row_bytes(); ++x)
                CHECK(*(reinterpret_cast<const std::uint8_t*>(plane.data) + y * plane.descriptor.pitch_bytes + x) ==
                      static_cast<std::uint8_t>(value * 31U + y * 7U + x));
    };
    static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
    REQUIRE(state->changed.Wait([&] { return state->acquisitions.load() != 0U; }));
    CHECK(state->captures == 0U);
    CHECK(live.snapshot().completed_frames == 0U);
    state->Offer();
    REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 1U; }));
    auto first = live.BorrowFrame();
    check(first, 1U);
    const auto first_pointer = first.plane(0U).plane().data;
    state->Offer();
    REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 2U; }));
    auto second = live.BorrowFrame();
    check(first, 1U);
    check(second, 2U);
    CHECK(second.plane(0U).plane().data != first_pointer);
    state->Offer();
    CHECK(live.snapshot().completed_frames == 2U);
    first = {};
    REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 3U; }));
    check(second, 2U);
    auto third = live.BorrowFrame();
    check(third, 3U);
    CHECK(third.plane(0U).plane().data == first_pointer);
    second = {};
    third = {};
    static_cast<void>(live.Stop());
    REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
    state->Offer();
    static_cast<void>(live.Start({.extent = {3U, 5U}, .frames_per_second = 240U}));
    REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 4U; }));
    auto resized = live.BorrowFrame();
    check(resized, 4U);
    CHECK(resized.plane(0U).plane().descriptor.width == 3U);
    CHECK(resized.plane(0U).plane().descriptor.height == 5U);
    resized = {};
    static_cast<void>(live.Stop());
    REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
    CHECK(backend->same_copies == 0U);
    CHECK(backend->plane_clears == 0U);
}
TEST_CASE("Live rejected or stopped complete captures never report frame progress") {
    const bool stop_after_capture = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    auto state = std::make_shared<ControlledLiveCapture>();
    state->result = stop_after_capture;
    EventGate events;
    LiveSystem live{kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean,
                                           [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U),
                    [&](LiveSystem::event_type) { events.Advance(); }};
    if (stop_after_capture) state->after_capture = [&] { static_cast<void>(live.Stop()); };
    state->Offer();
    static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
    if (stop_after_capture)
        REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
    else {
        REQUIRE(state->changed.Wait([&] { return state->acquisitions.load() >= 2U; }));
        static_cast<void>(live.Stop());
        REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
    }
    CHECK(state->captures == 1U);
    CHECK(live.snapshot().completed_frames == 0U);
    CHECK_FALSE(live.snapshot().frame.valid());
    CHECK_FALSE(live.BorrowFrame().valid());
}
TEST_CASE("Live receiver publication failure does not commit a captured candidate") {
    const auto point = GENERATE(FakeImageBackend::FailurePoint::RecordEvent, FakeImageBackend::FailurePoint::SynchronizeStream);
    auto backend = std::make_shared<FakeImageBackend>();
    auto state = std::make_shared<ControlledLiveCapture>();
    state->after_capture = [backend, point] { backend->FailAfter(point); };
    EventGate events;
    std::atomic_bool failed{false};
    LiveSystem live{kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean,
                                           [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U),
                    [&](LiveSystem::event_type event) {
                        if (std::holds_alternative<LiveFailed>(event)) failed = true;
                        events.Advance();
                    }};
    state->Offer();
    static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
    REQUIRE(events.Wait([&] { return failed.load(); }));
    CHECK(state->captures == 1U);
    CHECK(live.snapshot().completed_frames == 0U);
    CHECK_FALSE(live.BorrowFrame().valid());
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
