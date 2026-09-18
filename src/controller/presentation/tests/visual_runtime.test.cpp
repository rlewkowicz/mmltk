#include "src/common/io/scoped_fd.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/numa_topology.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/presentation/tests/support/visual_runtime_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/runtime_diagnostic_span.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/tests/support/explore_system_fixture.h"
#include "src/controller/subsystems/live/tests/support/live_system_fixture.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
TEST_CASE("Receiver-owned copy retains its source lease through completion") {
    auto backend = std::make_shared<FakeImageBackend>();
    mmltk::frameworks::gpu::SystemImageRuntime source{{.device = 0, .backend = backend}};
    mmltk::frameworks::gpu::SystemImageRuntime receiver{{.device = 0, .backend = backend}};
    backend->defer_events = true;
    auto event_gate = backend->HoldEventWaits("receiver source event wait");
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    const auto source_revision = source.Completed().revision();
    std::future<std::uint64_t> copy;
    std::future<void> superseding_publish;
    mmltk::testsupport::ScopedTestCleanup release_wait{[&] {
        event_gate->Release();
        backend->CompleteEvents();
    }};
    copy = std::async(std::launch::async, [&receiver, &source] {
        auto product = source.Borrow();
        const auto revision = product.plane(0U).revision();
        static_cast<void>(receiver.CopyFrom(std::move(product)));
        return revision;
    });
    REQUIRE(event_gate->WaitEntered(2s));
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
    REQUIRE_FALSE(source.TryAcquireOutput(baseline).valid());
    superseding_publish = std::async(std::launch::async, [&source] { source.Publish(4U, 4U, [](auto, auto, auto) {}); });
    event_gate->Release();
    backend->CompleteEvents();
    CHECK(mmltk::testsupport::await_test_future(copy, "receiver-owned source copy") == source_revision);
    mmltk::testsupport::await_test_future(superseding_publish, "superseding source publication");
}
class FailingExploreAlgorithm final : public SynchronousExploreAlgorithm {
   public:
    ExploreOpened Open(std::string_view, std::stop_token) override {
        return {
            .dataset = {.image_count = 1U, .image_width = 32U, .image_height = 32U},
            .order = {.matching_count = 1U, .visible_indices = {0U}},
        };
    }
    ExploreOrderCandidate PrepareFilter(const ExploreFilter&, std::uint64_t, std::size_t, std::stop_token) override { return {}; }
    void Commit(ExploreOrderCandidate) noexcept override {}
    void Reset() noexcept override {}
    ExploreOrderFacts Visible(ExploreViewport, const ExploreOrderCandidate* = nullptr) const override { return {}; }
    bool Contains(std::uint32_t) const override { return false; }
    std::optional<std::uint32_t> Adjacent(std::uint32_t, std::int64_t) const override { return {}; }
    void RenderProduct(const ExploreRenderPlan&, const ExploreOrderCandidate*, std::size_t, mmltk::frameworks::gpu::ImagePlaneView,
                       mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {
        throw std::runtime_error("deterministic compiled renderer failure");
    }
};
[[nodiscard]] detail::VisualRuntimeOwner::Notification no_op_visual_work(mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) { return {}; }
[[nodiscard]] std::uint64_t borrowed_visual_revision(detail::VisualRuntimeOwner& owner) {
    const auto borrowed = owner.Borrow();
    REQUIRE(borrowed.valid());
    return borrowed.plane(0U).revision();
}
[[nodiscard]] VisualRuntimeFactory gated_visual_construction(VisualRuntimeFactory factory, std::promise<void>& constructing, std::shared_future<void> release,
                                                             const std::size_t ordinal) {
    return [factory = std::move(factory), &constructing, release = std::move(release), ordinal, constructions = std::size_t{0U}](auto revisions) mutable {
        if (ordinal == 0U || ++constructions == ordinal) {
            constructing.set_value();
            release.wait();
        }
        return factory(std::move(revisions));
    };
}
// The reader task owns both acquisition and destruction of its shared locks.
// The test thread holds only the gate and the scalar completion result.
class HeldVisualReader final {
   public:
    HeldVisualReader(detail::VisualRuntimeOwner& owner, std::string name)
        : gate_(std::move(name)), reading_(std::async(std::launch::async, [&owner, receipt = gate_.receipt()] {
              const auto held = owner.Borrow();
              receipt.ArriveAndWait();
              return held.valid() ? held.plane(0U).revision() : 0U;
          })) {}
    ~HeldVisualReader() {
        gate_.Release();
        if (reading_.valid()) reading_.wait();
    }
    [[nodiscard]] bool WaitEntered() const { return gate_.WaitEntered(2s); }
    [[nodiscard]] std::uint64_t ReleaseAndWait() {
        gate_.Release();
        return mmltk::testsupport::await_test_future(reading_, gate_.name());
    }

   private:
    mmltk::testsupport::TestGate gate_;
    std::future<std::uint64_t> reading_;
};
void submit_visual_revision(detail::VisualRuntimeOwner& owner, std::promise<std::uint64_t>& completed, const bool staged = false) {
    auto work = [&completed](auto& runtime, std::stop_token) {
        runtime.Publish(8U, 8U, [](auto, auto, auto) {});
        const auto revision = runtime.OutputFacts().revision;
        return detail::VisualRuntimeOwner::Notification{[&completed, revision] { completed.set_value(revision); }};
    };
    if (staged)
        REQUIRE(owner.SubmitDiscrete(std::move(work), {}, true));
    else
        REQUIRE(owner.SubmitOrdered(std::move(work)));
}
void publish_visual_pixels(detail::VisualRuntimeOwner& owner, std::promise<void>& published) {
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(4U, 3U, [](auto clean, auto, auto) { Fill(clean, 93); });
        return detail::VisualRuntimeOwner::Notification{[&] { published.set_value(); }};
    }));
    mmltk::testsupport::await_test_promise(published, "initial display product");
}
mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate enqueue_test_output(mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::uintptr_t& stream) {
    auto candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 4U, 3U, [&](auto, auto, auto execution) { stream = execution; }, mmltk::frameworks::gpu::ImageSubmission::Enqueue);
    return candidate;
}
auto first_visual_failure(std::atomic_uint& count, std::promise<std::exception_ptr>& first) {
    return [&count, &first](std::exception_ptr failure) {
        if (count.fetch_add(1U) == 0U) first.set_value(failure);
    };
}
void submit_visual_workspace(detail::VisualRuntimeOwner& owner, const std::shared_ptr<FakeImageBackend>& backend,
                             std::promise<mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput>& created) {
    const bool submitted = owner.SubmitOrdered([&backend, &created](auto& runtime, std::stop_token) {
        auto [workspace, prepared] = mmltk::frameworks::gpu::test_support::PublishTestWorkspace(runtime, backend);
        REQUIRE(prepared);
        return detail::VisualRuntimeOwner::Notification{[&created, product = runtime.Completed()]() mutable { created.set_value(std::move(product)); }};
    });
    REQUIRE(submitted);
}
void submit_visual_completion(detail::VisualRuntimeOwner& owner, std::promise<void>& completed) {
    REQUIRE(owner.SubmitOrdered([&completed](auto& runtime, std::stop_token) {
        runtime.Publish(8U, 8U, [](auto, auto, auto) {});
        return detail::VisualRuntimeOwner::Notification{[&completed] { completed.set_value(); }};
    }));
}
// Declared before the runtime owner so recorded-work callbacks settle before
// their result storage and completion promise are destroyed.
class VisualWorkLog final {
   public:
    void Append(const int value) {
        std::scoped_lock lock(mutex_);
        values_.push_back(value);
    }
    [[nodiscard]] detail::VisualRuntimeOwner::Work Record(const int value, const bool completes = false) {
        return [this, value, completes](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
            Append(value);
            if (completes) completed_.set_value();
            return detail::VisualRuntimeOwner::Notification{};
        };
    }
    void AwaitCompletion() { mmltk::testsupport::await_test_promise(completed_, "recorded visual work completion", 2s); }
    // The test reads only after stopping and joining its runtime owner.
    [[nodiscard]] const std::vector<int>& values() const noexcept { return values_; }

