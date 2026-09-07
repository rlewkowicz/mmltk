#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <stdexcept>
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"

#include "src/backend/media/live/detail/live_physical_retirement.h"
#include "src/backend/media/live/detail/live_output_callback_lifetime.h"
#include "src/backend/media/live/detail/live_slot_state.h"

import mmltk.backend.media.live.live_types;
import mmltk.backend.media.live.live_session_controller;
import mmltk.backend.media.capture.capture_session;
import mmltk.backend.media.capture.status;

namespace mmltk::backend::media::live {
namespace {

TEST_CASE("Live rejects placement belonging to a different capture receiver before allocating resources", "[live][numa]") {
    int owner = 0;
    LiveDataPlaneConfig config;
    config.resource_worker = mmltk::frameworks::gpu::ResourceOwnerWorkerCapability{
        &owner, 1, [](const void*, std::uintptr_t) noexcept { return true; }, [](const void*, std::uintptr_t) noexcept { return true; }};
    config.capture.cuda_device_index = 0;
    config.capture.execution = mmltk::frameworks::gpu::DeviceExecution{.device = 1, .placement = {.numa_node = 3, .cpus = {17}}};
    REQUIRE_THROWS_AS(LiveMediaDataPlane(config), std::invalid_argument);
}

struct LeaseProbe final {
    std::size_t completions = 0U;
    std::size_t abandonments = 0U;
    PhysicalFrameRevision observed{};

    static void Complete(void* context, const PhysicalFrameRevision revision) noexcept {
        auto& probe = *static_cast<LeaseProbe*>(context);
        ++probe.completions;
        probe.observed = revision;
    }

    static void Abandon(void* context, const PhysicalFrameRevision revision) noexcept {
        auto& probe = *static_cast<LeaseProbe*>(context);
        ++probe.abandonments;
        probe.observed = revision;
    }
};

constexpr PhysicalFrameRevision kRevision{
    .revision = 3U,
    .frame = {.session = 4U, .sequence = 5U},
    .slot = 1U,
    .ready_event = 6U,
};

constexpr LiveOutputFrame kView{
    .pixels = 7U,
    .pitch_bytes = 32U,
    .width = 8U,
    .height = 2U,
    .ready_event = 6U,
};

TEST_CASE("completed Live output lease releases its exact source revision once") {
    LeaseProbe probe;
    auto lease = LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);

    std::move(lease).Complete();

    CHECK(probe.completions == 1U);
    CHECK(probe.abandonments == 0U);
    CHECK(probe.observed.revision == kRevision.revision);
    CHECK_FALSE(lease);
}

TEST_CASE("abandoned Live output lease terminalizes its exact source revision once") {
    LeaseProbe probe;
    {
        auto lease = LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);
        REQUIRE(lease);
    }

    CHECK(probe.completions == 0U);
    CHECK(probe.abandonments == 1U);
    CHECK(probe.observed.slot == kRevision.slot);
}

TEST_CASE("moving over an active Live output lease abandons only the displaced slot") {
    LeaseProbe first;
    LeaseProbe second;
    auto displaced = LiveCompositeOutputLease::Create(&first, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);
    auto replacement = LiveCompositeOutputLease::Create(&second, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView,
                                                        PhysicalFrameRevision{
                                                            .revision = 4U,
                                                            .frame = {.session = 4U, .sequence = 6U},
                                                            .slot = 2U,
                                                            .ready_event = 8U,
                                                        });

    displaced = std::move(replacement);
    std::move(displaced).Complete();

    CHECK(first.abandonments == 1U);
    CHECK(second.completions == 1U);
    CHECK(second.abandonments == 0U);
}

TEST_CASE("Live source slot publishes Free only after product scrub") {
    std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
    PhysicalFrameRevision product = kRevision;
    bool scrub_observed_exclusive = false;

    REQUIRE(claim_live_slot(state, SlotState::Acquired));
    publish_live_owner_slot(state, SlotState::Free, [&] noexcept {
        scrub_observed_exclusive = slot_state_is(state, SlotState::Completing);
        product = {};
    });

    CHECK(scrub_observed_exclusive);
    CHECK_FALSE(product.valid());
    CHECK(slot_state_is(state, SlotState::Free));
}

TEST_CASE("Live slot vocabulary retains every physical lifecycle phase") {
    constexpr std::array states{
        SlotState::Free, SlotState::Uploading, SlotState::Published, SlotState::Acquired, SlotState::Completing, SlotState::Terminal,
    };

    for (std::size_t index = 0U; index < states.size(); ++index)
        CHECK(slot_state_value(states[index]) == index);
}

