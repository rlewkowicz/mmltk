#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <optional>
#include <limits>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>
#include "src/frameworks/gpu/tests/fake_image_backend.h"
namespace mmltk::frameworks::gpu {
namespace {
using test_support::FakeImageBackend;
using test_support::RuntimeFactory;
using test_support::make_clean_semantic_runtime;
TEST_CASE("Display context rebinding shares same-device custody and cleans partial event construction", "[gpu][workspace]") {
    auto backend = std::make_shared<FakeImageBackend>();
    {
        DeviceContext producer(0, backend);
        auto same = producer.OnDevice(0);
        CHECK(backend->contexts_created == 1U);
        auto display = producer.OnDevice(1);
        CHECK(backend->contexts_created == 2U);
        backend->FailAfter(FakeImageBackend::FailurePoint::CreateEvent);
        CHECK_THROWS(display.CreateEvent());
        CHECK(backend->events_created == 0U);
        const auto event = display.CreateEvent();
        ImageStream stream(display);
        stream.Record(event);
        stream.AwaitEvent(event);
        stream.Synchronize();
        display.DestroyEvent(event);
        CHECK(backend->events_destroyed == 1U);
    }
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(backend->streams_destroyed == 1U);
}
TEST_CASE("Display import failure retains independent backing and reports cleanup failure", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    using test_support::ImportedImageBufferTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend});
    const auto initiating = std::make_exception_ptr(std::invalid_argument("rejected workspace initialization"));
    ImageWorkspaceTestAccess::initialize_failure = initiating;
    ImportedImageBufferTestAccess::unmap_result = CUDA_ERROR_UNKNOWN;
    auto workspace = ImageWorkspaceTestAccess::Create(backend, ImageWorkspaceTestAccess::Layout());
    std::exception_ptr failure;
    try {
        workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
    } catch (...) { failure = std::current_exception(); }
    REQUIRE(failure);
    CHECK(test_support::ContainsImageFailure(failure, initiating));
    CHECK(is_image_execution_failure(failure));
    CHECK(workspace->retired());
    CHECK_THROWS(workspace->Admit(workspace->identity(), workspace->layout().device_incarnation));
    workspace.reset();
    CHECK(ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(runtime.Retire().safe_to_destroy);
    ImageWorkspaceTestAccess::Reset();
}
TEST_CASE("Partial display stream construction retains its context and both failures", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend});
    const auto initiating = std::make_exception_ptr(std::runtime_error("display stream creation failed"));
    const auto cleanup = std::make_exception_ptr(std::runtime_error("display context unavailable during cleanup"));
    backend->FailAfter(FakeImageBackend::FailurePoint::CreateStream, 0U, initiating);
    backend->FailDeviceBinding(1, cleanup);
    std::exception_ptr failure;
    try {
        static_cast<void>(ImageWorkspaceTestAccess::Create(backend, ImageWorkspaceTestAccess::Layout()));
    } catch (...) { failure = std::current_exception(); }
    CHECK(test_support::ContainsImageFailure(failure, initiating));
    CHECK(test_support::ContainsImageFailure(failure, cleanup));
    CHECK(backend->contexts_created == 2U);
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(ImageWorkspaceTestAccess::initialized == 0U);
    runtime.BindContext();
    CHECK(runtime.Retire().safe_to_destroy);
}
TEST_CASE("Safe workspace rejection releases candidates and permits a fresh admission", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend});
    auto layout = ImageWorkspaceTestAccess::Layout();
    layout.pitch_bytes = 1U;
    CHECK_THROWS_AS(test_support::ImageWorkspaceTestAccess::Create(backend, layout), std::invalid_argument);
    CHECK(backend->contexts_created == 2U);
    ImageWorkspaceTestAccess::initialize_failure = std::make_exception_ptr(std::invalid_argument("rejected UUID"));
    {
        auto rejected = test_support::ImageWorkspaceTestAccess::Create(backend, ImageWorkspaceTestAccess::Layout());
        CHECK_THROWS_AS(rejected->Admit(rejected->identity(), rejected->layout().device_incarnation), std::invalid_argument);
    }
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(backend->streams_destroyed == 1U);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 1U);
    ImageWorkspaceTestAccess::initialize_failure = {};
    auto workspace = test_support::ImageWorkspaceTestAccess::Create(backend, ImageWorkspaceTestAccess::Layout());
    CHECK_THROWS_AS(workspace->Admit(workspace->identity(), 99U), std::invalid_argument);
    workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
    workspace.reset();
    auto retry = test_support::ImageWorkspaceTestAccess::Create(backend, ImageWorkspaceTestAccess::Layout());
    mmltk::common::io::ScopedFd queued(::memfd_create("pending-backing", MFD_CLOEXEC));
    const int descriptor = queued.get();
    REQUIRE(descriptor >= 0);
    REQUIRE(retry->QueueAllocation(std::move(queued)));
    CHECK_FALSE(retry->WriteAvailable());
    CHECK_FALSE(retry->ReserveWrite());
    CHECK(retry->StorageFootprint().device_bytes == 0U);
    retry->Withdraw();
    CHECK(::fcntl(descriptor, F_GETFD) == -1);
    CHECK(errno == EBADF);
    retry.reset();
    CHECK(runtime.Retire().safe_to_destroy);
}
struct WorkspaceRuntimeFixture final {
    std::shared_ptr<FakeImageBackend> backend = std::make_shared<FakeImageBackend>();
    DeviceContext display{0, backend};
    SystemImageRuntime runtime{mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend)};
    WorkspaceRuntimeFixture() { test_support::ImageWorkspaceTestAccess::Reset(); }
    auto PublishWorkspace(const int device) {
        auto workspace = test_support::ImageWorkspaceTestAccess::Create(display.OnDevice(device), test_support::ImageWorkspaceTestAccess::Layout(device));
        workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
        runtime.Publish(4U, 3U, [](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), 37, clean.descriptor.pitch_bytes * clean.descriptor.height);
        });
        REQUIRE(runtime.PrepareDisplay(runtime.Completed().revision(), workspace));
        // The next clean publication exercises the production direct-alias path.
        runtime.Publish(4U, 3U, [](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), 37, clean.descriptor.pitch_bytes * clean.descriptor.height);
        });
        return std::pair{std::move(workspace), runtime.Completed()};
    }
};
class WorkspaceAccessPeer final {
   public:
    explicit WorkspaceAccessPeer(std::shared_ptr<ImageWorkspace> workspace) : workspace_(std::move(workspace)) {
        auto descriptor = workspace_->ExportAccessDescriptor();
        void* mapping = ::mmap(nullptr, sizeof(ImageWorkspaceAccessSignal), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor.get(), 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("test workspace access mapping failed");
        signal_ = static_cast<ImageWorkspaceAccessSignal*>(mapping);
    }
    ~WorkspaceAccessPeer() {
        Release();
        ::munmap(signal_, sizeof(ImageWorkspaceAccessSignal));
    }
    std::uint64_t Access() const { return std::atomic_ref{signal_->access}.load(std::memory_order_acquire); }
    bool Acquire(std::uint64_t access, std::uint64_t generation) {
        if ((access & kWorkspaceAccessMask) != kWorkspaceAccessAvailable || (access & kWorkspaceAccessRevoked) != 0U ||
            std::atomic_ref{signal_->generation}.load(std::memory_order_relaxed) != generation)
            return false;
        return std::atomic_ref{signal_->access}.compare_exchange_strong(access, (access & ~kWorkspaceAccessMask) | kWorkspaceAccessReading,
                                                                        std::memory_order_acq_rel);
    }
    void Release() {
        const auto generation = std::atomic_ref{signal_->generation}.load(std::memory_order_acquire);
        if (workspace_->Acquired(generation)) workspace_->CompleteRead(generation);
    }
    void Abandon() {
        auto access = std::atomic_ref{signal_->access};
        auto expected = access.load(std::memory_order_acquire);
        do {
            const auto role = (expected & kWorkspaceAccessRevoked) != 0U ? kWorkspaceAccessEmpty : kWorkspaceAccessAvailable;
            if (access.compare_exchange_weak(expected, (expected & ~kWorkspaceAccessMask) | role, std::memory_order_acq_rel)) return;
        } while (true);
    }
    void TerminalComplete(std::uint64_t reading) {
        const auto revoked = reading | kWorkspaceAccessRevoked;
        auto expected = reading;
        if (std::atomic_ref{signal_->access}.compare_exchange_strong(expected, revoked, std::memory_order_acq_rel) || expected == revoked)
            std::atomic_ref{signal_->terminal_read_complete}.store(revoked, std::memory_order_release);
    }
    void StaleTerminalReceipt(std::uint64_t receipt) { std::atomic_ref{signal_->terminal_read_complete}.store(receipt, std::memory_order_release); }

   private:
    std::shared_ptr<ImageWorkspace> workspace_;
    ImageWorkspaceAccessSignal* signal_ = nullptr;
};
TEST_CASE("Shared display custody detaches raw aliases and never overwrites an unread offer", "[gpu][workspace][display]") {
    WorkspaceRuntimeFixture fixture;
    auto [workspace, product] = fixture.PublishWorkspace(0);
    const auto offered_revision = workspace->revision();
    const auto offered_plane = product.Borrow().plane(0U).plane().data;
    REQUIRE(workspace->ReserveDisplayWrite());
    workspace->CancelWrite();  // Publish the offer while retaining graphics ownership.
    CHECK_FALSE(workspace->ReserveWrite());
    CHECK_FALSE(workspace->ReserveDisplayWrite());
    WorkspaceAccessPeer peer(workspace);
    const auto publish_next = [&] {
        auto candidate = fixture.runtime.TryAcquireOutput(product);
        REQUIRE(candidate.valid());
        fixture.runtime.Publish(candidate, 4U, 3U, [](auto, auto, auto) {});
        product = fixture.runtime.CommitOutput(std::move(candidate));
    };
    publish_next();
    CHECK(product.Borrow().plane(0U).plane().data != offered_plane);
    CHECK(workspace->revision() == offered_revision);
    CHECK(workspace->product_owner() == 0U);
    REQUIRE(peer.Acquire(peer.Access(), offered_revision));
    CHECK_FALSE(workspace->ReserveWrite());
    peer.Release();
    REQUIRE(workspace->ReserveDisplayWrite());
    REQUIRE(fixture.runtime.PrepareDisplay(product.revision(), workspace));
    workspace->CancelWrite();
    REQUIRE(peer.Acquire(peer.Access(), product.revision()));
    peer.Release();
    const auto copies = fixture.backend->same_copies.load();
    publish_next();
    CHECK(product.Borrow().plane(0U).plane().data == offered_plane);
    // A first reattachment may preserve a private baseline. Subsequent clean
    // production writes directly into the admitted final allocation.
    CHECK(fixture.backend->same_copies.load() <= copies + 1U);
    const auto direct_copies = fixture.backend->same_copies.load();
    publish_next();
    CHECK(fixture.backend->same_copies.load() == direct_copies);
    product = {};
}
TEST_CASE("Two completed display offers settle independently without mailbox custody", "[gpu][workspace][display]") {
    WorkspaceRuntimeFixture fixture;
    auto [older, older_product] = fixture.PublishWorkspace(0);
    WorkspaceRuntimeFixture other;
    auto [newer, newer_product] = other.PublishWorkspace(0);
    REQUIRE(older->ReserveDisplayWrite());
    REQUIRE(newer->ReserveDisplayWrite());
    older->CancelWrite();
    newer->CancelWrite();
    WorkspaceAccessPeer old_peer(older), new_peer(newer);
    REQUIRE(new_peer.Acquire(new_peer.Access(), newer_product.revision()));
    REQUIRE(old_peer.Acquire(old_peer.Access(), older_product.revision()));
    // A failed release-only submission returns its acquisition reservation;
    // it does not return native display ownership or overwrite the offer.
    old_peer.Abandon();
    CHECK_FALSE(older->ReserveDisplayWrite());
    REQUIRE(old_peer.Acquire(old_peer.Access(), older_product.revision()));
    CHECK_FALSE(older->WriteAvailable());
    CHECK_FALSE(newer->WriteAvailable());
    const auto copies = fixture.backend->same_copies.load();
    old_peer.Release();  // Actual release-only completion, without a mailbox.
    CHECK(older->WriteAvailable());
    CHECK_FALSE(newer->WriteAvailable());
    REQUIRE(older->ReserveDisplayWrite());
    older->CancelDisplayWrite();
    new_peer.Release();  // The newer image's independent last-reader completion.
    CHECK(newer->WriteAvailable());
    REQUIRE(newer->ReserveDisplayWrite());
    newer->CancelDisplayWrite();
    CHECK(fixture.backend->same_copies.load() == copies);
    CHECK(older_product.revision() == older->revision());
    CHECK(newer_product.revision() == newer->revision());
}
TEST_CASE("A submitted display read survives terminal loss until exact completion", "[gpu][workspace][display]") {
    WorkspaceRuntimeFixture fixture;
    auto [workspace, product] = fixture.PublishWorkspace(0);
    REQUIRE(workspace->ReserveDisplayWrite());
    workspace->CancelWrite();
    WorkspaceAccessPeer peer(workspace);
    REQUIRE(peer.Acquire(peer.Access(), product.revision()));
    const auto reading = peer.Access();
    workspace->Withdraw();
    CHECK_FALSE(workspace->TerminalReadComplete(product.revision()));
    CHECK_FALSE(workspace->ReserveDisplayWrite());
    peer.TerminalComplete(reading);
    REQUIRE(workspace->TerminalReadComplete(product.revision()));
    workspace->CompleteRead(product.revision());
    CHECK_FALSE(workspace->Acquired(product.revision()));
}
TEST_CASE("Display producer detachment waits for actual raw readers and preserves retained products", "[gpu][workspace][display]") {
    WorkspaceRuntimeFixture fixture;
    auto [workspace, product] = fixture.PublishWorkspace(0);
    const auto revision = product.revision();
    auto raw = product.Borrow();
    REQUIRE(workspace->ReserveDisplayWrite());
    const auto allocation = raw.plane(0U).plane().data;
    CHECK_FALSE(fixture.runtime.DetachDisplay(workspace));
    raw = {};
    REQUIRE(fixture.runtime.DetachDisplay(workspace));
    CHECK(workspace->product_owner() == 0U);
    CHECK(product.revision() == revision);
    CHECK(product.Borrow().plane(0U).plane().data != allocation);
    workspace->CancelDisplayWrite();
    product = {};
}
TEST_CASE("Exact external acquisition races replacement without reusing a held fallback", "[gpu][workspace][acquisition]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto config = test_support::WorkspaceRuntimeConfig(backend);
    config.output_buffer_count = 2U;
    SystemImageRuntime runtime(std::move(config));
    const auto workspace = [&] {
        auto result = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
            backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
        return result;
    };
    const auto fill = [](std::uint8_t value) {
        return [value](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), value, clean.descriptor.pitch_bytes * clean.descriptor.height);
        };
    };
    auto current = workspace();
    auto overflow = workspace();
    WorkspaceAccessPeer current_peer(current);
    WorkspaceAccessPeer overflow_peer(overflow);
    auto candidate = runtime.AcquireOutput();
    runtime.Publish(candidate, 4U, 3U, fill(11U));
    auto baseline = runtime.CommitOutput(std::move(candidate));
    REQUIRE(runtime.PrepareDisplay(baseline.revision(), current));
    const auto held_revision = current->revision();
    auto current_read = baseline.BorrowWorkspace();
    REQUIRE(current_read.valid());
    const auto current_pointer = current_read.plane().data;
    current_read = {};
    REQUIRE(current_peer.Acquire(current_peer.Access(), held_revision));
    CHECK_FALSE(current->ReserveWrite());
    candidate = runtime.TryAcquireOutput(baseline);
    REQUIRE(candidate.valid());
    runtime.Publish(candidate, 4U, 3U, fill(22U));
    baseline = runtime.CommitOutput(std::move(candidate));
    REQUIRE(runtime.PrepareDisplay(baseline.revision(), overflow));
    const auto old_access = overflow_peer.Access();
    const auto old_revision = overflow->revision();
    const auto copies = backend->same_copies.load();
    REQUIRE(overflow->ReserveDisplayWrite());
    overflow->InvalidateWrite();
    candidate = runtime.TryAcquireOutput(baseline);
    REQUIRE(candidate.valid());
    CHECK_FALSE(overflow_peer.Acquire(old_access, old_revision));
    runtime.PublishRetained(candidate, 4U, 3U, fill(33U));
    baseline = runtime.CommitOutput(std::move(candidate));
    REQUIRE(runtime.PrepareDisplay(baseline.revision(), overflow));
    overflow->CancelDisplayWrite();
    CHECK(backend->same_copies == copies);
    CHECK_FALSE(overflow_peer.Acquire(old_access, old_revision));
    CHECK(current->revision() == held_revision);
    CHECK(*reinterpret_cast<const std::uint8_t*>(current_pointer) == 11U);
    REQUIRE(overflow_peer.Acquire(overflow_peer.Access(), overflow->revision()));
    auto raw_progress = runtime.TryAcquireOutput(baseline);
    REQUIRE(raw_progress.valid());
    raw_progress = {};
    CHECK_FALSE(overflow->ReserveWrite());
    current_peer.Release();
    candidate = runtime.TryAcquireOutput(baseline);
    REQUIRE(candidate.valid());
    candidate = {};
    overflow_peer.Release();
    // Canceling an untouched offer still advances admission's epoch: a stale
    // reader cannot win an ABA even when the physical pixel generation repeats.
    const auto prior = overflow_peer.Access();
    REQUIRE(overflow->ReserveWrite());
    overflow->CancelWrite();
    CHECK_FALSE(overflow_peer.Acquire(prior, overflow->revision()));
    CHECK(overflow_peer.Acquire(overflow_peer.Access(), overflow->revision()));
    // Withdrawal and acquisition share the same gate in either order. An
    // acquired reader settles normally; an unacquired offer cannot be acquired
    // after the native receiver has begun retiring its timeline and memory.
    const auto current_offer = current_peer.Access();
    current->Withdraw();
    CHECK_FALSE(current_peer.Acquire(current_offer, current->revision()));
    CHECK(current->WriteAvailable());
    REQUIRE(current->ReserveWrite());
    current->CancelWrite();
    CHECK_FALSE(current_peer.Acquire(current_peer.Access(), current->revision()));
    overflow->Withdraw();
    CHECK(overflow->Acquired(overflow->revision()));
    SECTION("Submitted read settles positively") { overflow_peer.Release(); }
    SECTION("Failed submission abandons without reopening withdrawn acquisition") { overflow_peer.Abandon(); }
    CHECK_FALSE(overflow->Acquired(overflow->revision()));
    CHECK_FALSE(overflow_peer.Acquire(overflow_peer.Access(), overflow->revision()));
    REQUIRE(overflow->ReserveWrite());
    overflow->CancelWrite();
    CHECK_FALSE(overflow_peer.Acquire(overflow_peer.Access(), overflow->revision()));
}
TEST_CASE("Workspace withdrawal linearizes against an external thread holding an observed offer", "[gpu][workspace][acquisition]") {
    for (const bool withdrawal_first : {false, true}) {
        CAPTURE(withdrawal_first);
        WorkspaceRuntimeFixture fixture;
        auto [workspace, baseline] = fixture.PublishWorkspace(1);
        WorkspaceAccessPeer peer(workspace);
        const auto offer = peer.Access();
        const auto revision = workspace->revision();
        std::barrier boundary{2};
        bool acquired = false;
        std::jthread browser([&] {
            if (!withdrawal_first) acquired = peer.Acquire(offer, revision);
            boundary.arrive_and_wait();
            if (withdrawal_first) acquired = peer.Acquire(offer, revision);
            boundary.arrive_and_wait();
        });
        if (withdrawal_first) workspace->Withdraw();
        boundary.arrive_and_wait();
        if (!withdrawal_first) workspace->Withdraw();
        boundary.arrive_and_wait();
        browser.join();
        CHECK(workspace->retired());
        CHECK(acquired == !withdrawal_first);
        CHECK(workspace->Acquired(revision) == acquired);
        CHECK((peer.Access() & kWorkspaceAccessRevoked) != 0U);
        CHECK_FALSE(peer.Acquire(offer, revision));
        if (acquired) {
            CHECK_FALSE(workspace->ReserveWrite());
            workspace->CompleteRead(revision);
            CHECK_THROWS(workspace->CompleteRead(revision));
        }
        REQUIRE(workspace->ReserveWrite());
        workspace->CancelWrite();
        CHECK_FALSE(peer.Acquire(peer.Access(), revision));
    }
}
TEST_CASE("Lost acquisition notification requires the exact terminal completion receipt before source reuse", "[gpu][workspace][acquisition]") {
    WorkspaceRuntimeFixture fixture;
    auto [workspace, baseline] = fixture.PublishWorkspace(1);
    WorkspaceAccessPeer peer(workspace);
    const auto revision = workspace->revision();
    REQUIRE(peer.Acquire(peer.Access(), revision));
    const auto previous_read = peer.Access();
    peer.Release();
    // A canceled write changes physical admission even if no pixels changed.
    REQUIRE(workspace->ReserveWrite());
    workspace->CancelWrite();
    CHECK(workspace->revision() == revision);
    peer.StaleTerminalReceipt(previous_read | kWorkspaceAccessRevoked);
    std::barrier submitted{2};
    std::barrier finish_gpu{2};
    std::barrier completed{2};
    bool acquired = false;
    std::jthread browser([&] {
        acquired = peer.Acquire(peer.Access(), revision);
        const auto read = peer.Access();
        submitted.arrive_and_wait();
        finish_gpu.arrive_and_wait();
        // The peer publishes this only at the physical GPU owner's completion
        // boundary. The host receives no Acquired socket record in this case.
        if (acquired) peer.TerminalComplete(read);
        completed.arrive_and_wait();
    });
    submitted.arrive_and_wait();
    CHECK(acquired);
    workspace->Withdraw();
    CHECK(workspace->Acquired(revision));
    CHECK_FALSE(workspace->TerminalReadComplete(revision));
    CHECK_FALSE(workspace->ReserveWrite());
    finish_gpu.arrive_and_wait();
    completed.arrive_and_wait();
    browser.join();
    REQUIRE(workspace->TerminalReadComplete(revision));
    CHECK_FALSE(workspace->TerminalReadComplete(revision + 1U));
    CHECK_FALSE(peer.Acquire(peer.Access(), revision));
    workspace->CompleteRead(revision);
    CHECK_FALSE(workspace->TerminalReadComplete(revision));
    CHECK_THROWS(workspace->CompleteRead(revision));
    const auto old_receipt = (peer.Access() & ~kWorkspaceAccessMask) | kWorkspaceAccessReading;
    REQUIRE(workspace->ReserveWrite());
    workspace->CancelWrite();
    peer.StaleTerminalReceipt(old_receipt);
    CHECK_FALSE(workspace->TerminalReadComplete(revision));
    CHECK_FALSE(peer.Acquire(peer.Access(), revision));
}
TEST_CASE("Completed producer alias failure clears the old mapping and permits a fresh context retry", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    const auto finalize = [](auto clean, auto, auto destination, auto, auto) { test_support::CopyImagePlane(destination, clean); };
    auto first = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend, .workspace_finalize = finalize});
    auto second = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend, .workspace_finalize = finalize});
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    first->Publish(
        4U, 3U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 17, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    REQUIRE(first->PrepareDisplay(first->Completed().revision(), workspace));
    const auto failure = std::make_exception_ptr(std::runtime_error("completed prior alias execution"));
    // Workspace settlement visits the display stream, then its distinct
    // producer-context alias stream.
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 1U, failure);
    const auto prior = workspace->Settle();
    REQUIRE(prior.completion_reached);
    CHECK(test_support::ContainsImageFailure(prior.failure, failure));
    REQUIRE(first->DetachDisplay(workspace));
    REQUIRE(first->Retire().safe_to_destroy);
    first.reset();
    second->Publish(
        4U, 3U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 63, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    const auto observed = second->ObserveWorkspace();
    std::exception_ptr rotated;
    try {
        static_cast<void>(second->PrepareDisplay(observed.product_revision, workspace));
    } catch (...) { rotated = std::current_exception(); }
    CHECK(test_support::ContainsImageFailure(rotated, failure));
    CHECK_FALSE(workspace->retired());
    CHECK_FALSE(workspace->Contains({observed.product_owner, observed.product_revision}));
    CHECK(workspace->revision() == 0U);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 1U);
    REQUIRE(second->PrepareDisplay(observed.product_revision, workspace));
    CHECK(workspace->Contains({observed.product_owner, observed.product_revision}));
    CHECK(*reinterpret_cast<const std::byte*>(workspace->plane(4U, 3U).data) == std::byte{63});
    REQUIRE(second->DetachDisplay(workspace));
    const auto retired = second->Retire();
    CHECK(retired.safe_to_destroy);
    CHECK_FALSE(retired.failure);
    CHECK_FALSE(retired.custody.valid());
    second.reset();
    const auto observation = workspace->ObserveRetirement();
    workspace.reset();
    const auto released = observation.TakeResult();
    CHECK(released.complete);
    CHECK(released.settlement.completion_reached);
    CHECK_FALSE(released.settlement.failure);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 3U);
    CHECK(test_support::ImportedImageBufferTestAccess::allocation_releases == 3U);
    CHECK(backend->streams_destroyed == 5U);
    CHECK(backend->events_created == backend->events_destroyed);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Producer mapping replacement reports release failure and retains backing", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    WorkspaceRuntimeFixture first;
    auto [workspace, product] = first.PublishWorkspace(0);
    REQUIRE(first.runtime.DetachDisplay(workspace));
    SystemImageRuntime second({.device = 0, .backend = first.backend, .workspace_finalize = [](auto clean, auto, auto destination, auto, auto) {
                                   test_support::CopyImagePlane(destination, clean);
                               }});
    second.Publish(4U, 3U, [](auto, auto, auto) {});
    const auto unmaps = test_support::ImportedImageBufferTestAccess::unmaps;
    test_support::ImportedImageBufferTestAccess::unmap_result = CUDA_ERROR_UNKNOWN;
    CHECK_THROWS(second.PrepareDisplay(second.Completed().revision(), workspace));
    CHECK(workspace->retired());
    CHECK_FALSE(workspace->Contains({product.Borrow().plane(0U).plane().allocation.owner, product.revision()}));
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == unmaps + 1U);
    const auto retirement = second.Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    CHECK(retirement.custody.valid());
    ImageWorkspaceTestAccess::Reset();
}
TEST_CASE("Delayed attached product release reports physical failure through producer custody", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources{true};
    REQUIRE(resources.prepared);
    auto& backend = resources.backend;
    auto& runtime = resources.runtime;
    auto& workspace = resources.workspace;
    auto product = runtime->Completed();
    workspace.reset();
    auto retirement = runtime->Retire();
    REQUIRE_FALSE(retirement.safe_to_destroy);
    REQUIRE(retirement.custody.deferred());
    CHECK_FALSE(retirement.failure);
    CHECK_FALSE(retirement.custody.FinishRetirement().completion_reached);
    runtime.reset();
    std::atomic<std::size_t> notifications{0U};
    retirement.custody.SetRetirementSink(std::make_shared<const std::function<void()>>([&] { ++notifications; }));
    const auto cleanup = std::make_exception_ptr(std::runtime_error("late display context failure"));
    backend->FailDeviceBinding(1, cleanup);
    product = {};
    CHECK(notifications.load() != 0U);
    const auto settled = retirement.custody.FinishRetirement();
    CHECK_FALSE(settled.completion_reached);
    CHECK(test_support::ContainsImageFailure(settled.failure, cleanup));
    CHECK_FALSE(retirement.custody.deferred());
    CHECK(retirement.custody.valid());
    retirement.custody.SetRetirementSink({});
}
TEST_CASE("Allocation responsibility transfer is durable across detachment in either order", "[gpu][workspace]") {
    const bool transfer_first = GENERATE(false, true);
    const bool fail_release = GENERATE(false, true);
    WorkspaceRuntimeFixture fixture;
    auto [workspace, product] = fixture.PublishWorkspace(1);
    product = {};
    auto observation = workspace->ObserveRetirement();
    if (transfer_first) {
        REQUIRE(observation.TransferToProducer());
        CHECK(observation.TransferToProducer());  // One accepted allocation, not two counts.
    }
    REQUIRE(fixture.runtime.DetachDisplay(workspace));
    CHECK(workspace->product_owner() == 0U);
    CHECK(observation.TransferToProducer() == transfer_first);
    auto retirement = fixture.runtime.Retire();
    CHECK(retirement.safe_to_destroy == !transfer_first);
    CHECK(retirement.custody.deferred() == transfer_first);
    std::atomic<unsigned> notifications{0U};
    if (transfer_first) retirement.custody.SetRetirementSink(std::make_shared<const std::function<void()>>([&] { ++notifications; }));
    const auto failure = std::make_exception_ptr(std::runtime_error("detached allocation cleanup"));
    if (fail_release) fixture.backend->FailDeviceBinding(1, failure);
    workspace.reset();
    const auto result = observation.TakeResult();
    REQUIRE(result.complete);
    CHECK(result.claimed == !transfer_first);
    CHECK(test_support::ContainsImageFailure(result.settlement.failure, failure) == fail_release);
    CHECK_FALSE(observation.TakeResult().claimed);
    if (!fail_release) {
        CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 1U);
        CHECK(test_support::ImportedImageBufferTestAccess::allocation_releases == 1U);
    }
    if (transfer_first) {
        CHECK(notifications.load() == 1U);
        const auto settled = retirement.custody.FinishRetirement();
        CHECK(settled.completion_reached == !fail_release);
        CHECK(test_support::ContainsImageFailure(settled.failure, failure) == fail_release);
        CHECK_FALSE(retirement.custody.deferred());
        retirement.custody.SetRetirementSink({});
    }
}
TEST_CASE("Completed transferred allocation failure permits ordinary producer retirement", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    mmltk::frameworks::gpu::test_support::WorkspaceTestFixture resources{true};
    REQUIRE(resources.prepared);
    auto& backend = resources.backend;
    auto& runtime = resources.runtime;
    auto& workspace = resources.workspace;
    auto observation = workspace->ObserveRetirement();
    REQUIRE(observation.TransferToProducer());
    REQUIRE(runtime->DetachDisplay(workspace));
    const auto failure = std::make_exception_ptr(std::runtime_error("completed display stream failure"));
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, failure);
    workspace.reset();
    const auto released = observation.TakeResult();
    REQUIRE(released.complete);
    CHECK_FALSE(released.claimed);
    REQUIRE(released.settlement.completion_reached);
    CHECK(test_support::ContainsImageFailure(released.settlement.failure, failure));
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(test_support::ImportedImageBufferTestAccess::allocation_releases == 1U);
    // The transferred failure is already in the tracker before runtime
    // retirement. The producer's own stream and physical storage are healthy.
    const auto retirement = runtime->Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK(test_support::ContainsImageFailure(retirement.failure, failure));
    CHECK_FALSE(retirement.custody.valid());
    CHECK_FALSE(retirement.custody.deferred());
    runtime.reset();
    CHECK(backend->planes_freed == backend->planes_allocated);
    CHECK(backend->streams_destroyed == 2U);
    CHECK(backend->events_destroyed == backend->events_created);
    CHECK(backend->contexts_destroyed == backend->contexts_created);
}
TEST_CASE("Replaced display attachment cannot accept responsibility from an already retired producer", "[gpu][workspace]") {
    WorkspaceRuntimeFixture fixture;
    auto [prior, product] = fixture.PublishWorkspace(1);
    product = {};
    auto observation = prior->ObserveRetirement();
    auto next = test_support::ImageWorkspaceTestAccess::Create(fixture.backend, test_support::ImageWorkspaceTestAccess::Layout(1));
    next->Admit(next->identity(), next->layout().device_incarnation);
    REQUIRE(fixture.runtime.PrepareDisplay(fixture.runtime.Completed().revision(), next));
    CHECK(prior->product_owner() == 0U);
    REQUIRE(fixture.runtime.DetachDisplay(next));
    REQUIRE(fixture.runtime.Retire().safe_to_destroy);
    CHECK_FALSE(observation.TransferToProducer());
    prior.reset();
    CHECK(observation.TakeResult().claimed);
}
TEST_CASE("Allocation retirement publishes cleanup before waking and closes late registration", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    const bool register_late = GENERATE(false, true);
    const bool fail_release = GENERATE(false, true);
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout());
    auto observation = workspace->ObserveRetirement();
    const auto cleanup = std::make_exception_ptr(std::runtime_error("allocation release binding"));
    std::size_t notifications = 0U;
    auto wake = std::make_shared<const std::function<void()>>([&] { ++notifications; });
    if (!register_late) observation.SetWake(wake);
    CHECK(notifications == 0U);
    CHECK_FALSE(observation.TakeResult().complete);
    if (fail_release) backend->FailDeviceBinding(1, cleanup);
    workspace.reset();
    if (register_late) {
        CHECK(notifications == 0U);
        observation.SetWake(wake);
    }
    REQUIRE(notifications == 1U);
    const auto released = observation.TakeResult();
    REQUIRE(released.complete);
    CHECK(released.claimed);
    CHECK(released.settlement.completion_reached == !fail_release);
    CHECK(test_support::ContainsImageFailure(released.settlement.failure, cleanup) == fail_release);
    CHECK_FALSE(observation.TakeResult().claimed);
    if (!fail_release) {
        CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 1U);
        CHECK(test_support::ImportedImageBufferTestAccess::allocation_releases == 1U);
    }
    observation.SetWake({});
}
TEST_CASE("Late workspace preparation preserves raw and counted read custody", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    WorkspaceRuntimeFixture fixture;
    auto& runtime = fixture.runtime;
    runtime.Publish(4U, 3U, [](auto clean, auto, auto) { *reinterpret_cast<std::byte*>(clean.data) = std::byte{73}; });
    auto observed = runtime.ObserveWorkspace();
    REQUIRE(observed.product_revision != 0U);
    auto workspace = test_support::ImageWorkspaceTestAccess::Create(fixture.display, ImageWorkspaceTestAccess::Layout(0));
    workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
    auto raw = runtime.Borrow();
    const auto address = raw.plane(0U).plane().data;
    CHECK_FALSE(runtime.PrepareDisplay(observed.product_revision, workspace));
    CHECK_FALSE(runtime.BorrowWorkspace().valid());
    CHECK(raw.plane(0U).plane().data == address);
    raw = {};
    REQUIRE(runtime.PrepareDisplay(observed.product_revision, workspace));
    auto borrowed = runtime.BorrowWorkspace();
    REQUIRE(borrowed.valid());
    CHECK(borrowed.revision() == observed.product_revision);
    auto completion = std::move(borrowed).TakeCompletion();
    CHECK(runtime.PrepareDisplay(observed.product_revision, workspace));
    CHECK_FALSE(runtime.DetachDisplay(workspace));
    completion->Complete();
    completion.reset();
    REQUIRE(runtime.DetachDisplay(workspace));
    REQUIRE(runtime.PrepareDisplay(observed.product_revision, workspace));
    CHECK(runtime.ObserveWorkspace().product_revision == observed.product_revision);
}
TEST_CASE("Workspace observation does not manufacture product availability edges", "[gpu][workspace]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend});
    runtime.Publish(4U, 3U, [](auto, auto, auto) {});
    std::size_t notifications = 0U;
    runtime.SetOutputAvailableSink([&] { ++notifications; });
    const auto before = notifications;
    const auto observed = runtime.ObserveWorkspace();
    REQUIRE(observed.product_owner != 0U);
    for (unsigned index = 0U; index != 8U; ++index) {
        CHECK(runtime.ObserveWorkspace().product_owner == observed.product_owner);
        CHECK(runtime.ObserveWorkspace().product_revision == observed.product_revision);
        CHECK_FALSE(runtime.BorrowWorkspace().valid());
    }
    CHECK(notifications == before);
    runtime.SetOutputAvailableSink({});
}
TEST_CASE("Healthy external workspace products retain exact raw aliases through deferred retirement", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(
        SystemImageRuntimeConfig{.device = 0, .backend = backend, .workspace_finalize = [](auto, auto, auto, auto, auto) {}});
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    auto candidate = runtime->AcquireOutput();
    runtime->Publish(candidate, 4U, 3U, [](auto clean, auto, auto) { *reinterpret_cast<std::byte*>(clean.data) = std::byte{73}; });
    auto initial = runtime->CommitOutput(std::move(candidate));
    REQUIRE(runtime->PrepareDisplay(initial.revision(), workspace));
    initial = {};
    runtime->Publish(4U, 3U, [](auto clean, auto, auto) { *reinterpret_cast<std::byte*>(clean.data) = std::byte{73}; });
    auto product = runtime->Completed();
    const auto revision = product.revision();
    const auto address = product.BorrowWorkspace().plane().data;
    workspace.reset();
    auto retirement = runtime->Retire();
    REQUIRE(retirement.custody.deferred());
    CHECK_FALSE(retirement.failure);
    runtime.reset();
    CHECK(product.revision() == revision);
    CHECK(product.Borrow().plane(0U).plane().data == address);
    CHECK(product.BorrowWorkspace().plane().data == address);
    CHECK(*reinterpret_cast<const std::byte*>(address) == std::byte{73});
    product = {};
    const auto settled = retirement.custody.FinishRetirement();
    CHECK(settled.completion_reached);
    CHECK_FALSE(settled.failure);
    CHECK_FALSE(retirement.custody.valid());
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 2U);
}
TEST_CASE("Workspace counted completion wakes retirement without destroying CUDA in the callback", "[gpu][workspace]") {
    WorkspaceRuntimeFixture fixture;
    auto& runtime = fixture.runtime;
    auto [workspace, product] = fixture.PublishWorkspace(0);
    auto completion = product.BorrowWorkspace().TakeCompletion();
    workspace.reset();
    product = {};
    auto retirement = runtime.Retire();
    REQUIRE(retirement.custody.deferred());
    CHECK_FALSE(retirement.failure);
    std::atomic<std::size_t> notifications{0U};
    retirement.custody.SetRetirementSink(std::make_shared<const std::function<void()>>([&] { ++notifications; }));
    const auto before = notifications.load();
    completion->Complete();
    CHECK(notifications.load() == before);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 0U);
    CHECK(fixture.backend->contexts_destroyed == 0U);
    CHECK_FALSE(retirement.custody.FinishRetirement().completion_reached);
    completion.reset();
    CHECK(notifications.load() > before);
    CHECK(retirement.custody.FinishRetirement().completion_reached);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 2U);
}
TEST_CASE("Failed display finalization closes admission and retains complete transfer custody", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    const auto initiating = std::make_exception_ptr(std::runtime_error("display finalizer failed after transfer"));
    const auto cleanup = std::make_exception_ptr(std::runtime_error("display completion unavailable"));
    SystemImageRuntime runtime(
        {.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .workspace_finalize = [&](auto, auto, auto, auto, auto) {
             backend->FailDeviceBinding(1, cleanup);
             std::rethrow_exception(initiating);
         }});
    runtime.Publish(4U, 3U, [](auto, auto, auto) {});
    auto completed = runtime.Completed();
    const auto raw = completed.Borrow().plane(0U).plane().data;
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout());
    std::exception_ptr failure;
    try {
        static_cast<void>(runtime.PrepareDisplay(completed.revision(), workspace));
    } catch (...) { failure = std::current_exception(); }
    CHECK(test_support::ContainsImageFailure(failure, initiating));
    CHECK(test_support::ContainsImageFailure(failure, cleanup));
    CHECK(workspace->retired());
    CHECK(completed.Borrow().plane(0U).plane().data == raw);
    auto retirement = runtime.Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK_FALSE(retirement.custody.deferred());
    CHECK(test_support::ContainsImageFailure(retirement.failure, initiating));
    CHECK(test_support::ContainsImageFailure(retirement.failure, cleanup));
    CHECK(backend->planes_allocated == 4U);
    CHECK(backend->planes_freed == 0U);
    CHECK(backend->pinned_allocated == 2U);
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(backend->streams_destroyed == 0U);
    CHECK(test_support::ImportedImageBufferTestAccess::unmaps == 0U);
}
TEST_CASE("Workspace retirement retains cross-device transfer and raw custody through real finalization", "[gpu][workspace][copy]") {
    using test_support::ImageWorkspaceTestAccess;
    for (const bool peer : {false, true}) {
        ImageWorkspaceTestAccess::Reset();
        auto backend = std::make_shared<FakeImageBackend>();
        backend->peer_access = peer;
        SystemImageRuntime runtime({.device = 0,
                                    .backend = backend,
                                    .output_layout = ImageProductLayout::CleanAndSemantic,
                                    .workspace_finalize = [](auto clean, auto semantic, auto destination, auto, auto) {
                                        CHECK(*reinterpret_cast<const std::byte*>(semantic.data) == std::byte{91});
                                        test_support::CopyImagePlane(destination, clean);
                                    }});
        runtime.Publish(4U, 3U, [](auto clean, auto semantic, auto) {
            for (const auto plane : {clean, semantic})
                std::memset(reinterpret_cast<void*>(plane.data), plane.descriptor.kind == ImagePlaneKind::Clean ? 37 : 91,
                            plane.descriptor.pitch_bytes * plane.descriptor.height);
        });
        auto completed = runtime.Completed();
        const auto revision = completed.revision();
        const auto raw = completed.Borrow().plane(0U).plane().data;
        auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
            backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout());
        REQUIRE(runtime.PrepareDisplay(completed.revision(), workspace));
        CHECK(completed.revision() == revision);
        CHECK(completed.Borrow().plane(0U).plane().data == raw);
        CHECK(completed.BorrowWorkspace().revision() == revision);
        CHECK(*reinterpret_cast<const std::byte*>(completed.BorrowWorkspace().plane().data) == std::byte{37});
        CHECK(backend->peer_copies == (peer ? 2U : 0U));
        CHECK(backend->staged_uploads == (peer ? 0U : 2U));
        CHECK(backend->staged_downloads == (peer ? 0U : 2U));
        workspace.reset();
        auto retirement = runtime.Retire();
        REQUIRE(retirement.custody.deferred());
        CHECK(backend->planes_freed == 0U);
        completed = {};
        CHECK(retirement.custody.FinishRetirement().completion_reached);
        CHECK(backend->planes_freed == 4U);
        CHECK(backend->contexts_destroyed == 2U);
    }
}
TEST_CASE("Shared display content rotates independent producer owners and layouts with bounded transfers", "[gpu][workspace][display]") {
    using test_support::ImageWorkspaceTestAccess;
    for (const int display_device : {0, 1}) {
        for (const bool peer : {false, true}) {
            for (const bool semantic_first : {false, true}) {
                CAPTURE(display_device, peer, semantic_first);
                ImageWorkspaceTestAccess::Reset();
                auto backend = std::make_shared<FakeImageBackend>();
                backend->peer_access = peer;
                DeviceContext display(display_device, backend);
                auto workspace = ImageWorkspaceTestAccess::Create(display, ImageWorkspaceTestAccess::Layout(display_device));
                workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
                std::array<std::uintptr_t, 2U> producer_contexts{};
                std::array<std::unique_ptr<SystemImageRuntime>, 2U> producers;
                std::array<ImageWorkspaceContent, 2U> contents{};
                for (std::size_t index = 0U; index != producers.size(); ++index) {
                    const bool semantic = (index == 0U) == semantic_first;
                    producers[index] = std::make_unique<SystemImageRuntime>(
                        SystemImageRuntimeConfig{.device = 0,
                                                 .backend = backend,
                                                 .output_layout = semantic ? ImageProductLayout::CleanAndSemantic : ImageProductLayout::Clean,
                                                 .workspace_finalize = [&, index, semantic](auto clean, auto semantics, auto destination, auto coverage, auto) {
                                                     CHECK(coverage.full_image);
                                                     CHECK(semantics.valid() == semantic);
                                                     if (display_device == 0) CHECK(backend->last_bound_context == producer_contexts[index]);
                                                     if (semantic) CHECK(*reinterpret_cast<const std::byte*>(semantics.data) == std::byte{91});
                                                     test_support::CopyImagePlane(destination, clean);
                                                 }});
                    auto& runtime = *producers[index];
                    runtime.BindContext();
                    producer_contexts[index] = backend->last_bound_context.load();
                    runtime.Publish(4U, 3U, [index](auto clean, auto semantics, auto) {
                        std::memset(reinterpret_cast<void*>(clean.data), index == 0U ? 37 : 73, clean.descriptor.pitch_bytes * clean.descriptor.height);
                        if (semantics.valid())
                            std::memset(reinterpret_cast<void*>(semantics.data), 91, semantics.descriptor.pitch_bytes * semantics.descriptor.height);
                    });
                    const auto observed = runtime.ObserveWorkspace();
                    contents[index] = {observed.product_owner, observed.product_revision};
                }
                REQUIRE(producer_contexts[0U] != producer_contexts[1U]);
                REQUIRE(contents[0U].owner != contents[1U].owner);
                REQUIRE(contents[0U].revision == contents[1U].revision);
                const auto raw_allocations = backend->planes_allocated.load();
                std::size_t warm_allocations = 0U, warm_pinned = 0U;
                for (std::size_t turn = 0U; turn != 8U; ++turn) {
                    const auto index = turn % 2U;
                    if (turn != 0U) {
                        REQUIRE(producers[1U - index]->DetachDisplay(workspace));
                        CHECK(workspace->Contains(contents[1U - index]));
                    }
                    REQUIRE(workspace->ReserveDisplayWrite());
                    workspace->InvalidateWrite();
                    CHECK_FALSE(workspace->Contains(contents[index]));
                    REQUIRE(producers[index]->PrepareDisplay(contents[index].revision, workspace));
                    REQUIRE(workspace->Contains(contents[index]));
                    CHECK_FALSE(workspace->Contains(contents[1U - index]));
                    const auto output = workspace->plane(4U, 3U);
                    for (std::uint32_t row = 0U; row != 3U; ++row) {
                        const auto* bytes = reinterpret_cast<const std::byte*>(output.data) + row * output.descriptor.pitch_bytes;
                        CHECK(std::ranges::all_of(std::span{bytes, output.descriptor.row_bytes()},
                                                  [index](auto byte) { return byte == (index == 0U ? std::byte{37} : std::byte{73}); }));
                    }
                    workspace->CancelDisplayWrite();
                    if (turn == 1U) {
                        warm_allocations = backend->planes_allocated;
                        warm_pinned = backend->pinned_allocated;
                    } else if (turn > 1U) {
                        CHECK(backend->planes_allocated == warm_allocations);
                        CHECK(backend->pinned_allocated == warm_pinned);
                    }
                }
                CHECK(backend->same_copies == 0U);
                if (display_device == 0) {
                    CHECK(backend->planes_allocated == raw_allocations);
                    CHECK(backend->peer_copies == 0U);
                    CHECK(backend->staged_downloads == 0U);
                    CHECK(backend->staged_uploads == 0U);
                } else {
                    CHECK(backend->peer_copies == (peer ? 12U : 0U));
                    CHECK(backend->staged_downloads == (peer ? 0U : 12U));
                    CHECK(backend->staged_uploads == (peer ? 0U : 12U));
                }
            }
        }
    }
}
TEST_CASE("Display partial coverage requires exact initialized prior content and recovers after failure", "[gpu][workspace][display]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext display(0, backend);
    auto workspace = ImageWorkspaceTestAccess::Create(display, ImageWorkspaceTestAccess::Layout(0));
    workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
    bool expected_full = true;
    bool fail = false;
    const auto finalize = [&](auto clean, auto, auto destination, auto coverage, auto) {
        CHECK(coverage.full_image == expected_full);
        if (coverage.full_image)
            test_support::CopyImagePlane(destination, clean);
        else
            for (const auto& region : coverage.regions) {
                for (auto row = region.y1; row != region.y2; ++row)
                    std::memcpy(reinterpret_cast<std::byte*>(destination.data) + row * destination.descriptor.pitch_bytes + region.x1 * 4U,
                                reinterpret_cast<const std::byte*>(clean.data) + row * clean.descriptor.pitch_bytes + region.x1 * 4U,
                                static_cast<std::size_t>(region.x2 - region.x1) * 4U);
            }
        if (fail) throw std::runtime_error("partial display submission failed");
    };
    SystemImageRuntime runtime({.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .workspace_finalize = finalize});
    const auto fill = [](auto clean, auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 37, clean.descriptor.pitch_bytes * clean.descriptor.height);
    };
    runtime.Publish(2U, 2U, fill);
    REQUIRE(runtime.PrepareDisplay(runtime.Completed().revision(), workspace));
    auto previous = runtime.ObserveWorkspace();
    const std::array<ImageWorkspaceRegion, 1U> regions{{{0, 0, 1, 1}}};
    auto candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 2U, 2U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 73, 4U); });
    CHECK_FALSE(workspace->Contains({previous.product_owner, previous.product_revision}));
    expected_full = false;
    runtime.FinalizeWorkspace(candidate, {workspace->identity(), regions, false, {previous.product_owner, previous.product_revision}});
    auto completed = runtime.CommitOutput(std::move(candidate));
    auto pixels = workspace->plane(2U, 2U);
    CHECK(*reinterpret_cast<const std::byte*>(pixels.data) == std::byte{73});
    CHECK(*(reinterpret_cast<const std::byte*>(pixels.data) + 4U) == std::byte{37});
    previous = runtime.ObserveWorkspace();
    completed = {};
    candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 2U, 2U, fill);
    fail = true;
    CHECK_THROWS(runtime.FinalizeWorkspace(candidate, {workspace->identity(), regions, false, {previous.product_owner, previous.product_revision}}));
    CHECK_FALSE(workspace->Contains({previous.product_owner, candidate.revision()}));
    fail = false;
    expected_full = true;
    runtime.FinalizeWorkspace(candidate, {workspace->identity(), regions, false, {previous.product_owner, previous.product_revision}});
    completed = runtime.CommitOutput(std::move(candidate));
    previous = runtime.ObserveWorkspace();
    completed = {};
    candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 4U, 3U, fill);
    runtime.FinalizeWorkspace(candidate, {workspace->identity(), regions, false, {previous.product_owner, previous.product_revision}});
    completed = runtime.CommitOutput(std::move(candidate));
    CHECK(workspace->Contains({previous.product_owner, completed.revision()}));
    previous = runtime.ObserveWorkspace();
    completed = {};
    candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 4U, 3U, fill);
    candidate = {};
    CHECK_FALSE(workspace->Contains({previous.product_owner, previous.product_revision}));
    candidate = runtime.AcquireOutput();
    runtime.PublishRetained(candidate, 4U, 3U, fill);
    runtime.FinalizeWorkspace(candidate, {workspace->identity(), regions, false, {previous.product_owner, previous.product_revision}});
    completed = runtime.CommitOutput(std::move(candidate));
    REQUIRE(runtime.DetachDisplay(workspace));
    SystemImageRuntime replacement({.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .workspace_finalize = finalize});
    replacement.Publish(4U, 3U, fill);
    REQUIRE(workspace->ReserveDisplayWrite());
    workspace->InvalidateWrite();
    REQUIRE(replacement.PrepareDisplay(replacement.Completed().revision(), workspace));
    workspace->CancelDisplayWrite();
}
TEST_CASE("Display remapping retries preserve raw products through partial construction and cancellation", "[gpu][workspace][display]") {
    using test_support::ImageWorkspaceTestAccess;
    for (const auto point : {FakeImageBackend::FailurePoint::None, FakeImageBackend::FailurePoint::CreateStream, FakeImageBackend::FailurePoint::CreateEvent}) {
        CAPTURE(point);
        ImageWorkspaceTestAccess::Reset();
        auto backend = std::make_shared<FakeImageBackend>();
        {
            DeviceContext display(0, backend);
            auto workspace = ImageWorkspaceTestAccess::Create(display, ImageWorkspaceTestAccess::Layout(0));
            workspace->Admit(workspace->identity(), workspace->layout().device_incarnation);
            SystemImageRuntime runtime({.device = 0, .backend = backend, .workspace_finalize = [](auto clean, auto, auto destination, auto coverage, auto) {
                                            CHECK(coverage.full_image);
                                            test_support::CopyImagePlane(destination, clean);
                                        }});
            runtime.Publish(4U, 3U, [](auto clean, auto, auto) {
                std::memset(reinterpret_cast<void*>(clean.data), 73, clean.descriptor.pitch_bytes * clean.descriptor.height);
            });
            const auto observed = runtime.ObserveWorkspace();
            const ImageWorkspaceContent content{observed.product_owner, observed.product_revision};
            const auto raw = runtime.Borrow().plane(0U).plane().data;
            REQUIRE(workspace->ReserveDisplayWrite());
            workspace->InvalidateWrite();
            if (point == FakeImageBackend::FailurePoint::None)
                ImageWorkspaceTestAccess::alias_failure = std::make_exception_ptr(std::runtime_error("alias import failed"));
            else
                backend->FailAfter(point);
            CHECK_THROWS(runtime.PrepareDisplay(content.revision, workspace));
            CHECK_FALSE(workspace->Contains(content));
            CHECK(runtime.Borrow().plane(0U).plane().data == raw);
            CHECK(*reinterpret_cast<const std::byte*>(raw) == std::byte{73});
            workspace->CancelDisplayWrite();
            ImageWorkspaceTestAccess::alias_failure = {};
            REQUIRE(workspace->ReserveDisplayWrite());
            workspace->InvalidateWrite();
            REQUIRE(runtime.PrepareDisplay(content.revision, workspace));
            CHECK(workspace->Contains(content));
            workspace->CancelDisplayWrite();
            const auto maps = ImageWorkspaceTestAccess::initialized;
            const auto events = backend->events_created.load();
            REQUIRE(workspace->ReserveDisplayWrite());
            workspace->CancelDisplayWrite();
            REQUIRE(workspace->Contains(content));
            REQUIRE(runtime.PrepareDisplay(content.revision, workspace));
            CHECK(ImageWorkspaceTestAccess::initialized == maps);
            CHECK(backend->events_created == events);
        }
        CHECK(backend->contexts_destroyed == backend->contexts_created);
        CHECK(backend->events_destroyed == backend->events_created);
        CHECK(backend->planes_freed == backend->planes_allocated);
    }
}
TEST_CASE("Repeated direct display handoffs reuse private high-water storage and preserve raw pixels", "[gpu][workspace][display]") {
    WorkspaceRuntimeFixture fixture;
    auto [workspace, product] = fixture.PublishWorkspace(0);
    std::size_t warm_allocations = 0U;
    for (unsigned turn = 0U; turn != 5U; ++turn) {
        const auto revision = product.revision();
        auto raw = product.Borrow();
        const auto owner = raw.plane(0U).plane().allocation.owner;
        CHECK_FALSE(fixture.runtime.DetachDisplay(workspace));
        raw = {};
        REQUIRE(fixture.runtime.DetachDisplay(workspace));
        CHECK(workspace->Contains({owner, revision}));
        CHECK(*reinterpret_cast<const std::byte*>(product.Borrow().plane(0U).plane().data) == std::byte{37});
        REQUIRE(fixture.runtime.PrepareDisplay(revision, workspace));
        auto candidate = fixture.runtime.TryAcquireOutput(product);
        REQUIRE(candidate.valid());
        fixture.runtime.PublishRetained(candidate, 4U, 3U, [](auto, auto, auto) {});
        product = fixture.runtime.CommitOutput(std::move(candidate));
        CHECK(*reinterpret_cast<const std::byte*>(product.Borrow().plane(0U).plane().data) == std::byte{37});
        if (turn == 0U)
            warm_allocations = fixture.backend->planes_allocated;
        else
            CHECK(fixture.backend->planes_allocated == warm_allocations);
    }
}
TEST_CASE("Workspace transfer routes preserve independent source and receiver pitch guards", "[gpu][workspace][copy]") {
    auto backend = std::make_shared<FakeImageBackend>();
    backend->pitch_padding_bytes = 16U;
    SystemImageRuntime source({.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic});
    source.Publish(4U, 3U, [](auto clean, auto semantic, auto) {
        for (const auto plane : {clean, semantic}) {
            for (std::uint32_t row = 0U; row < plane.descriptor.height; ++row)
                std::memset(reinterpret_cast<std::byte*>(plane.data) + row * plane.descriptor.pitch_bytes,
                            plane.descriptor.kind == ImagePlaneKind::Clean ? 29 : 78, plane.descriptor.row_bytes());
        }
    });
    backend->pitch_padding_bytes = 32U;
    for (const int device : {0, 1, 2}) {
        backend->peer_access = device == 1;
        SystemImageRuntime receiver({.device = device, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic});
        receiver.Publish(4U, 3U, [](auto clean, auto semantic, auto) {
            for (const auto plane : {clean, semantic})
                std::memset(reinterpret_cast<void*>(plane.data), 219, plane.descriptor.pitch_bytes * plane.descriptor.height);
        });
        const auto paths = receiver.CopyFrom(source.Borrow());
        const auto expected = device == 0 ? ImageCopyPath::SameDevice : device == 1 ? ImageCopyPath::Peer : ImageCopyPath::PinnedStaging;
        CHECK(paths[0U] == expected);
        CHECK(paths[1U] == expected);
        auto read = receiver.Borrow();
        for (std::size_t index = 0U; index != read.plane_count(); ++index) {
            const auto plane = read.plane(index).plane();
            REQUIRE(plane.descriptor.pitch_bytes == 48U);
            for (std::uint32_t row = 0U; row < plane.descriptor.height; ++row) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(plane.data) + row * plane.descriptor.pitch_bytes;
                for (std::size_t byte = 0U; byte < plane.descriptor.pitch_bytes; ++byte)
                    CHECK(bytes[byte] == (byte < plane.descriptor.row_bytes() ? (index == 0U ? 29U : 78U) : 219U));
            }
        }
    }
}
TEST_CASE("Workspace layout rejects overflow and inconsistent subresource bounds", "[gpu][workspace]") {
    ImageWorkspaceLayout layout{.device_incarnation = 7U,
                                .device_uuid = {1U},
                                .device = 0,
                                .width = 4U,
                                .height = 3U,
                                .pitch_bytes = 32U,
                                .offset_bytes = 128U,
                                .required_allocation_bytes = 256U,
                                .alignment_bytes = 64U};
    REQUIRE(layout.valid());
    auto invalid = layout;
    invalid.offset_bytes = std::numeric_limits<std::size_t>::max() - 1U;
    CHECK_FALSE(invalid.valid());
    invalid = layout;
    invalid.required_allocation_bytes = 128U;
    CHECK_FALSE(invalid.valid());
    invalid = layout;
    invalid.pitch_bytes = 15U;
    CHECK_FALSE(invalid.valid());
    invalid = layout;
    invalid.device_uuid = {};
    CHECK_FALSE(invalid.valid());
    invalid = layout;
    invalid.alignment_bytes = 3U;
    CHECK_FALSE(invalid.valid());
    ImportedImageBuffer allocation;
    std::string error;
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext context(0, backend);
    invalid = layout;
    invalid.offset_bytes = std::numeric_limits<std::size_t>::max();
    CHECK_FALSE(allocation.Import(context, {}, invalid, 1U, &error));
    CHECK_FALSE(allocation.owns_resources());
}
TEST_CASE("Late workspace admission preserves raw storage then aliases the next clean publication", "[gpu][workspace][hardware]") {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable");
    REQUIRE(cuInit(0U) == CUDA_SUCCESS);
    auto allocation = std::make_unique<test_support::VulkanWorkspaceFixture>(0, 4U, 3U);
    auto layout = allocation->layout();
    std::size_t finalizations = 0U;
    bool fail_finalization = false;
    SystemImageRuntimeConfig config{.device = 0, .context_mode = DeviceContextMode::PrimaryInterop};
    config.workspace_finalize = [&](ImagePlaneView clean, ImagePlaneView, ImagePlaneView destination, ImageWorkspaceCoverage coverage, std::uintptr_t stream) {
        CHECK(coverage.full_image);
        ++finalizations;
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = clean.data;
        copy.srcPitch = clean.descriptor.pitch_bytes;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = destination.data;
        copy.dstPitch = destination.descriptor.pitch_bytes;
        copy.WidthInBytes = clean.descriptor.row_bytes();
        copy.Height = clean.descriptor.height;
        REQUIRE(cuMemcpy2DAsync(&copy, reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS);
        if (fail_finalization) throw std::runtime_error("injected finalization failure after submission");
    };
    SystemImageRuntime runtime(std::move(config));
    DeviceContext display(0, cuda_image_copy_backend());
    const auto fill = [](ImagePlaneView clean, ImagePlaneView, std::uintptr_t stream) {
        REQUIRE(cuMemsetD2D8Async(clean.data, clean.descriptor.pitch_bytes, 37U, clean.descriptor.row_bytes(), clean.descriptor.height,
                                  reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS);
    };
    runtime.Publish(4U, 3U, fill);
    auto completed = runtime.Completed();
    const auto raw = runtime.Borrow().plane(0U).plane().data;
    auto workspace = ImageWorkspace::Create(display, layout);
    CHECK_THROWS(workspace->Admit(workspace->identity(), 8U));
    auto descriptor = allocation->Export();
    REQUIRE(descriptor.get() >= 0);
    mmltk::common::io::ScopedFd transferred(descriptor.release());
    CHECK(descriptor.get() == -1);
    REQUIRE(workspace->QueueAllocation(std::move(transferred)));
    workspace->Admit(workspace->identity(), layout.device_incarnation);
    allocation.reset();  // Native backing remains valid after all Vulkan owners retire.
    const auto check_pixels = [](ImagePlaneView plane, DeviceContext context) {
        context.Bind();
        std::vector<std::byte> pixels(plane.descriptor.row_bytes() * plane.descriptor.height);
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = plane.data;
        copy.srcPitch = plane.descriptor.pitch_bytes;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = pixels.data();
        copy.dstPitch = plane.descriptor.row_bytes();
        copy.WidthInBytes = plane.descriptor.row_bytes();
        copy.Height = plane.descriptor.height;
        REQUIRE(cuMemcpy2D(&copy) == CUDA_SUCCESS);
        CHECK(std::ranges::all_of(pixels, [](auto byte) { return byte == std::byte{37U}; }));
    };
    const auto prepare = [&](const std::shared_ptr<ImageWorkspace>& target) {
        if (!runtime.PrepareDisplay(runtime.OutputFacts().revision, target)) {
            const auto settled = target->Settle();
            REQUIRE(settled.completion_reached);
            REQUIRE_FALSE(settled.failure);
        }
        REQUIRE(runtime.PrepareDisplay(runtime.OutputFacts().revision, target));
    };
    prepare(workspace);
    CHECK(runtime.Borrow().plane(0U).plane().data == raw);
    CHECK(runtime.BorrowWorkspace().plane().data != raw);
    CHECK(finalizations == 1U);
    check_pixels(runtime.BorrowWorkspace().plane(), display);
    completed = {};
    runtime.Publish(4U, 3U, fill);
    prepare(workspace);
    CHECK(runtime.Borrow().plane(0U).plane().allocation.identity == workspace->identity());
    CHECK(finalizations == 1U);
    check_pixels(runtime.Borrow().plane(0U).plane(), runtime.Borrow().plane(0U).context());
    auto borrowed = runtime.BorrowWorkspace();
    REQUIRE(borrowed.valid());
    CHECK(borrowed.layout().offset_bytes == layout.offset_bytes);
    auto completion = std::move(borrowed).TakeCompletion();
    SystemImageRuntime::CompletedOutput baseline;
    CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
    completion->Complete();
    CHECK(runtime.TryAcquireOutput(baseline).valid());
    completion.reset();
    borrowed = {};
    runtime.Publish(8U, 3U, fill);
    CHECK_FALSE(runtime.BorrowWorkspace().valid());
    const auto grown_raw = runtime.Borrow().plane(0U).plane().data;
    const auto grown_revision = runtime.OutputFacts().revision;
    allocation = std::make_unique<test_support::VulkanWorkspaceFixture>(0, 8U, 3U);
    layout = allocation->layout();
    auto wrong_device = layout;
    wrong_device.device_uuid[0U] ^= 1U;
    auto rejected = ImageWorkspace::Create(display, wrong_device);
    REQUIRE(rejected->QueueAllocation(allocation->Export()));
    CHECK_THROWS(rejected->Admit(rejected->identity(), wrong_device.device_incarnation));
    rejected.reset();
    auto replacement = ImageWorkspace::Create(display, layout);
    CHECK(replacement->identity() != workspace->identity());
    REQUIRE(replacement->QueueAllocation(allocation->Export()));
    replacement->Admit(replacement->identity(), layout.device_incarnation);
    fail_finalization = true;
    CHECK_THROWS(runtime.PrepareDisplay(runtime.Completed().revision(), replacement));
    CHECK_FALSE(runtime.BorrowWorkspace().valid());
    CHECK(runtime.OutputFacts().revision == grown_revision);
    CHECK(runtime.Borrow().plane(0U).plane().data == grown_raw);
    fail_finalization = false;
    prepare(replacement);
    CHECK(runtime.BorrowWorkspace().revision() == grown_revision);
    CHECK(runtime.Borrow().plane(0U).plane().data == grown_raw);
    CHECK(finalizations == 3U);
    if (mmltk::testsupport::checked_cuda_device_count() > 1) {
        test_support::VulkanWorkspaceFixture remote_allocation(1, 8U, 3U);
        const auto remote_layout = remote_allocation.layout();
        auto remote = ImageWorkspace::Create(DeviceContext(1, cuda_image_copy_backend()), remote_layout);
        REQUIRE(remote->QueueAllocation(remote_allocation.Export()));
        remote->Admit(remote->identity(), remote_layout.device_incarnation);
        prepare(remote);
        CHECK(runtime.BorrowWorkspace().layout().device == 1);
        CHECK(runtime.Borrow().plane(0U).plane().data == grown_raw);
        CHECK(finalizations == 3U);
        CHECK(remote->StorageFootprint().device_bytes == remote->allocation_bytes());
    }
}
TEST_CASE("Asynchronous workspace finalization retains raw custody until the owner settles its notification", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime(mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend));
    runtime.Publish(
        4U, 3U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 37, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    const auto raw = runtime.ObserveWorkspace();
    const bool settle_directly = GENERATE(false, true);
    struct DisplayObservation {
        std::size_t notifications = 0U;
        bool ready = false;
    };
    const auto display = std::make_shared<DisplayObservation>();
    workspace->SetDisplayAvailabilitySink(std::make_shared<const std::function<void()>>(
        [display, weak = std::weak_ptr{workspace}, content = ImageWorkspaceContent{raw.product_owner, raw.product_revision}] {
            ++display->notifications;
            const auto completed = weak.lock();
            display->ready = completed && completed->Contains(content) && completed->WriteAvailable();
        }));
    backend->defer_notifications = true;
    CHECK_FALSE(runtime.PrepareDisplay(raw.product_revision, workspace));
    CHECK_FALSE(workspace->Contains({raw.product_owner, raw.product_revision}));
    CHECK_FALSE(runtime.DetachDisplay(workspace));
    auto baseline = runtime.Completed();
    CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
    CHECK(runtime.Borrow().valid());
    CHECK_FALSE(workspace->ReserveDisplayWrite());
    backend->CompleteNotifications();
    CHECK_FALSE(workspace->Contains({raw.product_owner, raw.product_revision}));
    CHECK(display->notifications == 0U);
    CHECK(backend->planes_freed == 0U);
    if (settle_directly) {
        const auto settled = workspace->Settle();
        REQUIRE(settled.completion_reached);
        REQUIRE_FALSE(settled.failure);
    } else {
        runtime.CompleteWorkspaces();
    }
    REQUIRE(workspace->Contains({raw.product_owner, raw.product_revision}));
    CHECK(display->notifications == 1U);
    CHECK(display->ready);
    runtime.CompleteWorkspaces();
    CHECK(display->notifications == 1U);
    REQUIRE(workspace->ReserveDisplayWrite());
    workspace->CancelDisplayWrite();
    workspace->SetDisplayAvailabilitySink({});
    CHECK(*reinterpret_cast<const std::byte*>(runtime.BorrowWorkspace().plane().data) == std::byte{37});
    CHECK(runtime.TryAcquireOutput(baseline).valid());
    CHECK(runtime.PrepareDisplay(raw.product_revision, workspace));
}
TEST_CASE("Shutdown settles pending workspace finalization with allocation-local cleanup custody", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    auto workspace = mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::CreateAdmitted(
        backend, mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess::Layout(0));
    auto observation = workspace->ObserveRetirement();
    {
        SystemImageRuntime runtime(mmltk::frameworks::gpu::test_support::WorkspaceRuntimeConfig(backend));
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        backend->defer_notifications = true;
        CHECK_FALSE(runtime.PrepareDisplay(runtime.OutputFacts().revision, workspace));
        REQUIRE(observation.TransferToProducer());
        workspace.reset();
        const auto retired = runtime.Retire();
        CHECK(retired.safe_to_destroy);
        CHECK_FALSE(retired.failure);
    }
    CHECK(observation.TakeResult().complete);
    CHECK_FALSE(observation.TakeResult().claimed);
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Stream notifications retain independent terminal status across moves and reuse", "[gpu][workspace]") {
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    {
        DeviceContext context(0, backend);
        ImageStream first(context), second(context);
        std::atomic_uint first_wakes{0U}, second_wakes{0U};
        first.Notify([&] { ++first_wakes; });
        second.Notify([&] { ++second_wakes; });
        const auto first_handle = first.native_handle();
        ImageStream moved(std::move(first));
        backend->CompleteNotifications(first_handle);
        CHECK(first_wakes == 1U);
        CHECK(second_wakes == 0U);
        CHECK(moved.Settle().completion_reached);
        CHECK(second_wakes == 0U);
        moved.Notify([&] { ++first_wakes; });
        backend->CompleteNotifications(first_handle, CUDA_ERROR_LAUNCH_FAILED);
        const auto failed = moved.Settle();
        CHECK(failed.completion_reached);
        CHECK(is_image_execution_failure(failed.failure));
        CHECK(first_wakes == 2U);
        CHECK(second_wakes == 0U);
        // Replacement settles the destination's pending callback before moving
        // the source's terminal status and its retained stream custody.
        second = std::move(moved);
        CHECK(second_wakes == 1U);
        CHECK(second.Settle().failure == failed.failure);
    }
    CHECK(backend->contexts_created == backend->contexts_destroyed);
    CHECK(backend->streams_destroyed == 2U);
}
TEST_CASE("Stream notification registration failure releases only unregistered callback storage", "[gpu][workspace]") {
    auto backend = std::make_shared<FakeImageBackend>();
    const auto rejected = std::make_exception_ptr(std::runtime_error("notification registration rejected"));
    {
        DeviceContext context(0, backend);
        ImageStream stream(context);
        auto closure = std::make_shared<int>(7);
        std::weak_ptr<int> observed = closure;
        backend->FailAfter(FakeImageBackend::FailurePoint::NotifyStream, 0U, rejected);
        try {
            stream.Notify([retained = std::move(closure)] {});
            FAIL("notification registration must report its failure");
        } catch (...) { CHECK(std::current_exception() == rejected); }
        CHECK(observed.expired());
        unsigned wakes = 0U;
        stream.Notify([&] { ++wakes; });
        CHECK(stream.Settle().completion_reached);
        CHECK(wakes == 1U);
    }
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Owner completion retains notification captures until the wake callback returns", "[gpu][workspace]") {
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_notifications = true;
    SystemImageRuntime runtime({.device = 0, .backend = backend});
    mmltk::testsupport::TestGate callback_return("notification wake callback return");
    auto captured = std::make_shared<int>(19);
    std::weak_ptr<int> retained = captured;
    auto admitted = backend->ObserveNextNotificationStream();
    runtime.NotifyWorkCompletion([capture = std::move(captured), gate = callback_return.receipt()] { gate.ArriveAndWait(); });
    const auto stream = mmltk::testsupport::await_test_future(admitted, "notification capture admitted");
    auto callback = std::async(std::launch::async, [&] { backend->CompleteNotifications(stream); });
    std::future<void> completed;
    auto cleanup = mmltk::testsupport::ScopedTestCleanup{[&] {
        callback_return.Release();
        if (callback.valid()) callback.wait();
        if (completed.valid()) completed.wait();
    }};
    REQUIRE(callback_return.WaitEntered(std::chrono::seconds{2}));
    completed = std::async(std::launch::async, [&] { runtime.CompleteWork(); });
    CHECK_FALSE(retained.expired());
    CHECK(completed.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
    callback_return.Release();
    mmltk::testsupport::await_test_future(callback, "notification callback returned");
    mmltk::testsupport::await_test_future(completed, "owner completion after callback return");
    CHECK(retained.expired());
}
auto prepare_delayed_workspace(SystemImageRuntime& runtime, const std::shared_ptr<FakeImageBackend>& backend) {
    auto workspace = test_support::ImageWorkspaceTestAccess::CreateAdmitted(backend, test_support::ImageWorkspaceTestAccess::Layout(0));
    backend->defer_notifications = true;
    auto admitted = backend->ObserveNextNotificationStream();
    CHECK_FALSE(runtime.PrepareDisplay(runtime.OutputFacts().revision, workspace));
    const auto stream = mmltk::testsupport::await_test_future(admitted, "display notification admitted");
    return std::pair{std::move(workspace), stream};
}
TEST_CASE("Output admission drains its completed workspace read without another render cycle", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend, .workspace_finalize = test_support::FakeWorkspaceFinalizer(backend)});
    runtime.Publish(
        4U, 3U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 83, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    auto [workspace, stream] = prepare_delayed_workspace(runtime, backend);
    auto baseline = runtime.Completed();
    REQUIRE_FALSE(runtime.TryAcquireOutput(baseline).valid());
    std::stop_source stop;
    std::promise<void> entered;
    std::future<SystemImageRuntime::OutputCandidate> acquired;
    auto cleanup = mmltk::testsupport::ScopedTestCleanup{[&] {
        stop.request_stop();
        backend->CompleteNotifications(stream);
        if (acquired.valid()) acquired.wait();
    }};
    SECTION("Notification precedes the admission epoch") {
        backend->CompleteNotifications(stream);
        auto candidate = runtime.AcquireOutput(stop.get_token(), std::move(baseline));
        REQUIRE(candidate.valid());
        CHECK(workspace->revision() != 0U);
    }
    SECTION("Notification races the admission check and wait") {
        acquired = std::async(std::launch::async, [&] {
            entered.set_value();
            return runtime.AcquireOutput(stop.get_token(), std::move(baseline));
        });
        mmltk::testsupport::await_test_promise(entered, "output acquisition entered");
        backend->CompleteNotifications(stream);
        auto candidate = mmltk::testsupport::await_test_future(acquired, "output acquisition after display completion");
        REQUIRE(candidate.valid());
        CHECK(workspace->revision() != 0U);
    }
    SECTION("An admission drain retains custody until physical settlement") {
        backend->CompleteNotifications(stream);
        auto settlement = backend->HoldStreamSettlements("output admission physical display settlement");
        acquired = std::async(std::launch::async, [&] { return runtime.AcquireOutput(stop.get_token(), std::move(baseline)); });
        REQUIRE(settlement->WaitEntered(std::chrono::seconds{2}));
        CHECK(acquired.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
        CHECK(backend->planes_freed == 0U);
        settlement->Release();
        REQUIRE(mmltk::testsupport::await_test_future(acquired, "output admission physical completion").valid());
    }
    SECTION("Stop wakes admission with the workspace read still pending") {
        acquired = std::async(std::launch::async, [&] {
            entered.set_value();
            return runtime.AcquireOutput(stop.get_token(), std::move(baseline));
        });
        mmltk::testsupport::await_test_promise(entered, "stoppable output acquisition entered");
        stop.request_stop();
        CHECK_FALSE(mmltk::testsupport::await_test_future(acquired, "stopped output acquisition").valid());
        CHECK(workspace->revision() == 0U);
    }
}
TEST_CASE("Workspace terminal notification propagates exact failure while preserving retained raw pixels", "[gpu][workspace]") {
    using test_support::ImageWorkspaceTestAccess;
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    const auto failure = std::make_exception_ptr(std::runtime_error("display execution failed"));
    {
        SystemImageRuntime runtime({.device = 0, .backend = backend, .workspace_finalize = test_support::FakeWorkspaceFinalizer(backend)});
        runtime.Publish(4U, 3U, [](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), 61, clean.descriptor.pitch_bytes * clean.descriptor.height);
        });
        auto [workspace, stream] = prepare_delayed_workspace(runtime, backend);
        backend->SetStreamSettlement(stream, {.completion_reached = true, .failure = failure});
        backend->CompleteNotifications(stream, CUDA_ERROR_LAUNCH_FAILED);
        auto baseline = runtime.Completed();
        try {
            static_cast<void>(runtime.AcquireOutput({}, std::move(baseline)));
            FAIL("workspace admission must report terminal execution failure");
        } catch (...) { CHECK(test_support::ContainsImageFailure(std::current_exception(), failure)); }
        CHECK(workspace->revision() == 0U);
        CHECK(*reinterpret_cast<const std::byte*>(runtime.Borrow().plane(0U).plane().data) == std::byte{61});
        CHECK(runtime.DetachDisplay(workspace));
    }
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
}
TEST_CASE("Workspace damage accumulates skipped raw revisions for each display baseline", "[gpu][workspace]") {
    ImageWorkspaceDamage damage;
    const std::array<ImageWorkspaceRegion, 2U> first{{{2, 3, 5, 7}, {9, 10, 12, 15}}};
    damage.Record({7U, 1U}, {});
    damage.Record({7U, 3U}, {.regions = first, .full_image = false, .baseline = {7U, 1U}});
    const ImageWorkspaceRegion second{20, 1, 25, 4};
    damage.Record({7U, 8U}, {.regions = {&second, 1U}, .full_image = false, .baseline = {7U, 3U}});
    const auto older = damage.Since({7U, 1U}, {7U, 8U}, 101U);
    REQUIRE_FALSE(older.full_image);
    REQUIRE(older.regions.size() == 1U);
    CHECK(older.allocation_identity == 101U);
    CHECK(older.regions.front() == ImageWorkspaceRegion{2, 1, 25, 15});
    const auto newer = damage.Since({7U, 3U}, {7U, 8U}, 102U);
    REQUIRE_FALSE(newer.full_image);
    CHECK(newer.regions.front() == second);
    CHECK(damage.Since({9U, 3U}, {7U, 8U}, 102U).full_image);
    CHECK(damage.Since({7U, 2U}, {7U, 8U}, 102U).full_image);
    damage.Record({7U, 9U}, {});
    CHECK(damage.Since({7U, 8U}, {7U, 9U}, 102U).full_image);
    for (std::uint64_t revision = 10U; revision != 100U; ++revision)
        damage.Record({7U, revision}, {.regions = {&second, 1U}, .full_image = false, .baseline = {7U, revision - 1U}});
    CHECK(damage.Since({7U, 9U}, {7U, 99U}, 102U).full_image);
    CHECK_FALSE(damage.Since({7U, 98U}, {7U, 99U}, 102U).full_image);
}

} // namespace
} // namespace mmltk::frameworks::gpu
