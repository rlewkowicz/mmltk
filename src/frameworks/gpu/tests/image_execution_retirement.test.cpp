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
    struct Destruction {
        bool destroyed = false;
        bool in_runtime_context = false;
    };
    RebindingReleaseModel(std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<Destruction> destruction, std::exception_ptr rebind_failure = {},
                          std::exception_ptr release_failure = {})
        : backend_(std::move(backend)),
          destruction_(std::move(destruction)),
          rebind_failure_(std::move(rebind_failure)),
          release_failure_(std::move(release_failure)) {}
    ~RebindingReleaseModel() override {
        destruction_->destroyed = true;
        destruction_->in_runtime_context = backend_->last_bound_context.load(std::memory_order_acquire) == 1U;
    }
    [[nodiscard]] Release ReleaseResources() noexcept override {
        backend_->BindContext(999U);
        if (rebind_failure_) backend_->FailAfter(FakeImageBackend::FailurePoint::Bind, 0U, rebind_failure_);
        return {.failure = release_failure_};
    }

   private:
    std::shared_ptr<FakeImageBackend> backend_;
    std::shared_ptr<Destruction> destruction_;
    std::exception_ptr rebind_failure_;
    std::exception_ptr release_failure_;
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
    auto destruction = std::make_shared<RebindingReleaseModel::Destruction>();
    backend->FailAfter(FakeImageBackend::FailurePoint::CreateStream);
    CHECK_THROWS(SystemImageRuntime(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<RebindingReleaseModel>(backend, destruction),
    }));
    CHECK(destruction->destroyed);
    CHECK(destruction->in_runtime_context);
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
    auto destruction = std::make_shared<RebindingReleaseModel::Destruction>();
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<RebindingReleaseModel>(backend, destruction),
    });
    CHECK(runtime->Retire().safe_to_destroy);
    CHECK(destruction->destroyed);
    CHECK(destruction->in_runtime_context);
}
TEST_CASE("post-release context bind failure retains model stream and context with ordered failures", "[gpu][retirement]") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto destruction = std::make_shared<RebindingReleaseModel::Destruction>();
    const bool release_fails = GENERATE(false, true);
    const auto binding_failure = std::make_exception_ptr(std::runtime_error("post-release binding failure"));
    const auto release_failure = release_fails ? std::make_exception_ptr(std::runtime_error("model release failure")) : std::exception_ptr{};
    auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
        .device = 0,
        .backend = backend,
        .model = std::make_unique<RebindingReleaseModel>(backend, destruction, binding_failure, release_failure),
    });
    const auto retirement = runtime->Retire();
    CHECK_FALSE(retirement.safe_to_destroy);
    REQUIRE(retirement.custody.valid());
    CHECK_FALSE(retirement.custody.deferred());
    CHECK(test_support::ContainsImageFailure(retirement.failure, binding_failure));
    if (release_fails) CHECK(test_support::ContainsImageFailure(retirement.failure, release_failure));
    CHECK_THROWS_WITH(std::rethrow_exception(retirement.failure), release_fails ? "model release failure" : "post-release binding failure");
    CHECK(retirement.custody.failure() == retirement.failure);
    CHECK(backend->last_bound_context == 999U);
    CHECK_FALSE(destruction->destroyed);
    CHECK(backend->synchronized == 1U);
    CHECK(backend->streams_destroyed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
    const auto binds = backend->contexts_bound.load();
    CHECK_FALSE(runtime->Retire().safe_to_destroy);
    CHECK(backend->contexts_bound == binds);
    runtime.reset();
    CHECK_FALSE(destruction->destroyed);
    CHECK(backend->streams_destroyed == 0U);
    CHECK(backend->contexts_destroyed == 0U);
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
// CLEANUP-IGNORE: These runtimes exercise distinct unsafe-retirement and last-reader ownership paths; retain their explicit lifetimes.
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
TEST_CASE("final borrowed view retires on the owning runtime path") {
    using namespace std::chrono_literals;
    // CLEANUP-IGNORE: These runtimes exercise distinct unsafe-retirement and last-reader ownership paths; retain their explicit lifetimes.
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
TEST_CASE("isolated CUDA contexts preserve stack depth across rebinding and retirement", "[gpu][hardware]") {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable");
    const bool nested = GENERATE(false, true);
    // A fresh thread gives the driver stack a known empty baseline without
    // disturbing CUDA state retained by other hardware fixtures.
    auto observed = std::async(std::launch::async, [nested] {
                        const auto check = [](CUresult status) {
                            if (status != CUDA_SUCCESS) throw std::runtime_error("CUDA context stack operation failed");
                        };
                        DeviceContext caller(0, cuda_image_copy_backend());
                        CUcontext caller_handle = nullptr;
                        check(cuCtxGetCurrent(&caller_handle));
                        if (nested) check(cuCtxPushCurrent(caller_handle));
                        bool created_current = false;
                        {
                            DeviceContext candidate(0, cuda_image_copy_backend());
                            CUcontext current = nullptr;
                            check(cuCtxGetCurrent(&current));
                            created_current = current && current != caller_handle;
                            caller.Bind();
                        }
                        CUcontext current = nullptr;
                        check(cuCtxGetCurrent(&current));
                        const bool caller_current = current == caller_handle;
                        unsigned entries = 0U;
                        while (current && entries < 4U) {
                            CUcontext popped = nullptr;
                            check(cuCtxPopCurrent(&popped));
                            ++entries;
                            check(cuCtxGetCurrent(&current));
                        }
                        return std::tuple{created_current, caller_current, entries, current == nullptr};
                    }).get();
    CHECK(std::get<0>(observed));
    CHECK(std::get<1>(observed));
    CHECK(std::get<2>(observed) == (nested ? 2U : 1U));
    CHECK(std::get<3>(observed));
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
