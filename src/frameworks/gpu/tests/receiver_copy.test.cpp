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
using test_support::make_clean_semantic_runtime;
using test_support::RuntimeFactory;
TEST_CASE("receiver selects same peer and reusable staged copy paths") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime source{{.device = 0, .backend = backend}};
    source.Publish(16U, 8U, [](auto, auto, auto) {});
    SystemImageRuntime same{{.device = 0, .backend = backend}};
    CHECK(same.CopyFrom(source.Borrow())[0U] == ImageCopyPath::SameDevice);
    CHECK(backend->same_copies == 1U);
    backend->peer_access = true;
    SystemImageRuntime peer{{.device = 1, .backend = backend}};
    CHECK(peer.CopyFrom(source.Borrow())[0U] == ImageCopyPath::Peer);
    CHECK(backend->peer_copies == 1U);
    CHECK(backend->pinned_allocated == 0U);
    backend->peer_access = false;
    SystemImageRuntime staged{{.device = 2, .backend = backend}};
    CHECK(staged.CopyFrom(source.Borrow())[0U] == ImageCopyPath::PinnedStaging);
    CHECK(staged.CopyFrom(source.Borrow())[0U] == ImageCopyPath::PinnedStaging);
    const auto copied_revision = staged.OutputFacts().revision;
    staged.Publish(16U, 8U, [](auto, auto, auto) {});
    CHECK(staged.OutputFacts().revision > copied_revision);
    CHECK(backend->pinned_allocated == 1U);
    CHECK(backend->pinned_receiver_device == 2);
    CHECK(staged.OutputFacts().staging_capacity_bytes == 16U * 8U * 4U);
    CHECK(backend->staged_downloads == 2U);
    CHECK(backend->staged_uploads == 2U);
}
TEST_CASE("output storage sums every physical plane and slot at retained high water") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime source{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic}};
    source.Publish(16U, 8U, [](auto, auto, auto) {});
    SystemImageRuntime receiver{{.device = 1, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U}};
    CHECK(receiver.OutputStorageFootprint().device_bytes == 0U);
    CHECK(receiver.OutputStorageFootprint().pinned_bytes == 0U);
    backend->peer_access = false;
    static_cast<void>(receiver.CopyFrom(source.Borrow()));
    auto first = receiver.Completed();
    const auto one = receiver.OutputStorageFootprint();
    CHECK(one.device_bytes == 2U * 16U * 4U * 8U);
    CHECK(one.pinned_bytes == 2U * 16U * 4U * 8U);
    static_cast<void>(receiver.CopyFrom(source.Borrow()));
    const auto both = receiver.OutputStorageFootprint();
    CHECK(both.device_bytes == 4U * 16U * 4U * 8U);
    CHECK(both.pinned_bytes == 4U * 16U * 4U * 8U);
    first = {};
    auto candidate = receiver.AcquireOutput();
    receiver.Publish(candidate, 4U, 2U, [](auto, auto, auto) {});
    // Uncommitted smaller logical output retains its exact physical allocation.
    CHECK(receiver.OutputStorageFootprint().device_bytes == both.device_bytes);
    CHECK(receiver.OutputStorageFootprint().pinned_bytes == both.pinned_bytes);
    receiver.CommitOutput(std::move(candidate));
    CHECK(receiver.OutputFacts().capacity_width == 16U);
    CHECK(receiver.OutputFacts().capacity_height == 8U);
    CHECK(receiver.OutputStorageFootprint().device_bytes == both.device_bytes);
    CHECK(receiver.OutputStorageFootprint().pinned_bytes == both.pinned_bytes);
}
TEST_CASE("borrowed image storage remains stable until receiver completion") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    {
        SystemImageRuntime runtime{{.device = 0, .backend = backend}};
        runtime.Publish(8U, 8U, [](auto, auto, auto) {});
        auto borrowed = runtime.Borrow();
        SystemImageRuntime::CompletedOutput baseline;
        auto reservation = std::async(std::launch::async, [&] { return runtime.TryAcquireOutput(baseline).valid(); });
        REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "held-reader reservation"));
        auto writer = std::async(std::launch::async, [&runtime] { runtime.Publish(16U, 16U, [](auto, auto, auto) {}); });
        mmltk::testsupport::ScopedTestCleanup release_borrow{[&] { borrowed = {}; }};
        borrowed = {};
        mmltk::testsupport::await_test_future(writer, "released single-slot writer");
        std::atomic_bool reader_released{false};
        bool caught = false;
        try {
            mmltk::testsupport::TestGate reader_gate{"test-side exception with physical read held"};
            std::promise<std::uint64_t> reader_ready;
            auto reader = std::async(std::launch::async, [&] {
                {
                    auto read = runtime.Borrow();
                    reader_ready.set_value(read.valid() ? read.plane(0U).revision() : 0U);
                    reader_gate.receipt().ArriveAndWait();
                }
                reader_released.store(true, std::memory_order_release);
            });
            mmltk::testsupport::ScopedTestCleanup release_reader{[&] { reader_gate.Release(); }};
            REQUIRE(reader_gate.WaitEntered(1s));
            CHECK(mmltk::testsupport::await_test_promise(reader_ready, "exception reader revision") == runtime.OutputFacts().revision);
            CHECK_FALSE(reader_released.load(std::memory_order_acquire));
            throw std::runtime_error("test-side read failure");
        } catch (const std::runtime_error& error) {
            CHECK(std::string_view{error.what()} == "test-side read failure");
            caught = true;
        }
        CHECK(caught);
        CHECK(reader_released.load(std::memory_order_acquire));
        CHECK(runtime.TryAcquireOutput(baseline).valid());
    }
    CHECK(backend->planes_freed.load() == backend->planes_allocated.load());
    CHECK(backend->streams_destroyed == 1U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("multi-plane products copy atomically and reject self copy") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime source{{
        .device = 0,
        .backend = backend,
        .input_layout = ImageProductLayout::CleanAndSemantic,
        .output_layout = ImageProductLayout::CleanAndSemantic,
    }};
    source.Publish(16U, 8U, [](const auto plane, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(plane.data), 0x31, plane.descriptor.pitch_bytes * plane.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x72, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    SystemImageRuntime receiver{{
        .device = 0,
        .backend = backend,
        .input_layout = ImageProductLayout::CleanAndSemantic,
        .output_layout = ImageProductLayout::CleanAndSemantic,
    }};
    const auto paths = receiver.CopyFrom(source.Borrow());
    CHECK(paths[0U] == ImageCopyPath::SameDevice);
    CHECK(paths[1U] == ImageCopyPath::SameDevice);
    auto product = receiver.Borrow();
    REQUIRE(product.valid());
    CHECK(product.plane_count() == 2U);
    CHECK(product.plane(1U).plane().descriptor.kind == ImagePlaneKind::Semantic);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(0U).plane().data) == 0x31U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(1U).plane().data) == 0x72U);
    product = {};
    CHECK_THROWS_AS(receiver.CopyFrom(receiver.Borrow()), std::invalid_argument);
}
TEST_CASE("receiver-owned missing planes initialize before borrowed-source copies") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime source{{.device = 0, .backend = backend}};
    source.Publish(8U, 4U, [](const auto clean, auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x37, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    SystemImageRuntime receiver{{.device = 0, .backend = backend, .input_layout = ImageProductLayout::CleanAndSemantic}};
    CHECK_THROWS_AS(receiver.CopyInputFrom(source.Borrow()), std::invalid_argument);
    const auto before = backend->same_copies.load();
    const auto failing_initializer = [&](const auto plane, auto) {
        CHECK(plane.descriptor.kind == ImagePlaneKind::Semantic);
        CHECK(backend->same_copies.load() == before);
        throw std::runtime_error("initializer failure");
    };
    CHECK_THROWS(receiver.CopyInputFrom(source.Borrow(), failing_initializer));
    CHECK(backend->same_copies.load() == before);
    CHECK_FALSE(receiver.BorrowInput().valid());
    const auto paths = receiver.CopyInputFrom(source.Borrow(), [&](const auto plane, auto) {
        CHECK(backend->same_copies.load() == before);
        std::memset(reinterpret_cast<void*>(plane.data), 0, plane.descriptor.pitch_bytes * plane.descriptor.height);
    });
    CHECK(paths[0U] == ImageCopyPath::SameDevice);
    CHECK(backend->same_copies.load() == before + 1U);
    const auto product = receiver.BorrowInput();
    REQUIRE(product.valid());
    REQUIRE(product.plane_count() == 2U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(0U).plane().data) == 0x37U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(1U).plane().data) == 0U);
}
TEST_CASE("failed single-plane mutations invalidate receiver storage") {
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext context{0, backend};
    ImageStream stream{context};
    ImageBuffer receiver{context};
    receiver.Write(stream, ImagePlaneKind::Clean, 8U, 8U, [](auto, auto) {});
    REQUIRE(receiver.Borrow().valid());
    CHECK_THROWS(receiver.Write(stream, ImagePlaneKind::Clean, 16U, 16U, [](auto, auto) { throw std::runtime_error("submit failure"); }));
    CHECK_FALSE(receiver.Borrow().valid());
    SystemImageRuntime source{{.device = 0, .backend = backend}};
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    receiver.Write(stream, ImagePlaneKind::Clean, 8U, 8U, [](auto, auto) {});
    backend->FailAfter(FakeImageBackend::FailurePoint::Copy);
    CHECK_THROWS(receiver.CopyFrom(stream, std::move(source.Borrow()).TakePlane(0U)));
    CHECK_FALSE(receiver.Borrow().valid());
}
TEST_CASE("failed product growth never exposes mixed planes") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{
        .device = 0,
        .backend = backend,
        .output_layout = ImageProductLayout::CleanAndSemantic,
    }};
    runtime.Publish(8U, 8U, [](auto, auto, auto) {});
    REQUIRE(runtime.Borrow().valid());
    backend->FailAfter(FakeImageBackend::FailurePoint::AllocatePlane, 1U);
    CHECK_THROWS(runtime.Publish(16U, 16U, [](auto, auto, auto) {}));
    CHECK(runtime.OutputFacts().revision == 0U);
    CHECK_FALSE(runtime.Borrow().valid());
}
TEST_CASE("partial layered receiver copies settle before failure releases source custody") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto clean, auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 17, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 93, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    auto receiver = make_clean_semantic_runtime(backend);
    const auto settled = backend->synchronized.load();
    backend->FailAfter(FakeImageBackend::FailurePoint::Copy, 1U);
    CHECK_THROWS_WITH(receiver.CopyFrom(source.Borrow()), "injected image backend failure");
    CHECK(backend->same_copies.load() == 1U);
    CHECK(backend->synchronized.load() == settled + 1U);
    CHECK_FALSE(receiver.Borrow().valid());
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    CHECK(receiver.CopyFrom(source.Borrow())[0U] == ImageCopyPath::SameDevice);
}
TEST_CASE("scalar receiver copies settle an enqueued read before event-record failure escapes") {
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext context{0, backend};
    ImageStream stream{context};
    ImageBuffer source{context}, receiver{context};
    source.Write(stream, ImagePlaneKind::Clean, 8U, 8U,
                 [](auto clean, auto) { std::memset(reinterpret_cast<void*>(clean.data), 17, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    const auto settled = backend->synchronized.load();
    backend->FailAfter(FakeImageBackend::FailurePoint::RecordEvent);
    CHECK_THROWS_WITH(receiver.CopyFrom(stream, source.Borrow()), "injected image backend failure");
    CHECK(backend->same_copies.load() == 1U);
    CHECK(backend->synchronized.load() == settled + 1U);
    CHECK_FALSE(receiver.Borrow().valid());
    CHECK(receiver.CopyFrom(stream, source.Borrow()) == ImageCopyPath::SameDevice);
}
void check_deferred_source_custody(SystemImageRuntime& source, const mmltk::testsupport::TestGate& event_gate) {
    REQUIRE(event_gate.WaitEntered(std::chrono::seconds{1}));
    CHECK(source.OutputFacts().revision == 1U);
    SystemImageRuntime::CompletedOutput baseline;
    // The reader acquired its borrow on the asynchronous task. This thread
    // probes reservation without recursively acquiring that task's locks.
    REQUIRE_FALSE(source.TryAcquireOutput(baseline).valid());
}
TEST_CASE("external image readers await a delayed producer while retaining its exact product") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_events = true;
    auto event_gate = backend->HoldEventWaits("deferred producer event wait");
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    DeviceContext context{0, backend};
    ImageStream stream{context};
    auto reading = std::async(std::launch::async, [&stream, &source] {
        auto product = source.Borrow();
        stream.Await(product);
        return product.plane(0U).revision();
    });
    mmltk::testsupport::ScopedTestCleanup release_reader{[&] {
        event_gate->Release();
        backend->CompleteEvents();
    }};
    check_deferred_source_custody(source, *event_gate);
    event_gate->Release();
    backend->CompleteEvents();
    CHECK(mmltk::testsupport::await_test_future(reading, "external reader completion") == 1U);
    CHECK_THROWS_AS(stream.Await(BorrowedImageProductReadView{}), std::invalid_argument);
    auto other = make_clean_semantic_runtime(std::make_shared<FakeImageBackend>());
    other.Publish(8U, 8U, [](auto, auto, auto) {});
    CHECK_THROWS_AS(stream.Await(other.Borrow()), std::invalid_argument);
}
TEST_CASE("terminal product custody retains pixels without blocking source shutdown or later writers") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    source->Publish(
        8U, 8U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 73, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    auto borrowed = source->Borrow();
    const auto data = borrowed.plane(0U).plane().data;
    borrowed.Quarantine();
    CHECK_FALSE(borrowed.valid());
    CHECK_FALSE(source->Borrow().valid());
    CHECK_THROWS_WITH(source->Publish(8U, 8U, [](auto, auto, auto) {}), "image product storage is quarantined");
    source.reset();
    CHECK(backend->planes_freed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(*reinterpret_cast<const unsigned char*>(data) == 73U);
    // The fake has no pending GPU work; explicitly ending custody is safe here.
    borrowed = {};
    CHECK(backend->planes_freed == 1U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("receiver completion releases product access across threads but retains physical storage") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = std::make_unique<SystemImageRuntime>(
        SystemImageRuntimeConfig{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 1U});
    source->Publish(8U, 8U, [](auto, auto, auto) {});
    std::atomic<unsigned> available{0U};
    source->SetOutputAvailableSink([&] { available.fetch_add(1U); });
    std::optional<ImageProductReadCompletion> completion;
    std::future<void> writer, callback;
    mmltk::testsupport::ScopedTestCleanup release_completion{[&] {
        if (completion) completion->Complete();
    }};
    // Conversion unlocks this thread's shared locks; the callback owns only
    // the counted receiver access, so completing it on another thread is valid.
    completion.emplace(source->Borrow());
    CHECK(completion->pending());
    const auto before = available.load();
    SystemImageRuntime::CompletedOutput baseline;
    REQUIRE_FALSE(source->TryAcquireOutput(baseline).valid());
    writer = std::async(std::launch::async, [&] { source->Publish(8U, 8U, [](auto, auto, auto) {}); });
    callback = std::async(std::launch::async, [&] { completion->Complete(); });
    mmltk::testsupport::await_test_future(callback, "receiver completion callback");
    CHECK_FALSE(completion->pending());
    CHECK(available.load() > before);
    // A one-slot producer can reuse its planes before diagnostic ownership of
    // the old physical handles is destroyed, without waiting for another sync.
    REQUIRE(writer.wait_for(1s) == std::future_status::ready);
    writer.get();
    CHECK(backend->planes_allocated == 2U);
    completion->Complete();
    source.reset();
    CHECK(backend->planes_freed == 0U);
    completion.reset();
    CHECK(backend->planes_freed == 2U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("unproved completion quarantines access without releasing physical custody") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    source->Publish(8U, 8U, [](auto, auto, auto) {});
    std::optional<ImageProductReadCompletion> completion;
    completion.emplace(source->Borrow());
    completion->Quarantine();
    CHECK_FALSE(completion->pending());
    CHECK_FALSE(source->Borrow().valid());
    CHECK_THROWS_WITH(source->Publish(8U, 8U, [](auto, auto, auto) {}), "image product storage is quarantined");
    source.reset();
    CHECK(backend->planes_freed == 0U);
    completion.reset();
    CHECK(backend->planes_freed == 1U);
}
TEST_CASE("unprovable receiver copies quarantine source storage and preserve ordinary runtime retirement") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    auto receiver = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    source->Publish(8U, 8U, [](auto, auto, auto) {});
    backend->FailPersistently(FakeImageBackend::FailurePoint::Bind);
    CHECK_THROWS_WITH(receiver->CopyFrom(source->Borrow()), "injected image backend failure");
    CHECK_FALSE(source->Borrow().valid());
    CHECK_THROWS(source->Publish(8U, 8U, [](auto, auto, auto) {}));
    // Restore the fake's established completion boundary before ordinary teardown.
    backend->FailAfter(FakeImageBackend::FailurePoint::None);
    source.reset();
    CHECK(backend->planes_freed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
    receiver.reset();
    CHECK(backend->planes_freed == 2U);
    CHECK(backend->contexts_destroyed == 2U);
}
TEST_CASE("a quarantined scalar read retains its allocation after source destruction") {
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext context{0, backend};
    ImageStream stream{context};
    auto source = std::make_unique<ImageBuffer>(context);
    source->Write(stream, ImagePlaneKind::Clean, 8U, 8U,
                  [](auto clean, auto) { std::memset(reinterpret_cast<void*>(clean.data), 29, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    auto borrowed = source->Borrow();
    const auto data = borrowed.plane().data;
    borrowed.Quarantine();
    CHECK_THROWS_WITH(source->Write(stream, ImagePlaneKind::Clean, 8U, 8U, [](auto, auto) {}), "image storage is quarantined");
    source.reset();
    CHECK(backend->planes_freed == 0U);
    CHECK(*reinterpret_cast<const unsigned char*>(data) == 29U);
    borrowed = {};
    CHECK(backend->planes_freed == 1U);
}
// CLEANUP-IGNORE: This receiver-copy case owns its gate and lease through copy completion; the other case tests a direct reader.
TEST_CASE("receiver retains a product lease through deferred source completion") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_events = true;
    auto event_gate = backend->HoldEventWaits("deferred producer event wait");
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    auto receiver = make_clean_semantic_runtime(backend);
    auto copy = std::async(std::launch::async, [&] { return receiver.CopyFrom(source.Borrow()); });
    mmltk::testsupport::ScopedTestCleanup release_copy{[&] {
        event_gate->Release();
        backend->CompleteEvents();
    }};
    check_deferred_source_custody(source, *event_gate);
    event_gate->Release();
    backend->CompleteEvents();
    static_cast<void>(mmltk::testsupport::await_test_future(copy, "deferred receiver copy"));
    CHECK(receiver.OutputFacts().revision == 1U);
}
TEST_CASE("admitted image copies preserve readers and refuse full capacity without waiting", "[gpu][image]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime source{{.device = 0, .backend = backend}};
    source.Publish(
        4U, 4U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 0x37, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    SystemImageRuntime receiver{{.device = 0, .backend = backend, .output_buffer_count = 2U}};
    SystemImageRuntime::CompletedOutput baseline;
    auto first = receiver.TryAcquireOutput(baseline);
    REQUIRE(first.valid());
    CHECK(receiver.CopyFrom(first, source.Borrow())[0U] == ImageCopyPath::SameDevice);
    auto retained = receiver.CommitOutput(std::move(first));
    auto reader = retained.Borrow();
    auto second = receiver.TryAcquireOutput(baseline);
    REQUIRE(second.valid());
    static_cast<void>(receiver.CopyFrom(second, source.Borrow()));
    auto latest = receiver.CommitOutput(std::move(second));
    auto latest_reader = latest.Borrow();
    REQUIRE_FALSE(receiver.TryAcquireOutput(baseline).valid());
    std::stop_source cancelled;
    cancelled.request_stop();
    CHECK_FALSE(receiver.AcquireOutput(cancelled.get_token(), receiver.Completed()).valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(reader.plane(0U).plane().data) == 0x37U);
    CHECK_THROWS_AS(receiver.CopyFrom({}), std::invalid_argument);
    reader = {};
    retained = {};
    auto available = receiver.TryAcquireOutput(baseline);
    REQUIRE(available.valid());
    available = {};
    CHECK(receiver.Completed().revision() == latest.revision());
}
}  // namespace
}  // namespace mmltk::frameworks::gpu
