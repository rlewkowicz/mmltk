#include "src/frameworks/gpu/system_image_worker.h"
#include "src/frameworks/gpu/exported_image_buffer.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#include "src/frameworks/gpu/tests/fake_image_backend.h"

namespace mmltk::frameworks::gpu {
namespace test_support {

// CLEANUP-IGNORE: This test access probe owns exported-buffer cleanup facts independently from Live receiver probes.
struct ExportedImageBufferTestAccess final {
    static inline CUresult unmap_result = CUDA_SUCCESS;
    static inline CUresult address_result = CUDA_SUCCESS;
    static inline CUresult allocation_result = CUDA_SUCCESS;
    static inline std::size_t unmaps = 0U;
    static inline std::size_t address_frees = 0U;
    static inline std::size_t allocation_releases = 0U;

    static void Reset() noexcept {
        unmap_result = CUDA_SUCCESS;
        address_result = CUDA_SUCCESS;
        allocation_result = CUDA_SUCCESS;
        unmaps = 0U;
        address_frees = 0U;
        allocation_releases = 0U;
    }

    static void Adopt(ExportedImageBuffer& buffer) noexcept {
        buffer.allocation_ = 11U;
        buffer.address_ = 22U;
        buffer.device_ptr_ = 22U;
        buffer.allocation_size_ = 4096U;
        buffer.reserved_bytes_ = 4096U;
        buffer.pitch_bytes_ = 64U;
        buffer.width_ = 16U;
        buffer.height_ = 16U;
        buffer.mapping_active_ = true;
    }

    static cudaError_t Release(ExportedImageBuffer& buffer) noexcept {
        return buffer.Release({
            .unmap = &Unmap,
            .free_address = &FreeAddress,
            .release_allocation = &ReleaseAllocation,
        });
    }

   private:
    static CUresult Unmap(CUdeviceptr, std::size_t) noexcept {
        ++unmaps;
        return unmap_result;
    }
    static CUresult FreeAddress(CUdeviceptr, std::size_t) noexcept {
        ++address_frees;
        return address_result;
    }
    static CUresult ReleaseAllocation(CUmemGenericAllocationHandle) noexcept {
        ++allocation_releases;
        return allocation_result;
    }
};

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

[[nodiscard]] std::unique_ptr<SystemImageRuntime> make_destruction_order_runtime(const std::shared_ptr<FakeImageBackend>& backend,
                                                                                 bool& stream_settled, bool& context_retained) {
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
          failure_(reports_failure ? std::make_exception_ptr(std::runtime_error("deterministic model release failure"))
                                   : std::exception_ptr{}),
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
    ~RebindingReleaseModel() override {
        destroyed_in_runtime_context_ = backend_->last_bound_context.load(std::memory_order_acquire) == 1U;
    }
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
    CHECK(first.output().capacity_width() == 32U);
    CHECK(first.output().capacity_height() == 24U);
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
    CHECK_THROWS(runtime->output());
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
    const auto copied_revision = staged.output().revision();
    staged.Publish(16U, 8U, [](auto, auto, auto) {});
    CHECK(staged.output().revision() > copied_revision);
    CHECK(backend->pinned_allocated == 1U);
    CHECK(backend->pinned_receiver_device == 2);
    CHECK(staged.output().staging_capacity_bytes() == 16U * 8U * 4U);
    CHECK(backend->staged_downloads == 2U);
    CHECK(backend->staged_uploads == 2U);
}

TEST_CASE("borrowed image storage remains stable until receiver completion") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{.device = 0, .backend = backend}};
    runtime.Publish(8U, 8U, [](auto, auto, auto) {});
    auto borrowed = runtime.Borrow();

    auto writer = std::async(std::launch::async, [&runtime] { runtime.Publish(16U, 16U, [](auto, auto, auto) {}); });
    CHECK(writer.wait_for(10ms) == std::future_status::timeout);
    borrowed = {};
    CHECK(writer.wait_for(1s) == std::future_status::ready);
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
    const auto product = receiver.Borrow();
    REQUIRE(product.valid());
    CHECK(product.plane_count() == 2U);
    CHECK(product.plane(1U).plane().descriptor.kind == ImagePlaneKind::Semantic);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(0U).plane().data) == 0x31U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(1U).plane().data) == 0x72U);
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
    CHECK(runtime.output().revision() == 0U);
    CHECK_FALSE(runtime.Borrow().valid());
}

