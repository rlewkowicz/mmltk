#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/acceptance/tests/async_test_utils.hpp"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"
#include "src/acceptance/tests/cuda_test_utils.hpp"
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
#include <unistd.h>
#include <sys/mman.h>
#include <vector>
#include "src/frameworks/gpu/tests/fake_image_backend.h"
namespace mmltk::frameworks::gpu {
namespace test_support {
struct ExternalGraphicsTimelineTestAccess final {
    static inline std::size_t imports = 0U;
    static void ImportFailure(mmltk::common::io::ScopedFd descriptor) {
        ExternalGraphicsTimeline timeline{std::move(descriptor), &FailImport};
        static_cast<void>(timeline);
    }

   private:
    static cudaError_t FailImport(cudaExternalSemaphore_t*, const cudaExternalSemaphoreHandleDesc*) noexcept {
        ++imports;
        return cudaErrorInvalidValue;
    }
};
}  // namespace test_support
namespace {
using test_support::FakeImageBackend;
using test_support::RuntimeFactory;
[[nodiscard]] SystemImageRuntime make_clean_semantic_runtime(const std::shared_ptr<FakeImageBackend>& backend) {
    return SystemImageRuntime{{
        .device = 0,
        .backend = backend,
        .output_layout = ImageProductLayout::CleanAndSemantic,
    }};
}
TEST_CASE("failed external timeline import closes its owned descriptor") {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
    const int imported = descriptors[0];
    mmltk::common::io::ScopedFd peer{descriptors[1]};
    test_support::ExternalGraphicsTimelineTestAccess::imports = 0U;
    CHECK_THROWS(test_support::ExternalGraphicsTimelineTestAccess::ImportFailure(mmltk::common::io::ScopedFd{imported}));
    CHECK(test_support::ExternalGraphicsTimelineTestAccess::imports == 1U);
    errno = 0;
    CHECK(::fcntl(imported, F_GETFD) == -1);
    CHECK(errno == EBADF);
}
class DestructionOrderModel final : public SystemImageModel {
   public:
    DestructionOrderModel(std::shared_ptr<FakeImageBackend> backend, bool& stream_settled, bool& context_retained) noexcept
        : backend_(std::move(backend)), stream_settled_(stream_settled), context_retained_(context_retained) {}
    ~DestructionOrderModel() override {
        stream_settled_ = backend_->synchronized == 1U && backend_->streams_destroyed == 0U;
        context_retained_ = backend_->contexts_destroyed == 0U;
    }

   private:
    std::shared_ptr<FakeImageBackend> backend_;
    bool& stream_settled_;
    bool& context_retained_;
};
[[nodiscard]] std::unique_ptr<SystemImageRuntime> make_destruction_order_runtime(const std::shared_ptr<FakeImageBackend>& backend, bool& stream_settled,
                                                                                 bool& context_retained) {
    return std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<DestructionOrderModel>(backend, stream_settled, context_retained),
    });
}
class FailingReleaseModel final : public SystemImageModel {
   public:
    FailingReleaseModel(std::shared_ptr<std::atomic_bool> destroyed, const bool all_released, const bool reports_failure = true,
                        std::shared_ptr<std::atomic_uint64_t> release_calls = {})
        : destroyed_(std::move(destroyed)),
          all_released_(all_released),
          failure_(reports_failure ? std::make_exception_ptr(std::runtime_error("deterministic model release failure")) : std::exception_ptr{}),
          release_calls_(std::move(release_calls)) {}
    ~FailingReleaseModel() override { destroyed_->store(true, std::memory_order_release); }
    [[nodiscard]] Release ReleaseResources() noexcept override {
        if (release_calls_) release_calls_->fetch_add(1U, std::memory_order_release);
        return {
            .all_released = all_released_,
            .failure = failure_,
        };
    }

   private:
    std::shared_ptr<std::atomic_bool> destroyed_;
    bool all_released_ = false;
    std::exception_ptr failure_;
    std::shared_ptr<std::atomic_uint64_t> release_calls_;
};
class RebindingReleaseModel final : public SystemImageModel {
   public:
    RebindingReleaseModel(std::shared_ptr<FakeImageBackend> backend, bool& destroyed_in_runtime_context)
        : backend_(std::move(backend)), destroyed_in_runtime_context_(destroyed_in_runtime_context) {}
    ~RebindingReleaseModel() override { destroyed_in_runtime_context_ = backend_->last_bound_context.load(std::memory_order_acquire) == 1U; }
    [[nodiscard]] Release ReleaseResources() noexcept override {
        backend_->BindContext(999U);
        return {};
    }