TEST_CASE("Live fanout slot publishes Free only after completion scrub") {
    std::atomic<std::uint32_t> state{slot_state_value(SlotState::Published)};
    REQUIRE(claim_live_slot(state, SlotState::Published));
    CHECK_FALSE(transition_slot_state(state, SlotState::Published, SlotState::Acquired));
    PhysicalFrameRevision product = kRevision;
    bool scrub_observed_completing = false;

    publish_live_owner_slot(state, SlotState::Free, [&] noexcept {
        scrub_observed_completing = slot_state_is(state, SlotState::Completing);
        product = {};
    });

    CHECK(scrub_observed_completing);
    CHECK_FALSE(product.valid());
    CHECK(slot_state_is(state, SlotState::Free));
}

struct CallbackWakeProbe final {
    std::size_t wakes = 0U;

    static void Wake(void* context) noexcept { ++static_cast<CallbackWakeProbe*>(context)->wakes; }
};

TEST_CASE("Live output callback guard protects post-Free source work") {
    CallbackWakeProbe wake;
    LiveOutputCallbackLifetime lifetime{1U, &wake, &CallbackWakeProbe::Wake};
    REQUIRE(lifetime.acquire());
    std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
    {
        LiveOutputCallbackGuard callback{lifetime};
        REQUIRE(claim_live_slot(state, SlotState::Acquired));
        publish_live_owner_slot(state, SlotState::Free, [] noexcept {});
        CHECK(slot_state_is(state, SlotState::Free));
        CHECK_FALSE(lifetime.idle());
    }

    CHECK(lifetime.idle());
    CHECK(wake.wakes == 1U);
}

TEST_CASE("Live output callback guard protects post-Terminal source work") {
    CallbackWakeProbe wake;
    LiveOutputCallbackLifetime lifetime{1U, &wake, &CallbackWakeProbe::Wake};
    REQUIRE(lifetime.acquire());
    std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
    {
        LiveOutputCallbackGuard callback{lifetime};
        REQUIRE(claim_live_slot(state, SlotState::Acquired));
        publish_live_owner_slot(state, SlotState::Terminal, [] noexcept {});
        CHECK(slot_state_is(state, SlotState::Terminal));
        CHECK_FALSE(lifetime.idle());
    }

    CHECK(lifetime.idle());
    CHECK(wake.wakes == 1U);
}

TEST_CASE("Live incomplete fanout release publishes Terminal after scrub") {
    std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
    PhysicalFrameRevision product = kRevision;
    REQUIRE(claim_live_slot(state, SlotState::Acquired));

    publish_live_owner_slot(state, SlotState::Terminal, [&] noexcept { product = {}; });

    CHECK_FALSE(product.valid());
    CHECK(slot_state_is(state, SlotState::Terminal));
}

struct RevisionSnapshotProbe final {
    std::atomic<std::size_t> calls{0U};
    PhysicalFrameRevision revision{};

    static std::optional<PhysicalFrameRevision> Snapshot(void* context) noexcept {
        auto& probe = *static_cast<RevisionSnapshotProbe*>(context);
        probe.calls.fetch_add(1U, std::memory_order_acq_rel);
        return probe.revision.valid() ? std::optional<PhysicalFrameRevision>{probe.revision} : std::nullopt;
    }
};

struct GatedRevisionSnapshotProbe final {
    GatedRevisionSnapshotProbe() : release(release_promise.get_future().share()) {}

    std::atomic<std::size_t> calls{0U};
    std::promise<void> snapshot_checked;
    std::promise<void> release_promise;
    std::shared_future<void> release;
    PhysicalFrameRevision revision{};

    static std::optional<PhysicalFrameRevision> Snapshot(void* context) noexcept {
        auto& probe = *static_cast<GatedRevisionSnapshotProbe*>(context);
        const std::size_t call = probe.calls.fetch_add(1U, std::memory_order_acq_rel);
        const PhysicalFrameRevision checked = probe.revision;
        if (call == 0U) {
            probe.snapshot_checked.set_value();
            probe.release.wait();
        }
        return checked.valid() ? std::optional<PhysicalFrameRevision>{checked} : std::nullopt;
    }
};

class GatedRevisionWait final {
   public:
    GatedRevisionWait() {
        revisions.Reset();
        auto checked = probe.snapshot_checked.get_future();
        result = std::async(std::launch::async,
                            [this] { return revisions.Wait(std::stop_token{}, &probe, &GatedRevisionSnapshotProbe::Snapshot); });
        checked.wait();
    }

    LiveRevisionWait revisions;
    GatedRevisionSnapshotProbe probe;
    std::future<std::optional<PhysicalFrameRevision>> result;
};

TEST_CASE("Live revision notification is retained after checked snapshot") {
    GatedRevisionWait wait;
    wait.probe.revision = kRevision;
    std::promise<void> notifier_started;
    auto notifier = std::async(std::launch::async, [&] {
        notifier_started.set_value();
        wait.revisions.RevisionReady();
    });
    notifier_started.get_future().wait();
    wait.probe.release_promise.set_value();

    notifier.get();
    const auto observed = wait.result.get();
    REQUIRE(observed.has_value());
    CHECK(observed->revision == kRevision.revision);
    CHECK(wait.probe.calls.load(std::memory_order_acquire) == 2U);
}