TEST_CASE("product candidates preserve exact committed planes until readers release them") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{
        .device = 0,
        .backend = backend,
        .output_layout = ImageProductLayout::CleanAndSemantic,
        .output_buffer_count = 2U,
    }};
    runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x21, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x43, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    auto incumbent = runtime.Borrow();
    REQUIRE(incumbent.valid());
    const auto incumbent_revision = incumbent.plane(0U).revision();

    auto candidate = runtime.AcquireOutput();
    REQUIRE(candidate.valid());
    runtime.Publish(candidate, 8U, 8U, ImageCandidateInitialization::PreserveSelected,
                    [](const auto, const auto semantic, auto) {
                        std::memset(reinterpret_cast<void*>(semantic.data), 0x65,
                                    semantic.descriptor.pitch_bytes * semantic.descriptor.height);
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
    CHECK(*reinterpret_cast<const std::uint8_t*>(incumbent.plane(1U).plane().data) == 0x43U);

    std::stop_source stop;
    auto admission = std::async(std::launch::async, [&runtime, token = stop.get_token()] {
        return runtime.AcquireOutput(token);
    });
    CHECK(admission.wait_for(10ms) == std::future_status::timeout);
    incumbent = {};
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
    runtime.Publish(first_candidate, 8U, 8U, ImageCandidateInitialization::Empty, [](const auto clean, const auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x31, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    auto first = runtime.CommitOutput(std::move(first_candidate));
    REQUIRE(first.valid());

    auto second_candidate = runtime.AcquireOutput();
    REQUIRE(second_candidate.valid());
    runtime.Publish(second_candidate, 8U, 8U, ImageCandidateInitialization::Empty, [](const auto clean, const auto, auto) {
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

    auto remaining = runtime.AcquireOutput();
    CHECK(remaining.valid());
}

TEST_CASE("failed candidate growth retains the committed product and later initializes every plane") {
    auto backend = std::make_shared<FakeImageBackend>();
    SystemImageRuntime runtime{{
        .device = 0,
        .backend = backend,
        .output_layout = ImageProductLayout::CleanAndSemantic,
        .output_buffer_count = 2U,
    }};
    runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 0x17, clean.descriptor.pitch_bytes * clean.descriptor.height);
        std::memset(reinterpret_cast<void*>(semantic.data), 0x29, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    });
    const auto committed_revision = runtime.output().revision();

    {
        auto candidate = runtime.AcquireOutput();
        REQUIRE(candidate.valid());
        backend->FailAfter(FakeImageBackend::FailurePoint::AllocatePlane, 1U);
        CHECK_THROWS(runtime.Publish(candidate, 16U, 16U, ImageCandidateInitialization::PreserveSelected,
                                     [](auto, auto, auto) {}));
    }
    auto retained = runtime.Borrow();
    REQUIRE(retained.valid());
    CHECK(retained.plane(0U).revision() == committed_revision);
    CHECK(*reinterpret_cast<const std::uint8_t*>(retained.plane(0U).plane().data) == 0x17U);
    retained = {};

    auto replacement = runtime.AcquireOutput();
    REQUIRE(replacement.valid());
    runtime.Publish(replacement, 16U, 16U, ImageCandidateInitialization::PreserveSelected,
                    [](const auto clean, const auto, auto) {
                        *reinterpret_cast<std::uint8_t*>(clean.data) = 0x7BU;
                    });
    runtime.CommitOutput(std::move(replacement));
    auto completed = runtime.Borrow();
    REQUIRE(completed.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) == 0x7BU);
    CHECK(*(reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) + 1U) == 0U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(1U).plane().data) == 0U);
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

TEST_CASE("exported image release is ordered and leaves an inert wrapper") {
    using test_support::ExportedImageBufferTestAccess;
    ExportedImageBufferTestAccess::Reset();
    ExportedImageBuffer buffer;
    ExportedImageBufferTestAccess::Adopt(buffer);

    CHECK(ExportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ExportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ExportedImageBufferTestAccess::address_frees == 1U);
    CHECK(ExportedImageBufferTestAccess::allocation_releases == 1U);
    CHECK(buffer.empty());
    CHECK_FALSE(buffer.owns_resources());

    CHECK(ExportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ExportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ExportedImageBufferTestAccess::address_frees == 1U);
    CHECK(ExportedImageBufferTestAccess::allocation_releases == 1U);
}

TEST_CASE("failed exported image release remains finite and inert") {
    using test_support::ExportedImageBufferTestAccess;
    ExportedImageBufferTestAccess::Reset();
    ExportedImageBufferTestAccess::unmap_result = CUDA_ERROR_UNKNOWN;
    ExportedImageBuffer buffer;
    ExportedImageBufferTestAccess::Adopt(buffer);

    CHECK(ExportedImageBufferTestAccess::Release(buffer) == cudaErrorUnknown);
    CHECK(ExportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ExportedImageBufferTestAccess::address_frees == 0U);
    CHECK(ExportedImageBufferTestAccess::allocation_releases == 1U);
    CHECK(buffer.empty());
    CHECK_FALSE(buffer.owns_resources());

    CHECK(ExportedImageBufferTestAccess::Release(buffer) == cudaSuccess);
    CHECK(ExportedImageBufferTestAccess::unmaps == 1U);
    CHECK(ExportedImageBufferTestAccess::allocation_releases == 1U);
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
    source.Write(stream, ImagePlaneKind::Clean, 8U, 8U, [](auto clean, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 17, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
    const auto settled = backend->synchronized.load();
    backend->FailAfter(FakeImageBackend::FailurePoint::RecordEvent);
    CHECK_THROWS_WITH(receiver.CopyFrom(stream, source.Borrow()), "injected image backend failure");
    CHECK(backend->same_copies.load() == 1U);
    CHECK(backend->synchronized.load() == settled + 1U);
    CHECK_FALSE(receiver.Borrow().valid());
    CHECK(receiver.CopyFrom(stream, source.Borrow()) == ImageCopyPath::SameDevice);
}

TEST_CASE("external image readers await a delayed producer while retaining its exact product") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    backend->defer_events = true;
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    DeviceContext context{0, backend};
    ImageStream stream{context};
    auto borrowed = source.Borrow();
    auto reading = std::async(std::launch::async, [&stream, product = std::move(borrowed)] {
        stream.Await(product);
        return product.plane(0U).revision();
    });
    CHECK(reading.wait_for(10ms) == std::future_status::timeout);
    backend->CompleteEvents();
    REQUIRE(reading.wait_for(1s) == std::future_status::ready);
    CHECK(reading.get() == 1U);
    CHECK_THROWS_AS(stream.Await({}), std::invalid_argument);
    auto other = make_clean_semantic_runtime(std::make_shared<FakeImageBackend>());
    other.Publish(8U, 8U, [](auto, auto, auto) {});
    CHECK_THROWS_AS(stream.Await(other.Borrow()), std::invalid_argument);
}

TEST_CASE("terminal product custody retains pixels without blocking source shutdown or later writers") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto source = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    source->Publish(8U, 8U, [](auto clean, auto, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 73, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
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
    source->Write(stream, ImagePlaneKind::Clean, 8U, 8U, [](auto clean, auto) {
        std::memset(reinterpret_cast<void*>(clean.data), 29, clean.descriptor.pitch_bytes * clean.descriptor.height);
    });
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
    auto source = make_clean_semantic_runtime(backend);
    source.Publish(8U, 8U, [](auto, auto, auto) {});
    auto receiver = make_clean_semantic_runtime(backend);
    auto copy = std::async(std::launch::async, [&] { return receiver.CopyFrom(source.Borrow()); });
    CHECK(copy.wait_for(10ms) == std::future_status::timeout);
    backend->CompleteEvents();
    REQUIRE(copy.wait_for(1s) == std::future_status::ready);
    CHECK(receiver.output().revision() == 1U);
}

TEST_CASE("final borrowed view retires on the owning runtime path") {
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeImageBackend>();
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend});
    runtime->Publish(8U, 8U, [](auto, auto, auto) {});
    auto borrowed = runtime->Borrow();
    auto retirement = std::async(std::launch::async, [owner = std::move(runtime)]() mutable { owner.reset(); });
    CHECK(retirement.wait_for(10ms) == std::future_status::timeout);
    CHECK(backend->contexts_destroyed == 0U);
    borrowed = {};
    CHECK(retirement.wait_for(1s) == std::future_status::ready);
    CHECK(backend->contexts_destroyed == 1U);
}

}  // namespace
}  // namespace mmltk::frameworks::gpu