   private:
    std::shared_ptr<FakeImageBackend> backend_;
    bool& destroyed_in_runtime_context_;
};
TEST_CASE("direct image contexts and high-water planes are independent") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime first{{.device = 0, .backend = backend}};
    SystemImageRuntime second{{.device = 0, .backend = backend}};
    CHECK(backend->contexts_created == 2U);
    first.Publish(32U, 24U, [](auto, auto, auto) {});
    first.Publish(16U, 12U, [](auto, auto, auto) {});
    CHECK(backend->planes_allocated == 1U);
    CHECK(first.OutputFacts().capacity_width == 32U);
    CHECK(first.OutputFacts().capacity_height == 24U);
    first.Publish(64U, 24U, [](auto, auto, auto) {});
    CHECK(backend->planes_allocated == 2U);
}
TEST_CASE("runtime terminal custody is local to each physical aggregate") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto model_destroyed = std::make_shared<std::atomic_bool>(false);
    std::vector<SystemImageRuntime::UnsafeCustody> retained;
    for (std::size_t attempt = 0U; attempt != 64U; ++attempt) {
        auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
            .device = 0,
            .backend = backend,
            .model = std::make_unique<FailingReleaseModel>(model_destroyed, false),
        });
        auto retirement = runtime->Retire();
        REQUIRE_FALSE(retirement.safe_to_destroy);
        REQUIRE(retirement.custody.valid());
        retained.push_back(std::move(retirement.custody));
        runtime.reset();
    }
    CHECK(retained.size() == 64U);
    CHECK(backend->contexts_created == 64U);
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(backend->streams_destroyed == 0U);
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
    retained.clear();
    CHECK(backend->contexts_destroyed == 0U);
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
}
TEST_CASE("partial runtime construction releases its model and established context") {
    auto backend = std::make_shared<FakeImageBackend>();
    bool model_destroyed_in_context = false;
    backend->FailAfter(FakeImageBackend::FailurePoint::CreateStream);
    CHECK_THROWS(SystemImageRuntime(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<RebindingReleaseModel>(backend, model_destroyed_in_context),
    }));
    CHECK(model_destroyed_in_context);
    CHECK(backend->contexts_destroyed == 1U);
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("context creation failure retains the adopted model and original typed failure") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto model_destroyed = std::make_shared<std::atomic_bool>(false);
    auto release_calls = std::make_shared<std::atomic_uint64_t>(0U);
    backend->FailAfter(FakeImageBackend::FailurePoint::CreateContext);
    std::exception_ptr failure;
    try {
        static_cast<void>(SystemImageRuntime{SystemImageRuntimeConfig{
            .device = 0,
            .backend = backend,
            .model = std::make_unique<FailingReleaseModel>(model_destroyed, false, true, release_calls),
        }});
    } catch (...) { failure = std::current_exception(); }
    auto custody = SystemImageRuntime::UnsafeConstruction(failure);
    REQUIRE(custody);
    CHECK(custody->valid());
    CHECK(custody->failure());
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
    CHECK(release_calls->load(std::memory_order_acquire) == 0U);
    CHECK(backend->contexts_created == 0U);
    CHECK(backend->contexts_destroyed == 0U);
    try {
        std::rethrow_exception(custody->failure());
    } catch (const std::runtime_error& error) { CHECK(std::string_view{error.what()} == "injected image backend failure"); }
    custody.reset();
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
}
TEST_CASE("checked model release failure retains custody after safe stream completion") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto model_destroyed = std::make_shared<std::atomic_bool>(false);
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<FailingReleaseModel>(model_destroyed, false),
    });
    const auto retirement = runtime->Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    CHECK(retirement.custody.valid());
    CHECK(retirement.custody.failure());
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
    CHECK(runtime->device() == -1);
    CHECK(runtime->model() == nullptr);
    CHECK_THROWS(runtime->Borrow());
    CHECK_THROWS(runtime->OutputFacts());
    runtime.reset();
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("retained model identity receives a typed fallback failure") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto model_destroyed = std::make_shared<std::atomic_bool>(false);
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<FailingReleaseModel>(model_destroyed, false, false),
    });
    const auto retirement = runtime->Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    CHECK(retirement.custody.valid());
    CHECK(retirement.custody.failure());
    CHECK_FALSE(model_destroyed->load(std::memory_order_acquire));
    runtime.reset();
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("release error after complete identity release permits destruction") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto model_destroyed = std::make_shared<std::atomic_bool>(false);
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<FailingReleaseModel>(model_destroyed, true),
    });
    const auto retirement = runtime->Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    CHECK(model_destroyed->load(std::memory_order_acquire));
    runtime.reset();
    CHECK(backend->streams_destroyed == 1U);
}
TEST_CASE("runtime rebinds its context immediately after model release") {
    auto backend = std::make_shared<FakeImageBackend>();
    bool destroyed_in_runtime_context = false;
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<RebindingReleaseModel>(backend, destroyed_in_runtime_context),
    });
    CHECK(runtime->Retire().safe_to_destroy);
    CHECK(destroyed_in_runtime_context);
}
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
class CleanSemanticRuntime final {
   public:
    explicit CleanSemanticRuntime(const std::size_t output_buffer_count = 2U)
        : backend(std::make_shared<FakeImageBackend>()),
          runtime{{
              .device = 0,
              .backend = backend,
              .output_layout = ImageProductLayout::CleanAndSemantic,
              .output_buffer_count = output_buffer_count,
          }} {}
    std::shared_ptr<FakeImageBackend> backend;
    SystemImageRuntime runtime;
};
TEST_CASE("product candidates preserve exact committed planes until readers release them") {
    using namespace std::chrono_literals;
    CleanSemanticRuntime scenario;
    auto& runtime = scenario.runtime;
    runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x21, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x43, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    mmltk::testsupport::TestGate incumbent_gate{"incumbent product reader"};
    std::promise<std::uint64_t> incumbent_ready;
    auto incumbent = std::async(std::launch::async, [&] {
        auto read = runtime.Borrow();
        incumbent_ready.set_value(read.valid() ? read.plane(0U).revision() : 0U);
        incumbent_gate.receipt().ArriveAndWait();
        return *reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data);
    });
    mmltk::testsupport::ScopedTestCleanup release_incumbent{[&] { incumbent_gate.Release(); }};
    const auto incumbent_revision = mmltk::testsupport::await_test_promise(incumbent_ready, "incumbent revision");
    REQUIRE(incumbent_revision != 0U);
    REQUIRE(incumbent_gate.WaitEntered(1s));
    auto candidate = runtime.AcquireOutput({}, runtime.Completed());
    REQUIRE(candidate.valid());
    runtime.Publish(candidate, 8U, 8U, [](const auto, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(semantic.data), 0x65, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    const auto candidate_revision = candidate.revision();
    CHECK(candidate_revision > incumbent_revision);
    CHECK(runtime.Borrow().plane(0U).revision() == incumbent_revision);
    runtime.CommitOutput(std::move(candidate));
    auto committed = runtime.Borrow();
    REQUIRE(committed.valid());
    CHECK(committed.plane(0U).revision() == candidate_revision);
    CHECK(*reinterpret_cast<const std::uint8_t*>(committed.plane(0U).plane().data) == 0x21U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(committed.plane(1U).plane().data) == 0x65U);
    SystemImageRuntime::CompletedOutput unavailable_baseline;
    auto reservation = std::async(std::launch::async, [&] { return runtime.TryAcquireOutput(unavailable_baseline).valid(); });
    REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "two-product reader reservation"));
    std::stop_source stop;
    auto admission = std::async(std::launch::async, [&runtime, token = stop.get_token()] { return runtime.AcquireOutput(token); });
    mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
    incumbent_gate.Release();
    CHECK(mmltk::testsupport::await_test_future(incumbent, "incumbent reader release") == 0x43U);
    REQUIRE(admission.wait_for(1s) == std::future_status::ready);
    CHECK(admission.get().valid());
}
TEST_CASE("completed products retain exact pixels and can be selected again") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{
        .device = 0,
        .backend = backend,
        .output_buffer_count = 3U,
    }};
    auto first_candidate = runtime.AcquireOutput();
    REQUIRE(first_candidate.valid());
    runtime.Publish(first_candidate, 8U, 8U, [](const auto clean, const auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x31, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    auto first = runtime.CommitOutput(std::move(first_candidate));
    REQUIRE(first.valid());
    auto second_candidate = runtime.AcquireOutput();
    REQUIRE(second_candidate.valid());
    runtime.Publish(second_candidate, 8U, 8U, [](const auto clean, const auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x52, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    auto second = runtime.CommitOutput(std::move(second_candidate));
    REQUIRE(second.valid());
    REQUIRE(runtime.Borrow().valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(runtime.Borrow().plane(0U).plane().data) == 0x52U);
    runtime.SelectOutput(first);
    auto selected = runtime.Borrow();
    REQUIRE(selected.valid());
    CHECK(selected.plane(0U).revision() == first.revision());
    CHECK(*reinterpret_cast<const std::uint8_t*>(selected.plane(0U).plane().data) == 0x31U);
    auto later = second.Borrow();
    REQUIRE(later.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(later.plane(0U).plane().data) == 0x52U);
    auto remaining = std::async(std::launch::async, [&] { return runtime.AcquireOutput(); });
    CHECK(mmltk::testsupport::await_test_future(remaining, "remaining product slot").valid());
}
TEST_CASE("failed candidate growth retains the committed product and later initializes every plane") {
    CleanSemanticRuntime scenario;
    auto& backend = scenario.backend;
    auto& runtime = scenario.runtime;
    runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x17, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x29, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    const auto committed_revision = runtime.OutputFacts().revision;
    {
        auto candidate = runtime.AcquireOutput({}, runtime.Completed());
        REQUIRE(candidate.valid());
        backend->FailAfter(FakeImageBackend::FailurePoint::AllocatePlane, 1U);
        CHECK_THROWS(runtime.Publish(candidate, 16U, 16U, [](auto, auto, auto) {}));
    }
    auto retained = runtime.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == committed_revision);
    CHECK(*reinterpret_cast<const std::uint8_t*>(retained.plane(0U).plane().data) == 0x17U);
    retained = {};
    auto replacement = runtime.AcquireOutput({}, runtime.Completed());
    REQUIRE(replacement.valid());
    runtime.Publish(replacement, 16U, 16U, [](const auto clean, const auto, auto) { *reinterpret_cast<std::uint8_t*>(clean.data) = 0x7BU; });
    runtime.CommitOutput(std::move(replacement));
    auto completed = runtime.Borrow();
    REQUIRE(completed.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) == 0x7BU);
    CHECK(*(reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) + 1U) == 0U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(1U).plane().data) == 0U);
}
TEST_CASE("candidate baselines remain exact across cached selection and handle moves") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 3U}};
    const auto publish = [&](std::uint8_t value) {
        auto candidate = runtime.AcquireOutput();
        runtime.Publish(candidate, 8U, 8U, [value](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), value, clean.descriptor.pitch_bytes * clean.descriptor.height);
        });
        return runtime.CommitOutput(std::move(candidate));
    };
    auto first = publish(0x19U);
    auto second = publish(0x37U);
    auto copied = second;
    auto moved = std::move(copied);
    CHECK_FALSE(copied.valid());
    CHECK(moved.revision() == second.revision());
    auto candidate = runtime.AcquireOutput({}, std::move(moved));
    auto working = std::move(candidate);
    CHECK_FALSE(candidate.valid());
    runtime.SelectOutput(first);
    runtime.Publish(working, 8U, 8U, [](auto, auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(semantic.data), 0x55, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    CHECK(runtime.Completed().revision() == first.revision());
    auto completed = runtime.CommitOutput(std::move(working));
    CHECK_FALSE(working.valid());
    auto read = completed.Borrow();
    REQUIRE(read.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(0U).plane().data) == 0x37U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data) == 0x55U);
    CHECK(read.plane(0U).revision() == completed.revision());
    CHECK(read.plane(1U).revision() == completed.revision());
    CHECK_THROWS_AS(runtime.SelectOutput({}), std::invalid_argument);
    CHECK_THROWS_AS(runtime.CommitOutput({}), std::invalid_argument);
    SystemImageRuntime foreign{{.device = 0, .backend = backend}};
    CHECK_THROWS_AS(foreign.SelectOutput(first), std::invalid_argument);
    CHECK_THROWS_AS(foreign.AcquireOutput({}, first), std::invalid_argument);
    auto foreign_candidate = foreign.AcquireOutput();
    CHECK_THROWS_AS(runtime.Publish(foreign_candidate, 8U, 8U, [](auto, auto, auto) {}), std::invalid_argument);
    CHECK_THROWS_AS(runtime.CommitOutput(std::move(foreign_candidate)), std::invalid_argument);
    CHECK(foreign_candidate.valid());
}
TEST_CASE("clean-only candidate preservation excludes invalid semantics and rolls back without disturbing readers") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 4U}};
    // CLEANUP-IGNORE: This publication establishes rollback/source-watch evidence, unlike the fixture's failed-growth baseline.
    runtime.Publish(8U, 8U, [](auto clean, auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x17, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x29, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    auto baseline = runtime.Completed();
    mmltk::testsupport::TestGate reader_gate{"clean-only rollback reader"};
    std::promise<void> reader_ready;
    auto reader = std::async(std::launch::async, [&] {
        auto read = baseline.Borrow();
        backend->watched_copy_source.store(read.plane(1U).plane().data);
        reader_ready.set_value();
        reader_gate.receipt().ArriveAndWait();
        return *reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data);
    });
    mmltk::testsupport::ScopedTestCleanup release_reader{[&] { reader_gate.Release(); }};
    mmltk::testsupport::await_test_promise(reader_ready, "clean-only source custody");
    const auto copied = backend->same_copies.load();
    {
        auto candidate = runtime.AcquireOutput({}, baseline, ImagePlanePreservation::Clean);
        CHECK_THROWS_WITH(runtime.Publish(candidate, 8U, 8U,
                                          [](auto, auto semantic, auto) {
                                              std::memset(reinterpret_cast<void*>(semantic.data), 0x58,
                                                          semantic.descriptor.pitch_bytes * semantic.descriptor.height);
                                              throw std::runtime_error("semantic preparation failed");
                                          }),
                          "semantic preparation failed");
    }
    CHECK(runtime.Completed().revision() == baseline.revision());
    CHECK(backend->same_copies.load() == copied + 1U);
    CHECK(backend->watched_source_copies.load() == 0U);
    auto candidate = runtime.AcquireOutput({}, baseline, ImagePlanePreservation::Clean);
    runtime.Publish(candidate, 8U, 8U, [](auto, auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(semantic.data), 0x68, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    auto completed = runtime.CommitOutput(std::move(candidate)).Borrow();
    CHECK(backend->same_copies.load() == copied + 2U);
    CHECK(backend->watched_source_copies.load() == 0U);
    reader_gate.Release();
    CHECK(mmltk::testsupport::await_test_future(reader, "clean-only reader release") == 0x29U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) == 0x17U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(1U).plane().data) == 0x68U);
}
TEST_CASE("single-slot candidates expose only committed selection") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend}};
    const auto absent = [&] {
        CHECK_FALSE(runtime.Completed().valid());
        CHECK(runtime.OutputFacts().revision == 0U);
        CHECK_FALSE(runtime.Borrow().valid());
    };
    absent();
    runtime.Publish(8U, 8U, [](auto, auto, auto) {});
    const auto original = runtime.OutputFacts().revision;
    {
        auto candidate = runtime.AcquireOutput({}, runtime.Completed());
        absent();
    }
    CHECK(runtime.Completed().revision() == original);
    CHECK(runtime.OutputFacts().revision == original);
    CHECK(runtime.Borrow().valid());
    {
        auto candidate = runtime.AcquireOutput({}, runtime.Completed());
        absent();
        runtime.Publish(candidate, 8U, 8U, [](auto, auto, auto) {});
        absent();
    }
    absent();
    auto candidate = runtime.AcquireOutput();
    absent();
    runtime.Publish(candidate, 8U, 8U, [](auto, auto, auto) {});
    absent();
    auto completed = runtime.CommitOutput(std::move(candidate));
    CHECK(completed.revision() > original);
    CHECK(runtime.Completed().revision() == completed.revision());
    CHECK(runtime.OutputFacts().revision == completed.revision());
    CHECK(runtime.Borrow().plane(0U).revision() == completed.revision());
    CHECK(backend->planes_allocated == 1U);
}
TEST_CASE("quarantined cached products reject reads and selection while retaining physical custody") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(
        SystemImageRuntimeConfig{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U});
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    auto healthy = runtime->Completed();
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    auto cached = runtime->Completed();
    auto copy = cached;
    auto borrowed = cached.Borrow();
    auto plane = std::move(borrowed).TakePlane(1U);
    borrowed = {};
    plane.Quarantine();
    CHECK_FALSE(cached.valid());
    CHECK_FALSE(copy.valid());
    CHECK(cached.revision() == 0U);
    CHECK_FALSE(cached.Borrow().valid());
    CHECK_FALSE(runtime->Completed().valid());
    CHECK(runtime->OutputFacts().revision == 0U);
    CHECK_FALSE(runtime->Borrow().valid());
    CHECK_THROWS_AS(runtime->SelectOutput(cached), std::invalid_argument);
    CHECK_THROWS_WITH(runtime->AcquireOutput(), "image product storage is quarantined");
    runtime->SelectOutput(healthy);
    CHECK(runtime->Completed().revision() == healthy.revision());
    CHECK(runtime->Borrow().valid());
    runtime.reset();
    CHECK(backend->planes_freed == 0U);
    healthy = {};
    CHECK(backend->planes_freed == 2U);
    cached = {};
    copy = {};
    CHECK(backend->planes_freed == 2U);
    CHECK(backend->contexts_destroyed == 0U);
    plane = {};
    CHECK(backend->planes_freed == 4U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("single-slot semantic replacement reuses clean pixels after its detached final reader") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic}};
    runtime.Publish(
        8U, 8U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 0x26, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    auto baseline = runtime.Completed();
    auto borrowed = baseline.Borrow();
    auto plane = std::move(borrowed).TakePlane(0U);
    borrowed = {};
    auto reservation = std::async(std::launch::async, [&] { return runtime.TryAcquireOutput(baseline).valid(); });
    REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "held-reader reservation"));
    std::stop_source stop;
    auto admission = std::async(std::launch::async, [&runtime, baseline = std::move(baseline), token = stop.get_token()]() mutable {
        return runtime.AcquireOutput(token, std::move(baseline));
    });
    mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
    plane = {};
    const auto status = admission.wait_for(1s);
    if (status != std::future_status::ready) stop.request_stop();
    REQUIRE(status == std::future_status::ready);
    auto candidate = admission.get();
    REQUIRE(candidate.valid());
    runtime.Publish(candidate, 8U, 8U, [](auto, auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(semantic.data), 0x48, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    auto completed = runtime.CommitOutput(std::move(candidate));
    auto read = completed.Borrow();
    REQUIRE(read.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(0U).plane().data) == 0x26U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data) == 0x48U);
    CHECK(backend->planes_allocated == 2U);
    CHECK(backend->same_copies == 0U);
}
TEST_CASE("retained completed handles bound admission and stopping releases its wait") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend}};
    runtime.Publish(8U, 8U, [](auto, auto, auto) {});
    auto retained = runtime.Completed();
    SystemImageRuntime::CompletedOutput unavailable_baseline;
    REQUIRE_FALSE(runtime.TryAcquireOutput(unavailable_baseline).valid());
    // The public reservation attempt proves capacity pressure. Cancellation
    // must settle with custody still held whether it wins before or after the
    // private condition-variable wait registers; no scheduling delay proves
    // which interleaving occurred.
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return runtime.AcquireOutput(stop.get_token()); });
    mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
    stop.request_stop();
    REQUIRE(waiting.wait_for(1s) == std::future_status::ready);
    CHECK_FALSE(waiting.get().valid());
    CHECK(retained.valid());
    CHECK(runtime.OutputFacts().revision == retained.revision());
    CHECK(backend->planes_allocated == 1U);
}
TEST_CASE("completed custody retains only its slot and context after pool retirement") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend, .output_buffer_count = 2U});
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    auto retained = runtime->Completed();
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    runtime->SetOutputAvailableSink([] { throw std::runtime_error("notification sink failure"); });
    auto copy = retained;
    copy = {};
    runtime.reset();
    CHECK(backend->planes_freed == 1U);
    CHECK(backend->contexts_destroyed == 0U);
    REQUIRE(retained.Borrow().valid());
    retained = {};
    CHECK(backend->planes_freed == 2U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("producer sequences reject zero and exhaustion without overwriting completed pixels") {
    CHECK_THROWS_AS(ImageProductRevisionSequence(0U), std::invalid_argument);
    auto backend = std::make_shared<FakeImageBackend>();
    auto revisions = std::make_shared<ImageProductRevisionSequence>(std::numeric_limits<std::uint64_t>::max() - 1U);
    SystemImageRuntime runtime{{.device = 0, .backend = backend, .product_revisions = revisions}};
    runtime.Publish(8U, 8U, [](auto, auto, auto) {});
    const auto revision = runtime.OutputFacts().revision;
    CHECK_THROWS_AS(runtime.Publish(8U, 8U, [](auto, auto, auto) {}), std::overflow_error);
    CHECK(runtime.Completed().revision() == revision);
    SystemImageRuntime replacement{{.device = 0, .backend = backend, .product_revisions = revisions}};
    CHECK_THROWS_AS(replacement.Publish(8U, 8U, [](auto, auto, auto) {}), std::overflow_error);
}
TEST_CASE("completed execution failure remains typed after a later successful settlement") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend}};
    const auto execution = std::make_exception_ptr(std::runtime_error("execution identity"));
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, execution);
    CHECK_THROWS_AS(runtime.Publish(8U, 8U, [](auto, auto, auto) {}), ImageStreamExecutionFailure);
    CHECK_FALSE(runtime.Completed().valid());
    const auto retirement = runtime.Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK(is_image_execution_failure(retirement.failure));
    CHECK(test_support::ContainsImageFailure(retirement.failure, execution));
}
TEST_CASE("image failure aggregation preserves initiating settlement retirement and policy identities") {
    const auto initiating = std::make_exception_ptr(std::runtime_error("initiating"));
    const auto settlement = std::make_exception_ptr(std::runtime_error("settlement"));
    const auto retirement = std::make_exception_ptr(std::runtime_error("retirement"));
    const auto policy = std::make_exception_ptr(std::runtime_error("policy restoration"));
    const auto execution = std::make_exception_ptr(ImageStreamExecutionFailure(settlement));
    auto failure = combine_image_failures(initiating, execution);
    failure = combine_image_failures(failure, retirement);
    failure = combine_image_failures(failure, policy);
    CHECK(is_image_execution_failure(failure));
    for (const auto& expected : {initiating, settlement, retirement, policy}) CHECK(test_support::ContainsImageFailure(failure, expected));
    CHECK_THROWS_WITH(std::rethrow_exception(failure), "initiating");
    CHECK(combine_image_failures(initiating, {}) == initiating);
    CHECK(combine_image_failures({}, initiating) == initiating);
    CHECK(combine_image_failures(initiating, initiating) == initiating);
}
TEST_CASE("stream settlement retains every distinct failure behind its sticky execution classification") {
    auto backend = std::make_shared<FakeImageBackend>();
    DeviceContext context{0, backend};
    ImageStream stream{context};
    const auto first = std::make_exception_ptr(std::runtime_error("first settlement"));
    const auto second = std::make_exception_ptr(std::runtime_error("second settlement"));
    const auto submission = std::make_exception_ptr(std::runtime_error("submission"));
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, first);
    CHECK(test_support::ContainsImageFailure(stream.Settle().failure, first));
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream, 0U, second);
    try {
        stream.RethrowAfterSettlement(submission);
        FAIL("settlement failure was not propagated");
    } catch (const ImageStreamExecutionFailure& failure) {
        CHECK(failure.primary());
        for (const auto& expected : {submission, first, second}) CHECK(test_support::ContainsImageFailure(failure.primary(), expected));
        CHECK_THROWS_WITH(std::rethrow_exception(failure.primary()), "submission");
    }
    const auto settled = stream.Settle();
    CHECK(settled.completion_reached);
    CHECK(is_image_execution_failure(settled.failure));
    CHECK(test_support::ContainsImageFailure(settled.failure, first));
    CHECK(test_support::ContainsImageFailure(settled.failure, second));
}
TEST_CASE("typed image failure inspection follows primary then secondary branches without changing identities") {
    const auto first = std::make_exception_ptr(std::invalid_argument("first typed failure"));
    const auto second = std::make_exception_ptr(std::invalid_argument("second typed failure"));
    const auto execution = std::make_exception_ptr(ImageStreamExecutionFailure(second));
    const auto tree = std::make_exception_ptr(ImageFailure(first, execution));
    CHECK(find_image_failure<std::invalid_argument>(tree) == first);
    CHECK(find_image_failure<ImageStreamExecutionFailure>(tree) == execution);
    CHECK_FALSE(find_image_failure<std::out_of_range>(tree));
    CHECK_FALSE(find_image_failure<std::invalid_argument>({}));
    CHECK(is_image_execution_failure(tree));
    const auto combined = combine_image_failures(std::make_exception_ptr(std::runtime_error("outer")), tree);
    CHECK(is_image_execution_failure(combined));
    CHECK(find_image_failure<std::invalid_argument>(combined) == first);
    CHECK(test_support::ContainsImageFailure(combined, second));
}
TEST_CASE("failed product completion invalidates the whole transaction") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    auto receiver = make_clean_semantic_runtime(backend);
    backend->FailAfter(FakeImageBackend::FailurePoint::RecordEvent);
    CHECK_THROWS(receiver.CopyFrom(source.Borrow()));
    CHECK_FALSE(receiver.Borrow().valid());
    receiver.Publish(8U, 8U, [](auto, auto, auto) {});
    backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream);
    CHECK_THROWS(receiver.CopyFrom(source.Borrow()));
    CHECK_FALSE(receiver.Borrow().valid());
}
TEST_CASE("image teardown retains its aggregate when context settlement cannot be established") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    backend->FailAfter(FakeImageBackend::FailurePoint::Bind);
    const auto retirement = runtime->Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    CHECK(retirement.custody.valid());
    CHECK_NOTHROW(runtime.reset());
    CHECK(backend->contexts_destroyed == 0U);
    CHECK(backend->streams_destroyed == 0U);
}
TEST_CASE("receiver stream settles before its model and remains valid during model retirement") {
    auto backend = std::make_shared<FakeImageBackend>();
    bool stream_settled = false;
    bool context_retained = false;
    auto runtime = make_destruction_order_runtime(backend, stream_settled, context_retained);
    runtime->BeginWork();
    runtime.reset();
    CHECK(stream_settled);
    CHECK(context_retained);
    CHECK(backend->streams_destroyed == 1U);
    CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("completed stream execution failure retires once and remains reportable") {
    auto backend = std::make_shared<FakeImageBackend>();
    bool stream_settled = false;
    bool context_retained = false;
    auto runtime = make_destruction_order_runtime(backend, stream_settled, context_retained);
    runtime->BeginWork();
    backend->FailPersistently(FakeImageBackend::FailurePoint::SynchronizeStream);
    const auto retirement = runtime->Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK(retirement.failure);
    runtime.reset();
    CHECK(stream_settled);
    CHECK(context_retained);
    CHECK(backend->streams_destroyed == 1U);
    CHECK(backend->synchronized == 1U);
    CHECK(backend->contexts_bound >= 2U);
}
TEST_CASE("imported image release is ordered and leaves an inert wrapper") {
    using test_support::ImportedImageBufferTestAccess;
    ImportedImageBufferTestAccess::Reset();
    ImportedImageBuffer buffer;
    mmltk::common::io::ScopedFd backing(::memfd_create("import-backing", MFD_CLOEXEC));
    const int retained = backing.get();
    REQUIRE(retained >= 0);
    ImportedImageBufferTestAccess::Adopt(buffer, std::move(backing));
    CHECK(ImportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ImportedImageBufferTestAccess::allocation_releases == 1U);
    CHECK(buffer.empty());
    CHECK_FALSE(buffer.owns_resources());
    CHECK(ImportedImageBufferTestAccess::last_freed_base == 22U);
    CHECK(::fcntl(retained, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(ImportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ImportedImageBufferTestAccess::allocation_releases == 1U);
}
TEST_CASE("failed imported image release remains finite and inert") {
    using test_support::ImportedImageBufferTestAccess;
    ImportedImageBufferTestAccess::Reset();
    ImportedImageBufferTestAccess::unmap_result = CUDA_ERROR_UNKNOWN;
    ImportedImageBuffer buffer;
    ImportedImageBufferTestAccess::Adopt(buffer);
    CHECK(ImportedImageBufferTestAccess::Release(buffer) == cudaErrorUnknown);
    CHECK(buffer.release_failure() == cudaErrorUnknown);
    CHECK(ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ImportedImageBufferTestAccess::allocation_releases == 0U);
    CHECK(buffer.empty());
    CHECK_FALSE(buffer.owns_resources());
    CHECK(ImportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ImportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ImportedImageBufferTestAccess::allocation_releases == 0U);
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
TEST_CASE("retained candidates expose allocation-local storage without clear or baseline copy", "[gpu][product][retained]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 3U});
    const auto fill = [](const std::uint8_t value) {
        return [value](auto clean, auto semantic, auto) {
            for (auto plane : {clean, semantic})
                std::memset(reinterpret_cast<void*>(plane.data), value, plane.descriptor.pitch_bytes * plane.descriptor.height);
        };
    };
    SystemImageRuntime::CompletedOutput baseline;
    auto first = runtime.TryAcquireOutput(baseline);
    CHECK(first.allocations()[0].identity == 0U);
    runtime.PublishRetained(first, 8U, 8U, fill(17U));
    const auto first_allocation = first.allocations();
    auto gallery = runtime.CommitOutput(std::move(first));
    auto second = runtime.TryAcquireOutput(baseline);
    runtime.PublishRetained(second, 8U, 8U, fill(33U));
    auto detail = runtime.CommitOutput(std::move(second));
    auto third = runtime.TryAcquireOutput(baseline);
    runtime.PublishRetained(third, 8U, 8U, fill(49U));
    auto selected = runtime.CommitOutput(std::move(third));
    auto held = gallery.Borrow();
    gallery = {};
    CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
    held = {};
    auto reused = runtime.TryAcquireOutput(baseline);
    REQUIRE(reused.valid());
    CHECK(reused.allocations() == first_allocation);
    runtime.PublishRetained(reused, 8U, 8U, [](auto clean, auto semantic, auto) {
        CHECK(*reinterpret_cast<const std::uint8_t*>(clean.data) == 17U);
        CHECK(*reinterpret_cast<const std::uint8_t*>(semantic.data) == 17U);
        *reinterpret_cast<std::uint8_t*>(clean.data) = 71U;
    });
    CHECK(reused.allocations() == first_allocation);
    gallery = runtime.CommitOutput(std::move(reused));
    CHECK(backend->plane_clears == 0U);
    CHECK(backend->same_copies == 0U);
    runtime.SelectOutput(detail);
    CHECK(runtime.Completed().revision() == detail.revision());
    gallery = {};
    auto grown = runtime.TryAcquireOutput(baseline);
    REQUIRE(grown.allocations() == first_allocation);
    runtime.PublishRetained(grown, 16U, 9U, fill(85U));
    CHECK(grown.allocations()[0].owner == first_allocation[0].owner);
    CHECK(grown.allocations()[0].identity != first_allocation[0].identity);
    CHECK(grown.allocations()[0].width == 16U);
    CHECK(grown.allocations()[0].height == 9U);
    CHECK(runtime.Completed().revision() == detail.revision());
}
TEST_CASE("failed retained publication preserves the selected product and exposes touched candidate storage", "[gpu][product][retained]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend, .output_buffer_count = 2U});
    runtime.Publish(
        4U, 4U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 7, clean.descriptor.pitch_bytes * clean.descriptor.height); });
    const auto selected = runtime.Completed();
    SystemImageRuntime::CompletedOutput baseline;
    auto candidate = runtime.TryAcquireOutput(baseline);
    CHECK_THROWS(runtime.PublishRetained(candidate, 4U, 4U, [](auto clean, auto, auto) {
        *reinterpret_cast<std::uint8_t*>(clean.data) = 123U;
        throw std::runtime_error("partial atlas write");
    }));
    const auto allocation = candidate.allocations();
    CHECK(candidate.revision() == 0U);
    CHECK(runtime.Completed().revision() == selected.revision());
    candidate = {};
    candidate = runtime.TryAcquireOutput(baseline);
    CHECK(candidate.allocations() == allocation);
    runtime.PublishRetained(candidate, 4U, 4U, [](auto clean, auto, auto) {
        CHECK(*reinterpret_cast<const std::uint8_t*>(clean.data) == 123U);
        std::memset(reinterpret_cast<void*>(clean.data), 31, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    CHECK(runtime.CommitOutput(std::move(candidate)).valid());
}
TEST_CASE("held current display leaves replaceable overflow and exact readers close both slots", "[gpu][product][retained]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime({.device = 0, .backend = backend, .output_buffer_count = 2U});
    const auto fill = [](std::uint8_t value) {
        return [value](auto clean, auto, auto) {
            std::memset(reinterpret_cast<void*>(clean.data), value, clean.descriptor.pitch_bytes * clean.descriptor.height);
        };
    };
    runtime.Publish(4U, 4U, fill(11U));
    auto current = runtime.Borrow();
    const auto current_pointer = current.plane(0U).plane().data;
    auto baseline = runtime.Completed();
    auto overflow = runtime.TryAcquireOutput(baseline);
    REQUIRE(overflow.valid());
    runtime.Publish(overflow, 4U, 4U, fill(22U));
    baseline = runtime.CommitOutput(std::move(overflow));
    const auto overflow_allocation = baseline.ObserveWorkspace().product_owner;
    const auto copies = backend->same_copies.load();
    auto replacement = runtime.TryAcquireOutput(baseline);
    REQUIRE(replacement.valid());
    runtime.Publish(replacement, 4U, 4U, fill(33U));
    baseline = runtime.CommitOutput(std::move(replacement));
    CHECK(baseline.ObserveWorkspace().product_owner == overflow_allocation);
    CHECK(backend->same_copies == copies);
    CHECK(*reinterpret_cast<const std::uint8_t*>(current_pointer) == 11U);
    auto encoded = baseline.Borrow();
    REQUIRE(encoded.valid());
    CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
    REQUIRE(baseline.valid());
    encoded = {};
    replacement = runtime.TryAcquireOutput(baseline);
    REQUIRE(replacement.valid());
    replacement = {};
    current = {};
    CHECK(runtime.TryAcquireOutput(baseline).valid());
}
TEST_CASE("final borrowed view retires on the owning runtime path") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    auto borrowed = runtime->Borrow();
    auto retirement = std::async(std::launch::async, [owner = std::move(runtime)]() mutable { owner.reset(); });
    mmltk::testsupport::ScopedTestCleanup release_borrow{[&] { borrowed = {}; }};
    CHECK(backend->contexts_destroyed == 0U);
    borrowed = {};
    REQUIRE(retirement.wait_for(1s) == std::future_status::ready);
    retirement.get();
    CHECK(backend->contexts_destroyed == 1U);
}
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
TEST_CASE("adopted image contexts validate complete ownership and retain independent custody") {
    auto backend = std::make_shared<FakeImageBackend>();
    std::optional<DeviceContext> context{std::in_place, 0, backend};
    // Omitting the backend selects the canonical CUDA owner; it cannot adopt a
    // fake context merely because no explicit backend pointer was supplied.
    CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{.device = 0, .adopted_context = context}), std::invalid_argument);
    CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{.device = 1, .backend = backend, .adopted_context = context}), std::invalid_argument);
    CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{.device = 0, .backend = std::make_shared<FakeImageBackend>(), .adopted_context = context}),
                    std::invalid_argument);
    CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{
                        .device = 0, .backend = backend, .context_mode = DeviceContextMode::PrimaryInterop, .adopted_context = context}),
                    std::invalid_argument);
    DeviceExecution unexpected;
    unexpected.device = 0;
    unexpected.placement.numa_node = 99;
    CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{.device = 0, .backend = backend, .execution = unexpected, .adopted_context = context}),
                    std::invalid_argument);
    {
        SystemImageRuntime runtime({.device = 0, .backend = backend, .adopted_context = context});
        CHECK(runtime.UsesContext(*context));
        {
            DeviceContext different(0, backend);
            CHECK_FALSE(runtime.UsesContext(different));
        }
        context.reset();
        CHECK(backend->contexts_destroyed == 1U);
        runtime.Publish(2U, 2U, [](auto, auto, auto) {});
        CHECK(runtime.Completed().valid());
    }
    CHECK(backend->contexts_created == 2U);
    CHECK(backend->contexts_destroyed == 2U);
}
TEST_CASE("canonical adoption validates ownership before changing the caller binding", "[gpu][hardware]") {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable");
    REQUIRE(cuInit(0U) == CUDA_SUCCESS);
    DeviceContext caller(0, cuda_image_copy_backend());
    caller.Bind();
    CUcontext before{};
    REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
    {
        DeviceContext adopted(0, cuda_image_copy_backend());
        caller.Bind();
        REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
        CHECK_THROWS_AS(SystemImageRuntime(SystemImageRuntimeConfig{.device = 0, .backend = std::make_shared<FakeImageBackend>(), .adopted_context = adopted}),
                        std::invalid_argument);
        CUcontext current{};
        REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
        CHECK(current == before);
        // Runtime execution deliberately binds its owner. An enclosing caller
        // context guard restores the caller across successful construction/retirement.
        REQUIRE(cuCtxPushCurrent(before) == CUDA_SUCCESS);
        {
            SystemImageRuntime runtime({.device = 0, .adopted_context = adopted});
            CHECK(runtime.UsesContext(adopted));
        }
        REQUIRE(cuCtxPopCurrent(&current) == CUDA_SUCCESS);
        REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
        CHECK(current == before);
    }
}
}  // namespace
}  // namespace mmltk::frameworks::gpu
namespace mmltk::frameworks::gpu {
TEST_CASE("exact CUDA context transactions restore or retain their physical owner once", "[gpu][context]") {
    enum class Failure { None, Query, Bind, RestoreOnce, RestoreAlways, BindAndRestore, LostRestore };
    const auto failure =
        GENERATE(Failure::None, Failure::Query, Failure::Bind, Failure::RestoreOnce, Failure::RestoreAlways, Failure::BindAndRestore, Failure::LostRestore);
    const bool same_context = GENERATE(false, true);
    struct Driver final {
        CUcontext caller = reinterpret_cast<CUcontext>(1U);
        CUcontext current = caller;
        CUcontext target;
        Failure failure;
        unsigned sets = 0U;
        unsigned restores = 0U;
        unsigned terminal = 0U;
        bool work = false;
    } driver{.target = reinterpret_cast<CUcontext>(same_context ? 1U : 2U), .failure = failure};
    const CudaContextApi api{&driver,
                             [](void* value, CUcontext* current) noexcept {
                                 auto& injected = *static_cast<Driver*>(value);
                                 if (injected.failure == Failure::Query) return CUDA_ERROR_INVALID_CONTEXT;
                                 *current = injected.current;
                                 return CUDA_SUCCESS;
                             },
                             [](void* value, CUcontext current) noexcept {
                                 auto& injected = *static_cast<Driver*>(value);
                                 if (++injected.sets == 1U && (injected.failure == Failure::Bind || injected.failure == Failure::BindAndRestore))
                                     return CUDA_ERROR_INVALID_CONTEXT;
                                 if (injected.sets > 1U && current == injected.caller) {
                                     ++injected.restores;
                                     if (injected.failure == Failure::LostRestore) return CUDA_ERROR_CONTEXT_IS_DESTROYED;
                                     if (injected.failure == Failure::RestoreAlways || injected.failure == Failure::BindAndRestore ||
                                         (injected.failure == Failure::RestoreOnce && injected.restores == 1U))
                                         return CUDA_ERROR_INVALID_CONTEXT;
                                 }
                                 injected.current = current;
                                 return CUDA_SUCCESS;
                             }};
    CudaContextScope scope({&driver, [](void* value) noexcept { ++static_cast<Driver*>(value)->terminal; }}, api);
    const auto run = [&] {
        scope.Run([&] {
            scope.Select(driver.target);
            driver.work = true;
        });
    };
    if (failure == Failure::None)
        CHECK_NOTHROW(run());
    else
        CHECK_THROWS(run());
    CHECK(driver.work == (failure != Failure::Query && failure != Failure::Bind && failure != Failure::BindAndRestore));
    const bool terminal =
        failure == Failure::Query || failure == Failure::RestoreAlways || failure == Failure::BindAndRestore || failure == Failure::LostRestore;
    CHECK(scope.terminal() == terminal);
    if (failure == Failure::LostRestore) CHECK(driver.restores == 1U);
    CHECK(driver.terminal == (terminal ? 1U : 0U));
    if (!terminal) CHECK(driver.current == driver.caller);
    if (terminal) {
        scope.Abandon();
        CHECK(driver.terminal == 1U);
        const auto calls = driver.sets;
        CHECK_THROWS(scope.Run([&] { scope.Select(driver.target); }));
        CHECK(driver.sets == calls);
    }
}
TEST_CASE("unproved execution retains runtime before any retirement GPU command", "[gpu][context][retirement]") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime::UnsafeCustody retained;
    unsigned ingress_calls = 0U;
    struct Model final : SystemImageModel {
        explicit Model(unsigned& count) : count_(count) {}
        void StopIngress() noexcept override { ++count_; }
        unsigned& count_;
    };
    {
        SystemImageRuntime runtime({.device = 0, .backend = backend, .model = std::make_unique<Model>(ingress_calls), .output_buffer_count = 2U});
        runtime.Publish(2U, 2U, [](auto, auto, auto) {});
        const auto settled = backend->synchronized.load();
        const auto destroyed = backend->streams_destroyed.load();
        auto outcome = runtime.Retire(std::make_exception_ptr(CudaContextFailure(true)));
        CHECK_FALSE(outcome.safe_to_destroy);
        CHECK(outcome.custody.valid());
        CHECK_FALSE(outcome.custody.deferred());
        CHECK_THROWS(runtime.BeginWork());
        CHECK_FALSE(runtime.Retire().safe_to_destroy);
        CHECK(backend->synchronized == settled);
        CHECK(backend->streams_destroyed == destroyed);
        retained = std::move(outcome.custody);
    }
    CHECK(ingress_calls == 0U);
    CHECK(backend->streams_destroyed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
}
}  // namespace mmltk::frameworks::gpu