   private:
    std::mutex mutex_;
    std::vector<int> values_;
    std::promise<void> completed_;
};
void submit_blocked_visual_work(detail::VisualRuntimeOwner& owner, std::promise<void>& entered, std::shared_future<void> release) {
    REQUIRE(owner.SubmitOrdered([&entered, release = std::move(release)](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        entered.set_value();
        release.wait();
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(entered, "entered");
}
void submit_counting_continuation(detail::VisualRuntimeOwner& owner, std::atomic_uint32_t& count) {
    owner.RegisterContinuation([&count](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        count.fetch_add(1U, std::memory_order_release);
        return detail::VisualRuntimeOwner::Notification{};
    });
    REQUIRE(owner.NotifyContinuation());
}
TEST_CASE("Visual workspace retirement resumes queued work only after its delayed physical outcome", "[workspace]") {
    using mmltk::frameworks::gpu::SystemImageRuntime;
    using mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess;
    const bool fail_release = GENERATE(false, true);
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    std::atomic<std::size_t> constructions{0U};
    std::promise<SystemImageRuntime::CompletedOutput> created_workspace;
    auto workspace_result = created_workspace.get_future();
    std::promise<void> staged_completed;
    auto staged_result = staged_completed.get_future();
    std::promise<void> queued_completed;
    auto queued_result = queued_completed.get_future();
    std::promise<std::exception_ptr> failed;
    auto failure_result = failed.get_future();
    std::atomic<std::size_t> failures{0U};
    mmltk::testsupport::TestGate staged_gate("workspace staged retirement");
    detail::VisualRuntimeOwner owner{
        [&](auto revisions) {
            ++constructions;
            auto runtime = std::make_unique<SystemImageRuntime>(mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend, std::move(revisions)));
            return runtime;
        },
        [&](std::exception_ptr failure) {
            if (failures.fetch_add(1U) == 0U) failed.set_value(failure);
        }};
    submit_visual_workspace(owner, backend, created_workspace);
    auto workspace = mmltk::testsupport::await_test_future(workspace_result, "external workspace creation");
    REQUIRE(owner.SubmitDiscrete(
        [&](auto& runtime, std::stop_token) {
            runtime.Publish(4U, 3U, [](auto, auto, auto) {});
            staged_gate.receipt().ArriveAndWait();
            return detail::VisualRuntimeOwner::Notification{[&] { staged_completed.set_value(); }};
        },
        {}, true));
    mmltk::testsupport::ScopedTestCleanup release_stage{[&] { staged_gate.Release(); }};
    REQUIRE(staged_gate.WaitEntered(2s));
    REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&] { queued_completed.set_value(); }}; }));
    staged_gate.Release();
    mmltk::testsupport::await_test_future(staged_result, "staged workspace retirement handoff");
    CHECK(failures.load() == 0U);
    CHECK(queued_result.wait_for(0s) == std::future_status::timeout);
    CHECK(constructions.load() == 2U);
    const auto cleanup = std::make_exception_ptr(std::runtime_error("delayed visual display release failure"));
    if (fail_release) backend->FailDeviceBinding(1, cleanup);
    workspace = {};
    if (fail_release) {
        const auto failure = mmltk::testsupport::await_test_future(failure_result, "delayed workspace terminal result");
        CHECK(mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, cleanup));
        CHECK_FALSE(owner.SubmitLatest([](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{}; }));
        CHECK(queued_result.wait_for(0s) == std::future_status::timeout);
    } else {
        mmltk::testsupport::await_test_future(queued_result, "queued work after healthy workspace release");
        CHECK(failures.load() == 0U);
        std::promise<void> resumed;
        auto resumed_result = resumed.get_future();
        submit_visual_completion(owner, resumed);
        mmltk::testsupport::await_test_future(resumed_result, "restored visual admission");
    }
    owner.StopAndWait();
    CHECK(constructions.load() == 2U);
}
TEST_CASE("Visual owner destruction detaches a healthy deferred workspace wake", "[workspace]") {
    using mmltk::frameworks::gpu::SystemImageRuntime;
    using mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    std::promise<SystemImageRuntime::CompletedOutput> created;
    auto ready = created.get_future();
    auto owner = std::make_unique<detail::VisualRuntimeOwner>(
        [&](auto revisions) {
            auto runtime = std::make_unique<SystemImageRuntime>(mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend, std::move(revisions)));
            return runtime;
        },
        [](std::exception_ptr) { FAIL("healthy delayed workspace unexpectedly failed"); });
    submit_visual_workspace(*owner, backend, created);
    auto workspace = mmltk::testsupport::await_test_future(ready, "workspace before visual owner destruction");
    owner.reset();
    CHECK(backend->contexts_destroyed == 0U);
    workspace = {};
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(mmltk::frameworks::gpu::test_support::ImportedImageBufferTestAccess::unmaps == 1U);
}
TEST_CASE("a failed visual aggregate publishes once and reconstructs lazily") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto constructions = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto failures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend,
                             [constructions] {
                                 const auto generation = constructions->fetch_add(1U, std::memory_order_acq_rel);
                                 if (generation == 0U)
                                     return std::unique_ptr<mmltk::frameworks::gpu::SystemImageModel>{std::make_unique<FailingExploreAlgorithm>()};
                                 return std::unique_ptr<mmltk::frameworks::gpu::SystemImageModel>{
                                     std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U))};
                             },
                             count_explore_failures(failures)};
    auto& explore = scenario.system();
    static_cast<void>(explore.Open({.viewport = {.extent = {32U, 32U}}, .compiled_source = "/test"}));
    REQUIRE(scenario.Wait([&] { return failures->load(std::memory_order_acquire) == 1U; }));
    CHECK_FALSE(explore.snapshot().ready);
    CHECK(explore.snapshot().dataset.image_count == 0U);
    CHECK_FALSE(explore.snapshot().frame.valid());
    CHECK_FALSE(explore.snapshot().busy);
    CHECK_FALSE(explore.snapshot().cancellation_requested);
    static_cast<void>(explore.Open({.viewport = {.extent = {32U, 32U}}, .compiled_source = "/test"}));
    REQUIRE(scenario.Wait([&] { return explore.snapshot().ready; }));
    CHECK(failures->load(std::memory_order_acquire) == 1U);
    CHECK(constructions->load(std::memory_order_acquire) == 2U);
}
TEST_CASE("Workspace input retains ordered high-water records for independent native owners") {
    WorkspaceInputQueue<WorkspaceMouse> queue;
    for (std::size_t index = 0U; index != 1024U; ++index)
        queue.Push({.source = PresentationSourceKind::Explore,
                    .peer_epoch = 1U,
                    .kind = WorkspaceMouseKind::Motion,
                    .point = WorkspacePoint{static_cast<float>(index) + 0.25F, 0.125F}});
    const auto capacity = queue.capacity();
    for (std::size_t index = 0U; index != 1024U; ++index) {
        const auto mouse = queue.Pop();
        REQUIRE(mouse);
        REQUIRE(mouse->point);
        CHECK(mouse->point->x == static_cast<float>(index) + 0.25F);
    }
    CHECK(queue.empty());
    CHECK(queue.capacity() == capacity);
    for (const auto source : presentation_source_metadata) {
        if (source.kind == PresentationSourceKind::None) continue;
        WorkspaceInput owner;
        owner.SetPeer(2U);
        for (const auto entry : mmltk::frameworks::reflection::enum_entries<WorkspaceMouseKind>()) {
            WorkspaceMouse mouse{.source = source.kind,
                                 .peer_epoch = 2U,
                                 .kind = entry.value,
                                 .button = WorkspaceMouseButton::Other,
                                 .other_button = 65535U,
                                 .click_count = 2U,
                                 .modifiers = 15U,
                                 .wheel_unit = WorkspaceWheelUnit::Pixels,
                                 .wheel = {-0.125F, 0.25F}};
            owner.Accept(mouse, source.kind);
            REQUIRE(owner.latest());
            CHECK(owner.latest()->kind == mouse.kind);
            CHECK(owner.latest()->other_button == 65535U);
            CHECK_FALSE(owner.latest()->point);
            CHECK(owner.latest()->wheel == mouse.wheel);
        }
        owner.SetPeer(3U);
        CHECK_FALSE(owner.latest());
    }
}
TEST_CASE("visual runtime reconstructs after consecutive factory failures") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto attempts = std::make_shared<std::atomic_uint32_t>(0U);
    auto failures = std::make_shared<std::atomic_uint32_t>(0U);
    EventGate failure_events;
    std::promise<void> completed;
    auto successful = test_live_runtime_factory(backend, captures);
    detail::VisualRuntimeOwner owner{[attempts, successful = std::move(successful)](auto revisions) mutable {
                                         if (attempts->fetch_add(1U, std::memory_order_acq_rel) < 2U)
                                             throw std::runtime_error("deterministic construction failure");
                                         return successful(std::move(revisions));
                                     },
                                     [failures, &failure_events](std::exception_ptr) {
                                         failures->fetch_add(1U, std::memory_order_acq_rel);
                                         failure_events.Advance();
                                     }};
    const auto submit = [&owner](detail::VisualRuntimeOwner::Work work) { REQUIRE(owner.SubmitDiscrete(std::move(work))); };
    submit(no_op_visual_work);
    REQUIRE(failure_events.Wait([&] { return failures->load(std::memory_order_acquire) == 1U; }));
    submit(no_op_visual_work);
    REQUIRE(failure_events.Wait([&] { return failures->load(std::memory_order_acquire) == 2U; }));
    submit([&completed](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        return detail::VisualRuntimeOwner::Notification{[&completed] { completed.set_value(); }};
    });
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(completed, "completed", 2s));
    CHECK(attempts->load(std::memory_order_acquire) == 3U);
    CHECK(failures->load(std::memory_order_acquire) == 2U);
}
TEST_CASE("visual producer revisions remain unique across staged runtime reconstruction") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<std::uint64_t> first_completed;
    std::promise<std::uint64_t> replacement_completed;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    submit_visual_revision(owner, first_completed);
    const auto first_revision = first_completed.get_future().get();
    submit_visual_revision(owner, replacement_completed, true);
    const auto replacement_revision = replacement_completed.get_future().get();
    CHECK(replacement_revision > first_revision);
    owner.StopAndWait();
}
TEST_CASE("failed visual runtime replacement keeps the exact completed product") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto attempts = std::make_shared<std::atomic_uint32_t>(0U);
    auto successful = test_live_runtime_factory(backend, captures);
    std::promise<std::uint64_t> first_completed;
    std::promise<void> failed;
    detail::VisualRuntimeOwner owner{[attempts, successful = std::move(successful)](auto revisions) mutable {
                                         if (attempts->fetch_add(1U, std::memory_order_acq_rel) == 1U)
                                             throw std::runtime_error("deterministic replacement failure");
                                         return successful(std::move(revisions));
                                     },
                                     [&failed](std::exception_ptr) { failed.set_value(); }};
    submit_visual_revision(owner, first_completed);
    const auto first_revision = first_completed.get_future().get();
    REQUIRE(owner.SubmitDiscrete(no_op_visual_work, {}, true));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
    auto retained = owner.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == first_revision);
    std::promise<void> rejected;
    REQUIRE(owner.SubmitDiscrete(
        [&rejected](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&rejected] { rejected.set_value(); }}; }, {}, true));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(rejected, "rejected", 2s));
    retained = owner.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == first_revision);
    retained = {};
    owner.StopAndWait();
}
TEST_CASE("staged cancellation keeps incumbent pixels visible until replacement settlement") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<std::uint64_t> first;
    std::promise<void> submitted;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::promise<void> settled;
    std::atomic_bool success_notified = false;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release);
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(8U, 8U, [](auto, auto, auto) {});
        const auto revision = runtime.Completed().revision();
        return detail::VisualRuntimeOwner::Notification{[&, revision] { first.set_value(revision); }};
    }));
    const auto incumbent = first.get_future().get();
    REQUIRE(owner.SubmitDiscrete(
        [&](auto& runtime, std::stop_token) {
            runtime.Publish(8U, 8U, [](auto, auto, auto) {});
            submitted.set_value();
            released.wait();
            return detail::VisualRuntimeOwner::Notification{[&] { success_notified = true; }};
        },
        [&] { settled.set_value(); }, true));
    mmltk::testsupport::await_test_promise(submitted, "submitted");
    auto read = owner.Borrow();
    const auto revision = read.valid() ? read.plane(0U).revision() : 0U;
    read = {};
    owner.RequestActiveStop();
    release.set_value();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(settled, "settled", 2s));
    CHECK(revision == incumbent);
    CHECK_FALSE(success_notified.load());
    auto retained = owner.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == incumbent);
    retained = {};
    owner.StopAndWait();
}
TEST_CASE("staged completion wins late stop before promotion and publishes its exact borrowed product") {
    const bool producer_claims_completion = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> first;
    std::promise<void> latched;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::promise<std::uint64_t> published;
    std::atomic_bool cancelled = false;
    detail::VisualRuntimeOwner owner{
        test_live_runtime_factory(backend, captures), [](std::exception_ptr) {},
        [&](detail::VisualRuntimeOwner::ActivityStage stage, std::uint64_t completed) noexcept {
            if (!producer_claims_completion && stage == detail::VisualRuntimeOwner::ActivityStage::StagedCompletionLatched && completed != 0U) {
                latched.set_value();
                released.wait();
            }
        }};
    auto settle_owner = settle_visual_on_exit(owner, release);
    submit_visual_completion(owner, first);
    mmltk::testsupport::await_test_promise(first, "first");
    const auto prior_revision = borrowed_visual_revision(owner);
    REQUIRE(owner.SubmitDiscrete(
        [&](auto& runtime, std::stop_token) {
            if (producer_claims_completion && !owner.TryCompleteActiveWork()) throw std::runtime_error("producer completion unexpectedly cancelled");
            runtime.Publish(8U, 8U, [](auto, auto, auto) {});
            const auto revision = runtime.Completed().revision();
            if (producer_claims_completion) {
                latched.set_value();
                released.wait();
            }
            return detail::VisualRuntimeOwner::Notification{[&, revision] { published.set_value(revision); }};
        },
        [&] { cancelled = true; }, true));
    mmltk::testsupport::await_test_promise(latched, "latched");
    owner.RequestActiveStop();
    release.set_value();
    auto publication = published.get_future();
    REQUIRE(publication.wait_for(2s) == std::future_status::ready);
    auto borrowed = owner.Borrow();
    REQUIRE(borrowed.valid());
    const auto revision = publication.get();
    CHECK(revision > prior_revision);
    CHECK(borrowed.plane(0U).revision() == revision);
    CHECK_FALSE(cancelled.load());
    borrowed = {};
    owner.StopAndWait();
}
TEST_CASE("runtime construction does not hold scheduler admission while stop is requested") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto factory = test_live_runtime_factory(backend, captures);
    std::promise<void> constructing;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic_bool entered = false;
    detail::VisualRuntimeOwner owner{gated_visual_construction(std::move(factory), constructing, released, 0U), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release);
    REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) {
        entered = true;
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(constructing, "constructing");
    auto stopped = std::async(std::launch::async, [&] { owner.RequestStop(); });
    const auto status = stopped.wait_for(2s);
    release.set_value();
    CHECK(status == std::future_status::ready);
    mmltk::testsupport::await_test_future(stopped, "released construction stop");
    owner.StopAndWait();
    CHECK(owner.stopped());
    CHECK_FALSE(entered.load());
}
TEST_CASE("safe incumbent retirement failure preserves the promoted runtime and permits later work") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> first;
    std::promise<void> failed;
    std::promise<void> next;
    std::atomic<std::uint64_t> promoted = 0U;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [&](std::exception_ptr) { failed.set_value(); }};
    submit_visual_completion(owner, first);
    mmltk::testsupport::await_test_promise(first, "first");
    REQUIRE(owner.SubmitDiscrete(
        [&](auto& runtime, std::stop_token) {
            runtime.Publish(8U, 8U, [](auto, auto, auto) {});
            promoted.store(runtime.Completed().revision(), std::memory_order_release);
            backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream);
            return detail::VisualRuntimeOwner::Notification{};
        },
        {}, true));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
    auto retained = owner.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == promoted.load(std::memory_order_acquire));
    retained = {};
    REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&] { next.set_value(); }}; }));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(next, "next", 2s));
    owner.StopAndWait();
}
// CLEANUP-IGNORE: Construction-stop and staged-replacement tests use distinct receipts and cancellation boundaries.
TEST_CASE("active stop during staged construction rejects replacement before domain work") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto factory = test_live_runtime_factory(backend, captures);
    std::promise<void> first;
    std::promise<void> constructing;
    std::promise<void> release;
    std::promise<void> cancelled;
    auto released = release.get_future().share();
    std::atomic_bool entered = false;
    detail::VisualRuntimeOwner owner{gated_visual_construction(std::move(factory), constructing, released, 2U), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release);
    submit_visual_completion(owner, first);
    mmltk::testsupport::await_test_promise(first, "first");
    const auto revision = borrowed_visual_revision(owner);
    REQUIRE(owner.SubmitDiscrete(
        [&](auto&, std::stop_token) {
            entered = true;
            return detail::VisualRuntimeOwner::Notification{};
        },
        [&] { cancelled.set_value(); }, true));
    mmltk::testsupport::await_test_promise(constructing, "constructing");
    owner.RequestActiveStop();
    release.set_value();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(cancelled, "cancelled", 2s));
    // CLEANUP-IGNORE: This assertion proves work never entered during construction cancellation, not absence of a completion notification.
    CHECK_FALSE(entered.load());
    // CLEANUP-IGNORE: Borrow retention after construction cancellation is a separate oracle from in-flight work cancellation.
    auto retained = owner.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == revision);
    retained = {};
    owner.StopAndWait();
}
// CLEANUP-IGNORE: Ordered replaceable-input semantics are independent from queued discrete cancellation.
TEST_CASE("ordered visual input retains boundaries and only the latest replaceable sample") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> boundary_entered;
    std::promise<void> release_boundary;
    auto release = release_boundary.get_future().share();
    VisualWorkLog work;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release_boundary);
    REQUIRE(owner.SubmitOrdered([&boundary_entered, release, &work](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        work.Append(1);
        boundary_entered.set_value();
        release.wait();
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(boundary_entered, "boundary_entered");
    // CLEANUP-IGNORE: Latest/discrete and continuation submissions exercise different queue contracts using shared
    // recording work.
    owner.SubmitLatest(work.Record(2));
    owner.SubmitLatest(work.Record(3));
    REQUIRE(owner.SubmitDiscrete(work.Record(4, true)));
    release_boundary.set_value();
    REQUIRE_NOTHROW(work.AwaitCompletion());
    owner.StopAndWait();
    CHECK(work.values() == std::vector<int>{1, 3, 4});
}
// CLEANUP-IGNORE: Coalescing owns a boundary gate distinct from the blocked-borrow concurrency scenario.
TEST_CASE("visual continuations coalesce behind the newest replaceable input") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> boundary_entered;
    std::promise<void> release_boundary;
    const auto release = release_boundary.get_future().share();
    VisualWorkLog work;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release_boundary);
    REQUIRE(owner.SubmitOrdered([&](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        boundary_entered.set_value();
        release.wait();
        work.Append(1);
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(boundary_entered, "boundary_entered");
    owner.RegisterContinuation(work.Record(4, true));
    REQUIRE(owner.NotifyContinuation());
    owner.SubmitLatest(work.Record(3));
    REQUIRE(owner.NotifyContinuation());
    release_boundary.set_value();
    REQUIRE_NOTHROW(work.AwaitCompletion());
    owner.StopAndWait();
    CHECK(work.values() == std::vector<int>{1, 3, 4});
    EventGate retry_events;
    std::atomic_uint32_t retry_calls{0U}, cycles{0U};
    std::atomic_uint64_t wake_again{0U};
    std::atomic_bool reserved{false}, failed_try{false};
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
    detail::VisualRuntimeOwner retry_owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {},
                                           [&](detail::VisualRuntimeOwner::ActivityStage stage, std::uint64_t value) noexcept {
                                               if (stage == detail::VisualRuntimeOwner::ActivityStage::CycleFinalized) {
                                                   wake_again.store(value, std::memory_order_release);
                                                   cycles.fetch_add(1U, std::memory_order_release);
                                                   retry_events.Advance();
                                               }
                                           }};
    retry_owner.RegisterContinuation(
        [&](auto& runtime, std::stop_token) {
            auto output = runtime.TryAcquireOutput(baseline);
            if (output.valid()) {
                retry_owner.SetOutputRetry(false);
                reserved.store(true, std::memory_order_release);
            }
            retry_calls.fetch_add(1U, std::memory_order_release);
            retry_events.Advance();
            return detail::VisualRuntimeOwner::Notification{};
        },
        {}, true);
    mmltk::testsupport::ScopedTestCleanup settle_retry{[&] {
        retry_owner.RequestStop();
        retry_owner.StopAndWait();
    }};
    REQUIRE(retry_owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        runtime.Publish(8U, 8U, [](auto, auto, auto) {});
        return detail::VisualRuntimeOwner::Notification{};
    }));
    REQUIRE(retry_events.Wait([&] { return cycles.load(std::memory_order_acquire) >= 1U; }));
    HeldVisualReader unarmed_reader{retry_owner, "unarmed output retry reader"};
    REQUIRE(unarmed_reader.WaitEntered());
    CHECK(wake_again.load(std::memory_order_acquire) == 0U);
    CHECK(retry_calls.load(std::memory_order_acquire) == 0U);
    CHECK(unarmed_reader.ReleaseAndWait() == 1U);
    // Settle an ordinary cycle after release to prove it did not arm a retry.
    REQUIRE(retry_owner.SubmitOrdered([](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{}; }));
    REQUIRE(retry_events.Wait([&] { return cycles.load(std::memory_order_acquire) >= 2U; }));
    CHECK(wake_again.load(std::memory_order_acquire) == 0U);
    CHECK(retry_calls.load(std::memory_order_acquire) == 0U);
    HeldVisualReader armed_reader{retry_owner, "armed output retry reader"};
    REQUIRE(armed_reader.WaitEntered());
    REQUIRE(retry_owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        baseline = runtime.Completed();
        retry_owner.SetOutputRetry(true);
        failed_try.store(!runtime.TryAcquireOutput(baseline).valid(), std::memory_order_release);
        // Retained-product notifications happen before physical receiver release;
        // several such signals must leave only one still-unsuccessful retry.
        for (unsigned index = 0U; index != 2U; ++index) { auto temporary = runtime.Completed(); }
        return detail::VisualRuntimeOwner::Notification{};
    }));
    REQUIRE(retry_events.Wait([&] { return retry_calls.load(std::memory_order_acquire) == 1U; }));
    CHECK(failed_try.load(std::memory_order_acquire));
    CHECK_FALSE(reserved.load(std::memory_order_acquire));
    CHECK(armed_reader.ReleaseAndWait() == 1U);
    REQUIRE(retry_events.Wait([&] { return reserved.load(std::memory_order_acquire); }));
    retry_owner.StopAndWait();
    CHECK(retry_calls.load(std::memory_order_acquire) == 2U);
}
TEST_CASE("visual continuation cancellation policy is independent of output availability registration") {
    const bool output_wake = GENERATE(false, true);
    // CLEANUP-IGNORE: Continuation input preservation and producer completion are different booleans with independent event receipts.
    const bool preserve_input = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> entered;
    std::promise<void> release_work;
    const auto released = release_work.get_future().share();
    std::promise<bool> cancelled;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release_work);
    owner.RegisterContinuation(
        [&](auto&, const std::stop_token stop) {
            entered.set_value();
            released.wait();
            const bool observed_stop = stop.stop_requested();
            const bool completed = owner.TryCompleteActiveWork();
            return detail::VisualRuntimeOwner::Notification{[&, observed_stop, completed] { cancelled.set_value(observed_stop && !completed); }};
        },
        {}, output_wake,
        preserve_input ? detail::VisualRuntimeOwner::ContinuationCancellation::PreserveOrderedInput
                       : detail::VisualRuntimeOwner::ContinuationCancellation::Cancel);
    REQUIRE(owner.NotifyContinuation());
    mmltk::testsupport::await_test_promise(entered, "continuation entered");
    static_cast<void>(owner.RequestActiveStop());
    release_work.set_value();
    CHECK(mmltk::testsupport::await_test_promise(cancelled, "continuation cancellation") == !preserve_input);
}
// CLEANUP-IGNORE: Borrow locking requires independent promises and runtime ownership from continuation draining.
TEST_CASE("visual completion notification remains independent of a blocked product borrow") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> publish_entered;
    std::promise<void> release_publish;
    const auto release = release_publish.get_future().share();
    std::promise<void> release_work;
    const auto finish = release_work.get_future().share();
    std::promise<void> notified;
    std::promise<void> borrow_locked;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {},
                                     [&](detail::VisualRuntimeOwner::ActivityStage stage, std::uint64_t) noexcept {
                                         if (stage == detail::VisualRuntimeOwner::ActivityStage::BorrowLocked) borrow_locked.set_value();
                                     }};
    mmltk::testsupport::ScopedTestCleanup settle_owner{[&] {
        mmltk::testsupport::release_test_promise(release_publish);
        mmltk::testsupport::release_test_promise(release_work);
        owner.RequestStop();
        owner.StopAndWait();
    }};
    std::future<std::uint64_t> borrow;
    std::future<bool> notification;
    mmltk::testsupport::ScopedTestCleanup release_futures{[&] {
        mmltk::testsupport::release_test_promise(release_publish);
        mmltk::testsupport::release_test_promise(release_work);
    }};
    owner.RegisterContinuation([&](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        notified.set_value();
        return detail::VisualRuntimeOwner::Notification{};
    });
    REQUIRE(owner.SubmitOrdered([&](mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token) {
        runtime.Publish(8U, 8U, [&](auto, auto, auto) {
            publish_entered.set_value();
            release.wait();
        });
        finish.wait();
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_promise(publish_entered, "publish_entered");
    borrow = std::async(std::launch::async, [&] {
        const auto view = owner.Borrow();
        return view.valid() ? view.plane(0U).revision() : 0U;
    });
    mmltk::testsupport::await_test_promise(borrow_locked, "borrow_locked");
    notification = std::async(std::launch::async, [&] { return owner.NotifyContinuation(); });
    // BorrowLocked identifies the actual runtime lock. Notification must
    // complete while the first physical publication is still held; cleanup
    // releases both promises before either future can unwind on a deadline.
    CHECK(mmltk::testsupport::await_test_future(notification, "continuation notification during held publication"));
    release_publish.set_value();
    release_work.set_value();
    CHECK(mmltk::testsupport::await_test_future(borrow, "released product borrow") == 1U);
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(notified, "notified", 2s));
    owner.StopAndWait();
}
TEST_CASE("visual continuation arrivals while draining survive without later input") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::promise<void> entered;
    std::promise<void> release_drain;
    const auto release = release_drain.get_future().share();
    std::promise<void> completed;
    std::atomic_uint32_t drains = 0U;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release_drain);
    owner.RegisterContinuation([&](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        if (drains.fetch_add(1U) == 0U) {
            entered.set_value();
            release.wait();
        } else {
            completed.set_value();
        }
        return detail::VisualRuntimeOwner::Notification{};
    });
    REQUIRE(owner.NotifyContinuation());
    mmltk::testsupport::await_test_promise(entered, "entered");
    REQUIRE(owner.NotifyContinuation());
    REQUIRE(owner.NotifyContinuation());
    release_drain.set_value();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(completed, "completed", 2s));
    owner.StopAndWait();
    CHECK(drains.load() == 2U);
    CHECK_FALSE(owner.NotifyContinuation());
}
TEST_CASE("visual terminal and failure boundaries discard pending continuations") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    std::atomic_uint32_t continuations = 0U;
    std::promise<void> boundary_entered;
    std::promise<void> release_boundary;
    const auto release = release_boundary.get_future().share();
    std::promise<void> terminal_completed;
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_owner = settle_visual_on_exit(owner, release_boundary);
    submit_blocked_visual_work(owner, boundary_entered, release);
    submit_counting_continuation(owner, continuations);
    REQUIRE(owner.SubmitTerminalBarrier([&](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
        terminal_completed.set_value();
        return detail::VisualRuntimeOwner::Notification{};
    }));
    release_boundary.set_value();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(terminal_completed, "terminal_completed", 2s));
    owner.StopAndWait();
    CHECK(continuations.load(std::memory_order_acquire) == 0U);
    std::promise<void> failed;
    std::promise<void> failure_boundary_entered;
    std::promise<void> release_failure_boundary;
    const auto failure_release = release_failure_boundary.get_future().share();
    detail::VisualRuntimeOwner failing{test_live_runtime_factory(backend, captures), [&](std::exception_ptr) { failed.set_value(); }};
    auto settle_failing = settle_visual_on_exit(failing, release_failure_boundary);
    submit_blocked_visual_work(failing, failure_boundary_entered, failure_release);
    REQUIRE(failing.SubmitOrdered([](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
        throw std::runtime_error("deterministic continuation boundary failure");
    }));
    submit_counting_continuation(failing, continuations);
    release_failure_boundary.set_value();
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
    failing.StopAndWait();
    CHECK(continuations.load(std::memory_order_acquire) == 0U);
    std::promise<void> stop_boundary_entered;
    std::promise<void> release_stop_boundary;
    const auto stop_release = release_stop_boundary.get_future().share();
    detail::VisualRuntimeOwner stopping{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {}};
    auto settle_stopping = settle_visual_on_exit(stopping, release_stop_boundary);
    submit_blocked_visual_work(stopping, stop_boundary_entered, stop_release);
    submit_counting_continuation(stopping, continuations);
    stopping.RequestStop();
    release_stop_boundary.set_value();
    stopping.StopAndWait();
    CHECK(continuations.load(std::memory_order_acquire) == 0U);
}
class RetainedConstructionModel final : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    RetainedConstructionModel(std::shared_ptr<std::atomic_bool> destroyed, std::shared_ptr<std::atomic_uint64_t> release_calls, std::exception_ptr failure = {},
                              bool all_released = false)
        : destroyed_(std::move(destroyed)),
          release_calls_(std::move(release_calls)),
          failure_(failure ? std::move(failure) : std::make_exception_ptr(std::runtime_error("deterministic retained construction resource"))),
          all_released_(all_released) {}
    ~RetainedConstructionModel() override { destroyed_->store(true, std::memory_order_release); }
    [[nodiscard]] Release ReleaseResources() noexcept override {
        release_calls_->fetch_add(1U, std::memory_order_release);
        return {
            .all_released = all_released_,
            .failure = failure_,
        };
    }

   private:
    std::shared_ptr<std::atomic_bool> destroyed_;
    std::shared_ptr<std::atomic_uint64_t> release_calls_;
    std::exception_ptr failure_;
    bool all_released_;
};
TEST_CASE("visual runtime failures retain initiating execution and model retirement identities") {
    using namespace mmltk::frameworks::gpu;
    const bool safe = GENERATE(false, true);
    const bool staged = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    auto releases = std::make_shared<std::atomic_uint64_t>(0U);
    const auto initiating = std::make_exception_ptr(GdrTransportUnavailable("domain operation"));
    const auto settlement = std::make_exception_ptr(std::runtime_error("stream settlement"));
    const auto retirement = std::make_exception_ptr(std::runtime_error("model retirement"));
    std::promise<std::exception_ptr> reported;
    detail::VisualRuntimeOwner owner{[&](auto revisions) {
                                         return std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
                                             .device = 0,
                                             .backend = backend,
                                             .model = std::make_unique<RetainedConstructionModel>(destroyed, releases, retirement, safe),
                                             .product_revisions = std::move(revisions)});
                                     },
                                     [&](std::exception_ptr failure) { reported.set_value(failure); }};
    auto work = [&](auto&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
        backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, settlement);
        std::rethrow_exception(initiating);
    };
    REQUIRE((staged ? owner.SubmitDiscrete(work, {}, true) : owner.SubmitOrdered(work)));
    auto result = reported.get_future();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto failure = result.get();
    CHECK(is_image_execution_failure(failure));
    CHECK(find_image_failure<GdrTransportUnavailable>(failure) == initiating);
    for (const auto& expected : {initiating, settlement, retirement}) CHECK(mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, expected));
    CHECK_THROWS_WITH(std::rethrow_exception(failure), "domain operation");
    CHECK(destroyed->load(std::memory_order_acquire) == safe);
    if (!safe) CHECK_FALSE(owner.SubmitOrdered(no_op_visual_work));
    owner.StopAndWait();
}
void check_visual_construction_custody(const FakeImageBackend::FailurePoint failure_point, const std::string_view expected_failure,
                                       const std::uint64_t expected_contexts, const std::uint64_t expected_release_calls) {
    auto backend = std::make_shared<FakeImageBackend>();
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    auto release_calls = std::make_shared<std::atomic_uint64_t>(0U);
    const auto construction_failure = std::make_exception_ptr(std::runtime_error("injected image backend failure"));
    const auto release_failure = std::make_exception_ptr(std::runtime_error("deterministic retained construction resource"));
    std::atomic_uint64_t constructions = 0U;
    std::atomic_bool reported_typed_failure = false;
    std::promise<void> failed;
    {
        detail::VisualRuntimeOwner owner{
            [&](auto revisions) {
                constructions.fetch_add(1U, std::memory_order_acq_rel);
                return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                    .device = 0,
                    .backend = backend,
                    .model = std::make_unique<RetainedConstructionModel>(destroyed, release_calls, release_failure),
                    .product_revisions = std::move(revisions),
                });
            },
            [&, expected_failure](const std::exception_ptr failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::runtime_error& error) {
                    const bool identities =
                        mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, construction_failure) &&
                        (expected_release_calls == 0U || mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, release_failure));
                    reported_typed_failure.store(std::string_view{error.what()} == expected_failure && identities, std::memory_order_release);
                } catch (...) {}
                failed.set_value();
            }};
        backend->FailAfter(failure_point, 0U, construction_failure);
        REQUIRE(owner.SubmitOrdered(no_op_visual_work));
        REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
        CHECK_FALSE(owner.SubmitOrdered(no_op_visual_work));
        owner.StopAndWait();
        CHECK(owner.stopped());
        CHECK(reported_typed_failure.load(std::memory_order_acquire));
        CHECK(constructions.load(std::memory_order_acquire) == 1U);
        CHECK(release_calls->load(std::memory_order_acquire) == expected_release_calls);
        CHECK_FALSE(destroyed->load(std::memory_order_acquire));
        CHECK(backend->contexts_created == expected_contexts);
        CHECK(backend->contexts_destroyed == 0U);
    }
    CHECK(release_calls->load(std::memory_order_acquire) == expected_release_calls);
    CHECK_FALSE(destroyed->load(std::memory_order_acquire));
    CHECK(backend->contexts_destroyed == 0U);
}
TEST_CASE("visual runtime owner retains unsafe factory construction and blocks reconstruction") {
    check_visual_construction_custody(FakeImageBackend::FailurePoint::CreateStream, "injected image backend failure", 1U, 1U);
}
TEST_CASE("visual runtime retirement preserves the model and context while release remains incomplete") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    auto releases = std::make_shared<std::atomic_uint64_t>(0U);
    auto runtime = std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
        .device = 0, .backend = backend, .model = std::make_unique<RetainedConstructionModel>(destroyed, releases)});
    const auto retirement = runtime->Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    REQUIRE(retirement.custody.valid());
    CHECK(retirement.custody.failure() == retirement.failure);
    REQUIRE(retirement.failure);
    CHECK_THROWS_WITH(std::rethrow_exception(retirement.failure), "deterministic retained construction resource");
    CHECK(releases->load(std::memory_order_acquire) == 1U);
    runtime.reset();
    CHECK_FALSE(destroyed->load(std::memory_order_acquire));
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("visual runtime owner retains an adopted model when context creation fails") {
    check_visual_construction_custody(FakeImageBackend::FailurePoint::CreateContext, "injected image backend failure", 0U, 0U);
}
TEST_CASE("visual retirement reports an unestablished context boundary and disables unsafe reuse") {
    auto backend = std::make_shared<FakeImageBackend>();
    std::atomic_uint64_t constructions = 0U;
    std::promise<void> failed;
    detail::VisualRuntimeOwner owner{[&](auto revisions) {
                                         constructions.fetch_add(1U, std::memory_order_acq_rel);
                                         return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                                             .device = 0, .backend = backend, .product_revisions = std::move(revisions)});
                                     },
                                     [&failed](std::exception_ptr) { failed.set_value(); }};
    REQUIRE(owner.SubmitOrdered([backend](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
        backend->FailPersistently(FakeImageBackend::FailurePoint::Bind);
        throw std::runtime_error("deterministic work failure before retirement");
    }));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
    CHECK_FALSE(owner.SubmitOrdered(no_op_visual_work));
    owner.StopAndWait();
    CHECK(owner.stopped());
    CHECK(constructions.load(std::memory_order_acquire) == 1U);
    CHECK(backend->streams_destroyed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
}
TEST_CASE("Visual diagnostics name every canonical completion and failure") {
    using namespace mmltk::frameworks::reflection;
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::TimelineReady) == "timeline.ready");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::CopyCompleted) == "copy.completed");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::WorkerFailure) == "worker.failure");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::PresentationReleaseWaitStarted) == "presentation.release_wait.started");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::AcceptanceCompiledRead) == "acceptance.compiled.read.started");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::ExploreSemanticPixels) == "explore.semantic.nonzero_pixels");
    CHECK(visual_diagnostic_event_name(VisualDiagnosticOperation::ExplorePrefetchReady) == "explore.prefetch.ready");
    for (const auto entry : enum_entries<VisualDiagnosticOperation>()) {
        const auto alias = visual_diagnostic_event_name(entry.value);
        CAPTURE(entry.name, alias);
        REQUIRE_FALSE(alias.empty());
        CHECK(alias.size() <= 96U);
        CHECK(enum_name(entry.value) == entry.name);
        CHECK(try_enum_from_name<VisualDiagnosticOperation>(entry.name) == entry.value);
        CHECK_FALSE(try_enum_from_name<VisualDiagnosticOperation>(alias).has_value());
    }
    CHECK(enum_name(VisualDiagnosticOperation::AcceptanceCompiledRead) == "AcceptanceCompiledRead");
    CHECK(enum_name(VisualDiagnosticOperation::ExploreSemanticPixels) == "ExploreSemanticPixels");
    for (unsigned int raw = 0U; raw <= std::numeric_limits<std::uint8_t>::max(); ++raw) {
        const auto operation = static_cast<VisualDiagnosticOperation>(raw);
        CAPTURE(raw);
        CHECK(visual_diagnostic_event_name(operation).empty() == !enum_contains(operation));
    }
}
TEST_CASE("Visual worker failures submit bounded valid UTF8 from dependency exception text") {
    std::string payload;
    std::string expected;
    SECTION("valid multibyte text crossing the byte limit is omitted whole") {
        const auto point = GENERATE(std::string_view{"\xc3\xa9"}, std::string_view{"\xe2\x82\xac"}, std::string_view{"\xf0\x9f\x98\x80"});
        expected.assign(kVisualFailureByteCapacity - 1U, 'a');
        payload = expected + std::string{point} + "tail";
    }
    SECTION("a complete code point at the byte limit is preserved") {
        payload.assign(kVisualFailureByteCapacity - 4U, 'a');
        payload += "\xf0\x9f\x98\x80";
        expected = payload;
        payload += "tail";
    }
    SECTION("malformed bytes are replaced without losing subsequent valid text") {
        const auto malformed = GENERATE(std::string_view{"\xff"}, std::string_view{"\xc0\x80"}, std::string_view{"\xed\xa0\x80"},
                                        std::string_view{"\xf4\x90\x80\x80"}, std::string_view{"\xe2\x82"});
        payload = "failure: " + std::string{malformed} + " tail \xc3\xa9";
        expected = "failure: ";
        for (std::size_t index = 0U; index < malformed.size(); ++index) expected += "\xef\xbf\xbd";
        expected += " tail \xc3\xa9";
    }
    SECTION("replacement also respects the byte limit") {
        expected.assign(kVisualFailureByteCapacity - 4U, 'a');
        payload = expected + "\xff\xff";
        expected += "\xef\xbf\xbd";
    }
    const auto detail = visual_failure_detail(std::make_exception_ptr(std::runtime_error{payload}), "fallback");
    CHECK(detail == expected);
    CHECK(detail.size() <= kVisualFailureByteCapacity);
    CHECK(visual_failure_detail({}, payload) == expected);
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-failure-text"};
    const auto path = temporary.path() / "trace.jsonl";
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(descriptor >= 0);
    services::DiagnosticsClient diagnostics{mmltk::common::io::ScopedFd{descriptor}, services::DiagnosticsExecutionPolicy::CallerDriven};
    services::RuntimeDiagnostics runtime{diagnostics.producer()};
    auto target = runtime.target();
    report_visual_worker_failure(visual_diagnostic_sink(target), contracts::DiagnosticOwner::Explore, 0, detail, 17U);
    CHECK(diagnostics.counters().accepted == 1U);
    CHECK(diagnostics.counters().dropped == 0U);
    diagnostics.close();
    CHECK(diagnostics.counters().flushed == 1U);
    std::ifstream input{path};
    const auto record = nlohmann::json::parse(input);
    CHECK(record["event"] == "worker.failure");
    CHECK(record["sequence"] == 17U);
    CHECK(record["message"] == expected);
}
TEST_CASE("Visual diagnostic boundaries capture immutable observations without owning products") {
    struct Capture final {
        std::array<VisualDiagnosticFact, 3U> facts{};
        std::size_t count = 0U;
        bool enabled = true;
    } capture;
    const VisualDiagnosticSink sink{
        .context = &capture,
        .write =
            [](void* context, VisualDiagnosticFact fact) noexcept {
                auto& output = *static_cast<Capture*>(context);
                if (output.count < output.facts.size()) output.facts[output.count++] = fact;
            },
        .enabled = [](void* context) noexcept { return static_cast<Capture*>(context)->enabled; },
    };
    VisualSourceObservation source{
        .frame = {.source = {PresentationSourceKind::Explore, 1U}, .extent = {16U, 8U}, .revision = 12U, .clean_revision = 7U},
        .snapshot_revision = 30U,
    };
    {
        services::RuntimeDiagnosticSpan span{sink, [&] {
                                                 return visual_diagnostic_boundary(
                                                     {.system = contracts::DiagnosticOwner::Presentation,
                                                      .operation = VisualDiagnosticOperation::PresentationPumpStarted,
                                                      .context = {.selection_generation = 20U, .source = visual_diagnostic_source(source)}},
                                                     VisualDiagnosticOperation::PresentationPumpCompleted);
                                             }};
        source.frame.revision = 8U;  // Selecting an older completed product is a new observation.
        ++source.snapshot_revision;
        span.Finish();
    }
    REQUIRE(capture.count == 2U);
    CHECK(capture.facts[1U].context.source.source_revision == 12U);
    CHECK(capture.facts[1U].context.source.clean_revision == 7U);
    CHECK(capture.facts[1U].context.source.source_observation_revision == 30U);
    CHECK(capture.facts[1U].context.source.source_width == 16U);
    CHECK(capture.facts[1U].context.span.span_outcome == contracts::DiagnosticSpanOutcome::Success);
    const auto projected = visual_runtime_diagnostic(capture.facts[1U]);
    CHECK(projected.owner == contracts::DiagnosticOwner::Presentation);
    CHECK(projected.event == "presentation.pump.completed");
    CHECK(projected.context.selection_generation == 20U);
    const auto completion = capture.facts[1U];
    source = {};  // Producer reconstruction cannot rewrite a captured completion.
    std::async(std::launch::async, [sink, completion] { sink(completion); }).get();
    REQUIRE(capture.count == 3U);
    CHECK(capture.facts[2U].context.source.source_observation_revision == 30U);
    CHECK(capture.facts[2U].context.source.source_revision == 12U);
    capture.enabled = false;
    bool collected = false;
    sink.Emit([&] {
        collected = true;
        return VisualDiagnosticFact{};
    });
    CHECK_FALSE(collected);
    CHECK_FALSE(sink.pixel_probes_enabled());
}
TEST_CASE("recoverable visual failure restores creator policy before rebuilding placed runtime") {
    using namespace mmltk::common::system;
    const auto topology = NumaTopology::Capture();
    const auto baseline = allowed_cpu_set();
    const auto first = std::ranges::find(topology.cpus, baseline.front(), &CpuTopology::cpu);
    const mmltk::frameworks::gpu::DeviceExecution execution{.device = 0, .placement = resolve_placement(topology, first->node)};
    auto backend = std::make_shared<FakeImageBackend>();
    std::vector<std::vector<int>> construction_affinities;
    std::promise<void> failed;
    std::promise<std::vector<int>> completed;
    detail::VisualRuntimeOwner owner{[&](auto revisions) {
                                         construction_affinities.push_back(allowed_cpu_set());
                                         return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                                             .device = 0, .backend = backend, .execution = execution, .product_revisions = std::move(revisions)});
                                     },
                                     [&](std::exception_ptr) { failed.set_value(); }};
    REQUIRE(owner.SubmitOrdered(
        [](auto&, std::stop_token) -> detail::VisualRuntimeOwner::Notification { throw std::runtime_error("recoverable operation failure"); }));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(failed, "failed", 2s));
    REQUIRE(owner.SubmitOrdered([&](auto&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
        completed.set_value(allowed_cpu_set());
        return {};
    }));
    const auto active = completed.get_future().get();
    owner.StopAndWait();
    CHECK(active == std::vector<int>{execution.placement.cpus.front()});
    CHECK(construction_affinities == std::vector<std::vector<int>>{baseline, baseline});
    CHECK(backend->contexts_created == 2U);
    CHECK(backend->contexts_destroyed == 2U);
}
TEST_CASE("Shared visual GPU completion preserves ordered work and independent producers", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    const auto factory = RuntimeFactory(0, backend);
    std::promise<void> failure, submitted, completed, following, independent, second_submitted;
    std::uintptr_t first_stream = 0U, second_stream = 0U;
    std::atomic<unsigned> order{0U};
    detail::VisualRuntimeOwner first(factory, [&](auto) { failure.set_value(); });
    detail::VisualRuntimeOwner second(factory, [&](auto) { failure.set_value(); });
    const bool submitted_first = first.SubmitOrdered([&](auto& runtime, std::stop_token) {
        auto candidate = enqueue_test_output(runtime, first_stream);
        first.DeferCompletion(runtime, [&, candidate = std::move(candidate)]() mutable {
            runtime.CommitOutput(std::move(candidate));
            CHECK(order.fetch_add(1U) == 0U);
            completed.set_value();
        });
        return detail::VisualRuntimeOwner::Notification{[&] { submitted.set_value(); }};
    });
    REQUIRE(submitted_first);
    mmltk::testsupport::await_test_promise(submitted, "asynchronous raster submission");
    const bool submitted_following = first.SubmitOrdered([&](auto&, std::stop_token) {
        CHECK(order.fetch_add(1U) == 1U);
        following.set_value();
        return detail::VisualRuntimeOwner::Notification{};
    });
    REQUIRE(submitted_following);
    REQUIRE(second.SubmitOrdered([&](auto& runtime, std::stop_token) {
        auto candidate = enqueue_test_output(runtime, second_stream);
        second.DeferCompletion(runtime, [&, candidate = std::move(candidate)]() mutable {
            runtime.CommitOutput(std::move(candidate));
            independent.set_value();
        });
        return detail::VisualRuntimeOwner::Notification{[&] { second_submitted.set_value(); }};
    }));
    mmltk::testsupport::await_test_promise(second_submitted, "second asynchronous producer submission");
    auto settlement = backend->HoldStreamSettlements("first producer physical settlement", first_stream);
    backend->CompleteNotifications(first_stream);
    REQUIRE(settlement->WaitEntered(2s));
    backend->CompleteNotifications(second_stream);
    mmltk::testsupport::await_test_promise(independent, "independent producer progress");
    CHECK(order.load() == 0U);
    settlement->Release();
    mmltk::testsupport::await_test_promise(completed, "settled raster completion");
    mmltk::testsupport::await_test_promise(following, "ordered work after GPU completion");
    first.StopAndWait();
    second.StopAndWait();
    CHECK(order.load() == 2U);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Deferred product completion reports terminal failure without additional application traffic", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    const bool registration_failure = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    const auto expected = std::make_exception_ptr(std::runtime_error("deferred product terminal failure"));
    std::promise<std::exception_ptr> failed;
    std::promise<void> submitted;
    std::atomic_uint failures{0U}, completions{0U};
    detail::VisualRuntimeOwner owner(RuntimeFactory(0, backend), first_visual_failure(failures, failed));
    auto admitted = backend->ObserveNextNotificationStream();
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        std::uintptr_t stream = 0U;
        auto candidate = enqueue_test_output(runtime, stream);
        if (registration_failure)
            backend->FailAfter(FakeImageBackend::FailurePoint::NotifyStream, 0U, expected);
        else
            backend->SetStreamSettlement(stream, {.completion_reached = true, .failure = expected});
        owner.DeferCompletion(runtime, [&, candidate = std::move(candidate)]() mutable {
            runtime.CommitOutput(std::move(candidate));
            ++completions;
        });
        return detail::VisualRuntimeOwner::Notification{[&] { submitted.set_value(); }};
    }));
    if (!registration_failure) {
        const auto stream = mmltk::testsupport::await_test_future(admitted, "deferred terminal notification admitted");
        mmltk::testsupport::await_test_promise(submitted, "deferred product submitted");
        backend->CompleteNotifications(stream, CUDA_ERROR_LAUNCH_FAILED);
    }
    const auto failure = mmltk::testsupport::await_test_promise(failed, "autonomous deferred product failure");
    CHECK(gpu::test_support::ContainsImageFailure(failure, expected));
    CHECK(completions == 0U);
    owner.StopAndWait();
    CHECK(failures == 1U);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Stopping a deferred product settles pending notification before candidate destruction", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    std::atomic_uint completions{0U}, failures{0U};
    detail::VisualRuntimeOwner owner(RuntimeFactory(0, backend), [&](auto) { ++failures; });
    auto admitted = backend->ObserveNextNotification();
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        auto candidate = runtime.AcquireOutput();
        runtime.PublishRetained(candidate, 4U, 3U, [](auto, auto, auto) {}, gpu::ImageSubmission::Enqueue);
        owner.DeferCompletion(runtime, [candidate = std::move(candidate), &completions] { ++completions; });
        return detail::VisualRuntimeOwner::Notification{};
    }));
    mmltk::testsupport::await_test_future(admitted, "pending notification before stop");
    auto settlement = backend->HoldStreamSettlements("shutdown callback physical settlement");
    std::future<void> stopped = std::async(std::launch::async, [&] { owner.StopAndWait(); });
    auto cleanup = mmltk::testsupport::ScopedTestCleanup{[&] {
        settlement->Release();
        if (stopped.valid()) stopped.wait();
    }};
    REQUIRE(settlement->WaitEntered(2s));
    CHECK(backend->planes_freed == 0U);
    CHECK(stopped.wait_for(0s) == std::future_status::timeout);
    settlement->Release();
    mmltk::testsupport::await_test_future(stopped, "deferred product shutdown");
    CHECK(completions == 0U);
    CHECK(failures == 0U);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Terminal product notification reports unproved settlement with physical custody retained", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    const auto expected = std::make_exception_ptr(std::runtime_error("producer context unavailable for settlement"));
    std::promise<std::exception_ptr> failed;
    std::atomic_uint failures{0U}, completions{0U};
    {
        detail::VisualRuntimeOwner owner(RuntimeFactory(0, backend), first_visual_failure(failures, failed));
        auto admitted = backend->ObserveNextNotificationStream();
        REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
            std::uintptr_t stream = 0U;
            auto candidate = enqueue_test_output(runtime, stream);
            backend->SetStreamSettlement(stream, {.failure = expected});
            owner.DeferCompletion(runtime, [candidate = std::move(candidate), &completions] { ++completions; });
            return detail::VisualRuntimeOwner::Notification{};
        }));
        const auto stream = mmltk::testsupport::await_test_future(admitted, "unproved product notification admitted");
        backend->CompleteNotifications(stream, CUDA_ERROR_INVALID_CONTEXT);
        const auto failure = mmltk::testsupport::await_test_promise(failed, "unproved product terminal failure");
        CHECK(gpu::test_support::ContainsImageFailure(failure, expected));
        owner.StopAndWait();
        CHECK(completions == 0U);
    }
    CHECK(failures == 1U);
    CHECK(backend->planes_allocated > backend->planes_freed);
    CHECK(backend->contexts_created > backend->contexts_destroyed);
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("Display terminal failure wakes request readiness and preserves the last completed display", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    fixture::ImageWorkspaceTestAccess::Reset();
    const bool registration_failure = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    const auto expected = std::make_exception_ptr(std::runtime_error("display terminal failure"));
    std::promise<std::exception_ptr> failed;
    std::promise<void> published;
    std::atomic_uint failures{0U};
    detail::VisualRuntimeOwner owner(RuntimeFactory(0, backend, gpu::ImageProductLayout::Clean, {}, 1U, fixture::FakeWorkspaceFinalizer(backend)),
                                     first_visual_failure(failures, failed));
    publish_visual_pixels(owner, published);
    auto make_workspace = [&] {
        auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
            backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
        return workspace;
    };
    ProducerWorkspaceRequest previous{.workspace = make_workspace()};
    previous.Request(owner);
    previous.CheckCompleted(owner);
    const auto previous_pixel = *reinterpret_cast<const std::byte*>(previous.workspace->plane(4U, 3U).data);
    ProducerWorkspaceRequest candidate{.workspace = make_workspace()};
    backend->defer_notifications = true;
    auto admitted = backend->ObserveNextNotificationStream();
    if (registration_failure) backend->FailAfter(FakeImageBackend::FailurePoint::NotifyStream, 0U, expected);
    candidate.Request(owner);
    if (!registration_failure) {
        const auto stream = mmltk::testsupport::await_test_future(admitted, "failing display notification admitted");
        backend->SetStreamSettlement(stream, {.completion_reached = true, .failure = expected});
        backend->CompleteNotifications(stream, CUDA_ERROR_LAUNCH_FAILED);
    }
    const auto failure = mmltk::testsupport::await_test_promise(failed, "display terminal failure delivery");
    mmltk::testsupport::await_test_future(candidate.ready, "failed display request readiness");
    CHECK(fixture::ContainsImageFailure(failure, expected));
    CHECK_FALSE(candidate.workspace->Contains(candidate.content));
    CHECK(previous.workspace->Contains(previous.content));
    CHECK(*reinterpret_cast<const std::byte*>(previous.workspace->plane(4U, 3U).data) == previous_pixel);
    previous.workspace.reset();
    candidate.workspace.reset();
    owner.StopAndWait();
    CHECK(failures == 1U);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Background visual work yields until requested display pixels physically complete", "[presentation][workspace][background]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    fixture::ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    ProducerWorkspaceRequest display{.workspace = fixture::ImageWorkspaceTestAccess::CreateAdmitted(backend, fixture::ImageWorkspaceTestAccess::Layout(0))};
    std::promise<void> published, entered, preempted, release, resumed;
    auto released = release.get_future().share();
    std::atomic_uint runs{0U};
    std::atomic_bool resumed_with_pixels{false};
    detail::VisualRuntimeOwner owner(RuntimeFactory(0, backend, gpu::ImageProductLayout::Clean, {}, 1U, fixture::FakeWorkspaceFinalizer(backend)),
                                     [](auto) { FAIL("background display fixture unexpectedly failed"); });
    auto settle = settle_visual_on_exit(owner, release);
    owner.RegisterContinuation(
        [&](auto&, std::stop_token stop) {
            if (runs.fetch_add(1U) == 0U) {
                std::stop_callback observe_stop{stop, [&] { preempted.set_value(); }};
                entered.set_value();
                released.wait();
                static_cast<void>(owner.NotifyContinuation());
            } else {
                resumed_with_pixels = display.workspace->Contains(display.content);
                resumed.set_value();
            }
            return detail::VisualRuntimeOwner::Notification{};
        },
        {}, false, detail::VisualRuntimeOwner::ContinuationCancellation::YieldToWorkspace);
    publish_visual_pixels(owner, published);
    REQUIRE(owner.NotifyContinuation());
    mmltk::testsupport::await_test_promise(entered, "background preparation entered");
    backend->defer_notifications = true;
    auto admitted = backend->ObserveNextNotificationStream();
    display.Request(owner);
    mmltk::testsupport::await_test_promise(preempted, "background preparation preempted by display");
    release.set_value();
    const auto stream = mmltk::testsupport::await_test_future(admitted, "display physical completion registered");
    CHECK(runs == 1U);
    CHECK_FALSE(display.workspace->Contains(display.content));
    backend->CompleteNotifications(stream);
    display.CheckCompleted(owner);
    mmltk::testsupport::await_test_promise(resumed, "background preparation resumed");
    CHECK(runs == 2U);
    CHECK(resumed_with_pixels);
    display.workspace.reset();
    owner.StopAndWait();
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
}  // namespace
}  // namespace mmltk::controller