TEST_CASE("Live admitted-run terminal notification is retained after checked snapshot") {
    GatedRevisionWait wait;

    struct TypedTerminalProbe final {
        LiveRevisionWait* revisions = nullptr;
        std::size_t notifications = 0U;
        static void Notify(void* context, const LivePhysicalTerminal&) noexcept {
            auto& probe = *static_cast<TypedTerminalProbe*>(context);
            ++probe.notifications;
            probe.revisions->Fail();
        }
    } terminal_probe{&wait.revisions};
    auto capture_terminal = std::make_shared<mmltk::backend::media::capture::CaptureStopTerminal>();
    capture_terminal->identity = {.session = 1U, .generation = 1U};
    LivePhysicalTerminal terminal{std::move(capture_terminal),
                                  {mmltk::backend::media::capture::StatusCode::kInternalError, "post-start failure"}};
    LiveAdmittedRunTerminalListener listener{&terminal_probe, &TypedTerminalProbe::Notify};
    std::promise<void> notifier_started;
    auto notifier = std::async(std::launch::async, [&] {
        notifier_started.set_value();
        listener(terminal);
    });
    notifier_started.get_future().wait();
    wait.probe.release_promise.set_value();

    notifier.get();
    CHECK_FALSE(wait.result.get().has_value());
    CHECK(wait.revisions.failed());
    CHECK(terminal_probe.notifications == 1U);
    CHECK(wait.probe.calls.load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Live no-custody start failure is synchronous and has no admitted terminal") {
    const mmltk::backend::media::capture::CaptureSessionStartResult rejected{
        mmltk::backend::media::capture::CaptureSessionStartPhase::NoCustody,
        {mmltk::backend::media::capture::StatusCode::kNoDevice, "capture unavailable"},
        {}};

    CHECK_FALSE(rejected.running());
    CHECK_FALSE(rejected.has_custody());
}

TEST_CASE("Live revision wait returns one checked snapshot without refetch") {
    LiveRevisionWait revisions;
    revisions.Reset();
    RevisionSnapshotProbe probe;
    probe.revision = kRevision;

    const auto observed = revisions.Wait(std::stop_token{}, &probe, &RevisionSnapshotProbe::Snapshot);

    REQUIRE(observed.has_value());
    CHECK(observed->revision == kRevision.revision);
    CHECK(probe.calls.load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Live terminal race refuses one observed output without borrowing") {
    struct AcquireProbe final {
        std::size_t calls = 0U;
        static bool Acquire(void* context, PhysicalFrameRevision, LiveCompositeOutputLease*) {
            ++static_cast<AcquireProbe*>(context)->calls;
            return false;
        }
    } probe;
    LiveCompositeOutputLease lease;

    CHECK_FALSE(try_acquire_live_output(&probe, kRevision, &AcquireProbe::Acquire, &lease));
    CHECK(probe.calls == 1U);
    CHECK_FALSE(lease);
}

TEST_CASE("rejected Live release leaves its local handle inert") {
    std::uintptr_t handle = 41U;
    std::size_t attempts = 0U;
    const auto reject = [&attempts](std::uintptr_t) noexcept {
        ++attempts;
        return cudaErrorNotPermitted;
    };

    CHECK(retire_live_local_handle(handle, std::uintptr_t{0U}, reject) == cudaErrorNotPermitted);
    CHECK(handle == 0U);
    CHECK(attempts == 1U);
    CHECK(retire_live_local_handle(handle, std::uintptr_t{0U}, reject) == cudaSuccess);
    CHECK(attempts == 1U);
}

TEST_CASE("Live physical aggregate retires once on its owner worker") {
    struct Aggregate final {
        std::thread::id owner;
        std::atomic<std::size_t>* releases = nullptr;
        std::atomic_bool* correct_worker = nullptr;
        ~Aggregate() {
            correct_worker->store(owner == std::this_thread::get_id(), std::memory_order_release);
            releases->fetch_add(1U, std::memory_order_acq_rel);
        }
    };

    std::atomic<std::size_t> releases{0U};
    std::atomic_bool correct_worker{false};
    auto retirement = std::async(std::launch::async, [&] {
        std::optional<Aggregate> aggregate{std::in_place, std::this_thread::get_id(), &releases, &correct_worker};
        retire_live_physical_aggregate(aggregate);
        return !aggregate.has_value();
    });

    CHECK(retirement.get());
    CHECK(releases.load(std::memory_order_acquire) == 1U);
    CHECK(correct_worker.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace mmltk::backend::media::live
