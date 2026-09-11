#include "src/acceptance/tests/async_test_utils.hpp"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/runtime_paths.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/browser/application_materializer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <sys/file.h>
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include <nlohmann/json.hpp>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/live/live_receiver_copy.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/common/types/generation.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/controller/presentation/detail/workspace_frame_signal.h"
#include "src/controller/presentation/detail/workspace_surface_import_channel.h"
#include "src/acceptance/tests/workspace_surface_socket_test_utils.hpp"
#include "src/controller/services/settings_system.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "filesystem_test_utils.hpp"

namespace mmltk::controller {
namespace {

using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;

TEST_CASE("Gallery demand clips four neighboring rows independently", "[explore][cache]") {
    using explore_detail::GalleryThumbnailCache;
    const auto columns = GENERATE(4U, 10U);
    ExploreViewport viewport{.extent = {columns * 32U, 96U}, .first_row = 17U, .row_count = 3U, .columns = columns};
    CHECK(GalleryThumbnailCache::WindowFirst(1000U, viewport) == 13U * columns);
    CHECK(GalleryThumbnailCache::WindowCount(1000U, viewport) == 11U * columns);
    viewport.first_row = 0U;
    CHECK(GalleryThumbnailCache::WindowFirst(1000U, viewport) == 0U);
    CHECK(GalleryThumbnailCache::WindowCount(1000U, viewport) == 7U * columns);
    viewport.first_row = 999U / columns;
    CHECK(GalleryThumbnailCache::WindowFirst(1000U, viewport) == (viewport.first_row - 4U) * columns);
    CHECK(GalleryThumbnailCache::WindowFirst(1000U, viewport) + GalleryThumbnailCache::WindowCount(1000U, viewport) == 1000U);
    CHECK(GalleryThumbnailCache::WindowCount(0U, viewport) == 0U);
    viewport.first_row = 0U;
    CHECK(GalleryThumbnailCache::WindowCount(columns + 1U, viewport) == columns + 1U);
}

TEST_CASE("Gallery cache identity survives five six five demand and reorder with bounded collision rollback", "[explore][cache]") {
    using explore_detail::GalleryThumbnailCache;
    GalleryThumbnailCache cache;
    const GalleryThumbnailCache::Identity identity{.dataset = 19U, .seed = 23U, .extent = 32U};
    cache.Configure(14U, identity);
    const auto capacity = cache.capacity();
    auto meaning = std::make_shared<const explore_detail::GalleryTileMeaning>();
    std::array<std::uint32_t, 14U> images{};
    std::iota(images.begin(), images.end(), 0U);
    cache.Admit(std::span{images}.first(13U), 0U);
    for (auto image : std::span{images}.first(13U))
        cache.Complete(image, image, meaning, 1U, 1U, 0U);
    const auto original_slot = cache.Slot(4U);
    for (const auto rows : {6U, 5U, 6U, 5U}) {
        cache.Configure(rows + 8U, identity);
        cache.BeginUpdate();
        cache.Admit(std::span{images}.first(rows + 8U), 0U);
        REQUIRE(cache.Find(4U));
        CHECK(cache.Find(4U)->bank == 1U);
        CHECK(cache.Find(4U)->meaning == meaning);
        CHECK(cache.Slot(4U) == original_slot);
        cache.CommitUpdate();
        CHECK(cache.capacity() == capacity);
        CHECK(cache.size() == 14U);
    }
    std::ranges::reverse(images);
    cache.BeginUpdate();
    cache.Admit(images, 100U);
    REQUIRE(cache.Find(4U));
    CHECK(cache.Slot(109U) == original_slot);
    CHECK(cache.Position(4U) == 109U);
    cache.RollbackUpdate();
    CHECK(cache.Position(4U) == 4U);
    CHECK(cache.Slot(4U) == original_slot);

    // Every hash bucket collides. All incumbent slots are needed on rollback,
    // while the candidate pins a disjoint replacement demand.
    for (std::size_t index = 0U; index < images.size(); ++index)
        images[index] = static_cast<std::uint32_t>((index + 1U) * 29U);
    cache.BeginUpdate();
    cache.Admit(images, 50U);
    for (std::size_t index = 0U; index < images.size(); ++index) {
        const auto slot = cache.Slot(50U + index);
        const auto old = cache.Protected(slot);
        cache.Complete(50U + index, images[index], meaning, 2U, static_cast<std::uint8_t>(1U - old.bank));
        CHECK(cache.Protected(slot).compiled_index == old.compiled_index);
        REQUIRE(cache.Find(images[index]));
    }
    cache.RollbackUpdate();
    for (std::uint32_t image = 0U; image < 13U; ++image) {
        REQUIRE(cache.Find(image));
        CHECK(cache.Find(image)->semantic_identity == 1U);
        CHECK(cache.Find(image)->bank == 1U);
    }
}

TEST_CASE("Gallery cache candidate replacement preserves incumbent meaning and content identity", "[explore][cache]") {
    using explore_detail::GalleryThumbnailCache;
    explore_detail::GalleryProductState incumbent;
    auto identity = GalleryThumbnailCache::Identity{.dataset = 31U, .seed = 37U, .extent = 16U};
    incumbent.cache.Configure(20U, identity);
    auto meaning = std::make_shared<explore_detail::GalleryTileMeaning>();
    meaning->annotations.resize(2U);
    meaning->runs.push_back({.start = 5U, .length = 3U});
    const std::uint64_t overlay = 1U;
    incumbent.cache.Complete(4U, 7U, meaning, overlay);
    const auto bytes = incumbent.cache.MeaningBytes();
    auto candidate = incumbent;
    candidate.cache.Complete(4U, 7U, meaning, 2U, 1U);
    REQUIRE(candidate.cache.Find(7U));
    CHECK(candidate.cache.Find(7U)->meaning == incumbent.cache.Find(7U)->meaning);
    CHECK(candidate.cache.Find(7U)->bank == 1U);
    CHECK(incumbent.cache.Find(7U)->bank == 0U);
    CHECK(candidate.cache.Find(7U)->semantic_identity != incumbent.cache.Find(7U)->semantic_identity);
    CHECK(candidate.cache.MeaningBytes() - candidate.cache.MetadataBytes() == bytes - incumbent.cache.MetadataBytes());

    SECTION("Changing the loaded artifact invalidates even when stable seed identity is unchanged") {
        // Only identity comparison is exercised; the cache never dereferences
        // an incarnation. Product storage retains the real artifact owner.
        identity.incarnation = reinterpret_cast<const mmltk::backend::data::CompiledDataset*>(&incumbent);
    }
    SECTION("Changing augmentation seed invalidates clean pixels") { ++identity.seed; }
    SECTION("Changing augmentation policy invalidates clean pixels") { identity.augmented = true; }
    SECTION("Changing augmentation configuration invalidates clean pixels") { identity.augmentation.enabled = true; }
    SECTION("Changing dataset identity invalidates clean pixels") { ++identity.dataset; }
    SECTION("Changing thumbnail extent invalidates clean pixels") { ++identity.extent; }
    candidate.cache.Configure(20U, identity);
    CHECK_FALSE(candidate.cache.Find(7U));
    REQUIRE(incumbent.cache.Find(7U));
    CHECK(incumbent.cache.Find(7U)->meaning->runs.front().start == 5U);
    const auto capacity = candidate.Capacity();
    candidate.Clear();
    CHECK(candidate.Size() == 0U);
    CHECK(candidate.Capacity() == capacity);
    REQUIRE(incumbent.cache.Find(7U));
}

TEST_CASE("Gallery cache memory deduplicates shared meaning across slot versions", "[explore][cache]") {
    explore_detail::GalleryThumbnailCache cache;
    cache.Configure(20U, {.extent = 8U});
    auto meaning = explore_detail::MakeGalleryShared<explore_detail::GalleryTileMeaning>();
    meaning->annotations.reserve(3U);
    meaning->runs.reserve(7U);
    const std::uint64_t key = 1U;
    cache.Complete(0U, 0U, meaning, key);
    cache.Complete(1U, 1U, meaning, key);
    const auto expected = cache.MetadataBytes() + explore_detail::GallerySharedBytes(meaning) +
                          meaning->annotations.capacity() * sizeof(decltype(meaning->annotations)::value_type) +
                          meaning->runs.capacity() * sizeof(decltype(meaning->runs)::value_type);
    CHECK(cache.MeaningBytes() == expected);
    const std::uint64_t other = 2U;
    cache.Complete(2U, 2U, meaning, other);
    CHECK(cache.MeaningBytes() == expected);
    auto candidate = cache;
    const auto incumbent_bytes = cache.MeaningBytes();
    CHECK(cache.MeaningBytes(&candidate) == incumbent_bytes + candidate.MetadataBytes());
    auto replacement = explore_detail::MakeGalleryShared<explore_detail::GalleryTileMeaning>();
    replacement->runs.reserve(11U);
    candidate.Complete(20U, 20U, replacement, other, 1U, 1U);
    CHECK(cache.MeaningBytes(&candidate) == incumbent_bytes + candidate.MetadataBytes() + explore_detail::GallerySharedBytes(replacement) +
                                                replacement->runs.capacity() * sizeof(decltype(replacement->runs)::value_type));
}

TEST_CASE("Gallery product metadata accounts both retained vector high waters and bit storage", "[explore][cache]") {
    explore_detail::GalleryProductState product;
    product.visible_indices.resize(256U);
    product.window_indices.resize(390U);
    product.priority_slots.resize(390U);
    product.active_classes.resize(kExploreClassCapacity);
    product.completed_slots.resize(256U);
    product.tile_meanings.resize(256U);
    product.plan.overlay.class_selection.classes.resize(kExploreClassCapacity);
    const auto expected = (product.visible_indices.capacity() + product.window_indices.capacity() + product.priority_slots.capacity() +
                           product.plan.overlay.class_selection.classes.capacity()) *
                              sizeof(std::uint32_t) +
                          product.active_classes.capacity() * sizeof(decltype(product.active_classes)::value_type) +
                          product.completed_slots.capacity() / 8U +
                          product.tile_meanings.capacity() * sizeof(decltype(product.tile_meanings)::value_type);
    CHECK(product.MetadataBytes() == expected);
    explore_detail::GalleryProductState inactive;
    inactive.ReserveFor(product);
    CHECK(inactive.MetadataBytes() >= expected);
    product.Clear();
    inactive.Clear();
    CHECK(product.MetadataBytes() == expected);
    CHECK(inactive.MetadataBytes() >= expected);
}

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

TEST_CASE("Committed visual frames exactly authorize one- and two-plane products") {
    auto backend = std::make_shared<FakeImageBackend>();
    mmltk::frameworks::gpu::SystemImageRuntime clean{
        {.device = 0, .backend = backend, .output_layout = mmltk::frameworks::gpu::ImageProductLayout::Clean}};
    clean.Publish(8U, 6U, [](auto, auto, auto) {});
    auto clean_product = clean.Borrow();
    REQUIRE(clean_product.valid());
    const auto clean_frame = visual_frame({PresentationSourceKind::Explore, 1U}, {8U, 6U}, clean_product.plane(0U).revision());

    CHECK(visual_product_matches_frame(clean_frame, clean_product));
    CHECK_FALSE(visual_product_matches_frame({}, clean_product));
    CHECK_FALSE(
        visual_product_matches_frame(visual_frame(clean_frame.source, clean_frame.extent, clean_frame.revision + 1U), clean_product));
    CHECK_FALSE(visual_product_matches_frame(
        visual_frame(clean_frame.source, {clean_frame.extent.width + 1U, clean_frame.extent.height}, clean_frame.revision), clean_product));
    CHECK_FALSE(visual_product_matches_frame(clean_frame, {}));

    mmltk::frameworks::gpu::SystemImageRuntime layered{
        {.device = 0, .backend = backend, .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}};
    layered.Publish(9U, 7U, [](auto, auto, auto) {});
    auto layered_product = layered.Borrow();
    REQUIRE(layered_product.valid());
    REQUIRE(layered_product.plane_count() == 2U);
    const auto layered_frame = visual_frame({PresentationSourceKind::Annotation, 1U}, {9U, 7U}, layered_product.plane(0U).revision());
    CHECK(visual_product_matches_frame(layered_frame, layered_product));
}

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

void Fill(const mmltk::frameworks::gpu::ImagePlaneView plane, const std::uint8_t value) {
    std::memset(reinterpret_cast<void*>(plane.data), value, plane.descriptor.pitch_bytes * plane.descriptor.height);
}

struct ExploreRenderGate final {
    std::atomic<std::size_t> calls{0U};
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};

struct ExploreFinalizationGate final {
    void Arm() noexcept { armed.store(true, std::memory_order_release); }
    std::atomic_bool armed{false};
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};

struct ExplorePostRenderGate final {
    explicit ExplorePostRenderGate(const std::size_t target) : target_call(target) {}
    std::size_t target_call;
    std::atomic<std::size_t> calls{0U};
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};

struct ExploreWorkProbe final {
    std::atomic<std::size_t> opens{0U};
    std::atomic<std::size_t> prepares{0U};
    std::atomic<std::size_t> renders{0U};
    std::atomic<std::size_t> rendered_count{0U};
    std::atomic<float> rendered_copy_paste_probability{0.0F};
    std::array<std::atomic<std::uint32_t>, 2U> rendered_slots{};
};

struct ExploreDetailExtentProbe final {
    VisualExtent padded{64U, 64U};
    VisualExtent original{48U, 32U};
};

[[nodiscard]] ExploreAtlasLayout test_atlas_layout(const ExploreRenderPlan& plan, const mmltk::frameworks::gpu::ImagePlaneView plane) {
    const auto side = explore_atlas_card_extent(plan.viewport);
    return {plan.viewport.first_row, plan.viewport.row_count, plane.descriptor.height / side, 0U, plan.viewport.columns, side};
}

class SynchronousExploreAlgorithm : public ExploreAlgorithm {
   public:
    void SetCurrentDemand(ExploreDemandCheck demand) override {
        if (demand_bound_) throw std::logic_error("test Explore demand rebound");
        demand_ = std::move(demand);
        demand_bound_ = true;
    }
    ExploreOutputChange OutputChange(const ExploreRenderPlan&, const ExploreOrderCandidate*) const override {
        return ExploreOutputChange::Initialize;
    }
    void AbortRenderGeneration() override { generation_ = 0U; }
    void DiscardCandidate() override {}
    void SetGalleryReadySink(GalleryReadySink) final {}
    void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation) noexcept override {}
    void PrepareOutputPublication(ExploreOutputChange, ExploreMode) final {}
    void CommitOutputPublication() noexcept final {}
    bool RollbackOutputPublication() noexcept final { return true; }
    ExploreGalleryPublication BeginGallery(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc,
                                           const mmltk::frameworks::gpu::ImagePlaneView clean,
                                           const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) final {
        RenderProduct(plan, candidate, nproc, clean, semantic, stream);
        generation_ = plan.generation;
        return {.generation = generation_, .layout = test_atlas_layout(plan, clean)};
    }
    ExploreGalleryPublication AdvanceGallery() final { return {.generation = generation_}; }
    bool HasGalleryTiles() const final { return false; }
    ExploreGalleryPublication PublishGalleryTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView,
                                                  std::uintptr_t) final {
        return {.generation = generation_};
    }
    void RenderDetail(const ExploreRenderPlan& plan, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
                      const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) final {
        RenderProduct(plan, nullptr, nproc, clean, semantic, stream);
        generation_ = 0U;
    }

   protected:
    virtual void RenderProduct(const ExploreRenderPlan&, const ExploreOrderCandidate*, std::size_t, mmltk::frameworks::gpu::ImagePlaneView,
                               mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;

   private:
    ExploreDemandCheck demand_;
    bool demand_bound_ = false;
    std::uint64_t generation_ = 0U;
};

struct ExplorePublicationDemandGate final {
    ExploreDemandCheck demand;
    std::uint64_t generation = 0U;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic_bool release_timed_out{false};
};

struct StreamingExploreAssignment final {
    std::uint64_t generation = 0U;
    std::uint32_t compiled_index = 0U;
    std::uint32_t slot = 0U;
    bool read_started = false;
    bool ready = false;
    bool gpu_pending = false;
};

struct StreamingExplorePublicationState {
    std::vector<StreamingExploreAssignment> assignments;
    std::vector<std::uint32_t> visible;
    std::vector<bool> completed_slots;
    std::vector<std::uint32_t> priority_slots;
    std::uint64_t generation = 0U;
    std::size_t nproc = 1U;
    std::size_t next_slot = 0U;
    std::size_t cumulative = 0U;
    ExploreAtlasLayout layout{};
};

struct StreamingExploreProbe final : StreamingExplorePublicationState {
    void Release(const std::uint32_t compiled_index) {
        ExploreAlgorithm::GalleryReadySink wake;
        {
            std::scoped_lock lock(mutex);
            const auto found = std::ranges::find(assignments, compiled_index, &StreamingExploreAssignment::compiled_index);
            REQUIRE(found != assignments.end());
            found->ready = true;
            wake = ready_sink;
            changed.notify_all();
        }
        if (wake) wake();
    }

    void ReleaseAll() {
        ExploreAlgorithm::GalleryReadySink wake;
        {
            std::scoped_lock lock(mutex);
            for (auto& assignment : assignments)
                assignment.ready = true;
            wake = ready_sink;
            changed.notify_all();
        }
        if (wake) wake();
    }
    void StartRead(const std::uint32_t compiled_index) {
        std::scoped_lock lock(mutex);
        const auto found = std::ranges::find(assignments, compiled_index, &StreamingExploreAssignment::compiled_index);
        REQUIRE(found != assignments.end());
        REQUIRE_FALSE(found->read_started);
        found->read_started = true;
        ++mapped_reads;
        changed.notify_all();
    }
    void AllowAllocation() {
        ExploreAlgorithm::GalleryReadySink wake;
        {
            std::scoped_lock lock(mutex);
            allocation_allowed = true;
            wake = ready_sink;
        }
        if (wake) wake();
    }
    void AllowPreRead() { SignalGate(pre_read_allowed); }
    void AllowPostRead() { SignalGate(post_read_allowed); }
    void CompleteCallbacks() {
        ExploreAlgorithm::GalleryReadySink wake;
        {
            std::scoped_lock lock(mutex);
            callback_completion_allowed = true;
            ++callback_completions;
            std::erase_if(assignments, [this](const auto& assignment) {
                if (!assignment.gpu_pending) return false;
                if (assignment.generation == generation) {
                    completed_slots[assignment.slot] = 1U;
                    ++cumulative;
                }
                return true;
            });
            wake = ready_sink;
            changed.notify_all();
        }
        if (wake) wake();
    }
    [[nodiscard]] bool Wait(std::function<bool()> predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, std::move(predicate));
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    ExploreAlgorithm::GalleryReadySink ready_sink;
    std::size_t publications = 0U;
    std::size_t stale = 0U;
    std::size_t maximum_active = 0U;
    bool allocation_allowed = false;
    bool pre_read_allowed = false;
    bool post_read_allowed = false;
    bool callback_completion_allowed = true;
    bool fail_callback_admission = false;
    std::size_t lane_preparations = 0U;
    std::size_t mapped_reads = 0U;
    std::size_t queued_closures = 0U;
    std::size_t callback_completions = 0U;
    std::size_t quiescences = 0U;
    std::size_t aborted_assignments = 0U;
    ExploreScrollDirection scroll_direction = ExploreScrollDirection::Forward;
    bool fail_next_render = false;
    bool fail_next_labels = false;
    bool fail_rollback = false;
    std::size_t rollbacks = 0U;
    std::string opened_source;
    std::shared_ptr<ExplorePostRenderGate> render_gate;
    std::shared_ptr<ExplorePublicationDemandGate> publication_gate;
    std::vector<ExploreDemandCheck> bound_demands;
    std::size_t bound_opens = 0U;

   private:
    void SignalGate(bool& gate) {
        ExploreAlgorithm::GalleryReadySink wake;
        {
            std::scoped_lock lock(mutex);
            gate = true;
            wake = ready_sink;
        }
        if (wake) wake();
    }
};

[[nodiscard]] std::vector<std::uint32_t> visible_test_order(const std::vector<std::uint32_t>& order, const ExploreViewport viewport) {
    if (!viewport.valid()) return order;
    const auto first = std::min<std::size_t>(static_cast<std::size_t>(viewport.first_row) * viewport.columns, order.size());
    const auto count = std::min<std::size_t>(static_cast<std::size_t>(viewport.row_count) * viewport.columns, order.size() - first);
    return {order.begin() + static_cast<std::ptrdiff_t>(first), order.begin() + static_cast<std::ptrdiff_t>(first + count)};
}

class ControlledStreamingExploreAlgorithm final : public ExploreAlgorithm {
    void SetCurrentDemand(ExploreDemandCheck demand) override {
        if (demand_bound_) throw std::logic_error("test streaming Explore demand rebound");
        demand_ = std::move(demand);
        demand_bound_ = true;
        std::scoped_lock lock(probe_->mutex);
        probe_->bound_demands.push_back(demand_);
    }
    ExploreOutputChange OutputChange(const ExploreRenderPlan&, const ExploreOrderCandidate*) const override {
        return ExploreOutputChange::Initialize;
    }

   public:
    explicit ControlledStreamingExploreAlgorithm(std::shared_ptr<StreamingExploreProbe> probe) : probe_(std::move(probe)) {}
    ~ControlledStreamingExploreAlgorithm() override {
        std::scoped_lock lock(probe_->mutex);
        ++probe_->quiescences;
        probe_->assignments.clear();
        probe_->ready_sink = {};
    }

    ExploreOpened Open(const std::string_view source, std::stop_token) override {
        if (!demand_bound_ || !demand_.generation()) throw std::logic_error("Explore ingress preceded demand binding");
        std::scoped_lock lock(probe_->mutex);
        ++probe_->bound_opens;
        probe_->opened_source = source;
        order_ = {0U, 1U, 2U, 3U, 4U, 5U};
        return {
            .dataset = {.image_count = 6U, .image_width = 4U, .image_height = 4U},
            .order = Visible({}),
        };
    }
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, const std::uint64_t seed, std::size_t, std::stop_token) override {
        return {.filter = filter,
                .order = {.matching_count = static_cast<std::uint32_t>(order_.size()), .shuffle_seed = seed, .visible_indices = order_},
                .generation = ++candidate_generation_};
    }
    void Commit(ExploreOrderCandidate) noexcept override {}
    void AbortRenderGeneration() override { ClearStreamingState(); }
    void DiscardCandidate() override {}
    void Reset() noexcept override {
        ClearStreamingState();
        order_.clear();
    }
    ExploreOrderFacts Visible(const ExploreViewport viewport, const ExploreOrderCandidate* candidate = nullptr) const override {
        const auto& order = candidate == nullptr ? order_ : candidate->order.visible_indices;
        return {
            .matching_count = static_cast<std::uint32_t>(order.size()),
            .visible_indices = visible_test_order(order, viewport),
        };
    }
    bool Contains(const std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
    std::optional<std::uint32_t> Adjacent(const std::uint32_t value, const std::int64_t offset) const override {
        if (!Contains(value)) return {};
        const auto count = static_cast<std::int64_t>(order_.size());
        const auto adjacent = (static_cast<std::int64_t>(value) + offset % count + count) % count;
        return static_cast<std::uint32_t>(adjacent);
    }
    void SetGalleryReadySink(GalleryReadySink sink) override {
        std::scoped_lock lock(probe_->mutex);
        probe_->ready_sink = std::move(sink);
    }
    void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation) noexcept override {}
    void PrepareOutputPublication(ExploreOutputChange, ExploreMode) override {
        std::scoped_lock lock(probe_->mutex);
        if (checkpoint_) return;
        checkpoint_.emplace(PublicationCheckpoint{
            .assignments = probe_->assignments,
            .visible = probe_->visible,
            .completed_slots = probe_->completed_slots,
            .priority_slots = probe_->priority_slots,
            .generation = probe_->generation,
            .nproc = probe_->nproc,
            .next_slot = probe_->next_slot,
            .cumulative = probe_->cumulative,
            .layout = probe_->layout,
        });
    }
    void CommitOutputPublication() noexcept override {
        checkpoint_.reset();
        if (auto gate = std::exchange(probe_->publication_gate, {})) {
            gate->demand = demand_;
            gate->generation = probe_->generation;
            gate->entered.set_value();
            gate->release_timed_out.store(gate->released.wait_for(5s) != std::future_status::ready);
        }
    }
    bool RollbackOutputPublication() noexcept override {
        GalleryReadySink wake;
        {
            std::scoped_lock lock(probe_->mutex);
            if (!checkpoint_) return true;
            ++probe_->rollbacks;
            if (probe_->fail_rollback) return false;
            ++probe_->quiescences;
            probe_->aborted_assignments += probe_->assignments.size();
            probe_->assignments = std::move(checkpoint_->assignments);
            probe_->visible = std::move(checkpoint_->visible);
            probe_->completed_slots = std::move(checkpoint_->completed_slots);
            probe_->priority_slots = std::move(checkpoint_->priority_slots);
            probe_->generation = checkpoint_->generation;
            probe_->nproc = checkpoint_->nproc;
            probe_->next_slot = checkpoint_->next_slot;
            probe_->cumulative = checkpoint_->cumulative;
            probe_->layout = checkpoint_->layout;
            checkpoint_.reset();
            wake = probe_->ready_sink;
            probe_->changed.notify_all();
        }
        if (wake) wake();
        return true;
    }
    std::vector<ExploreLabel> Labels() const override {
        std::scoped_lock lock(probe_->mutex);
        if (std::exchange(probe_->fail_next_labels, false)) throw std::runtime_error("deterministic Explore prepared-label failure");
        return {};
    }
    ExploreGalleryPublication BeginGallery(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc,
                                           const mmltk::frameworks::gpu::ImagePlaneView clean,
                                           const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
        std::shared_ptr<ExplorePostRenderGate> gate;
        {
            std::scoped_lock lock(probe_->mutex);
            if (std::exchange(probe_->fail_next_render, false)) throw std::runtime_error("deterministic streaming Explore render failure");
            gate = probe_->render_gate;
        }
        if (gate && gate->calls.fetch_add(1U, std::memory_order_acq_rel) == gate->target_call) {
            gate->entered.set_value();
            gate->released.wait();
        }
        {
            std::scoped_lock lock(probe_->mutex);
            if (probe_->generation == plan.generation) {
                RestorePixels(clean, semantic);
                PrioritizeLocked(plan);
                probe_->next_slot = 0U;
                return FactsLocked(0U);
            }
        }
        Fill(clean, 0x11U);
        Fill(semantic, 0U);
        std::scoped_lock lock(probe_->mutex);
        probe_->generation = plan.generation;
        probe_->layout = test_atlas_layout(plan, clean);
        probe_->visible = Visible(plan.viewport, candidate).visible_indices;
        probe_->completed_slots.assign(probe_->visible.size(), 0U);
        probe_->nproc = nproc;
        probe_->next_slot = 0U;
        probe_->cumulative = 0U;
        PrioritizeLocked(plan);
        return FactsLocked(0U);
    }
    ExploreGalleryPublication AdvanceGallery() override {
        std::scoped_lock lock(probe_->mutex);
        const auto before = probe_->assignments.size();
        std::erase_if(probe_->assignments,
                      [this](const auto& assignment) { return assignment.ready && assignment.generation != probe_->generation; });
        const auto discarded = before - probe_->assignments.size();
        probe_->stale += discarded;
        if (probe_->allocation_allowed) ScheduleLocked();
        for (auto& assignment : probe_->assignments) {
            if (assignment.generation != probe_->generation) continue;
            if (!assignment.read_started && probe_->pre_read_allowed) {
                assignment.read_started = true;
                ++probe_->mapped_reads;
            }
            if (assignment.read_started && probe_->post_read_allowed) assignment.ready = true;
        }
        probe_->changed.notify_all();
        return FactsLocked(discarded);
    }
    bool HasGalleryTiles() const override {
        std::scoped_lock lock(probe_->mutex);
        return std::ranges::any_of(probe_->assignments, [this](const auto& assignment) {
            return assignment.ready && !assignment.gpu_pending && assignment.generation == probe_->generation;
        });
    }
    ExploreGalleryPublication PublishGalleryTiles(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                  const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
        GalleryReadySink wake;
        ExploreGalleryPublication facts;
        {
            std::scoped_lock lock(probe_->mutex);
            if (probe_->fail_callback_admission) throw std::runtime_error("deterministic Explore callback admission failure");
            RestorePixels(clean, semantic);
            for (auto& assignment : probe_->assignments) {
                if (!assignment.ready || assignment.gpu_pending || assignment.generation != probe_->generation) continue;
                FillSlot(clean, assignment.slot, probe_->visible.size(), static_cast<std::uint8_t>(0x40U + assignment.compiled_index));
                FillSlot(semantic, assignment.slot, probe_->visible.size(), static_cast<std::uint8_t>(0x80U + assignment.compiled_index));
                assignment.gpu_pending = true;
            }
            if (probe_->callback_completion_allowed) {
                ++probe_->callback_completions;
                std::erase_if(probe_->assignments, [this](const auto& assignment) {
                    if (!assignment.gpu_pending || assignment.generation != probe_->generation) return false;
                    probe_->completed_slots[assignment.slot] = 1U;
                    ++probe_->cumulative;
                    return true;
                });
            }
            ++probe_->publications;
            facts = FactsLocked(0U);
            probe_->changed.notify_all();
            if (probe_->callback_completion_allowed) wake = probe_->ready_sink;
        }
        if (wake) wake();
        return facts;
    }
    void RenderDetail(const ExploreRenderPlan& plan, std::size_t, const mmltk::frameworks::gpu::ImagePlaneView clean,
                      const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
        Fill(clean, static_cast<std::uint8_t>(0x60U + plan.selected_image.value_or(0U)));
        Fill(semantic, 0U);
        std::scoped_lock lock(probe_->mutex);
        probe_->generation = 0U;
        probe_->assignments.clear();
    }

   private:
    using PublicationCheckpoint = StreamingExplorePublicationState;

    void RestorePixels(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic) const {
        Fill(clean, 0x11U);
        Fill(semantic, 0U);
        const auto place = [&](const std::size_t slot, const std::uint32_t image) {
            FillSlot(clean, slot, probe_->visible.size(), static_cast<std::uint8_t>(0x40U + image));
            FillSlot(semantic, slot, probe_->visible.size(), static_cast<std::uint8_t>(0x80U + image));
        };
        for (std::size_t slot = 0U; slot < probe_->completed_slots.size(); ++slot)
            if (probe_->completed_slots[slot]) place(slot, probe_->visible[slot]);
        for (const auto& assignment : probe_->assignments)
            if (assignment.gpu_pending && assignment.generation == probe_->generation) place(assignment.slot, assignment.compiled_index);
    }

    void PrioritizeLocked(const ExploreRenderPlan& plan) {
        probe_->scroll_direction = plan.scroll_direction;
        probe_->priority_slots.clear();
        if (plan.focused_image) {
            const auto focused = std::ranges::find(probe_->visible, *plan.focused_image);
            if (focused != probe_->visible.end())
                probe_->priority_slots.push_back(static_cast<std::uint32_t>(focused - probe_->visible.begin()));
        }
        for (std::size_t slot = 0U; slot != probe_->visible.size(); ++slot)
            if (probe_->priority_slots.empty() || slot != probe_->priority_slots.front())
                probe_->priority_slots.push_back(static_cast<std::uint32_t>(slot));
    }
    void ClearStreamingState() noexcept {
        std::scoped_lock lock(probe_->mutex);
        ++probe_->quiescences;
        probe_->aborted_assignments += probe_->assignments.size();
        probe_->assignments.clear();
        probe_->visible.clear();
        probe_->priority_slots.clear();
        probe_->generation = 0U;
        probe_->next_slot = 0U;
        probe_->cumulative = 0U;
    }
    static void FillSlot(const mmltk::frameworks::gpu::ImagePlaneView plane, const std::size_t slot, const std::size_t count,
                         const std::uint8_t value) {
        const auto width = plane.descriptor.width / static_cast<std::uint32_t>(std::max<std::size_t>(count, 1U));
        for (std::uint32_t y = 0U; y != plane.descriptor.height; ++y) {
            auto* row = reinterpret_cast<std::uint8_t*>(plane.data) + y * plane.descriptor.pitch_bytes;
            std::memset(row + slot * width * 4U, value, width * 4U);
        }
    }
    void ScheduleLocked() {
        while (probe_->assignments.size() < probe_->nproc && probe_->next_slot != probe_->visible.size()) {
            const auto slot = probe_->priority_slots[probe_->next_slot++];
            if (probe_->completed_slots[slot] != 0U || std::ranges::any_of(probe_->assignments, [&](const auto& work) {
                    return work.generation == probe_->generation && work.slot == slot;
                }))
                continue;
            probe_->assignments.push_back({
                .generation = probe_->generation,
                .compiled_index = probe_->visible[slot],
                .slot = slot,
            });
            ++probe_->lane_preparations;
            ++probe_->queued_closures;
        }
        probe_->maximum_active = std::max(probe_->maximum_active, probe_->assignments.size());
        probe_->changed.notify_all();
    }
    [[nodiscard]] ExploreGalleryPublication FactsLocked(const std::size_t stale) const {
        return {
            .generation = probe_->generation,
            .layout = probe_->layout,
            .ready_slots = probe_->completed_slots,
            .cumulative_tiles = probe_->cumulative,
            .remaining_tiles = probe_->visible.size() - probe_->cumulative,
            .active_pinned_bytes = probe_->assignments.size() * 64U,
            .stale_discarded = stale,
        };
    }

    std::shared_ptr<StreamingExploreProbe> probe_;
    ExploreDemandCheck demand_;
    bool demand_bound_ = false;
    std::vector<std::uint32_t> order_;
    std::uint64_t candidate_generation_ = 0U;
    std::optional<PublicationCheckpoint> checkpoint_;
};

[[nodiscard]] VisualRuntimeFactory streaming_explore_runtime_factory(std::shared_ptr<FakeImageBackend> backend,
                                                                     std::shared_ptr<StreamingExploreProbe> probe) {
    return RuntimeFactory(
        0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
        [probe = std::move(probe)] { return std::make_unique<ControlledStreamingExploreAlgorithm>(probe); }, 3U);
}

class TestExploreAlgorithm : public SynchronousExploreAlgorithm {
   public:
    explicit TestExploreAlgorithm(
        std::shared_ptr<std::atomic<std::size_t>> observed_nproc, std::shared_ptr<ExploreRenderGate> gate = {},
        std::shared_ptr<std::atomic_uint64_t> commits = {}, std::shared_ptr<ExploreFinalizationGate> finalization_gate = {},
        std::shared_ptr<ExploreWorkProbe> work_probe = {}, std::shared_ptr<ExplorePostRenderGate> post_render_gate = {},
        std::shared_ptr<ExploreDetailExtentProbe> detail_extent = {})  // CLEANUP-IGNORE: This test algorithm has its own injected controls.
        : observed_nproc_(
              std::move(observed_nproc)),  // CLEANUP-IGNORE: Distinct test algorithms directly retain their own injected controls.
          gate_(std::move(gate)),
          commits_(std::move(commits)),
          finalization_gate_(std::move(finalization_gate)),
          work_probe_(std::move(work_probe)),
          post_render_gate_(std::move(post_render_gate)),
          detail_extent_(std::move(detail_extent)) {}
    ExploreOpened Open(const std::string_view source, std::stop_token) override {
        if (work_probe_) work_probe_->opens.fetch_add(1U, std::memory_order_release);
        if (source == "/failed") throw std::runtime_error("deterministic Explore open failure");
        candidate_order_ = {0U, 1U, 2U};
        candidate_render_failure_ = source == "/render-failed";
        if (source == "/allocation-failed") throw std::bad_alloc{};
        const std::vector<contracts::ArtifactClassName> class_names =
            source == "/different-catalog" ? std::vector<contracts::ArtifactClassName>{{"animal"}, {"building"}}
                                           : std::vector<contracts::ArtifactClassName>{{"person"}, {"vehicle"}};
        return {.dataset = {.image_count = 3U, .image_width = 64U, .image_height = 64U, .class_names = class_names},
                .order = {.matching_count = static_cast<std::uint32_t>(candidate_order_.size()), .visible_indices = candidate_order_}};
    }
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, std::uint64_t seed, std::size_t, std::stop_token) override {
        if (work_probe_) work_probe_->prepares.fetch_add(1U, std::memory_order_release);
        if (filter.minimum_instances == 7U) throw std::bad_alloc{};
        if (filter.minimum_instances == 8U) candidate_render_failure_ = true;
        if (candidate_order_.empty()) candidate_order_ = order_;
        return {.filter = filter,
                .order = {.matching_count = static_cast<std::uint32_t>(candidate_order_.size()),
                          .shuffle_seed = seed,
                          .visible_indices = candidate_order_},
                .generation = ++candidate_generation_};
    }
    void Commit(ExploreOrderCandidate) noexcept override {
        order_ = std::move(candidate_order_);
        candidate_render_failure_ = false;
        if (commits_) commits_->fetch_add(1U, std::memory_order_release);
    }
    void DiscardCandidate() override {
        candidate_order_.clear();
        candidate_render_failure_ = false;
    }
    void Reset() noexcept override {
        order_.clear();
        candidate_order_.clear();
        candidate_render_failure_ = false;
        candidate_generation_ = 0U;
    }
    ExploreOrderFacts Visible(const ExploreViewport viewport, const ExploreOrderCandidate* candidate = nullptr) const override {
        const auto& order = candidate == nullptr ? order_ : candidate->order.visible_indices;
        ExploreOrderFacts facts{
            .matching_count = static_cast<std::uint32_t>(order.size()),
            .shuffle_seed = candidate == nullptr ? 0U : candidate->order.shuffle_seed,
        };
        if (candidate != nullptr) {
            if (finalization_gate_ && finalization_gate_->armed.exchange(false, std::memory_order_acq_rel)) {
                finalization_gate_->entered.set_value();
                finalization_gate_->released.wait();
            }
        }
        facts.visible_indices = visible_test_order(order, viewport);
        return facts;
    }
    bool Contains(std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
    VisualExtent DetailExtent(const ExploreRenderPlan& plan) const override {
        return detail_extent_ ? detail_extent_->padded : ExploreAlgorithm::DetailExtent(plan);
    }
    VisualRegion DetailContent(const ExploreRenderPlan&) const override {
        if (!detail_extent_) return {};
        return {(detail_extent_->padded.width - detail_extent_->original.width) / 2U,
                (detail_extent_->padded.height - detail_extent_->original.height) / 2U, detail_extent_->original.width,
                detail_extent_->original.height};
    }
    std::optional<std::uint32_t> Adjacent(std::uint32_t selected, std::int64_t offset) const override {
        return static_cast<std::uint32_t>((static_cast<std::int64_t>(selected) + offset % 3 + 3) % 3);
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc,
                       const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                       std::uintptr_t) override {
        if (candidate != nullptr && candidate_render_failure_) throw std::runtime_error("deterministic Explore candidate render failure");
        if (work_probe_) work_probe_->renders.fetch_add(1U, std::memory_order_release);
        if (work_probe_) {
            work_probe_->rendered_copy_paste_probability.store(plan.augmentation_config.copy_paste_probability, std::memory_order_release);
            const auto visible = Visible(plan.viewport, candidate).visible_indices;
            work_probe_->rendered_count.store(visible.size(), std::memory_order_release);
            for (std::size_t slot = 0U; slot != work_probe_->rendered_slots.size(); ++slot)
                work_probe_->rendered_slots[slot].store(slot < visible.size() ? visible[slot] : std::numeric_limits<std::uint32_t>::max(),
                                                        std::memory_order_release);
        }
        observed_nproc_->store(nproc, std::memory_order_release);
        if (gate_ && gate_->calls.fetch_add(1U, std::memory_order_acq_rel) == 1U) {
            gate_->entered.set_value();
            gate_->released.wait();
        }
        Fill(clean, static_cast<std::uint8_t>(plan.generation));
        Fill(semantic, 0x5aU);
        if (post_render_gate_ && post_render_gate_->calls.fetch_add(1U, std::memory_order_acq_rel) == post_render_gate_->target_call) {
            post_render_gate_->entered.set_value();
            post_render_gate_->released.wait();
        }
    }

   private:
    std::shared_ptr<std::atomic<std::size_t>> observed_nproc_;
    std::shared_ptr<ExploreRenderGate> gate_;
    std::shared_ptr<std::atomic_uint64_t> commits_;
    std::shared_ptr<ExploreFinalizationGate> finalization_gate_;
    std::shared_ptr<ExploreWorkProbe> work_probe_;
    std::shared_ptr<ExplorePostRenderGate> post_render_gate_;
    std::shared_ptr<ExploreDetailExtentProbe> detail_extent_;
    std::vector<std::uint32_t> order_;
    std::vector<std::uint32_t> candidate_order_;
    std::uint64_t candidate_generation_ = 0U;
    bool candidate_render_failure_ = false;
};

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

struct CancellationProbe final {
    std::promise<void> entered;
    std::promise<void> cancelled;
};

[[nodiscard]] bool wait_for_cancellation(const std::stop_token stop, CancellationProbe& probe) {
    probe.entered.set_value();
    std::mutex mutex;
    std::condition_variable_any changed;
    std::stop_callback wake{stop, [&changed] { changed.notify_all(); }};
    std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    if (!stop.stop_requested()) return false;
    probe.cancelled.set_value();
    return true;
}

class CancellableExploreAlgorithm final : public SynchronousExploreAlgorithm {
   public:
    explicit CancellableExploreAlgorithm(std::shared_ptr<CancellationProbe> probe, std::shared_ptr<ExploreDemandCheck> demand = {})
        : probe_(std::move(probe)), retained_demand_(std::move(demand)) {}
    void SetCurrentDemand(ExploreDemandCheck demand) override {
        if (retained_demand_) *retained_demand_ = demand;
        SynchronousExploreAlgorithm::SetCurrentDemand(std::move(demand));
    }
    ExploreOpened Open(std::string_view, const std::stop_token stop) override {
        if (opened_) {
            static_cast<void>(wait_for_cancellation(stop, *probe_));
            return {};
        }
        opened_ = true;
        order_ = {0U};
        return {.dataset = {.image_count = 1U, .image_width = 32U, .image_height = 32U, .class_names = {}}, .order = Visible({})};
    }
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, std::uint64_t seed, std::size_t, std::stop_token) override {
        return {.filter = filter, .order = Visible({}), .generation = seed + 1U};
    }
    void Commit(ExploreOrderCandidate) noexcept override {}
    void Reset() noexcept override {
        order_.clear();
        opened_ = false;
    }
    ExploreOrderFacts Visible(ExploreViewport, const ExploreOrderCandidate* candidate = nullptr) const override {
        if (candidate != nullptr) return candidate->order;
        return {.matching_count = 1U, .shuffle_seed = 0U, .visible_indices = order_};
    }
    bool Contains(std::uint32_t value) const override { return value == 0U; }
    std::optional<std::uint32_t> Adjacent(std::uint32_t, std::int64_t) const override { return 0U; }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t,
                       const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                       std::uintptr_t) override {
        Fill(clean, static_cast<std::uint8_t>(plan.generation));
        Fill(semantic, 2U);
    }

   private:
    std::shared_ptr<CancellationProbe> probe_;
    std::shared_ptr<ExploreDemandCheck> retained_demand_;
    std::vector<std::uint32_t> order_;
    bool opened_ = false;
};

struct OpenPreparationProbe final {
    explicit OpenPreparationProbe(const std::size_t blocked) : blocked_call(blocked) {}
    std::size_t blocked_call;
    std::atomic<std::size_t> prepare_calls{0U};
    std::atomic<std::size_t> commits{0U};
    std::atomic<std::size_t> resets{0U};
    std::promise<void> entered;
    std::promise<void> cancelled;
};

class CancellableOpenPreparationAlgorithm final : public SynchronousExploreAlgorithm {
   public:
    explicit CancellableOpenPreparationAlgorithm(std::shared_ptr<OpenPreparationProbe> probe) : probe_(std::move(probe)) {}
    ExploreOpened Open(std::string_view, std::stop_token) override {
        candidate_order_ = {0U};
        return {.dataset = {.image_count = 1U, .image_width = 32U, .image_height = 32U, .class_names = {{"person"}}},
                .order = {.matching_count = 1U, .visible_indices = candidate_order_}};
    }
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, const std::uint64_t seed, std::size_t,
                                        const std::stop_token stop) override {
        const auto call = probe_->prepare_calls.fetch_add(1U, std::memory_order_acq_rel);
        if (call == probe_->blocked_call) {
            probe_->entered.set_value();
            std::mutex mutex;
            std::condition_variable_any changed;
            std::stop_callback wake{stop, [&changed] { changed.notify_all(); }};
            std::unique_lock lock(mutex);
            changed.wait(lock, stop, [] { return false; });
            if (stop.stop_requested()) {
                probe_->cancelled.set_value();
                return {};
            }
        }
        return {.filter = filter,
                .order = {.matching_count = 1U, .shuffle_seed = seed, .visible_indices = candidate_order_},
                .generation = static_cast<std::uint64_t>(call) + 1U};
    }
    void Commit(ExploreOrderCandidate candidate) noexcept override {
        order_ = std::move(candidate.order.visible_indices);
        candidate_order_.clear();
        probe_->commits.fetch_add(1U, std::memory_order_release);
    }
    void DiscardCandidate() override { candidate_order_.clear(); }
    void Reset() noexcept override {
        order_.clear();
        candidate_order_.clear();
        probe_->resets.fetch_add(1U, std::memory_order_release);
    }
    ExploreOrderFacts Visible(ExploreViewport, const ExploreOrderCandidate* candidate = nullptr) const override {
        return candidate == nullptr
                   ? ExploreOrderFacts{.matching_count = static_cast<std::uint32_t>(order_.size()), .visible_indices = order_}
                   : candidate->order;
    }
    bool Contains(const std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
    std::optional<std::uint32_t> Adjacent(const std::uint32_t value, std::int64_t) const override {
        return Contains(value) ? std::optional{value} : std::nullopt;
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t,
                       const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                       std::uintptr_t) override {
        Fill(clean, static_cast<std::uint8_t>(plan.generation));
        Fill(semantic, 0U);
    }

   private:
    std::shared_ptr<OpenPreparationProbe> probe_;
    std::vector<std::uint32_t> order_;
    std::vector<std::uint32_t> candidate_order_;
};

class AtomicFilterExploreAlgorithm final : public SynchronousExploreAlgorithm {
   public:
    AtomicFilterExploreAlgorithm(std::shared_ptr<CancellationProbe> probe, std::shared_ptr<std::atomic_uint64_t> commits)
        : probe_(std::move(probe)), commits_(std::move(commits)) {}
    ExploreOpened Open(std::string_view, std::stop_token) override {
        committed_ = {0U};
        return {.dataset = {.image_count = 2U, .image_width = 32U, .image_height = 32U, .class_names = {}},
                .order = {.matching_count = 1U, .visible_indices = {0U}}};
    }
    ExploreOrderCandidate PrepareFilter(
        const ExploreFilter& filter, std::uint64_t seed, std::size_t,
        // CLEANUP-IGNORE: Atomic-filter cancellation and reopen cancellation exercise different algorithm boundaries.
        const std::stop_token stop) override {
        if (prepare_count_++ == 0U)
            return {.filter = filter, .order = {.matching_count = 1U, .shuffle_seed = seed, .visible_indices = {0U}}, .generation = 1U};
        if (wait_for_cancellation(stop, *probe_)) return {};
        return {.filter = filter, .order = {.matching_count = 1U, .shuffle_seed = seed, .visible_indices = {1U}}, .generation = 1U};
    }
    void Commit(ExploreOrderCandidate candidate) noexcept override {
        committed_ = std::move(candidate.order.visible_indices);
        commits_->fetch_add(1U, std::memory_order_release);
    }
    void Reset() noexcept override {
        committed_.clear();
        prepare_count_ = 0U;
    }
    ExploreOrderFacts Visible(ExploreViewport, const ExploreOrderCandidate* candidate = nullptr) const override {
        if (candidate != nullptr) return candidate->order;
        return {.matching_count = static_cast<std::uint32_t>(committed_.size()), .visible_indices = committed_};
    }
    bool Contains(std::uint32_t value) const override { return std::ranges::find(committed_, value) != committed_.end(); }
    std::optional<std::uint32_t> Adjacent(std::uint32_t value, std::int64_t) const override {
        return Contains(value) ? std::optional{value} : std::nullopt;
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t,
                       mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic,
                       std::uintptr_t) override {
        if (plan.generation == 1U) {
            Fill(clean, 1U);
            Fill(semantic, 0U);
            return;
        }
        Fill(clean, 2U);
        Fill(semantic, 0U);
    }

   private:
    std::shared_ptr<CancellationProbe> probe_;
    std::shared_ptr<std::atomic_uint64_t> commits_;
    std::vector<std::uint32_t> committed_{0U};
    std::uint64_t prepare_count_ = 0U;
};

struct MutationCommitProbe final {
    std::promise<void> committed;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};

struct AnnotationRenderProbe final {
    std::atomic_uint64_t calls{0U};
    std::atomic_bool fail_open{false};
    std::atomic_bool fail_render{false};
    std::mutex mutex;
    std::shared_ptr<MutationCommitProbe> hold;
    std::shared_ptr<MutationCommitProbe> sample_hold;

    void Wait(std::shared_ptr<MutationCommitProbe>& pending) {
        std::shared_ptr<MutationCommitProbe> gate;
        { std::scoped_lock lock(mutex); gate = std::exchange(pending, {}); }
        if (gate) { gate->committed.set_value(); gate->released.wait(); }
    }
};

class TestAnnotationAlgorithm final : public AnnotationAlgorithm {
   public:
    explicit TestAnnotationAlgorithm(std::shared_ptr<AnnotationRenderProbe> probe = {}) : probe_(std::move(probe)) {}
    void Open(mmltk::frameworks::gpu::ImagePlaneView, VisualRegion) override {
        if (probe_ && probe_->fail_open.exchange(false)) throw std::runtime_error("deterministic source preparation failure");
    }
    contracts::AnnotationColor Sample(contracts::AnnotationPoint) override {
        if (probe_) probe_->Wait(probe_->sample_hold);
        return {120.0F, 1.0F, 1.0F};
    }
    void Render(const AnnotationRenderState&, const mmltk::frameworks::gpu::ImagePlaneView source,
                const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                std::uintptr_t) const override {
        if (probe_) {
            probe_->calls.fetch_add(1U, std::memory_order_release);
            probe_->Wait(probe_->hold);
            if (probe_->fail_render.exchange(false)) throw std::runtime_error("deterministic render failure");
        }
        if (source.valid()) mmltk::frameworks::gpu::test_support::CopyImagePlane(clean, source);
        Fill(semantic, 0xa5U);
    }
   private:
    std::shared_ptr<AnnotationRenderProbe> probe_;
};

class TestUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target,
                   std::uintptr_t) override {
        Fill(target, source.valid() ? *reinterpret_cast<const std::uint8_t*>(source.data) : 0U);
    }
    explicit TestUpscaleAlgorithm(std::shared_ptr<std::atomic<UpscaleKernel>> kernel, std::shared_ptr<MutationCommitProbe> gate = {},
                                  std::shared_ptr<std::atomic_uint32_t> runs = {}, std::uint32_t gate_run = 1U)
        : kernel_(std::move(kernel)), gate_(std::move(gate)), runs_(std::move(runs)), gate_run_(gate_run) {}
    [[nodiscard]] static VisualRuntimeFactory CreateRuntime(std::shared_ptr<FakeImageBackend> backend,
                                                            std::shared_ptr<std::atomic<UpscaleKernel>> kernel,
                                                            std::shared_ptr<std::atomic_uint32_t> runs) {
        return RuntimeFactory(
            0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            [kernel = std::move(kernel), runs = std::move(runs)] { return std::make_unique<TestUpscaleAlgorithm>(kernel, nullptr, runs); },
            4U);
    }
    void Warm() override {}
    void Run(const UpscaleKernel kernel, mmltk::frameworks::gpu::ImagePlaneView, const mmltk::frameworks::gpu::ImagePlaneView target,
             std::uintptr_t, const std::function<bool()>&) override {
        kernel_->store(kernel, std::memory_order_release);
        if (runs_) runs_->fetch_add(1U, std::memory_order_acq_rel);
        if (gate_ && (!runs_ || runs_->load() == gate_run_)) {
            gate_->committed.set_value();
            gate_->released.wait();
            gate_.reset();
        }
        Fill(target, static_cast<std::uint8_t>(kernel) + 1U);
    }

   private:
    std::shared_ptr<std::atomic<UpscaleKernel>> kernel_;
    std::shared_ptr<MutationCommitProbe> gate_;
    std::shared_ptr<std::atomic_uint32_t> runs_;
    std::uint32_t gate_run_;
};

class FailingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit FailingUpscaleAlgorithm(std::shared_ptr<std::atomic_uint32_t> runs, const bool physical = false)
        : runs_(std::move(runs)), physical_(physical) {}
    void Warm() override {}
    void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView, const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t,
             const std::function<bool()>&) override {
        if (runs_->fetch_add(1U, std::memory_order_acq_rel) == 1U) {
            if (physical_)
                throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(std::make_exception_ptr(
                    mmltk::frameworks::gpu::CudaError(cudaErrorIllegalAddress, "injected shared execution failure")));
            throw std::runtime_error("deterministic Upscale failure");
        }
        Fill(target, 1U);
    }

   private:
    std::shared_ptr<std::atomic_uint32_t> runs_;
    bool physical_ = false;
};

struct UpscaleActivationProbe final {
    std::atomic<std::size_t> warms{0U};
    std::atomic<std::size_t> runs{0U};
    std::atomic<std::size_t> fallback_activations{0U};
    std::array<std::atomic_bool, 3U> ready{};
    std::atomic_bool fail_warm{false};
    std::atomic_bool hold_warm{false};
    std::promise<void> warm_entered;
    std::promise<void> warm_release;
    // CLEANUP-IGNORE: This shared future is activation-gate custody, not shell wiring state.
    std::shared_future<void> warm_released = warm_release.get_future().share();
    // CLEANUP-IGNORE: First-warm completion is a separate one-shot activation observation.
    std::promise<void> first_warm_completed;
};

class ActivationUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ActivationUpscaleAlgorithm(std::shared_ptr<UpscaleActivationProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {
        const auto call = probe_->warms.fetch_add(1U, std::memory_order_acq_rel);
        if (call == 0U && probe_->hold_warm.load(std::memory_order_acquire)) {
            probe_->warm_entered.set_value();
            probe_->warm_released.wait();
        }
        if (probe_->fail_warm.exchange(false, std::memory_order_acq_rel)) throw std::runtime_error("deterministic Upscale warm failure");
        Activate();
        if (call == 0U) probe_->first_warm_completed.set_value();
    }
    void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target,
             std::uintptr_t, const std::function<bool()>&) override {
        if (!probe_->ready[0U].load(std::memory_order_acquire)) {
            probe_->fallback_activations.fetch_add(1U, std::memory_order_acq_rel);
            Activate();
        }
        probe_->runs.fetch_add(1U, std::memory_order_acq_rel);
        Fill(target, *reinterpret_cast<const std::uint8_t*>(source.data));
    }

   private:
    void Activate() noexcept {
        for (auto& ready : probe_->ready)
            ready.store(true, std::memory_order_release);
    }
    std::shared_ptr<UpscaleActivationProbe> probe_;
};

struct UpscaleExtentProbe final {
    std::mutex mutex;
    std::vector<mmltk::frameworks::gpu::ImagePlaneView> sources;
    // CLEANUP-IGNORE: Captured output views and scalar admission dimensions are distinct test evidence.
    std::vector<mmltk::frameworks::gpu::ImagePlaneView> targets;
};

class ExtentUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ExtentUpscaleAlgorithm(std::shared_ptr<UpscaleExtentProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {}
    void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target,
             std::uintptr_t, const std::function<bool()>&) override {
        {
            std::scoped_lock lock(probe_->mutex);
            probe_->sources.push_back(source);
            probe_->targets.push_back(target);
        }
        Fill(target, *reinterpret_cast<const std::uint8_t*>(source.data));
    }

   private:
    std::shared_ptr<UpscaleExtentProbe> probe_;
};

struct UpscaleAdmissionRaceProbe final {
    std::atomic<std::uint8_t> received_value{0U};
    std::atomic<std::uint32_t> received_width{0U};
    std::atomic<std::uint32_t> received_height{0U};
};

class RacingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit RacingUpscaleAlgorithm(std::shared_ptr<UpscaleAdmissionRaceProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {}
    void Run(UpscaleKernel, const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView target,
             std::uintptr_t, const std::function<bool()>&) override {
        const auto value = *reinterpret_cast<const std::uint8_t*>(source.data);
        probe_->received_value.store(value, std::memory_order_release);
        probe_->received_width.store(source.descriptor.width, std::memory_order_release);
        probe_->received_height.store(source.descriptor.height, std::memory_order_release);
        Fill(target, value);
    }

   private:
    std::shared_ptr<UpscaleAdmissionRaceProbe> probe_;
};

struct UpscaleReleaseFailureProbe final {
    std::promise<void> warmed;
    std::atomic<std::size_t> releases{0U};
    std::atomic_bool destroyed{false};
};

class ReleaseFailingUpscaleAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ReleaseFailingUpscaleAlgorithm(std::shared_ptr<UpscaleReleaseFailureProbe> probe)
        : probe_(std::move(probe)), failure_(std::make_exception_ptr(std::runtime_error("deterministic Upscale release failure"))) {}
    ~ReleaseFailingUpscaleAlgorithm() override { probe_->destroyed.store(true, std::memory_order_release); }
    void Warm() override { probe_->warmed.set_value(); }
    void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t,
             const std::function<bool()>&) override {}
    [[nodiscard]] Release ReleaseResources() noexcept override {
        probe_->releases.fetch_add(1U, std::memory_order_acq_rel);
        return {.all_released = false, .failure = failure_};
    }

   private:
    std::shared_ptr<UpscaleReleaseFailureProbe> probe_;
    std::exception_ptr failure_;
};

class TestLiveAlgorithm final : public LiveAlgorithm {
   public:
    explicit TestLiveAlgorithm(std::shared_ptr<std::atomic<std::uint64_t>> captures, std::shared_ptr<std::atomic_bool> token_changed = {})
        : captures_(std::move(captures)), token_changed_(std::move(token_changed)) {}
    void Start(const LiveStart&) override {}
    void SetOutputAvailableSink(std::function<void()>) override {}
    bool AcquireOutput() override { return true; }
    bool Capture(const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t, const std::stop_token stop) override {
        if (!first_stop_)
            first_stop_ = stop;
        else if (*first_stop_ != stop && token_changed_)
            token_changed_->store(true, std::memory_order_release);
        if (stop.stop_requested()) return false;
        const auto value = captures_->fetch_add(1U, std::memory_order_acq_rel) + 1U;
        Fill(target, static_cast<std::uint8_t>(value));
        return true;
    }
    void Stop() noexcept override {}

   private:
    std::shared_ptr<std::atomic<std::uint64_t>> captures_;
    std::shared_ptr<std::atomic_bool> token_changed_;
    std::optional<std::stop_token> first_stop_;
};

[[nodiscard]] VisualRuntimeFactory test_live_runtime_factory(std::shared_ptr<FakeImageBackend> backend,
                                                             std::shared_ptr<std::atomic<std::uint64_t>> captures,
                                                             std::shared_ptr<std::atomic_bool> token_changed = {}) {
    return RuntimeFactory(0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::Clean,
                          [captures = std::move(captures), token_changed = std::move(token_changed)] {
                              return std::make_unique<TestLiveAlgorithm>(captures, token_changed);
                          });
}

[[nodiscard]] detail::VisualRuntimeOwner::Notification no_op_visual_work(mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
    return {};
}

[[nodiscard]] auto settle_visual_on_exit(detail::VisualRuntimeOwner& owner, std::promise<void>& release) {
    return mmltk::testsupport::ScopedTestCleanup{[&owner, &release] {
        mmltk::testsupport::release_test_promise(release);
        owner.RequestStop();
        owner.StopAndWait();
    }};
}

[[nodiscard]] std::uint64_t borrowed_visual_revision(detail::VisualRuntimeOwner& owner) {
    const auto borrowed = owner.Borrow();
    REQUIRE(borrowed.valid());
    return borrowed.plane(0U).revision();
}

[[nodiscard]] VisualRuntimeFactory gated_visual_construction(VisualRuntimeFactory factory, std::promise<void>& constructing,
                                                             std::shared_future<void> release, const std::size_t ordinal) {
    return [factory = std::move(factory), &constructing, release = std::move(release), ordinal,
            constructions = std::size_t{0U}](auto revisions) mutable {
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

struct TestPresentationWriterState final {
    std::atomic_bool advertise_waiting_candidate{false};
    TestPresentationWriterState() : readiness(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)), pump_release(release_pump.get_future().share()) {}
    void SignalReadiness() const {
        const std::uint64_t value = 1U;
        REQUIRE(::write(readiness.get(), &value, sizeof(value)) == sizeof(value));
    }
    void AcknowledgeAllocationRetirement(const std::uint64_t generation) {
        retirement_acknowledgement.store(generation, std::memory_order_release);
        SignalReadiness();
    }
    void RecordPump() {
        {
            std::scoped_lock lock(pump_mutex);
            ++pump_count;
        }
        pump_changed.notify_all();
    }
    [[nodiscard]] std::uint64_t PumpCount() const {
        std::scoped_lock lock(pump_mutex);
        return pump_count;
    }
    [[nodiscard]] bool WaitForPumpAfter(const std::uint64_t prior) {
        std::unique_lock lock(pump_mutex);
        return pump_changed.wait_for(lock, 2s, [&] { return pump_count > prior; });
    }
    [[nodiscard]] std::uint64_t ArmWaiting() {
        std::scoped_lock lock(pump_mutex);
        return ++waiting_step;
    }
    void RecordWaiting(bool connected, std::uint64_t source_revision, bool has_pending, bool readiness_consumed) {
        if (!readiness_consumed) return;
        {
            std::scoped_lock lock(pump_mutex);
            waiting = {.step = waiting_step, .connected = connected, .source_revision = source_revision, .pending = has_pending};
        }
        pump_changed.notify_all();
    }
    [[nodiscard]] bool WaitDisconnected(std::uint64_t step, bool pending, std::uint64_t source_revision) {
        std::unique_lock lock(pump_mutex);
        return pump_changed.wait_for(lock, 2s, [&] {
            return waiting.step == step && !waiting.connected && waiting.pending == pending && waiting.source_revision == source_revision;
        });
    }
    struct Waiting {
        std::uint64_t step = 0;
        bool connected = true;
        std::uint64_t source_revision = 0;
        bool pending = false;
    } waiting;
    std::uint64_t waiting_step = 0;
    // CLEANUP-IGNORE: This descriptor begins presentation-writer readiness and lifecycle evidence, not fake GPU
    // synchronization and transfer counters.
    mmltk::common::io::ScopedFd readiness;
    // CLEANUP-OFF: These presentation lifecycle facts are independent test evidence with distinct assertions, not
    // a second physical-backend telemetry schema.
    std::atomic<std::uint64_t> timeline{0U};
    std::atomic<std::uint64_t> presentation_revision{0U};
    std::atomic<std::uint64_t> submissions{0U};
    std::atomic<std::uint64_t> browser_terminals{0U};
    std::atomic<std::uint64_t> retirements{0U};
    std::atomic<std::uint64_t> allocation_retirements{0U};
    std::atomic<std::uint64_t> retained_allocation_generation{0U};
    std::atomic<std::uint64_t> retirement_acknowledgement{0U};
    std::atomic<std::uint64_t> constructions{0U};
    std::atomic<std::uint64_t> context_bindings{0U};
    // CLEANUP-ON
    std::atomic_bool allow_publication{true};
    std::atomic_bool fail_pump{false};
    std::atomic_bool block_pump{false};
    std::atomic_bool pump_block_reported{false};
    std::atomic_bool terminal_release_succeeds{true};
    std::atomic_bool terminal_source_settled{true};
    std::promise<void> first_submission;
    std::promise<void> second_submission;
    std::promise<void> third_submission;
    std::promise<void> allocation_retired;
    std::promise<void> pump_entered;
    std::promise<void> release_pump;
    std::shared_future<void> pump_release;
    mutable std::mutex pump_mutex;
    std::condition_variable pump_changed;
    std::uint64_t pump_count = 0U;
};

// Declared immediately after the system, before assertions. Event/callback
// storage precedes the system and therefore outlives terminal settlement.
class PresentationScenario final {
   public:
    PresentationScenario(PresentationSystem& system, std::shared_ptr<TestPresentationWriterState> state)
        : system_(system), state_(std::move(state)) {}
    ~PresentationScenario() {
        mmltk::testsupport::release_test_promise(state_->release_pump);
        if (system_.stopped()) return;
        system_.CloseAdmission();
        system_.BrowserPeerLost();
        static_cast<void>(system_.Shutdown());
    }
    PresentationScenario(const PresentationScenario&) = delete;
    PresentationScenario& operator=(const PresentationScenario&) = delete;

   private:
    PresentationSystem& system_;
    std::shared_ptr<TestPresentationWriterState> state_;
};

class TestPresentationWriter final : public PresentationNativeWriter {
   public:
    [[nodiscard]] static PresentationNativeWriterFactory Factory(std::shared_ptr<FakeImageBackend> backend,
                                                                 std::shared_ptr<TestPresentationWriterState> state) {
        return [backend = std::move(backend), state = std::move(state)] {
            return std::make_unique<TestPresentationWriter>(0, backend, state);
        };
    }

    TestPresentationWriter(const int device, std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<TestPresentationWriterState> state)
        : context_(device, std::move(backend)),
          stream_(context_),
          sample_arena_(context_, mmltk::frameworks::gpu::ImageProductLayout::Clean),
          state_(std::move(state)) {
        state_->constructions.fetch_add(1U, std::memory_order_acq_rel);
    }
    ~TestPresentationWriter() override { state_->retirements.fetch_add(1U, std::memory_order_acq_rel); }

    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        context_.Bind();
        state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        const auto submission = state_->submissions.fetch_add(1U, std::memory_order_acq_rel);
        if (submission == 0U) state_->first_submission.set_value();
        if (submission == 1U) state_->second_submission.set_value();
        if (submission == 2U) state_->third_submission.set_value();
        submitted_ = submitted;
        const auto& frame = submitted.observation.frame;
        const auto width = frame.extent.width;
        const auto height = frame.extent.height;
        if (!Contains(active_, width, height)) {
            if (candidate_ && !Contains(candidate_, width, height)) candidate_.reset();
            if (!candidate_) {
                candidate_ = Allocation{
                    .width = active_ ? std::max(active_->width, width) : width,
                    .height = active_ ? std::max(active_->height, height) : height,
                    .generation = next_generation_++,
                };
            }
        }
        const auto& target = Contains(active_, width, height) ? *active_ : *candidate_;
        reader_ = std::addressof(reader);
        pending_ = PresentationPublication{
            .capability =
                {
                    .surface_high = 1U,
                    .surface_low = target.generation,
                    .extent = {target.width, target.height},
                    .generation = target.generation,
                    .condition = PresentationCapabilityCondition::Ready,
                },
        };
    }
    PresentationNativeOutcome Pump(const std::uint64_t current_selection_generation) override {
        context_.Bind();
        state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        std::uint64_t wake = 0U;
        ssize_t consumed = -1;
        do {
            consumed = ::read(state_->readiness.get(), &wake, sizeof(wake));
        } while (consumed < 0 && errno == EINTR);
        if (consumed < 0 && errno != EAGAIN) throw std::runtime_error("test presentation readiness read failed");
        if (state_->fail_pump.load(std::memory_order_acquire)) throw std::runtime_error("test presentation writer failure");
        if (state_->block_pump.load(std::memory_order_acquire)) {
            if (!state_->pump_block_reported.exchange(true, std::memory_order_acq_rel)) state_->pump_entered.set_value();
            state_->pump_release.wait();
        }
        const auto retirement_acknowledgement = state_->retirement_acknowledgement.exchange(0U, std::memory_order_acq_rel);
        if (retiring_ && retirement_acknowledgement == retiring_->generation) {
            retiring_.reset();
            state_->retained_allocation_generation.store(0U, std::memory_order_release);
            const auto prior = state_->allocation_retirements.fetch_add(1U, std::memory_order_acq_rel);
            if (prior == 0U) state_->allocation_retired.set_value();
        }
        mmltk::testsupport::ScopedTestCleanup pumped{[state = state_] { state->RecordPump(); }};
        if (submitted_.selection_generation != current_selection_generation) {
            pending_.reset();
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = submitted_,
            };
        }
        const bool peer_connected = application_peer_connected_.load(std::memory_order_acquire);
        if (!pending_ || !peer_connected || !state_->allow_publication.load(std::memory_order_acquire)) {
            state_->RecordWaiting(peer_connected, submitted_.observation.frame.revision, pending_.has_value(), consumed > 0);
            return {.capability = capability()};
        }
        const bool candidate_target = candidate_ && pending_->capability.generation == candidate_->generation;
        if (candidate_target && retiring_) return {.capability = capability()};
        auto source = reader_->borrow();
        if (!visual_product_matches_frame(submitted_.observation.frame, source)) {
            pending_.reset();
            reader_ = nullptr;
            return {
                .progress = PresentationNativeProgress::Superseded,
                .submitted = submitted_,
                .capability = capability(),
            };
        }
        // This injected writer includes the simulated browser receiver and its sample storage.
        static_cast<void>(sample_arena_.CopyFrom(stream_, std::move(source)));
        stream_.Synchronize();
        if (candidate_target) {
            if (active_) {
                retiring_ = std::exchange(active_, std::nullopt);
                state_->retained_allocation_generation.store(retiring_->generation, std::memory_order_release);
            }
            active_ = std::exchange(candidate_, std::nullopt);
        }
        pending_->timeline_ready = state_->timeline.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        pending_->transfer_sequence = pending_->timeline_ready;
        pending_->presentation_revision = state_->presentation_revision.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        auto outcome = PresentationNativeOutcome{
            .progress = PresentationNativeProgress::Published,
            .submitted = submitted_,
            .publication = std::exchange(pending_, std::nullopt).value(),
            .capability = capability(),
        };
        reader_ = nullptr;
        return outcome;
    }
    int poll_fd() const noexcept override { return state_->readiness.get(); }
    int completion_fd() const noexcept override { return -1; }
    bool wants_write() const noexcept override { return false; }
    void SetApplicationPeerConnected(const bool connected) noexcept override {
        application_peer_connected_.store(connected, std::memory_order_release);
    }
    void SetExpectedBrowserProcessGroup(pid_t) override {}
    Retirement BrowserPeerLost() noexcept override {
        try {
            context_.Bind();
            state_->context_bindings.fetch_add(1U, std::memory_order_acq_rel);
        } catch (...) { return {.all_released = false}; }
        state_->browser_terminals.fetch_add(1U, std::memory_order_acq_rel);
        return {.all_released = state_->terminal_release_succeeds.load(std::memory_order_acquire),
                .safe_to_destroy = state_->terminal_source_settled.load(std::memory_order_acquire)};
    }

   private:
    struct Allocation final {
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint64_t generation = 0U;
    };

    [[nodiscard]] static bool Contains(const std::optional<Allocation>& allocation, const std::uint32_t width,
                                       const std::uint32_t height) noexcept {
        return allocation && width <= allocation->width && height <= allocation->height;
    }

    [[nodiscard]] PresentationCapability capability() const noexcept {
        const auto* allocation = candidate_ ? std::addressof(*candidate_) : active_ ? std::addressof(*active_) : nullptr;
        if (allocation == nullptr) return {};
        return {
            .surface_high = 1U,
            .surface_low = allocation->generation + (state_->advertise_waiting_candidate.load() ? 100U : 0U),
            .extent = {allocation->width, allocation->height},
            .generation = allocation->generation + (state_->advertise_waiting_candidate.load() ? 100U : 0U),
            .condition = state_->allow_publication.load(std::memory_order_acquire) ? PresentationCapabilityCondition::Ready
                                                                                   : PresentationCapabilityCondition::Admitted,
        };
    }
    mmltk::frameworks::gpu::DeviceContext context_;
    mmltk::frameworks::gpu::ImageStream stream_;
    mmltk::frameworks::gpu::ImageProductBuffer sample_arena_;
    std::shared_ptr<TestPresentationWriterState> state_;
    PresentationSubmittedSource submitted_{};
    const VisualSourceReader* reader_ = nullptr;
    std::optional<PresentationPublication> pending_;
    std::optional<Allocation> active_;
    std::optional<Allocation> candidate_;
    std::optional<Allocation> retiring_;
    std::atomic_bool application_peer_connected_{true};
    std::uint64_t next_generation_ = 1U;
};

TEST_CASE("Workspace layout rejects delayed undersized capacity") {
    namespace abi = presentation::detail::workspace_surface_import;
    abi::Record layout{.opcode = abi::Opcode::ArenaReady,
                       .id_high = 1U,
                       .width = 100U,
                       .height = 100U,
                       .stride = 400U,
                       .size = 40'000U,
                       .device_incarnation = 2U,
                       .alignment = 16U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    CHECK(abi::valid(layout));
    layout.width = 200U;
    layout.height = 200U;
    CHECK_FALSE(abi::valid(layout));
    layout.stride = 1024U;
    layout.size = 204'800U;
    CHECK(abi::valid(layout));
    layout.stride = 799U;
    CHECK_FALSE(abi::valid(layout));
}

class EventGate final {
   public:
    void Advance() {
        {
            std::scoped_lock lock(mutex_);
            ++revision_;
        }
        changed_.notify_all();
    }
    template <class Predicate>
    bool Wait(Predicate predicate, std::chrono::seconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::move(predicate));
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

   private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t revision_ = 0U;
};

struct ExplorePressureProbe final {
    EventGate events{};
    std::atomic_size_t attempts{0U};
    std::atomic_size_t idle_attempts{0U};
    std::atomic_bool fail_render{false};
    std::atomic_size_t runtimes{0U};
    std::shared_ptr<ExplorePostRenderGate> render_gate;
    std::atomic_bool semantic_detail{false};

    [[nodiscard]] VisualDiagnosticSink sink() noexcept {
        return {.context = this, .write = [](void* context, const VisualDiagnosticFact fact) noexcept {
                    auto& probe = *static_cast<ExplorePressureProbe*>(context);
                    if (fact.operation == VisualDiagnosticOperation::ExploreContinuationStarted &&
                        fact.detail == 30U + static_cast<std::uint64_t>(detail::VisualRuntimeOwner::ActivityStage::CycleFinalized) &&
                        fact.value == 0U) {
                        probe.idle_attempts.store(probe.attempts.load(std::memory_order_acquire), std::memory_order_release);
                        probe.events.Advance();
                    }
                }};
    }
    void WaitIdle(const std::size_t expected) {
        REQUIRE(events.Wait([&] { return idle_attempts.load(std::memory_order_acquire) >= expected; }));
        CHECK(attempts.load(std::memory_order_acquire) == expected);
    }
};

class CapacityExploreAlgorithm final : public TestExploreAlgorithm {
   public:
    explicit CapacityExploreAlgorithm(ExplorePressureProbe& probe)
        : TestExploreAlgorithm(std::make_shared<std::atomic_size_t>(0U), nullptr, nullptr, nullptr, nullptr, probe.render_gate),
          probe_(probe) {}
    ExploreOutputChange OutputChange(const ExploreRenderPlan& plan, const ExploreOrderCandidate*) const override {
        probe_.attempts.fetch_add(1U, std::memory_order_release);
        probe_.events.Advance();
        return probe_.semantic_detail && plan.mode == ExploreMode::Detail ? ExploreOutputChange::Semantic : ExploreOutputChange::Initialize;
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc,
                       mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic,
                       const std::uintptr_t stream) override {
        if (probe_.fail_render.exchange(false, std::memory_order_acq_rel)) throw std::runtime_error("Explore pending predecessor failed");
        TestExploreAlgorithm::RenderProduct(plan, candidate, nproc, clean, semantic, stream);
    }

   private:
    ExplorePressureProbe& probe_;
};

class DiagnosticCapture final {
   public:
    [[nodiscard]] VisualDiagnosticSink sink() noexcept {
        return {
            .context = this,
            .write =
                [](void* context, const VisualDiagnosticFact fact) noexcept {
                    auto& capture = *static_cast<DiagnosticCapture*>(context);
                    capture.last_system.store(fact.system, std::memory_order_release);
                    if (fact.operation == VisualDiagnosticOperation::StaleThumbnailDiscarded)
                        capture.stale_discarded.fetch_add(fact.value, std::memory_order_acq_rel);
                    if (fact.operation == VisualDiagnosticOperation::UpscaleResultReused) {
                        capture.reused_revision.store(fact.value, std::memory_order_release);
                        capture.reused_meaning.store(fact.context.document_meaning_identity, std::memory_order_release);
                        capture.reused_observation.store(fact.context.observation_revision, std::memory_order_release);
                    }
                    capture.count.fetch_add(1U, std::memory_order_acq_rel);
                },
        };
    }
    // CLEANUP-OFF: These named trace assertions are independent domain evidence, not physical fake-backend
    // allocation and transfer counters.
    std::atomic<std::uint64_t> count{0U};
    std::atomic<std::uint64_t> stale_discarded{0U};
    std::atomic<std::uint64_t> reused_revision{0U};
    std::atomic<std::uint64_t> reused_meaning{0U};
    std::atomic<std::uint64_t> reused_observation{0U};
    std::atomic<contracts::DiagnosticOwner> last_system{contracts::DiagnosticOwner::Explore};
    // CLEANUP-ON
};

constexpr VisualDeviceSettings kDevice{
    .device = 0,
    .maximum_width = 1024U,
    .maximum_height = 1024U,
};

[[nodiscard]] bool has_cuda_device() noexcept {
    int device_count = 0;
    return ::cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}

void check_gallery_first_pixels(ExploreSystem& explore, const std::uint8_t first, const std::uint8_t second) {
    auto product = explore.BorrowFrame();
    REQUIRE(product.valid());
    const auto plane = product.plane(0U).plane();
    CHECK(*reinterpret_cast<const std::uint8_t*>(plane.data) == first);
    CHECK(*(reinterpret_cast<const std::uint8_t*>(plane.data) + 16U) == second);
}

void open_streaming_gallery(ExploreSystem& explore, EventGate& events, ExploreViewport viewport) {
    static_cast<void>(explore.Open({.viewport = viewport, .compiled_source = "/stream"}));
    REQUIRE(events.Wait([&explore] { return explore.snapshot().ready; }));
}

class LoadedSettings final {
   public:
    explicit LoadedSettings(SystemEventSink<SettingsSystem::event_type> events = {}) : system_(std::move(events)) {
        REQUIRE(system_.Load(location()).applied());
    }
    [[nodiscard]] SettingsSystem& system() noexcept { return system_; }
    void BreakPersistence() {
        std::filesystem::remove_all(directory_.path());
        mmltk::testsupport::write_text_file(directory_.path(), "blocked");
    }
    void RestorePersistence() {
        std::filesystem::remove_all(directory_.path());
        std::filesystem::create_directories(directory_.path());
    }
    void MakeUnavailable() {
        std::filesystem::remove_all(directory_.path());
        std::filesystem::create_directories(directory_.path() / "gui.json");
        REQUIRE_FALSE(system_.Load(location()).applied());
    }
    [[nodiscard]] services::SettingsLocation location() const {
        return services::SettingsLocation{(directory_.path() / "gui.json").string()};
    }

   private:
    mmltk::testsupport::ScopedTempDir directory_{"mmltk-explore-settings"};
    SettingsSystem system_;
};

class OpenedExplore final {
   public:
    OpenedExplore(std::shared_ptr<FakeImageBackend> backend, const VisualExtent extent)
        : explore_(settings_.system(), kDevice, 2U,
                   RuntimeFactory(
                       0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U)); }, 3U),
                   [this](ExploreSystem::event_type) { events_.Advance(); }) {
        static_cast<void>(
            explore_.Open({.viewport = {.extent = extent, .columns = extent.width / extent.height}, .compiled_source = "/test"}));
        REQUIRE(events_.Wait([this] { return explore_.snapshot().ready; }));
    }

    [[nodiscard]] ExploreSystem& system() noexcept { return explore_; }
    void Reopen(const VisualExtent extent) {
        const auto admitted =
            explore_.Open({.viewport = {.extent = extent, .columns = extent.width / extent.height}, .compiled_source = "/reopened"});
        REQUIRE(events_.Wait([this, admitted] { return explore_.snapshot().ready && explore_.snapshot().revision > admitted.revision; }));
    }

   private:
    EventGate events_;
    LoadedSettings settings_;
    ExploreSystem explore_;
};

class ExploreScenario final {
   public:
    using ModelFactory = std::function<std::unique_ptr<mmltk::frameworks::gpu::SystemImageModel>()>;
    using Observer = std::function<void(ExploreSystem::event_type)>;

    ExploreScenario(LoadedSettings& settings, std::shared_ptr<FakeImageBackend> backend, ModelFactory model = {}, Observer observer = {},
                    VisualDiagnosticSink diagnostics = {})
        : ExploreScenario(settings, 2U,
                          RuntimeFactory(0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                                         model ? std::move(model) : DefaultModel(), 3U),
                          std::move(observer), diagnostics) {}

    ExploreScenario(LoadedSettings& settings, const std::size_t nproc, VisualRuntimeFactory runtime, Observer observer = {},
                    VisualDiagnosticSink diagnostics = {})
        : observer_(std::move(observer)),
          explore_(
              settings.system(), kDevice, nproc, std::move(runtime),
              [this](ExploreSystem::event_type event) {
                  if (observer_) observer_(std::move(event));
                  events_.Advance();
              },
              diagnostics) {}

    [[nodiscard]] ExploreSystem& system() noexcept { return explore_; }

    void OpenAndWait(const ExploreViewport viewport, const std::string_view compiled_source = "/test") {
        const auto admitted = explore_.Open({.viewport = viewport, .compiled_source = std::string{compiled_source}});
        REQUIRE(Wait([this, admitted] {
            const auto current = explore_.snapshot();
            return current.ready && !current.busy && current.revision > admitted.revision;
        }));
    }

    [[nodiscard]] bool Wait(std::function<bool()> predicate) { return events_.Wait(std::move(predicate)); }

    [[nodiscard]] static ModelFactory TrackCommits(std::shared_ptr<std::atomic_uint64_t> commits) {
        return [commits = std::move(commits)] {
            return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits);
        };
    }

    [[nodiscard]] static ModelFactory TrackWork(std::shared_ptr<ExploreWorkProbe> work) {
        return [work = std::move(work)] {
            return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr, work);
        };
    }

    [[nodiscard]] static ModelFactory GateAfterRender(std::shared_ptr<ExplorePostRenderGate> gate,
                                                      std::shared_ptr<std::atomic_uint64_t> commits = {},
                                                      std::shared_ptr<ExploreWorkProbe> work = {}) {
        return [gate = std::move(gate), commits = std::move(commits), work = std::move(work)] {
            return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits, nullptr, work,
                                                          gate);
        };
    }

   private:
    [[nodiscard]] static ModelFactory DefaultModel() {
        return [] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U)); };
    }

    Observer observer_;
    EventGate events_;
    ExploreSystem explore_;
};

TEST_CASE("Explore announces the acquired detail allocation before framework prewrites can fail") {
    class DetailWriteAlgorithm final : public TestExploreAlgorithm {
       public:
        DetailWriteAlgorithm(std::shared_ptr<ExploreWorkProbe> work, std::atomic_uint64_t& owner)
            : TestExploreAlgorithm(std::make_shared<std::atomic_size_t>(0U), nullptr, nullptr, nullptr, std::move(work)), owner_(owner) {}
        void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation allocation) noexcept override {
            owner_.store(allocation.owner, std::memory_order_release);
        }

       private:
        std::atomic_uint64_t& owner_;
    };
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto work = std::make_shared<ExploreWorkProbe>();
    std::atomic_uint64_t notified_owner{0U};
    ExploreScenario scenario{settings, backend, [&] { return std::make_unique<DetailWriteAlgorithm>(work, notified_owner); }};
    auto& explore = scenario.system();
    const ExploreViewport viewport{.extent = {16U, 16U}};
    scenario.OpenAndWait(viewport);
    const auto first = explore.snapshot().frame;
    auto next = viewport;
    next.first_row = 1U;
    explore.UpdateViewport({.viewport = next});
    REQUIRE(scenario.Wait([&] { return explore.snapshot().frame != first; }));
    const auto incumbent = explore.snapshot();
    const auto rendered = work->renders.load(std::memory_order_acquire);
    backend->FailAfter(FakeImageBackend::FailurePoint::Clear, 1U);
    static_cast<void>(explore.Select({.compiled_index = 1U}));
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().failure.empty(); }));
    CHECK(notified_owner.load(std::memory_order_acquire) != 0U);
    CHECK(work->renders.load(std::memory_order_acquire) == rendered);
    CHECK(explore.snapshot().frame == incumbent.frame);
    CHECK(explore.snapshot().mode == ExploreMode::Gallery);
}

class ExplorePressureFixture final {
   public:
    struct Observation final {
        ExploreSnapshot snapshot;
        std::size_t runtimes;
        bool failed;
    };
    explicit ExplorePressureFixture(const bool gated_resume = false)
        : probe{.render_gate = gated_resume ? std::make_shared<ExplorePostRenderGate>(3U) : nullptr},
          scenario{settings, backend,
                   [&] {
                       probe.runtimes.fetch_add(1U, std::memory_order_release);
                       return std::make_unique<CapacityExploreAlgorithm>(probe);
                   },
                   [this](ExploreSystem::event_type event) {
                       std::scoped_lock lock(observation_mutex);
                       const bool failed = std::holds_alternative<ExploreFailed>(event);
                       observations.push_back({std::visit([](auto& value) { return std::move(value.snapshot); }, event),
                                               probe.runtimes.load(std::memory_order_acquire), failed});
                   },
                   probe.sink()} {}
    ~ExplorePressureFixture() {
        if (probe.render_gate) mmltk::testsupport::release_test_promise(probe.render_gate->release);
        static_cast<void>(scenario.system().Stop());
        held = {};
        scenario.system().Shutdown();
    }
    void HoldGalleryPool() {
        auto& explore = scenario.system();
        scenario.OpenAndWait(viewport);
        held[0U] = explore.BorrowFrame();
        for (std::uint32_t row = 1U; row != 3U; ++row) {
            auto next = viewport;
            next.first_row = row;
            explore.UpdateViewport({.viewport = next});
            REQUIRE(scenario.Wait([&] { return explore.snapshot().viewport == next; }));
            held[row] = explore.BorrowFrame();
        }
        probe.WaitIdle(3U);
    }
    void RequestViewport(const std::uint32_t row, const std::optional<std::uint32_t> focus = {}) {
        auto next = viewport;
        next.first_row = row;
        const auto before = probe.attempts.load(std::memory_order_acquire);
        scenario.system().UpdateViewport({.viewport = next, .focused_compiled_index = focus});
        probe.WaitIdle(before + 1U);
    }
    [[nodiscard]] std::vector<Observation> Recorded() {
        std::scoped_lock lock(observation_mutex);
        return observations;
    }
    LoadedSettings settings;
    std::shared_ptr<FakeImageBackend> backend = std::make_shared<FakeImageBackend>();
    ExplorePressureProbe probe;
    std::mutex observation_mutex;
    std::vector<Observation> observations;
    ExploreScenario scenario;
    const ExploreViewport viewport{.extent = {16U, 16U}};
    std::array<mmltk::frameworks::gpu::BorrowedImageProductReadView, 3U> held;
};

TEST_CASE("Explore retries only the newest desired product after held reads release output capacity") {
    ExplorePressureFixture fixture;
    fixture.HoldGalleryPool();
    auto& explore = fixture.scenario.system();
    const auto held = explore.snapshot().frame;
    fixture.RequestViewport(0U);
    fixture.RequestViewport(1U);
    const auto generation = explore.LastInteractionGeneration();
    fixture.RequestViewport(1U, 1U);
    CHECK(explore.LastInteractionGeneration() == generation);
    CHECK(explore.snapshot().frame == held);
    fixture.held[0U] = {};
    REQUIRE(fixture.scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.frame != held && state.viewport.first_row == 1U && state.focused_image == 1U;
    }));
    CHECK(fixture.held[1U].valid());
    CHECK(fixture.held[2U].valid());
    CHECK_FALSE(explore.snapshot().busy);
    const auto retained = explore.snapshot();
    auto filter = retained.filter;
    filter.minimum_instances = 1U;
    CHECK(explore.UpdateFilter({.filter = filter, .overlay = retained.overlay}).busy);
    CHECK_THROWS_AS(explore.UpdateViewport({.viewport = fixture.viewport}), contracts::BusyError);
    static_cast<void>(explore.Stop());
    REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy; }));
    CHECK(explore.snapshot().frame == retained.frame);
}

TEST_CASE("Explore retains semantic detail baseline custody through an idle output wait") {
    ExplorePressureFixture fixture;
    auto& explore = fixture.scenario.system();
    fixture.scenario.OpenAndWait(fixture.viewport);
    fixture.held[0U] = explore.BorrowFrame();
    static_cast<void>(explore.Select({.compiled_index = 0U}));
    REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().mode == ExploreMode::Detail; }));
    fixture.held[1U] = explore.BorrowFrame();
    fixture.probe.WaitIdle(2U);
    fixture.probe.semantic_detail = true;
    auto overlay = explore.snapshot().overlay;
    overlay.show_boxes = !overlay.show_boxes;
    static_cast<void>(explore.UpdateOverlay(overlay));
    REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().overlay == overlay; }));
    fixture.held[2U] = explore.BorrowFrame();
    fixture.probe.WaitIdle(3U);
    const auto retained = explore.snapshot();
    overlay.show_masks = !overlay.show_masks;
    static_cast<void>(explore.UpdateOverlay(overlay));
    fixture.probe.WaitIdle(4U);
    CHECK(explore.snapshot().frame == retained.frame);
    SECTION("actual release resumes the retained semantic request") {
        static_cast<void>(explore.UpdateDetail({.show_original_dimensions = true}));
        fixture.held[1U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().overlay == overlay; }));
        CHECK(explore.snapshot().selected_image == retained.selected_image);
        CHECK(explore.snapshot().mode == ExploreMode::Detail);
        CHECK(explore.snapshot().detail.show_original_dimensions);
        CHECK(fixture.settings.system().explore_settings_candidate().preferences.policy.overlay == overlay);
        CHECK(fixture.settings.system().explore_settings_candidate().show_original_dimensions);
    }
    SECTION("newer navigation accumulates once while retaining pending settings") {
        static_cast<void>(explore.Navigate({.direction = ExploreNavigation::Next}));
        fixture.probe.WaitIdle(5U);
        static_cast<void>(explore.Navigate({.direction = ExploreNavigation::Next}));
        fixture.probe.WaitIdle(6U);
        fixture.held[1U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().selected_image == 2U; }));
        CHECK(explore.snapshot().overlay == overlay);
        CHECK(fixture.settings.system().explore_settings_candidate().preferences.policy.overlay == overlay);
    }
    SECTION("Stop releases the pending baseline and leaves the completed detail selected") {
        static_cast<void>(explore.Stop());
        fixture.held = {};
        explore.Shutdown();
        CHECK(explore.snapshot().frame == retained.frame);
        CHECK(explore.snapshot().overlay == retained.overlay);
    }
}

TEST_CASE("Explore settles an accepted viewport before a discrete filter under output pressure") {
    ExplorePressureFixture fixture;
    fixture.HoldGalleryPool();
    auto& explore = fixture.scenario.system();
    const auto incumbent = explore.snapshot();
    fixture.RequestViewport(0U, 0U);
    auto policy = ExploreFilterUpdate{.filter = incumbent.filter, .overlay = incumbent.overlay};
    policy.filter.minimum_instances = 1U;
    policy.overlay.show_boxes = !policy.overlay.show_boxes;
    CHECK(explore.UpdateFilter(policy).busy);
    CHECK_THROWS_AS(explore.Reroll(), contracts::BusyError);
    SECTION("separate releases settle predecessor, filter and a later viewport coherently") {
        fixture.held[0U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().viewport.first_row == 0U; }));
        const auto predecessor = explore.snapshot();
        CHECK(predecessor.busy);
        CHECK(predecessor.filter == incumbent.filter);
        CHECK(predecessor.overlay == incumbent.overlay);
        CHECK(predecessor.focused_image == 0U);
        fixture.held[1U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().filter == policy.filter; }));
        const auto filtered = explore.snapshot();
        CHECK(filtered.frame != predecessor.frame);
        CHECK(filtered.viewport.first_row == 0U);
        CHECK(filtered.overlay == policy.overlay);
        CHECK_FALSE(filtered.selected_image);
        CHECK_FALSE(filtered.focused_image);
        fixture.RequestViewport(1U, 1U);
        const auto later = explore.snapshot();
        CHECK(later.filter == policy.filter);
        CHECK(later.overlay == policy.overlay);
        CHECK(later.viewport.first_row == 1U);
        CHECK(later.order.visible_indices == std::vector<std::uint32_t>{1U});
        CHECK_FALSE(later.selected_image);
        CHECK(later.focused_image == 1U);
        const auto persisted = fixture.settings.system().explore_settings_candidate().preferences.policy;
        CHECK(persisted.filter == policy.filter);
        CHECK(persisted.overlay == policy.overlay);
    }
    SECTION("Stop cancels waiting stages and preserves the last completed predecessor") {
        const bool predecessor_settled = GENERATE(false, true);
        CAPTURE(predecessor_settled);
        auto completed_frame = incumbent.frame;
        if (predecessor_settled) {
            fixture.held[0U] = {};
            REQUIRE(fixture.scenario.Wait([&] { return explore.snapshot().viewport.first_row == 0U; }));
            completed_frame = explore.snapshot().frame;
            fixture.probe.WaitIdle(5U);
        }
        static_cast<void>(explore.Stop());
        fixture.held = {};
        explore.Shutdown();
        CHECK_FALSE(explore.snapshot().busy);
        CHECK(explore.snapshot().frame == completed_frame);
        CHECK(explore.snapshot().filter == incumbent.filter);
    }
    SECTION("ordinary predecessor failure settles before the admitted filter") {
        fixture.probe.fail_render.store(true, std::memory_order_release);
        fixture.held[0U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().filter == policy.filter; }));
        CHECK(explore.snapshot().viewport == incumbent.viewport);
        CHECK(explore.snapshot().overlay == policy.overlay);
        const auto observations = fixture.Recorded();
        const auto failed = std::ranges::find_if(observations, [](const auto& event) { return event.failed; });
        REQUIRE(failed != observations.end());
        CHECK(failed->snapshot.busy);
        CHECK(failed->snapshot.frame == incumbent.frame);
        CHECK(failed->snapshot.filter == incumbent.filter);
        REQUIRE(std::next(failed) != observations.end());
        CHECK(observations.back().snapshot.filter == policy.filter);
        fixture.held[1U] = {};
        fixture.RequestViewport(1U);
        CHECK(explore.snapshot().viewport.first_row == 1U);
        CHECK(explore.snapshot().filter == policy.filter);
        CHECK(explore.snapshot().overlay == policy.overlay);
    }
}

TEST_CASE("Explore settles a pending predecessor on the incumbent before reconstructing Open") {
    ExplorePressureFixture fixture;
    fixture.HoldGalleryPool();
    auto& explore = fixture.scenario.system();
    fixture.RequestViewport(0U, 0U);
    const bool initial_h2d = fixture.settings.system().explore_settings_candidate().loading.h2d_dataloader;
    contracts::SettingsUpdateRequest update;
    update.updates.push_back(
        {.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
    static_cast<void>(fixture.settings.system().Update(std::move(update)));
    CHECK(explore.Open({.viewport = fixture.viewport, .compiled_source = "/replacement"}).busy);
    CHECK(fixture.probe.runtimes.load(std::memory_order_acquire) == 1U);
    fixture.held = {};
    REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy; }));
    const auto observations = fixture.Recorded();
    const auto predecessor = std::ranges::find_if(
        observations, [](const auto& event) { return event.snapshot.busy && event.snapshot.viewport.first_row == 0U; });
    REQUIRE(predecessor != observations.end());
    CHECK(predecessor->runtimes == 1U);
    CHECK(predecessor->snapshot.order.visible_indices == std::vector<std::uint32_t>{0U});
    CHECK(predecessor->snapshot.focused_image == 0U);
    CHECK(fixture.probe.runtimes.load(std::memory_order_acquire) == 2U);
    CHECK(explore.snapshot().frame != predecessor->snapshot.frame);
    CHECK_FALSE(explore.snapshot().focused_image);
}

TEST_CASE("Stop cancels Explore work resumed by an output availability continuation") {
    ExplorePressureFixture fixture{true};
    fixture.HoldGalleryPool();
    auto& explore = fixture.scenario.system();
    const auto incumbent = explore.snapshot();
    fixture.RequestViewport(0U);
    fixture.held[0U] = {};
    mmltk::testsupport::await_test_promise(fixture.probe.render_gate->entered, "resumed Explore render");
    static_cast<void>(explore.Stop());
    fixture.probe.render_gate->release.set_value();
    fixture.held = {};
    explore.Shutdown();
    CHECK(explore.snapshot().frame == incumbent.frame);
    CHECK(explore.snapshot().viewport == incumbent.viewport);
    CHECK_FALSE(explore.snapshot().busy);
}

[[nodiscard]] auto settle_explore_on_exit(ExploreSystem& explore, std::promise<void>& release) {
    return mmltk::testsupport::ScopedTestCleanup{[&explore, &release] {
        mmltk::testsupport::release_test_promise(release);
        static_cast<void>(explore.Stop());
        explore.Shutdown();
    }};
}

[[nodiscard]] auto settle_upscale_on_exit(UpscaleSystem& upscale, std::promise<void>& release) {
    return mmltk::testsupport::ScopedTestCleanup{[&upscale, &release] {
        mmltk::testsupport::release_test_promise(release);
        upscale.Stop();
        upscale.Shutdown();
    }};
}

[[nodiscard]] ExploreFilterPreferences select_explore_subset(LoadedSettings& settings, ExploreScenario& scenario,
                                                             const std::uint32_t class_index) {
    auto& explore = scenario.system();
    const auto admitted = explore.UpdateFilter({
        .filter = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {class_index}}},
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::None}},
    });
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    return settings.system().explore_settings_candidate().preferences;
}

void check_catalog_selection_preserved(const ExploreFilterPreferences& actual, const ExploreFilterPreferences& expected) {
    CHECK(actual.class_catalog_identity == expected.class_catalog_identity);
    CHECK(actual.policy.filter.class_selection == expected.policy.filter.class_selection);
    CHECK(actual.policy.overlay.class_selection == expected.policy.overlay.class_selection);
}

[[nodiscard]] ExploreScenario::Observer count_explore_failures(std::shared_ptr<std::atomic_uint64_t> failures) {
    return [failures = std::move(failures)](ExploreSystem::event_type event) {
        if (std::holds_alternative<ExploreFailed>(event)) failures->fetch_add(1U, std::memory_order_acq_rel);
    };
}

class StreamingExploreFixture final {
   public:
    explicit StreamingExploreFixture(const std::size_t nproc)
        : backend_(std::make_shared<FakeImageBackend>()),
          probe_(std::make_shared<StreamingExploreProbe>()),
          failures_(std::make_shared<std::atomic_uint64_t>(0U)),
          scenario_(settings_, nproc, streaming_explore_runtime_factory(backend_, probe_), count_explore_failures(failures_)) {}

    [[nodiscard]] LoadedSettings& settings() noexcept { return settings_; }
    [[nodiscard]] FakeImageBackend& backend() noexcept { return *backend_; }
    [[nodiscard]] StreamingExploreProbe& probe() noexcept { return *probe_; }
    [[nodiscard]] std::uint64_t failure_count() const noexcept { return failures_->load(std::memory_order_acquire); }
    [[nodiscard]] ExploreSystem& system() noexcept { return scenario_.system(); }
    void OpenAndWait(const ExploreViewport viewport) { scenario_.OpenAndWait(viewport, "/stream"); }
    [[nodiscard]] bool Wait(std::function<bool()> predicate) { return scenario_.Wait(std::move(predicate)); }
    void ReleaseAndWaitForFrameAfter(const std::uint64_t revision) {
        probe_->ReleaseAll();
        REQUIRE(Wait([&] { return system().snapshot().frame.revision > revision; }));
    }

   private:
    LoadedSettings settings_;
    std::shared_ptr<FakeImageBackend> backend_;
    std::shared_ptr<StreamingExploreProbe> probe_;
    std::shared_ptr<std::atomic_uint64_t> failures_;
    ExploreScenario scenario_;
};

[[nodiscard]] ExploreScenario tracked_explore_scenario(LoadedSettings& settings, std::shared_ptr<FakeImageBackend> backend,
                                                       std::shared_ptr<std::atomic_uint64_t> commits,
                                                       ExploreScenario::Observer observer = {}) {
    return ExploreScenario{settings, std::move(backend), ExploreScenario::TrackCommits(std::move(commits)), std::move(observer)};
}

[[nodiscard]] ExploreSnapshot wait_for_explore_failure(ExploreScenario& scenario, const std::shared_ptr<std::atomic_uint64_t>& failures,
                                                       const std::uint64_t admission_revision) {
    auto& explore = scenario.system();
    REQUIRE(scenario.Wait([&] {
        return failures->load(std::memory_order_acquire) == 1U && !explore.snapshot().busy &&
               explore.snapshot().revision > admission_revision;
    }));
    return explore.snapshot();
}

class TrackedExploreFailureFixture final {
   public:
    TrackedExploreFailureFixture()
        : backend_(std::make_shared<FakeImageBackend>()),
          failures_(std::make_shared<std::atomic_uint64_t>(0U)),
          commits_(std::make_shared<std::atomic_uint64_t>(0U)),
          scenario_(tracked_explore_scenario(settings_, backend_, commits_, count_explore_failures(failures_))) {}

    [[nodiscard]] ExploreScenario& scenario() noexcept { return scenario_; }
    [[nodiscard]] ExploreSystem& system() noexcept { return scenario_.system(); }
    [[nodiscard]] const std::shared_ptr<std::atomic_uint64_t>& failures() const noexcept { return failures_; }
    [[nodiscard]] std::uint64_t commit_count() const noexcept { return commits_->load(std::memory_order_acquire); }
    void OpenAndWait() { scenario_.OpenAndWait({.extent = {64U, 64U}}); }

   private:
    LoadedSettings settings_;
    std::shared_ptr<FakeImageBackend> backend_;
    std::shared_ptr<std::atomic_uint64_t> failures_;
    std::shared_ptr<std::atomic_uint64_t> commits_;
    ExploreScenario scenario_;
};

void check_restored_filter(const ExploreSnapshot& restored, const ExploreSnapshot& committed) {
    CHECK(restored.ready);
    CHECK(restored.filter == committed.filter);
    CHECK(restored.order.visible_indices == committed.order.visible_indices);
    CHECK(restored.frame == committed.frame);
}

[[nodiscard]] bool assignments_match(const StreamingExploreProbe& probe, const std::span<const std::uint32_t> visible_indices) {
    return std::ranges::all_of(probe.assignments, [&](const auto& assignment) {
        return assignment.generation == probe.generation &&
               std::ranges::find(visible_indices, assignment.compiled_index) != visible_indices.end();
    });
}

[[nodiscard]] ExploreClassCatalogIdentity test_explore_catalog_identity() {
    const std::array names{
        contracts::ArtifactClassName{.value = "person"},
        contracts::ArtifactClassName{.value = "vehicle"},
    };
    return explore_class_catalog_identity(names);
}

void persist_explore_catalog(SettingsSystem& settings, const ExploreClassCatalogIdentity identity) {
    const auto candidate = settings.explore_settings_candidate();
    auto preferences = candidate.preferences.policy;
    preferences.filter.class_selection = {};
    preferences.overlay.class_selection = {};
    settings.persist_explore_class_catalog(candidate, identity, preferences);
}

void queue_failed_explore_overlay_update(LoadedSettings& settings) {
    settings.BreakPersistence();
    contracts::SettingsUpdateRequest request;
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.explore.show_boxes",
        .value = mmltk::frameworks::serialization::wire::FlatValue{false},
    });
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.explore.min_instances",
        .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{2U}},
    });
    CHECK_THROWS_AS(settings.system().Update(std::move(request)), contracts::FailedError);
    CHECK(settings.system().snapshot().settings_state.workflows.explore.show_boxes);
    const auto pending = settings.system().explore_settings_candidate().preferences.policy;
    CHECK_FALSE(pending.overlay.show_boxes);
    CHECK(pending.filter.minimum_instances == 2U);
}

void enable_dark_mode(LoadedSettings& settings) {
    contracts::SettingsUpdateRequest update;
    update.updates.push_back({
        .path = "ui.dark_mode",
        .value = mmltk::frameworks::serialization::wire::FlatValue{true},
    });
    static_cast<void>(settings.system().Update(std::move(update)));
}

void release_and_wait_for_idle(ExploreScenario& scenario, std::promise<void>& release) {
    release.set_value();
    REQUIRE(scenario.Wait([&scenario] { return !scenario.system().snapshot().busy; }));
}

[[nodiscard]] VisualDocumentRead test_document(mmltk::frameworks::gpu::BorrowedImageProductReadView pixels) {
    static const auto document = [] {
        auto value = std::make_shared<VisualDocument>();
        value->scene.document = contracts::WorkspaceResource::From("test://image", 1U);
        value->scene.categories.push_back({.value = "object"});
        return std::shared_ptr<const VisualDocument>{std::move(value)};
    }();
    return {std::move(pixels), document};
}
[[nodiscard]] UpscaleRequest test_upscale_request(UpscaleRequest request) {
    request.document = test_document({}).document->facts();
    return request;
}
[[nodiscard]] ExactVisualDocumentBorrower borrow_exactly_from(ExploreSystem& explore) {
    return [&explore](const VisualFrame& frame) {
        if (frame.source != explore.snapshot().frame.source) return VisualDocumentRead{};
        return test_document(borrow_matching_visual_product(frame, explore.BorrowFrame()));
    };
}

template <class System>
[[nodiscard]] VisualSourceReader read_from(System& system) {
    return {
        .source = system.snapshot().frame.source,
        .observe =
            [&system] {
                const auto snapshot = system.snapshot();
                return VisualSourceObservation{.frame = snapshot.frame, .snapshot_revision = snapshot.revision};
            },
        .borrow = [&system] { return system.BorrowFrame(); },
    };
}

void present_and_wait(PresentationSystem& presentation, TestPresentationWriterState& writer, EventGate& events,
                      const VisualSourceReader& source, const std::uint64_t revision) {
    static_cast<void>(presentation.Select(source.source));
    writer.SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == revision; }));
}

class MutableVisualSource final {
   public:
    MutableVisualSource(std::shared_ptr<FakeImageBackend> backend, const VisualExtent extent, const std::uint8_t value = 1U)
        : runtime_(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
              .device = 0, .backend = std::move(backend), .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}) {
        Publish(extent, value);
    }
    void Publish(const VisualExtent extent, const std::uint8_t value) {
        runtime_.Publish(extent.width, extent.height, [value](const auto clean, const auto semantic, const auto) {
            Fill(clean, value);
            Fill(semantic, static_cast<std::uint8_t>(value + 20U));
        });
        const auto borrowed = runtime_.Borrow();
        REQUIRE(borrowed.valid());
        std::scoped_lock lock(mutex_);
        frame_ = visual_frame(identity_, extent, borrowed.plane(0U).revision());
        frame_.clean_revision = frame_.revision;
        snapshot_revision_ = mmltk::common::types::advance_monotonic_identity(snapshot_revision_);
    }
    void SetSemantics(const std::uint8_t value) {
        const auto current = frame();
        auto candidate = runtime_.AcquireOutput({}, runtime_.Completed());
        runtime_.Publish(candidate, current.extent.width, current.extent.height,
                         [value](const auto, const auto semantic, const auto) { Fill(semantic, value); });
        static_cast<void>(runtime_.CommitOutput(std::move(candidate)));
        std::scoped_lock lock(mutex_);
        frame_.revision = runtime_.OutputFacts().revision;
        snapshot_revision_ = mmltk::common::types::advance_monotonic_identity(snapshot_revision_);
    }
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate TryReserveOutput() {
        mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
        return runtime_.TryAcquireOutput(baseline);
    }
    [[nodiscard]] VisualFrame frame() const {
        std::scoped_lock lock(mutex_);
        return frame_;
    }
    [[nodiscard]] VisualDocumentRead BorrowExact(const VisualFrame& frame) const {
        if (frame.source != identity_) return {};
        return test_document(borrow_matching_visual_product(frame, runtime_.Borrow()));
    }
    [[nodiscard]] VisualSourceReader reader() {
        return {
            .source = identity_,
            .observe =
                [this] {
                    std::scoped_lock lock(mutex_);
                    return VisualSourceObservation{.frame = frame_, .snapshot_revision = snapshot_revision_};
                },
            .borrow = [this] { return runtime_.Borrow(); },
        };
    }

   private:
    mutable std::mutex mutex_;
    mmltk::frameworks::gpu::SystemImageRuntime runtime_;
    PresentationSourceIdentity identity_{PresentationSourceKind::Explore, 1U};
    VisualFrame frame_{};
    std::uint64_t snapshot_revision_ = 0U;
};

struct UpscaleSourceFixture final {
    MutableVisualSource source;
    EventGate events;
    UpscaleSystem upscale;

    UpscaleSourceFixture(const std::shared_ptr<FakeImageBackend>& backend, VisualExtent extent, std::uint8_t value,
                         VisualRuntimeFactory runtime)
        : source(backend, extent, value),
          upscale{kDevice, std::move(runtime), [this](const VisualFrame& frame) { return source.BorrowExact(frame); },
                  [this](UpscaleSystem::event_type) { events.Advance(); }} {}
};

class PresentationSourceFixture final {
   public:
    explicit PresentationSourceFixture(const std::size_t buffers = 1U)
        : backend_(std::make_shared<FakeImageBackend>()),
          revisions_(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>()),
          source_(std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
              .device = 0, .backend = backend_, .output_buffer_count = buffers, .product_revisions = revisions_})),
          sources_{VisualSourceReader{
              .source = identity_,
              .observe =
                  [this] {
                      return VisualSourceObservation{
                          .frame = visual_frame(identity_, {16U, 16U}, latest_revision_.load(std::memory_order_acquire)),
                          .snapshot_revision = snapshot_revision_.load(std::memory_order_acquire),
                      };
                  },
              .borrow = [this] { return source_->Borrow(); },
          }} {
        source_->Publish(16U, 16U, [](auto, auto, auto) {});
    }

    [[nodiscard]] std::shared_ptr<FakeImageBackend> backend() const noexcept { return backend_; }
    [[nodiscard]] std::span<const VisualSourceReader> sources() const noexcept { return sources_; }
    [[nodiscard]] PresentationSourceIdentity identity() const noexcept { return identity_; }
    void AdvanceObservation() { snapshot_revision_.fetch_add(1U, std::memory_order_acq_rel); }
    void PublishUnobserved() {
        source_->Publish(16U, 16U, [](auto, auto, auto) {});
    }
    void Advance() {
        PublishUnobserved();
        UpdateObservation();
    }
    [[nodiscard]] auto Completed() const { return source_->Completed(); }
    void SelectCompleted(const mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput& product) {
        source_->SelectOutput(product);
        UpdateObservation();
    }
    void Reconstruct() {
        source_ = std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(
            mmltk::frameworks::gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend_, .product_revisions = revisions_});
        Advance();
    }

   private:
    void UpdateObservation() {
        const auto borrowed = source_->Borrow();
        REQUIRE(borrowed.valid());
        latest_revision_.store(borrowed.plane(0U).revision(), std::memory_order_release);
        snapshot_revision_.fetch_add(1U, std::memory_order_acq_rel);
    }

    std::shared_ptr<FakeImageBackend> backend_;
    std::shared_ptr<mmltk::frameworks::gpu::ImageProductRevisionSequence> revisions_;
    std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime> source_;
    std::atomic<std::uint64_t> latest_revision_{1U};
    std::atomic<std::uint64_t> snapshot_revision_{1U};
    PresentationSourceIdentity identity_{PresentationSourceKind::Explore, 1U};
    std::array<VisualSourceReader, 1U> sources_;
};

class ProductPresentationSources final {
   public:
    ProductPresentationSources() : backend_(std::make_shared<FakeImageBackend>()) {
        constexpr std::array kKinds{
            PresentationSourceKind::Explore, PresentationSourceKind::Annotation, PresentationSourceKind::Upscale,
            PresentationSourceKind::Live,    PresentationSourceKind::Predict,
        };
        runtimes_.reserve(kKinds.size());
        sources_.reserve(kKinds.size());
        for (std::size_t index = 0U; index != kKinds.size(); ++index) {
            auto runtime = std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(
                mmltk::frameworks::gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend_});
            const auto extent = VisualExtent{static_cast<std::uint32_t>(16U + index), 16U};
            runtime->Publish(extent.width, extent.height, [](auto, auto, auto) {});
            auto* const private_runtime = runtime.get();
            const PresentationSourceIdentity identity{kKinds[index], 1U};
            runtimes_.push_back(std::move(runtime));
            sources_.push_back({
                .source = identity,
                .observe =
                    [identity, extent] {
                        return VisualSourceObservation{
                            .frame = visual_frame(identity, extent, 1U),
                            .snapshot_revision = 1U,
                        };
                    },
                .borrow = [private_runtime] { return private_runtime->Borrow(); },
            });
        }
    }

    [[nodiscard]] std::shared_ptr<FakeImageBackend> backend() const noexcept { return backend_; }
    [[nodiscard]] std::span<const VisualSourceReader> sources() const noexcept { return sources_; }

   private:
    std::shared_ptr<FakeImageBackend> backend_;
    std::vector<std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime>> runtimes_;
    std::vector<VisualSourceReader> sources_;
};

class PresentationFailureProbe final {
   public:
    void Observe(PresentationSystem::event_type event) {
        if (std::holds_alternative<PresentationFailed>(event)) failures_.fetch_add(1U, std::memory_order_acq_rel);
        events_.Advance();
    }
    [[nodiscard]] EventGate& events() noexcept { return events_; }
    [[nodiscard]] std::size_t failures() const noexcept { return failures_.load(std::memory_order_acquire); }

   private:
    EventGate events_;
    std::atomic<std::size_t> failures_{0U};
};

[[nodiscard]] PresentationSystem make_failure_presentation(const PresentationSourceFixture& source,
                                                           const std::shared_ptr<TestPresentationWriterState>& writer_state,
                                                           PresentationFailureProbe& failures) {
    return PresentationSystem{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources(),
                              [&failures](PresentationSystem::event_type event) { failures.Observe(std::move(event)); }};
}

TEST_CASE("Presentation monotonic identities fail before wrap") {
    namespace identity = mmltk::common::types;
    CHECK(identity::advance_monotonic_identity(0U) == 1U);
    CHECK(identity::advance_monotonic_identity(4U, 2U) == 6U);
    CHECK_THROWS_AS(identity::advance_monotonic_identity(4U, 0U), std::overflow_error);
    CHECK_THROWS_AS(identity::advance_monotonic_identity(std::numeric_limits<std::uint64_t>::max()), std::overflow_error);
    std::uint64_t next = 0U;
    CHECK_THROWS_AS(identity::take_monotonic_identity(next), std::overflow_error);
    CHECK(next == 0U);
    next = 3U;
    CHECK(identity::take_monotonic_identity(next, 0U) == 3U);
    CHECK(next == 3U);
    CHECK(identity::take_monotonic_identity(next, 2U) == 3U);
    CHECK(next == 5U);
    std::uint64_t timeline = std::numeric_limits<std::uint64_t>::max() - 1U;
    CHECK_THROWS_AS(identity::take_monotonic_identity(timeline, 2U), std::overflow_error);
    CHECK(timeline == std::numeric_limits<std::uint64_t>::max() - 1U);
}

TEST_CASE("Workspace transfer sequences map to their odd timeline values") {
    namespace workspace = mmltk::controller::presentation::detail;
    CHECK(workspace::workspace_timeline_ready(1U) == 1U);
    CHECK(workspace::workspace_timeline_ready(2U) == 3U);
    CHECK(workspace::workspace_timeline_ready(3U) == 5U);
    CHECK_THROWS_AS(workspace::workspace_timeline_ready(0U), std::overflow_error);
    CHECK_THROWS_AS(workspace::workspace_timeline_ready(std::numeric_limits<std::uint64_t>::max()), std::overflow_error);
}

TEST_CASE("Explore parallelism follows the current Linux affinity limit") {
    const auto automatic = normalize_explore_parallelism(0U);
    CHECK(automatic >= 1U);
    CHECK(automatic <= kExploreMaximumParallelism);
    CHECK(normalize_explore_parallelism(kExploreMaximumParallelism + 100U) == automatic);
    CHECK(normalize_explore_parallelism(1U) == 1U);
}

TEST_CASE("Explore demand retains only its scalar and observes supersession and restoration", "[explore][demand]") {
    CHECK(ExploreDemandCheck{}(0U));
    CHECK(ExploreDemandCheck{}(99U));
    ExploreDemandCheck retained;
    std::weak_ptr<const std::atomic<std::uint64_t>> lifetime;
    {
        auto issuer = std::make_shared<std::atomic<std::uint64_t>>(11U);
        lifetime = issuer;
        retained = ExploreDemandCheck{issuer};
        CHECK(retained(11U));
        std::promise<void> superseded;
        auto changed = superseded.get_future();
        auto observer = std::async(std::launch::async, [retained, changed = std::move(changed)]() mutable {
            changed.wait();
            return !retained(11U) && retained(12U);
        });
        issuer->store(12U, std::memory_order_release);
        superseded.set_value();
        CHECK(observer.get());
        issuer->store(11U, std::memory_order_release);
        CHECK(retained(11U));
        CHECK_FALSE(retained(12U));
        issuer->store(0U, std::memory_order_release);
    }
    CHECK_FALSE(lifetime.expired());
    CHECK_FALSE(retained(11U));
    CHECK(retained(0U));
    retained = {};
    CHECK(lifetime.expired());
}

TEST_CASE("Explore demand completes on another thread while publication owns finalization", "[explore][demand]") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    auto gate = std::make_shared<ExplorePublicationDemandGate>();
    probe->publication_gate = gate;
    auto entered = gate->entered.get_future();
    EventGate events;
    ExploreSystem explore{settings.system(), kDevice, 2U, streaming_explore_runtime_factory(backend, probe),
                          [&events](ExploreSystem::event_type) { events.Advance(); }};
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    static_cast<void>(explore.Open({.viewport = {.extent = {8U, 4U}, .row_count = 1U, .columns = 2U}, .compiled_source = "/test"}));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    auto observation = std::async(std::launch::async, [gate] { return gate->demand(gate->generation); });
    // Release before joining or asserting: an owner-mutex demand callback must
    // fail the bounded completion check without stranding either worker.
    const auto completion = observation.wait_for(200ms);
    gate->release.set_value();
    CHECK(completion == std::future_status::ready);
    CHECK(mmltk::testsupport::await_test_future(observation, "released Explore demand observation"));
    REQUIRE(events.Wait([&] { return explore.snapshot().ready; }));
    CHECK_FALSE(gate->release_timed_out.load());
}

TEST_CASE("Each Explore runtime binds the same retained demand before ingress", "[explore][demand]") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    auto replacement_probe = std::make_shared<StreamingExploreProbe>();
    auto first_factory = streaming_explore_runtime_factory(backend, probe);
    auto replacement_factory = streaming_explore_runtime_factory(backend, replacement_probe);
    auto constructions = std::make_shared<std::atomic<std::size_t>>(0U);
    ExploreDemandCheck retained;
    std::uint64_t committed_generation = 0U;
    {
        EventGate events;
        ExploreSystem explore{settings.system(), kDevice, 2U,
                              [first_factory, replacement_factory, constructions](auto revisions) {
                                  return constructions->fetch_add(1U) == 0U ? first_factory(std::move(revisions))
                                                                            : replacement_factory(std::move(revisions));
                              },
                              [&events](ExploreSystem::event_type) { events.Advance(); }};
        const ExploreViewport viewport{.extent = {8U, 4U}, .row_count = 1U, .columns = 2U};
        open_streaming_gallery(explore, events, viewport);
        {
            std::scoped_lock lock(probe->mutex);
            REQUIRE(probe->bound_demands.size() == 1U);
            CHECK(probe->bound_opens == 1U);
            retained = probe->bound_demands.front();
        }
        const auto first = explore.snapshot();
        CHECK(retained(first.gallery.generation));
        contracts::SettingsUpdateRequest update;
        update.updates.push_back({.path = "workflows.explore.h2d_dataloader",
                                  .value = mmltk::frameworks::serialization::wire::FlatValue{
                                      !settings.system().explore_settings_candidate().loading.h2d_dataloader}});
        static_cast<void>(settings.system().Update(std::move(update)));
        static_cast<void>(explore.Open({.viewport = viewport, .compiled_source = "/replacement"}));
        REQUIRE(events.Wait([&] { return explore.snapshot().ready && explore.snapshot().frame.revision > first.frame.revision; }));
        {
            std::scoped_lock lock(replacement_probe->mutex);
            REQUIRE(replacement_probe->bound_demands.size() == 1U);
            CHECK(replacement_probe->bound_opens == 1U);
            CHECK(replacement_probe->bound_demands.front().generation() == retained.generation());
        }
        committed_generation = explore.LastInteractionGeneration();
        CHECK(retained(committed_generation));
        CHECK_FALSE(retained(first.gallery.generation));
    }
    CHECK_FALSE(retained(committed_generation));
}

TEST_CASE("Explore shutdown invalidates retained demand after cancelled replacement rollback", "[explore][demand]") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<CancellationProbe>();
    auto retained = std::make_shared<ExploreDemandCheck>();
    auto entered = probe->entered.get_future();
    EventGate events;
    ExploreSystem explore{settings.system(), kDevice, 2U,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [probe, retained] { return std::make_unique<CancellableExploreAlgorithm>(probe, retained); }, 3U),
                          [&events](ExploreSystem::event_type) { events.Advance(); }};
    const ExploreViewport viewport{.extent = {32U, 32U}};
    static_cast<void>(explore.Open({.viewport = viewport, .compiled_source = "/incumbent"}));
    REQUIRE(events.Wait([&] { return explore.snapshot().ready; }));
    const auto incumbent = explore.snapshot();
    static_cast<void>(explore.Open({.viewport = viewport, .compiled_source = "/replacement"}));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    const auto replacement = explore.LastInteractionGeneration();
    CHECK((*retained)(replacement));
    explore.Shutdown();
    CHECK(explore.snapshot().frame == incumbent.frame);
    CHECK_FALSE((*retained)(incumbent.gallery.generation));
    CHECK_FALSE((*retained)(replacement));
}

TEST_CASE("Explore publishes placeholders before loaders and patches released lanes incrementally") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    EventGate events;
    ExploreSystem explore{settings.system(), kDevice, 2U, streaming_explore_runtime_factory(backend, probe),
                          [&events](ExploreSystem::event_type) { events.Advance(); }};
    open_streaming_gallery(explore, events, {.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    const auto placeholder = explore.snapshot();
    CHECK_FALSE(placeholder.busy);
    CHECK(placeholder.order.visible_indices == std::vector<std::uint32_t>{0U, 1U});
    CHECK(placeholder.gallery.generation != 0U);
    CHECK(placeholder.gallery.slots == std::vector<bool>{false, false});
    check_gallery_first_pixels(explore, 0x11U, 0x11U);
    {
        std::scoped_lock lock(probe->mutex);
        CHECK(probe->assignments.empty());
        CHECK(probe->maximum_active == 0U);
        CHECK(probe->publications == 0U);
        CHECK(probe->lane_preparations == 0U);
        CHECK(probe->queued_closures == 0U);
    }
    probe->AllowAllocation();
    REQUIRE(probe->Wait([&] { return probe->assignments.size() == 2U; }));
    {
        std::scoped_lock lock(probe->mutex);
        CHECK(probe->assignments.size() == 2U);
        CHECK(probe->maximum_active == 2U);
        CHECK(probe->publications == 0U);
        CHECK(probe->lane_preparations == 2U);
        CHECK(probe->queued_closures == 2U);
    }

    probe->Release(0U);
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > placeholder.frame.revision; }));
    const auto first_patch = explore.snapshot();
    CHECK(first_patch.gallery.slots == std::vector<bool>{true, false});
    check_gallery_first_pixels(explore, 0x40U, 0x11U);
    probe->Release(1U);
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > first_patch.frame.revision; }));
    check_gallery_first_pixels(explore, 0x40U, 0x41U);
    const auto before_focus = explore.snapshot();
    CHECK(before_focus.gallery.slots == std::vector<bool>{true, true});
    explore.UpdateViewport({.viewport = {.extent = {8U, 4U}, .row_count = 1U, .columns = 2U}, .focused_compiled_index = 1U});
    REQUIRE(events.Wait([&] { return explore.snapshot().focused_image == 1U; }));
    CHECK(explore.snapshot().gallery.generation == before_focus.gallery.generation);
    CHECK(explore.snapshot().gallery.slots == before_focus.gallery.slots);
    std::scoped_lock lock(probe->mutex);
    CHECK(probe->lane_preparations == 2U);
}

TEST_CASE("Explore owns direction across accepted logical rows and zero-movement updates", "[explore][priority]") {
    StreamingExploreFixture scenario{2U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    const auto check = [&](const std::uint32_t row, const ExploreScrollDirection direction) {
        const auto before = explore.snapshot().revision;
        explore.UpdateViewport({.viewport = {.extent = {8U, 4U}, .first_row = row, .row_count = 1U, .columns = 2U}});
        REQUIRE(scenario.Wait([&] { return explore.snapshot().viewport.first_row == row && explore.snapshot().revision > before; }));
        std::scoped_lock lock(scenario.probe().mutex);
        CHECK(scenario.probe().scroll_direction == direction);
    };
    check(1U, ExploreScrollDirection::Forward);
    check(2U, ExploreScrollDirection::Forward);
    check(1U, ExploreScrollDirection::Backward);
    check(1U, ExploreScrollDirection::Backward);
    check(0U, ExploreScrollDirection::Backward);
    check(2U, ExploreScrollDirection::Forward);
}

TEST_CASE("Explore batches ready lanes and stale viewport completions cannot publish") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    EventGate events;
    std::atomic_uint64_t changed_events = 0U;
    DiagnosticCapture diagnostics;
    ExploreSystem explore{settings.system(),
                          kDevice,
                          2U,
                          streaming_explore_runtime_factory(backend, probe),
                          [&events, &changed_events](ExploreSystem::event_type event) {
                              if (std::holds_alternative<ExploreChanged>(event)) changed_events.fetch_add(1U, std::memory_order_release);
                              events.Advance();
                          },
                          diagnostics.sink()};
    open_streaming_gallery(explore, events, {.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    probe->AllowAllocation();
    REQUIRE(probe->Wait([&] { return probe->assignments.size() == 2U; }));
    const auto placeholder_revision = explore.snapshot().frame.revision;
    probe->ReleaseAll();
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > placeholder_revision; }));
    {
        std::scoped_lock lock(probe->mutex);
        CHECK(probe->publications == 1U);
        CHECK(probe->cumulative == 2U);
        CHECK(probe->maximum_active == 2U);
    }

    const auto before_old_viewport = explore.snapshot().frame.revision;
    static_cast<void>(explore.UpdateAugmentation({.enabled = true}));
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > before_old_viewport; }));
    REQUIRE(probe->Wait([&] {
        return probe->assignments.size() == 2U && std::ranges::all_of(probe->assignments, [](const auto& assignment) {
                   return assignment.compiled_index == 0U || assignment.compiled_index == 1U;
               });
    }));
    probe->StartRead(0U);
    explore.UpdateViewport({.viewport = {.extent = {8U, 4U}, .first_row = 2U, .row_count = 1U, .columns = 2U}});
    REQUIRE(events.Wait([&] { return explore.snapshot().order.visible_indices == std::vector<std::uint32_t>{4U, 5U}; }));
    const auto newest_placeholder = explore.snapshot().frame.revision;
    const auto events_at_placeholder = changed_events.load(std::memory_order_acquire);
    probe->Release(0U);
    probe->Release(1U);
    REQUIRE(probe->Wait([&] {
        return probe->stale == 2U && std::ranges::all_of(probe->assignments, [](const auto& assignment) {
                   return assignment.compiled_index == 4U || assignment.compiled_index == 5U;
               });
    }));
    CHECK(explore.snapshot().frame.revision == newest_placeholder);
    CHECK(changed_events.load(std::memory_order_acquire) == events_at_placeholder);
    CHECK(explore.snapshot().order.visible_indices == std::vector<std::uint32_t>{4U, 5U});
    CHECK(diagnostics.stale_discarded.load(std::memory_order_acquire) == 2U);
    {
        std::scoped_lock lock(probe->mutex);
        CHECK(probe->mapped_reads == 1U);
    }
    probe->ReleaseAll();
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > newest_placeholder; }));
    CHECK(explore.snapshot().order.visible_indices == std::vector<std::uint32_t>{4U, 5U});
}

TEST_CASE("Explore filter rollback quiesces candidate lanes before restoring committed streaming work") {
    StreamingExploreFixture scenario{2U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    scenario.probe().AllowAllocation();
    REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 2U; }));
    const auto committed = explore.snapshot();
    scenario.settings().BreakPersistence();
    const auto admitted = explore.UpdateFilter({.filter = {.minimum_instances = 1U}});
    REQUIRE(scenario.Wait(
        [&] { return scenario.failure_count() == 1U && !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    const auto restored = explore.snapshot();
    CHECK(restored.ready);
    CHECK(restored.order.visible_indices == committed.order.visible_indices);
    CHECK(restored.filter == committed.filter);
    CHECK(restored.frame == committed.frame);
    CHECK_FALSE(restored.failure.empty());
    REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 2U; }));
    {
        std::scoped_lock lock(scenario.probe().mutex);
        CHECK(scenario.probe().quiescences >= 1U);
        CHECK(assignments_match(scenario.probe(), committed.order.visible_indices));
    }
    const auto restored_frame = restored.frame.revision;
    scenario.ReleaseAndWaitForFrameAfter(restored_frame);
    CHECK(explore.snapshot().order.visible_indices == committed.order.visible_indices);
}

TEST_CASE("Explore prepared-product failure restores exact borrows or retires failed rollback") {
    const bool rollback_fails = GENERATE(false, true);
    // CLEANUP-IGNORE: This fixture configures prepared-product and rollback faults; the preceding scenario configures
    // persistence failure, so their identical admission preamble is not shared behavior.
    StreamingExploreFixture scenario{2U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    scenario.probe().AllowAllocation();
    REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 2U; }));
    const auto incumbent = explore.snapshot();
    auto borrowed = rollback_fails ? mmltk::frameworks::gpu::BorrowedImageProductReadView{} : explore.BorrowFrame();
    if (!rollback_fails) REQUIRE(borrowed.valid());
    {
        std::scoped_lock lock(scenario.probe().mutex);
        scenario.probe().fail_next_labels = true;
        scenario.probe().fail_rollback = rollback_fails;
    }
    explore.UpdateViewport({.viewport = {.extent = {8U, 4U}, .first_row = 1U, .row_count = 1U, .columns = 2U}});
    REQUIRE(scenario.Wait([&] { return scenario.failure_count() == 1U; }));
    const auto failed = explore.snapshot();
    CHECK_FALSE(failed.failure.empty());
    {
        std::scoped_lock lock(scenario.probe().mutex);
        CHECK(scenario.probe().rollbacks == 1U);
    }
    if (rollback_fails) {
        CHECK_FALSE(failed.ready);
        CHECK_FALSE(explore.BorrowFrame().valid());
        CHECK(scenario.backend().streams_destroyed == 1U);
    } else {
        CHECK(failed.frame == incumbent.frame);
        CHECK(failed.gallery.generation == incumbent.gallery.generation);
        CHECK(failed.gallery.slots == incumbent.gallery.slots);
        CHECK(failed.viewport == incumbent.viewport);
        CHECK(failed.order.visible_indices == incumbent.order.visible_indices);
        CHECK(borrowed.plane(0U).revision() == incumbent.frame.revision);
        borrowed = {};
        scenario.ReleaseAndWaitForFrameAfter(incumbent.frame.revision);
    }
}

// CLEANUP-IGNORE: This test begins a lifecycle-stop contract distinct from the preceding rollback transaction.
TEST_CASE("Explore Stop abandons unfinished thumbnails while retaining completed product meaning") {
    // CLEANUP-IGNORE: Stop coverage deliberately creates unfinished lanes but does not configure candidate failure.
    StreamingExploreFixture scenario{2U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    scenario.probe().AllowAllocation();
    REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 2U; }));
    const auto incumbent = explore.snapshot();
    static_cast<void>(explore.Stop());
    scenario.probe().ReleaseAll();
    explore.Shutdown();
    const auto stopped = explore.snapshot();
    CHECK(stopped.frame == incumbent.frame);
    CHECK(stopped.gallery.generation == incumbent.gallery.generation);
    CHECK(stopped.gallery.slots == incumbent.gallery.slots);
    CHECK(stopped.order.visible_indices == incumbent.order.visible_indices);
    CHECK(scenario.failure_count() == 0U);
}

TEST_CASE("Explore failed staged runtime construction resumes the incumbent unfinished gallery") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    auto replacement_probe = std::make_shared<StreamingExploreProbe>();
    auto attempts = std::make_shared<std::atomic_uint32_t>(0U);
    LoadedSettings settings;
    const auto initial_h2d = settings.system().explore_settings_candidate().loading.h2d_dataloader;
    const auto select_transport = [&](const bool h2d) {
        contracts::SettingsUpdateRequest update;
        update.updates.push_back(
            {.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{h2d}});
        static_cast<void>(settings.system().Update(std::move(update)));
    };
    auto incumbent_factory = streaming_explore_runtime_factory(backend, probe);
    auto replacement_factory = streaming_explore_runtime_factory(backend, replacement_probe);
    std::promise<ExploreSnapshot> failed;
    auto failure = failed.get_future();
    ExploreSystem* active = nullptr;
    std::atomic_bool exact_incumbent_borrow = false;
    ExploreScenario scenario{settings, 2U,
                             [attempts, incumbent_factory, replacement_factory](auto revisions) {
                                 if (attempts->fetch_add(1U) == 0U) return incumbent_factory(std::move(revisions));
                                 // Construct real replacement execution resources, then reject before
                                 // the staged owner can replace the still-live incumbent runtime.
                                 auto rejected = replacement_factory(std::move(revisions));
                                 throw std::runtime_error("deterministic staged Explore construction failure");
                             },
                             [&](ExploreSystem::event_type event) {
                                 if (const auto* value = std::get_if<ExploreFailed>(&event)) {
                                     exact_incumbent_borrow = visual_product_matches_frame(value->snapshot.frame, active->BorrowFrame());
                                     // Remove the rejected execution request before the next normal
                                     // completion rechecks settings. No new Explore demand is sent.
                                     select_transport(initial_h2d);
                                     failed.set_value(value->snapshot);
                                     probe->ReleaseAll();
                                 }
                             }};
    auto& explore = scenario.system();
    active = &explore;
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U}, "/incumbent");
    probe->AllowAllocation();
    REQUIRE(probe->Wait([&] { return probe->assignments.size() == 2U; }));
    const auto incumbent = explore.snapshot();
    auto held = explore.BorrowFrame();
    REQUIRE(held.valid());
    select_transport(!initial_h2d);
    static_cast<void>(
        explore.Open({.viewport = {.extent = {8U, 4U}, .first_row = 1U, .row_count = 1U, .columns = 2U}, .compiled_source = "/rejected"}));
    REQUIRE(failure.wait_for(2s) == std::future_status::ready);
    const auto restored = failure.get();
    CHECK(restored.ready);
    CHECK_FALSE(restored.failure.empty());
    CHECK(restored.dataset.identity == incumbent.dataset.identity);
    CHECK(restored.order.visible_indices == incumbent.order.visible_indices);
    CHECK(restored.viewport == incumbent.viewport);
    CHECK(restored.gallery.generation == incumbent.gallery.generation);
    CHECK(restored.gallery.slots == incumbent.gallery.slots);
    CHECK(restored.frame == incumbent.frame);
    CHECK(exact_incumbent_borrow.load());
    CHECK(held.plane(0U).revision() == incumbent.frame.revision);
    REQUIRE(scenario.Wait([&] { return explore.snapshot().frame.revision > incumbent.frame.revision; }));
    CHECK(explore.snapshot().order.visible_indices == incumbent.order.visible_indices);
    CHECK(explore.snapshot().viewport == incumbent.viewport);
    CHECK(attempts->load() == 2U);
    {
        std::scoped_lock lock(probe->mutex);
        CHECK(probe->quiescences == 0U);
        CHECK(probe->opened_source == "/incumbent");
        CHECK(probe->cumulative == incumbent.order.visible_indices.size());
    }
    {
        std::scoped_lock lock(replacement_probe->mutex);
        CHECK(replacement_probe->quiescences == 1U);
        CHECK(replacement_probe->opened_source.empty());
    }
    held = {};
    explore.Shutdown();
}

TEST_CASE("Explore render mutations abort queued lanes before failed cancelled and stale restoration") {
    enum class Mutation : std::uint8_t {
        Augmentation,
        Reroll,
    };
    enum class Rejection : std::uint8_t { Failed, Cancelled, Stale };
    const auto run = [](const Mutation mutation, const Rejection rejection) {
        StreamingExploreFixture scenario{2U};
        auto& explore = scenario.system();
        scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
        if (mutation == Mutation::Reroll) {
            const auto enabled = explore.UpdateAugmentation({.enabled = true});
            REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > enabled.revision; }));
        }
        scenario.probe().AllowAllocation();
        REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 2U; }));
        scenario.probe().AllowPreRead();
        REQUIRE(scenario.probe().Wait([&] { return scenario.probe().mapped_reads != 0U; }));
        const auto committed = explore.snapshot();
        std::size_t publications_before;
        {
            std::scoped_lock lock(scenario.probe().mutex);
            publications_before = scenario.probe().publications;
        }
        std::shared_ptr<ExplorePostRenderGate> gate;
        std::future<void> entered;
        mmltk::testsupport::ScopedTestCleanup settle_mutation{[&] {
            if (gate) mmltk::testsupport::release_test_promise(gate->release);
            scenario.probe().ReleaseAll();
            static_cast<void>(explore.Stop());
            explore.Shutdown();
        }};
        if (rejection == Rejection::Failed) {
            std::scoped_lock lock(scenario.probe().mutex);
            scenario.probe().fail_next_render = true;
        } else {
            gate = std::make_shared<ExplorePostRenderGate>(0U);
            entered = gate->entered.get_future();
            std::scoped_lock lock(scenario.probe().mutex);
            scenario.probe().render_gate = gate;
        }
        const auto admitted = [&] {
            switch (mutation) {
                case Mutation::Augmentation:
                    return explore.UpdateAugmentation({.enabled = true});
                case Mutation::Reroll:
                    return explore.RerollAugmentation();
            }
            std::terminate();
        }();
        if (gate) {
            REQUIRE(entered.wait_for(2s) == std::future_status::ready);
            entered.get();
            if (rejection == Rejection::Cancelled) {
                static_cast<void>(explore.Stop());
            } else {
                enable_dark_mode(scenario.settings());
            }
            gate->release.set_value();
        }
        REQUIRE(scenario.Wait([&] {
            const auto snapshot = explore.snapshot();
            return !snapshot.busy && snapshot.revision > admitted.revision;
        }));
        // CLEANUP-IGNORE: Render-mutation restoration checks preview/detail facts; open failure checks catalog facts.
        const auto restored = explore.snapshot();
        CHECK(restored.ready);
        CHECK(restored.augmentation.enabled == committed.augmentation.enabled);
        CHECK(restored.augmentation.seed == committed.augmentation.seed);
        CHECK(restored.detail.show_original_dimensions == committed.detail.show_original_dimensions);
        CHECK(restored.order.visible_indices == committed.order.visible_indices);
        CHECK((rejection == Rejection::Cancelled) == restored.failure.empty());
        CHECK(scenario.failure_count() == (rejection == Rejection::Cancelled ? 0U : 1U));
        REQUIRE(scenario.probe().Wait([&] { return scenario.probe().rollbacks != 0U && scenario.probe().assignments.size() == 2U; }));
        {
            std::scoped_lock lock(scenario.probe().mutex);
            CHECK(scenario.probe().aborted_assignments >= 2U);
            CHECK(scenario.probe().publications == publications_before);
            CHECK(assignments_match(scenario.probe(), committed.order.visible_indices));
            scenario.probe().render_gate.reset();
        }
        const auto restored_frame = restored.frame.revision;
        if (rejection == Rejection::Cancelled) {
            // Stop abandons the restored continuation. A subsequent explicit
            // viewport demand resumes the same logical gallery.
            explore.UpdateViewport({.viewport = restored.viewport});
        }
        scenario.ReleaseAndWaitForFrameAfter(restored_frame);
        CHECK(explore.snapshot().order.visible_indices == committed.order.visible_indices);
        explore.Shutdown();
    };

    for (const auto mutation : {Mutation::Augmentation, Mutation::Reroll})
        for (const auto rejection : {Rejection::Failed, Rejection::Cancelled, Rejection::Stale}) {
            CAPTURE(static_cast<int>(mutation), static_cast<int>(rejection));
            run(mutation, rejection);
        }
}

TEST_CASE("Explore callback admission failure quiesces its runtime and permits a clean reopen") {
    StreamingExploreFixture scenario{1U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {4U, 4U}, .row_count = 1U, .columns = 1U});
    scenario.probe().AllowAllocation();
    REQUIRE(scenario.probe().Wait([&] { return scenario.probe().assignments.size() == 1U; }));
    {
        std::scoped_lock lock(scenario.probe().mutex);
        scenario.probe().fail_callback_admission = true;
    }
    scenario.backend().FailPersistently(FakeImageBackend::FailurePoint::SynchronizeStream);
    scenario.probe().Release(0U);
    REQUIRE(scenario.Wait([&] { return scenario.failure_count() == 1U; }));
    CHECK_FALSE(explore.snapshot().ready);
    CHECK_FALSE(explore.snapshot().failure.empty());
    CHECK(scenario.backend().synchronized >= 1U);
    CHECK(scenario.backend().streams_destroyed == 1U);
    {
        std::scoped_lock lock(scenario.probe().mutex);
        CHECK(scenario.probe().quiescences >= 1U);
        CHECK(scenario.probe().assignments.empty());
    }

    {
        std::scoped_lock lock(scenario.probe().mutex);
        scenario.probe().fail_callback_admission = false;
    }
    scenario.backend().FailAfter(FakeImageBackend::FailurePoint::None);
    static_cast<void>(explore.Open({.viewport = {.extent = {4U, 4U}, .row_count = 1U, .columns = 1U}, .compiled_source = "/stream"}));
    REQUIRE(scenario.Wait([&] { return explore.snapshot().ready && explore.snapshot().failure.empty(); }));
}

TEST_CASE("Explore lane read and callback gates preserve the accepted atlas through shutdown") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<StreamingExploreProbe>();
    EventGate events;
    ExploreSystem explore{settings.system(), kDevice, 1U, streaming_explore_runtime_factory(backend, probe),
                          [&events](ExploreSystem::event_type) { events.Advance(); }};
    open_streaming_gallery(explore, events, {.extent = {4U, 4U}, .row_count = 1U, .columns = 1U});
    const auto placeholder_revision = explore.snapshot().frame.revision;
    probe->AllowAllocation();
    REQUIRE(probe->Wait([&] { return probe->lane_preparations == 1U; }));
    probe->AllowPreRead();
    REQUIRE(probe->Wait([&] { return probe->mapped_reads == 1U; }));
    CHECK(explore.snapshot().frame.revision == placeholder_revision);
    {
        std::scoped_lock lock(probe->mutex);
        probe->callback_completion_allowed = false;
    }
    probe->AllowPostRead();
    REQUIRE(probe->Wait([&] { return probe->publications == 1U; }));
    REQUIRE(events.Wait([&] { return explore.snapshot().frame.revision > placeholder_revision; }));
    const auto submitted_revision = explore.snapshot().frame.revision;
    CHECK(submitted_revision > placeholder_revision);
    CHECK(explore.snapshot().gallery.slots == std::vector<bool>{false});
    {
        std::scoped_lock lock(probe->mutex);
        REQUIRE(probe->assignments.size() == 1U);
        CHECK(probe->assignments.front().gpu_pending);
    }
    probe->CompleteCallbacks();
    REQUIRE(events.Wait([&] { return explore.snapshot().gallery.slots == std::vector<bool>{true}; }));
    CHECK(explore.snapshot().frame.revision == submitted_revision);
    REQUIRE(probe->Wait([&] { return probe->assignments.empty(); }));

    explore.Shutdown();
    CHECK(explore.stopped());
    CHECK(backend->synchronized >= 1U);
    std::scoped_lock lock(probe->mutex);
    CHECK(probe->quiescences >= 1U);
    CHECK(probe->assignments.empty());
}

TEST_CASE("Explore storage diagnostics aggregate renderer and all three output slots lazily") {
    const bool logging_enabled = GENERATE(false, true);
    CAPTURE(logging_enabled);
    class StorageAlgorithm final : public TestExploreAlgorithm {
       public:
        explicit StorageAlgorithm(std::shared_ptr<std::atomic<std::size_t>> calls)
            : TestExploreAlgorithm(std::make_shared<std::atomic<std::size_t>>(0U)), calls_(std::move(calls)) {}
        [[nodiscard]] ExploreStorageFootprint StorageFootprint() const override {
            ++*calls_;
            return {.host_bytes = 101U,
                    .device_bytes = 1000U,
                    .pinned_bytes = 200U,
                    .cache_device_bytes = 300U,
                    .descriptor_bytes = 400U,
                    .augmentation_device_bytes = 50U,
                    .augmentation_pinned_bytes = 25U,
                    .cache_cards = 60U};
        }

       private:
        std::shared_ptr<std::atomic<std::size_t>> calls_;
    };
    struct Capture final {
        std::mutex mutex;
        VisualDiagnosticFact storage{};
        std::atomic_bool enabled{true};
    } capture;
    capture.enabled.store(logging_enabled);
    const VisualDiagnosticSink diagnostics{
        .context = &capture,
        .write =
            [](void* context, VisualDiagnosticFact fact) noexcept {
                if (fact.operation != VisualDiagnosticOperation::ExploreCacheStorage) return;
                auto& observed_capture = *static_cast<Capture*>(context);
                std::scoped_lock lock(observed_capture.mutex);
                observed_capture.storage = fact;
            },
        .enabled = [](void* context) noexcept { return static_cast<Capture*>(context)->enabled.load(); }};
    auto calls = std::make_shared<std::atomic<std::size_t>>(0U);
    auto backend = std::make_shared<FakeImageBackend>();
    auto factory = RuntimeFactory(
        0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
        [calls] { return std::make_unique<StorageAlgorithm>(calls); }, 3U);
    mmltk::frameworks::gpu::SystemImageRuntime* physical = nullptr;
    LoadedSettings settings;
    EventGate events;
    ExploreSystem explore{settings.system(),
                          kDevice,
                          2U,
                          [factory = std::move(factory), &physical](auto revisions) mutable {
                              auto runtime = factory(std::move(revisions));
                              physical = runtime.get();
                              return runtime;
                          },
                          [&events](ExploreSystem::event_type) { events.Advance(); },
                          diagnostics};
    static_cast<void>(explore.Open({.viewport = {.extent = {32U, 32U}, .columns = 1U}, .compiled_source = "/test"}));
    REQUIRE(events.Wait([&] { return explore.snapshot().ready; }));
    const auto first_revision = explore.snapshot().frame.revision;
    auto overlay = explore.snapshot().overlay;
    overlay.show_masks = !overlay.show_masks;
    static_cast<void>(explore.UpdateOverlay(overlay));
    REQUIRE(events.Wait([&] {
        const auto current = explore.snapshot();
        return current.frame.revision > first_revision && current.overlay == overlay;
    }));
    REQUIRE(physical);
    const auto outputs = physical->OutputStorageFootprint();
    CHECK(outputs.device_bytes == 4U * 32U * 32U * 4U);
    if (logging_enabled) {
        std::scoped_lock lock(capture.mutex);
        CHECK(capture.storage.value == 101U);
        CHECK(capture.storage.context.gpu_bytes == 1000U + outputs.device_bytes);
        CHECK(capture.storage.context.staging_bytes == 200U + outputs.pinned_bytes);
        CHECK(capture.storage.context.cache_bytes == 401U);
        CHECK(capture.storage.context.descriptor_bytes == 400U);
        CHECK(capture.storage.context.augmentation_device_bytes == 50U);
        CHECK(capture.storage.context.augmentation_pinned_bytes == 25U);
    }
    const auto before_restore = explore.snapshot().frame.revision;
    overlay.show_masks = !overlay.show_masks;
    static_cast<void>(explore.UpdateOverlay(overlay));
    REQUIRE(events.Wait([&] {
        const auto current = explore.snapshot();
        return current.frame.revision > before_restore && current.overlay == overlay;
    }));
    // Settle every continuation before checking the complete logging mode.
    explore.Shutdown();
    CHECK((calls->load() != 0U) == logging_enabled);
}

TEST_CASE("Explore Open rejects never-loaded settings before runtime construction") {
    SettingsSystem settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto constructions = std::make_shared<std::atomic_uint64_t>(0U);
    auto events = std::make_shared<std::atomic_uint64_t>(0U);
    auto factory = RuntimeFactory(
        0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
        [] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U)); }, 3U);
    ExploreSystem explore{settings, kDevice, 2U,
                          [factory = std::move(factory), constructions](auto revisions) mutable {
                              constructions->fetch_add(1U, std::memory_order_release);
                              return factory(std::move(revisions));
                          },
                          [events](ExploreSystem::event_type) { events->fetch_add(1U, std::memory_order_release); }};
    const auto before = explore.snapshot();

    CHECK_THROWS_AS(explore.Open({.viewport = {}, .compiled_source = "/test"}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}), contracts::UnavailableError);
    CHECK(constructions->load(std::memory_order_acquire) == 0U);
    CHECK(events->load(std::memory_order_acquire) == 0U);
    CHECK(explore.snapshot().revision == before.revision);
    CHECK(explore.snapshot().busy == before.busy);
}

TEST_CASE("Explore settings guards reject reopen and live filter before candidate work") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto work = std::make_shared<ExploreWorkProbe>();
    EventGate events;
    ExploreSystem explore{settings.system(), kDevice, 2U,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [work] {
                                  return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr,
                                                                                nullptr, nullptr, work);
                              },
                              3U),
                          [&events](ExploreSystem::event_type) { events.Advance(); }};
    static_cast<void>(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}));
    REQUIRE(events.Wait([&] { return explore.snapshot().ready; }));
    const auto before = explore.snapshot();
    const auto opens = work->opens.load(std::memory_order_acquire);
    const auto prepares = work->prepares.load(std::memory_order_acquire);
    const auto renders = work->renders.load(std::memory_order_acquire);
    settings.MakeUnavailable();

    CHECK_THROWS_AS(explore.UpdateFilter({
                        .filter = {.minimum_instances = 2U, .maximum_instances = 1U},
                    }),
                    contracts::InvalidIntentError);
    CHECK_THROWS_AS(explore.UpdateFilter({.filter = before.filter, .overlay = before.overlay}), contracts::UnavailableError);
    CHECK_THROWS_AS(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/reopen"}), contracts::UnavailableError);
    CHECK(work->opens.load(std::memory_order_acquire) == opens);
    CHECK(work->prepares.load(std::memory_order_acquire) == prepares);
    CHECK(work->renders.load(std::memory_order_acquire) == renders);
    CHECK(explore.snapshot().revision == before.revision);
    CHECK(explore.snapshot().ready);
    CHECK_FALSE(explore.snapshot().busy);
    CHECK(explore.snapshot().filter == before.filter);
    CHECK(explore.snapshot().frame.source == before.frame.source);
    CHECK(explore.snapshot().frame.extent == before.frame.extent);
    CHECK(explore.snapshot().frame.revision == before.frame.revision);
}

TEST_CASE("Explore viewport and Annotation pointer work preserve their intended ordering") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto observed_nproc = std::make_shared<std::atomic<std::size_t>>(0U);
    DiagnosticCapture diagnostics;
    EventGate explore_events;
    LoadedSettings settings;
    ExploreSystem explore{settings.system(),
                          kDevice,
                          4U,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [observed_nproc] { return std::make_unique<TestExploreAlgorithm>(observed_nproc); }, 3U),
                          [&explore_events](ExploreSystem::event_type) { explore_events.Advance(); },
                          diagnostics.sink()};
    CHECK_FALSE(explore.BorrowFrame().valid());
    CHECK_THROWS_AS(explore.UpdateViewport({.viewport = {.extent = {48U, 48U}}}), contracts::UnavailableError);
    const auto explore_admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"});
    CHECK(explore_admitted.busy);
    CHECK(explore_admitted.revision != 0U);
    REQUIRE(explore_events.Wait([&] { return explore.snapshot().ready; }));
    CHECK_FALSE(explore.snapshot().busy);
    CHECK(explore.snapshot().revision > explore_admitted.revision);
    explore.UpdateViewport({.viewport = {.extent = {48U, 48U}}});
    explore.UpdateViewport({.viewport = {.extent = {32U, 32U}}});
    REQUIRE(explore_events.Wait([&] {
        const auto state = explore.snapshot();
        return state.viewport.extent.width == 32U && state.frame.extent.width == 32U;
    }));
    CHECK(explore.snapshot().nproc == 4U);
    REQUIRE(explore.BorrowFrame().valid());
    CHECK(explore.BorrowFrame().plane(0U).revision() == explore.snapshot().frame.revision);
    CHECK(observed_nproc->load(std::memory_order_acquire) == 4U);
    CHECK(diagnostics.count.load(std::memory_order_acquire) != 0U);
    CHECK(diagnostics.last_system.load(std::memory_order_acquire) == contracts::DiagnosticOwner::Explore);

    EventGate annotation_events;
    std::atomic_uint64_t consumed{0U}, failures{0U};
    auto probe = std::make_shared<AnnotationRenderProbe>();
    AnnotationSystem annotation{kDevice,
        RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [probe] { return std::make_unique<TestAnnotationAlgorithm>(probe); }),
        borrow_exactly_from(explore), [&](AnnotationSystem::event_type event) {
            if (std::holds_alternative<AnnotationFailed>(event)) failures.fetch_add(1U);
            annotation_events.Advance();
        }};
    CHECK_FALSE(annotation.BorrowFrame().valid());
    const auto admitted = annotation.Open({.source = explore.snapshot().frame});
    CHECK(admitted.busy);
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    annotation.SetInputPeer(7U, [&](AnnotationInputProgress progress) {
        consumed.store(progress.consumed_sequence, std::memory_order_release);
        annotation_events.Advance();
    });
    const auto edit = annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}});
    REQUIRE(annotation_events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > edit.revision; }));
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().rendered.scene_revision == annotation.snapshot().ui.scene_revision; }));
    auto retained = annotation.BorrowFrame();
    REQUIRE(retained.valid());
    const auto initial = annotation.snapshot();
    const auto input = [&](std::uint64_t batch, contracts::AnnotationPointerPhase phase, std::uint64_t sequence, float x) {
        return AnnotationInputBatch{.epoch = 7U, .document_epoch = initial.input_document_epoch, .sequence = batch,
            .samples = {{.phase = phase, .interaction_id = 1U, .sequence = sequence, .point = {x, 3.0F + x}}}};
    };
    auto invalid = input(1U, contracts::AnnotationPointerPhase::Begin, 1U, 2.0F);
    invalid.samples.front().target = {.object = 0U, .element = 0U};
    CHECK_THROWS_AS(annotation.Input(invalid), contracts::InvalidIntentError);
    annotation.Input(input(1U, contracts::AnnotationPointerPhase::Begin, 1U, 2.0F));
    REQUIRE(annotation_events.Wait([&] { return consumed.load() == 1U; }));
    annotation.Input(input(2U, contracts::AnnotationPointerPhase::Update, 2U, 7.0F));
    REQUIRE(annotation_events.Wait([&] { return consumed.load() == 2U; }));
    CHECK(annotation.snapshot().ui.document_revision == initial.ui.document_revision);
    annotation.Input(input(3U, contracts::AnnotationPointerPhase::End, 3U, 9.0F));
    REQUIRE(annotation_events.Wait([&] { return consumed.load() == 3U && annotation.snapshot().ui.document_revision > initial.ui.document_revision; }));
    CHECK(annotation.snapshot().frame == initial.frame);
    CHECK(annotation.snapshot().rendered == initial.rendered);
    CHECK_THROWS_AS(annotation.Input(input(3U, contracts::AnnotationPointerPhase::End, 3U, 9.0F)), contracts::InvalidIntentError);
    // Save and undo settle without waiting for the held output, and preserve the
    // ordered box commit in the real reducer/history rather than a mock journal.
    mmltk::testsupport::ScopedTempDir saved_document{"mmltk-annotation-independent-save"};
    const auto destination = saved_document.path() / "annotation.cbor";
    const auto saved = annotation.Save({.destination = destination.string()});
    REQUIRE(annotation_events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > saved.revision; }));
    CHECK(annotation.snapshot().ui.save_status == contracts::AnnotationSaveStatus::Saved);
    std::filesystem::remove(destination);
    const auto objects = annotation.snapshot().ui.scene.objects.size();
    const auto undo = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
    REQUIRE(annotation_events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > undo.revision; }));
    CHECK(annotation.snapshot().ui.scene.objects.size() + 1U == objects);
    CHECK(annotation.snapshot().frame == initial.frame);
    retained = {};
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().frame.revision > initial.frame.revision; }));
    CHECK(annotation.snapshot().frame.clean_revision == initial.frame.clean_revision);
    CHECK(annotation.snapshot().rendered.scene_revision == annotation.snapshot().ui.scene_revision);
    const auto refused = annotation.Save({.destination = "/missing-annotation-directory/file.cbor"});
    REQUIRE(annotation_events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > refused.revision; }));
    CHECK(failures.load() == 1U);
    CHECK(annotation.snapshot().ui.save_status == contracts::AnnotationSaveStatus::Failed);
    explore.Shutdown();
    CHECK(annotation.BorrowFrame().valid());
    annotation.Shutdown();
    CHECK_THROWS_AS(annotation.Input(input(4U, contracts::AnnotationPointerPhase::Begin, 1U, 2.0F)), contracts::UnavailableError);
}

TEST_CASE("Explore installs and atomically persists typed live filter preferences") {
    auto settings_events = std::make_shared<std::atomic_uint64_t>(0U);
    LoadedSettings settings{[settings_events](SettingsSystem::event_type) { settings_events->fetch_add(1U, std::memory_order_acq_rel); }};
    persist_explore_catalog(settings.system(), test_explore_catalog_identity());
    const auto initial_events = settings_events->load(std::memory_order_acquire);
    (void)settings.system().persist_explore_filter(
        settings.system().explore_settings_candidate(),
        {
            .filter =
                {
                    .class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {1U}},
                    .minimum_instances = 1U,
                    .maximum_instances = 9U,
                    .minimum_compiled_index = 0U,
                    .maximum_compiled_index = 2U,
                    .order = ExploreOrder::Shuffled,
                    .shuffle_seed = 41U,
                    .require_boxes = true,
                },
            .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {1U}},
                        .show_boxes = false,
                        .show_masks = true},
        });
    CHECK(settings_events->load(std::memory_order_acquire) == initial_events + 1U);

    auto backend = std::make_shared<FakeImageBackend>();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto scenario = tracked_explore_scenario(settings, backend, commits);
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}});
    CHECK(explore.snapshot().filter.class_selection.classes == std::vector<std::uint32_t>{1U});
    CHECK(explore.snapshot().filter.minimum_instances == 1U);
    CHECK(explore.snapshot().filter.order == ExploreOrder::Shuffled);
    CHECK(explore.snapshot().order.shuffle_seed == 41U);
    CHECK(explore.snapshot().overlay.class_selection.classes == std::vector<std::uint32_t>{1U});
    CHECK_FALSE(explore.snapshot().overlay.show_boxes);

    const ExploreFilterUpdate updated{
        .filter =
            {
                .class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {0U}},
                .minimum_instances = 2U,
                .maximum_instances = 8U,
                .minimum_compiled_index = 1U,
                .maximum_compiled_index = 2U,
                .order = ExploreOrder::Sequential,
                .require_masks = true,
            },
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {0U}},
                    .show_boxes = true,
                    .show_masks = false},
    };
    const auto before_update_events = settings_events->load(std::memory_order_acquire);
    const auto admitted = explore.UpdateFilter(updated);
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().filter == updated.filter);
    CHECK(explore.snapshot().overlay.show_boxes == updated.overlay.show_boxes);
    CHECK(explore.snapshot().overlay.show_masks == updated.overlay.show_masks);
    CHECK(settings.system().explore_settings_candidate().preferences.policy.filter == updated.filter);
    CHECK(settings.system().explore_settings_candidate().preferences.policy.overlay == updated.overlay);
    CHECK(explore.snapshot().order.shuffle_seed == 0U);
    CHECK_THROWS_AS(explore.Reroll(), contracts::UnavailableError);
    CHECK(settings_events->load(std::memory_order_acquire) == before_update_events + 1U);
    CHECK(commits->load(std::memory_order_acquire) == 2U);
}

TEST_CASE("Explore preserves classes for one catalog and resets both selections after a different successful catalog") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto first_identity = explore.snapshot().dataset.class_catalog_identity;
    REQUIRE(first_identity != 0U);

    const ExploreFilterUpdate selected{
        .filter =
            {
                .class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {1U}},
                .minimum_instances = 2U,
            },
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::None}},
    };
    auto admitted = explore.UpdateFilter(selected);
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));

    admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/same-catalog"});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().dataset.class_catalog_identity == first_identity);
    CHECK(explore.snapshot().filter.class_selection == selected.filter.class_selection);
    CHECK(explore.snapshot().overlay.class_selection == selected.overlay.class_selection);

    admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/different-catalog"});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    const auto changed = explore.snapshot();
    CHECK(changed.dataset.class_catalog_identity != first_identity);
    CHECK(changed.filter.minimum_instances == selected.filter.minimum_instances);
    CHECK(changed.filter.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(changed.overlay.class_selection.mode == ExploreClassSelectionMode::All);
    const auto persisted = settings.system().explore_settings_candidate().preferences;
    CHECK(persisted.class_catalog_identity == changed.dataset.class_catalog_identity);
    CHECK(persisted.policy.filter.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(persisted.policy.overlay.class_selection.mode == ExploreClassSelectionMode::All);
}

TEST_CASE("Explore augmentation and detail mutations persist and keep reroll identities distinct") {
    auto backend = std::make_shared<FakeImageBackend>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 48U}, .row_count = 1U, .columns = 2U});
    REQUIRE_FALSE(explore.snapshot().augmentation.enabled);
    REQUIRE(explore.snapshot().augmentation.seed == 0U);

    auto admitted = explore.UpdateAugmentation({.enabled = true});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    const auto order_seed = explore.snapshot().order.shuffle_seed;
    admitted = explore.RerollAugmentation();
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().augmentation.seed == 1U);
    CHECK(explore.snapshot().order.shuffle_seed == order_seed);

    admitted = explore.UpdateDetail({.show_original_dimensions = true});
    CHECK(explore.snapshot().revision == admitted.revision);
    CHECK(explore.snapshot().detail.show_original_dimensions);
    const auto persisted = settings.system().explore_settings_candidate();
    CHECK(persisted.augmentation_preview_enabled);
    CHECK(persisted.show_original_dimensions);

    admitted = explore.UpdateAugmentation({.enabled = false});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().augmentation.seed == 1U);
    CHECK_THROWS_AS(explore.RerollAugmentation(), contracts::UnavailableError);
}

// CLEANUP-IGNORE: Detail-extent and focus-slot scenarios inject different algorithm evidence at distinct boundaries.
TEST_CASE("Explore original-content sampling preserves the full native detail product") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto extents = std::make_shared<ExploreDetailExtentProbe>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, [extents] {
                                 return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr,
                                                                               nullptr, nullptr, nullptr, nullptr, extents);
                             }};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 48U}, .row_count = 1U, .columns = 2U});
    auto admitted = explore.Select({.compiled_index = 1U});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().frame.extent == extents->padded);

    const auto full_frame = explore.snapshot().frame;
    CHECK((full_frame.content == VisualRegion{8U, 16U, 48U, 32U}));
    admitted = explore.UpdateDetail({.show_original_dimensions = true});
    CHECK(admitted.detail.show_original_dimensions);
    CHECK(explore.snapshot().frame == full_frame);
}

TEST_CASE("Explore Open commits the effective pending filter with a different catalog") {
    auto settings_events = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_revision = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_catalog = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_minimum_instances = std::make_shared<std::atomic_uint32_t>(0U);
    auto emitted_show_boxes = std::make_shared<std::atomic_bool>(true);
    LoadedSettings settings{[settings_events, emitted_revision, emitted_catalog, emitted_minimum_instances,
                             emitted_show_boxes](SettingsSystem::event_type event) {
        const auto& snapshot = std::get<SettingsChanged>(event).snapshot;
        emitted_revision->store(snapshot.revision, std::memory_order_release);
        emitted_catalog->store(snapshot.settings_state.workflows.explore.class_catalog_identity, std::memory_order_release);
        emitted_minimum_instances->store(snapshot.settings_state.workflows.explore.min_instances, std::memory_order_release);
        emitted_show_boxes->store(snapshot.settings_state.workflows.explore.show_boxes, std::memory_order_release);
        settings_events->fetch_add(1U, std::memory_order_acq_rel);
    }};
    queue_failed_explore_overlay_update(settings);
    const auto events_before_open = settings_events->load(std::memory_order_acquire);
    settings.RestorePersistence();

    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}}, "/different-catalog");
    const auto opened = explore.snapshot();
    CHECK_FALSE(opened.overlay.show_boxes);
    CHECK(opened.filter.minimum_instances == 2U);
    CHECK(opened.filter.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(opened.overlay.class_selection.mode == ExploreClassSelectionMode::All);

    const auto committed = settings.system().explore_settings_candidate().preferences;
    CHECK(committed.class_catalog_identity == opened.dataset.class_catalog_identity);
    CHECK(committed.policy.filter == opened.filter);
    CHECK(committed.policy.overlay == opened.overlay);
    CHECK(settings_events->load(std::memory_order_acquire) == events_before_open + 1U);
    const auto committed_snapshot = settings.system().snapshot();
    CHECK(emitted_revision->load(std::memory_order_acquire) == committed_snapshot.revision);
    CHECK(emitted_catalog->load(std::memory_order_acquire) == committed.class_catalog_identity);
    CHECK(emitted_minimum_instances->load(std::memory_order_acquire) == committed.policy.filter.minimum_instances);
    CHECK_FALSE(emitted_show_boxes->load(std::memory_order_acquire));

    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(settings.location()).applied());
    const auto durable = reloaded.explore_settings_candidate().preferences;
    CHECK(durable.class_catalog_identity == committed.class_catalog_identity);
    CHECK(durable.policy.filter == committed.policy.filter);
    CHECK(durable.policy.overlay == committed.policy.overlay);
    CHECK(reloaded.snapshot().revision == committed_snapshot.revision);
}

TEST_CASE("failed Explore catalog persistence restores the pending filter for ordinary Retry") {
    LoadedSettings settings;
    auto failures = std::make_shared<std::atomic_uint64_t>(0U);
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend, {}, count_explore_failures(failures)};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto prior = select_explore_subset(settings, scenario, 1U);
    const auto committed = explore.snapshot();
    queue_failed_explore_overlay_update(settings);

    const auto admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/different-catalog"});
    REQUIRE(scenario.Wait([&] { return failures->load(std::memory_order_acquire) == 1U && !explore.snapshot().busy; }));
    CHECK(explore.snapshot().ready);
    CHECK(explore.snapshot().dataset.class_catalog_identity == committed.dataset.class_catalog_identity);
    CHECK(explore.snapshot().order.visible_indices == committed.order.visible_indices);
    CHECK(explore.snapshot().filter == committed.filter);
    CHECK(explore.snapshot().overlay == committed.overlay);
    CHECK(explore.snapshot().frame == committed.frame);
    CHECK(explore.snapshot().revision > admitted.revision);
    CHECK(settings.system().snapshot().settings_state.workflows.explore.class_catalog_identity == prior.class_catalog_identity);

    settings.RestorePersistence();
    REQUIRE(settings.system().Retry().applied());
    const auto recovered = settings.system().explore_settings_candidate().preferences;
    CHECK_FALSE(recovered.policy.overlay.show_boxes);
    CHECK(recovered.policy.filter.minimum_instances == 2U);
    check_catalog_selection_preserved(recovered, prior);

    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(settings.location()).applied());
    const auto durable = reloaded.explore_settings_candidate().preferences;
    CHECK(durable.policy.filter == recovered.policy.filter);
    CHECK(durable.policy.overlay == recovered.policy.overlay);
    CHECK(durable.class_catalog_identity == prior.class_catalog_identity);
}

TEST_CASE("Explore Open rejects a settings candidate made stale during rendering") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto gate = std::make_shared<ExplorePostRenderGate>(0U);
    auto entered = gate->entered.get_future();
    auto failures = std::make_shared<std::atomic_uint64_t>(0U);
    ExploreScenario scenario{settings, backend, ExploreScenario::GateAfterRender(gate), count_explore_failures(failures)};
    auto& explore = scenario.system();
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    static_cast<void>(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/different-catalog"}));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();

    enable_dark_mode(settings);
    gate->release.set_value();
    REQUIRE(scenario.Wait([&] { return failures->load(std::memory_order_acquire) == 1U && !explore.snapshot().busy; }));
    CHECK_FALSE(explore.snapshot().ready);
    CHECK_FALSE(explore.snapshot().failure.empty());
    const auto current = settings.system().snapshot();
    CHECK(current.settings_state.ui.dark_mode);
    CHECK(current.settings_state.workflows.explore.class_catalog_identity == 0U);
}

TEST_CASE("failed and cancelled Explore opens preserve the last successful catalog preferences") {
    SECTION("failed open") {
        LoadedSettings settings;
        auto backend = std::make_shared<FakeImageBackend>();
        ExploreScenario scenario{settings, backend};
        auto& explore = scenario.system();
        scenario.OpenAndWait({.extent = {64U, 64U}});
        const auto before = select_explore_subset(settings, scenario, 0U);

        const auto admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/failed"});
        REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
        CHECK_FALSE(explore.snapshot().failure.empty());
        CHECK(explore.snapshot().ready);
        CHECK(explore.snapshot().dataset.class_catalog_identity == before.class_catalog_identity);
        const auto after = settings.system().explore_settings_candidate().preferences;
        check_catalog_selection_preserved(after, before);
    }

    SECTION("cancelled open") {
        LoadedSettings settings;
        auto backend = std::make_shared<FakeImageBackend>();
        auto gate = std::make_shared<ExploreFinalizationGate>();
        auto entered = gate->entered.get_future();
        ExploreScenario scenario{settings, backend, [gate] {
                                     return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr,
                                                                                   nullptr, gate);
                                 }};
        auto& explore = scenario.system();
        auto settle_explore = settle_explore_on_exit(explore, gate->release);
        scenario.OpenAndWait({.extent = {64U, 64U}});
        const auto before = select_explore_subset(settings, scenario, 0U);

        gate->Arm();
        const auto admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/different-catalog"});
        REQUIRE(entered.wait_for(2s) == std::future_status::ready);
        entered.get();
        static_cast<void>(explore.Stop());
        gate->release.set_value();
        REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
        const auto after = settings.system().explore_settings_candidate().preferences;
        check_catalog_selection_preserved(after, before);
    }
}

TEST_CASE("Explore candidate allocation and render failures restore the committed dataset and frame") {
    const auto run = [](const std::string_view source) {
        TrackedExploreFailureFixture fixture;
        auto& explore = fixture.system();
        fixture.OpenAndWait();
        const auto before = explore.snapshot();

        const auto admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = std::string{source}});
        const auto restored = wait_for_explore_failure(fixture.scenario(), fixture.failures(), admitted.revision);
        CHECK(restored.ready);
        CHECK(restored.dataset.class_catalog_identity == before.dataset.class_catalog_identity);
        CHECK(restored.dataset.image_count == before.dataset.image_count);
        CHECK(restored.dataset.class_names == before.dataset.class_names);
        CHECK(restored.order.matching_count == before.order.matching_count);
        CHECK(restored.order.shuffle_seed == before.order.shuffle_seed);
        CHECK(restored.order.visible_indices == before.order.visible_indices);
        CHECK(restored.frame == before.frame);
        CHECK_FALSE(restored.failure.empty());
        CHECK(fixture.commit_count() == 1U);
    };

    SECTION("candidate allocation") { run("/allocation-failed"); }
    SECTION("candidate render") { run("/render-failed"); }
}

TEST_CASE("Explore filter preparation and render failures discard candidates without invalidating the runtime") {
    const auto run = [](const std::uint32_t minimum_instances) {
        TrackedExploreFailureFixture fixture;
        auto& explore = fixture.system();
        fixture.OpenAndWait();
        const auto committed = explore.snapshot();
        const auto admitted = explore.UpdateFilter({.filter = {.minimum_instances = minimum_instances}});
        const auto restored = wait_for_explore_failure(fixture.scenario(), fixture.failures(), admitted.revision);
        check_restored_filter(restored, committed);
        CHECK_FALSE(explore.snapshot().failure.empty());
        CHECK(fixture.commit_count() == 1U);
        const auto selected = explore.Select({.compiled_index = committed.order.visible_indices.front()});
        REQUIRE(fixture.scenario().Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > selected.revision; }));
        CHECK(explore.snapshot().mode == ExploreMode::Detail);
    };

    SECTION("prepare allocation") { run(7U); }
    SECTION("candidate render") { run(8U); }
}

TEST_CASE("Explore shuffled policy rerolls with a fresh resolved seed and validates dataset policy") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {3U, 80U}, .first_row = 99U, .row_count = 80U, .columns = 3U});
    CHECK(explore.snapshot().viewport.first_row == 0U);
    CHECK(explore.snapshot().viewport.row_count == 1U);
    CHECK(explore.snapshot().viewport.columns == 3U);

    ExploreFilterUpdate shuffled{
        .filter = {.maximum_compiled_index = 2U, .order = ExploreOrder::Shuffled, .shuffle_seed = 97U},
        .overlay = {},
    };
    auto admitted = explore.UpdateFilter(shuffled);
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().order.shuffle_seed == 97U);
    admitted = explore.Reroll();
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().order.shuffle_seed != 0U);
    CHECK(explore.snapshot().order.shuffle_seed != 97U);

    shuffled.filter.minimum_compiled_index = 3U;
    CHECK_THROWS_AS(explore.UpdateFilter(shuffled), contracts::InvalidIntentError);
    shuffled.filter.minimum_compiled_index = 0U;
    shuffled.overlay.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {2U}};
    CHECK_THROWS_AS(explore.UpdateFilter(shuffled), contracts::InvalidIntentError);
}

TEST_CASE("Explore normalizes saved dataset policy locally and projects every class selection mode") {
    LoadedSettings settings;
    persist_explore_catalog(settings.system(), test_explore_catalog_identity());
    const ExploreFilterUpdate saved{
        .filter =
            {
                .class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {1U, 9U}},
                .minimum_compiled_index = 10U,
                .maximum_compiled_index = 20U,
            },
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {9U}}},
    };
    (void)settings.system().persist_explore_filter(settings.system().explore_settings_candidate(), saved);
    const auto saved_revision = settings.system().snapshot().revision;

    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}});
    CHECK(explore.snapshot().filter.class_selection == ExploreClassSelection{.mode = ExploreClassSelectionMode::Subset, .classes = {1U}});
    CHECK(explore.snapshot().filter.minimum_compiled_index == 2U);
    CHECK(explore.snapshot().filter.maximum_compiled_index == 2U);
    CHECK(explore.snapshot().overlay.class_selection == ExploreClassSelection{.mode = ExploreClassSelectionMode::None});
    CHECK(settings.system().explore_settings_candidate().preferences.policy.filter == saved.filter);
    CHECK(settings.system().explore_settings_candidate().preferences.policy.overlay == saved.overlay);
    CHECK(settings.system().snapshot().revision == saved_revision);

    const auto update_and_wait = [&scenario, &explore](ExploreFilterUpdate update) {
        const auto admitted = explore.UpdateFilter(std::move(update));
        REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    };
    update_and_wait({
        .filter = {.class_selection = {.mode = ExploreClassSelectionMode::All}, .maximum_compiled_index = 2U},
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::None}},
    });
    CHECK(explore.snapshot().filter.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(explore.snapshot().filter.class_selection.classes.empty());
    CHECK(explore.snapshot().overlay.class_selection.mode == ExploreClassSelectionMode::None);
    CHECK(explore.snapshot().overlay.class_selection.classes.empty());

    update_and_wait({
        .filter = {.class_selection = {.mode = ExploreClassSelectionMode::None}, .maximum_compiled_index = 2U},
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {0U}}},
    });
    CHECK(explore.snapshot().filter.class_selection.mode == ExploreClassSelectionMode::None);
    CHECK(explore.snapshot().filter.class_selection.classes.empty());
    CHECK(explore.snapshot().overlay.class_selection == ExploreClassSelection{.mode = ExploreClassSelectionMode::Subset, .classes = {0U}});
}

TEST_CASE("Explore persistence failure retains the ready runtime product") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto failures = std::make_shared<std::atomic_uint64_t>(0U);
    ExploreScenario scenario{
        settings, backend,
        [commits] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits); },
        count_explore_failures(failures)};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto prior = explore.snapshot();
    const auto persisted = settings.system().snapshot();
    settings.BreakPersistence();

    static_cast<void>(explore.UpdateFilter({
        .filter = {.minimum_instances = 3U},
        .overlay = {.show_boxes = false, .show_masks = false},
    }));
    REQUIRE(scenario.Wait([&] { return failures->load(std::memory_order_acquire) == 1U && !explore.snapshot().busy; }));
    const auto failed = explore.snapshot();
    CHECK(failed.ready);
    CHECK_FALSE(failed.failure.empty());
    CHECK(failed.filter == prior.filter);
    CHECK(failed.order.matching_count == prior.order.matching_count);
    CHECK(failed.order.shuffle_seed == prior.order.shuffle_seed);
    CHECK(failed.order.visible_indices == prior.order.visible_indices);
    CHECK(failed.mode == prior.mode);
    CHECK(failed.selected_image == prior.selected_image);
    CHECK(failed.overlay.show_boxes == prior.overlay.show_boxes);
    CHECK(failed.frame == prior.frame);
    REQUIRE(explore.BorrowFrame().valid());
    CHECK(explore.BorrowFrame().plane(0U).revision() == failed.frame.revision);
    CHECK(settings.system().snapshot().revision == persisted.revision);
    CHECK(commits->load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Explore stale filter persistence discards the rendered candidate and restores the committed product") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto gate = std::make_shared<ExplorePostRenderGate>(1U);
    auto entered = gate->entered.get_future();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto failures = std::make_shared<std::atomic_uint64_t>(0U);
    ExploreScenario scenario{settings, backend, ExploreScenario::GateAfterRender(gate, commits), count_explore_failures(failures)};
    auto& explore = scenario.system();
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto committed = explore.snapshot();
    const auto admission = explore.UpdateFilter({.filter = {.minimum_instances = 1U}});
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    enable_dark_mode(settings);
    gate->release.set_value();
    const auto restored = wait_for_explore_failure(scenario, failures, admission.revision);
    check_restored_filter(restored, committed);
    CHECK(settings.system().snapshot().settings_state.ui.dark_mode);
    CHECK(settings.system().explore_settings_candidate().preferences.policy.filter == committed.filter);
    CHECK(commits->load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Explore successful persistence is settled before Stop can observe it") {
    auto persistence_entered = std::make_shared<std::promise<void>>();
    auto release_persistence = std::make_shared<std::promise<void>>();
    auto released = release_persistence->get_future().share();
    auto settings_events = std::make_shared<std::atomic_uint64_t>(0U);
    LoadedSettings settings{[persistence_entered, released, settings_events](SettingsSystem::event_type) {
        if (settings_events->fetch_add(1U, std::memory_order_acq_rel) == 2U) {
            persistence_entered->set_value();
            released.wait();
        }
    }};
    auto backend = std::make_shared<FakeImageBackend>();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto scenario = tracked_explore_scenario(settings, backend, commits);
    auto& explore = scenario.system();
    std::future<ExploreSnapshot> stopped;
    auto settle_persistence = settle_explore_on_exit(explore, *release_persistence);
    scenario.OpenAndWait({.extent = {64U, 64U}});
    static_cast<void>(explore.UpdateFilter({
        .filter = {.minimum_instances = 1U},
    }));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(*persistence_entered, "persistence_entered", 2s));
    stopped = std::async(std::launch::async, [&explore] { return explore.Stop(); });
    // Persistence has reached its dependency-owned publication boundary.
    // Stop must observe its committed outcome after release, independently of
    // when the concurrent call reaches the private admission mutex.
    release_persistence->set_value();
    REQUIRE(stopped.wait_for(2s) == std::future_status::ready);
    const auto stopping = stopped.get();
    CHECK_FALSE(stopping.busy);
    CHECK_FALSE(stopping.cancellation_requested);
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return !state.busy && !state.cancellation_requested && state.filter.minimum_instances == 1U;
    }));
    CHECK(commits->load(std::memory_order_acquire) == 2U);
}

TEST_CASE("Explore Open cancellation wins at its post-render finalization boundary") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto gate = std::make_shared<ExploreFinalizationGate>();
    auto entered = gate->entered.get_future();
    ExploreScenario scenario{settings, backend, [commits, gate] {
                                 return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr,
                                                                               commits, gate);
                             }};
    auto& explore = scenario.system();
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    gate->Arm();
    static_cast<void>(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    const auto stopping = explore.Stop();
    CHECK(stopping.busy);
    CHECK(stopping.cancellation_requested);
    release_and_wait_for_idle(scenario, gate->release);
    CHECK_FALSE(explore.snapshot().ready);
    CHECK_FALSE(explore.BorrowFrame().valid());
    CHECK(commits->load(std::memory_order_acquire) == 0U);
}

TEST_CASE("Explore Open and Stop linearize around catalog persistence") {
    auto persistence_entered = std::make_shared<std::promise<void>>();
    auto release_persistence = std::make_shared<std::promise<void>>();
    auto released = release_persistence->get_future().share();
    auto block_persistence = std::make_shared<std::atomic_bool>(false);
    LoadedSettings settings{[persistence_entered, released, block_persistence](SettingsSystem::event_type) {
        if (block_persistence->exchange(false, std::memory_order_acq_rel)) {
            persistence_entered->set_value();
            released.wait();
        }
    }};
    auto backend = std::make_shared<FakeImageBackend>();
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto changed_events = std::make_shared<std::atomic_uint64_t>(0U);
    auto changed_revision = std::make_shared<std::atomic_uint64_t>(0U);
    auto scenario =
        tracked_explore_scenario(settings, backend, commits, [changed_events, changed_revision](ExploreSystem::event_type event) {
            if (const auto* changed = std::get_if<ExploreChanged>(&event)) {
                changed_revision->store(changed->snapshot.revision, std::memory_order_release);
                changed_events->fetch_add(1U, std::memory_order_acq_rel);
            }
        });
    auto& explore = scenario.system();
    std::future<ExploreSnapshot> stopped;
    auto settle_persistence = settle_explore_on_exit(explore, *release_persistence);
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto first_identity = explore.snapshot().dataset.class_catalog_identity;
    static_cast<void>(select_explore_subset(settings, scenario, 1U));
    const auto events_before_open = changed_events->load(std::memory_order_acquire);

    block_persistence->store(true, std::memory_order_release);
    // CLEANUP-IGNORE: This Open drives catalog replacement through a blocked persistence boundary; the prior filter
    // operation tests successful persistence before Stop.
    static_cast<void>(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/different-catalog"}));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(*persistence_entered, "persistence_entered", 2s));
    stopped = std::async(std::launch::async, [&explore] { return explore.Stop(); });
    // Persistence has reached its dependency-owned publication boundary.
    // Stop must observe its committed outcome after release, independently of
    // when the concurrent call reaches the private admission mutex.

    release_persistence->set_value();
    REQUIRE(stopped.wait_for(2s) == std::future_status::ready);
    const auto stop_result = stopped.get();
    REQUIRE(scenario.Wait(
        [&] { return changed_events->load(std::memory_order_acquire) == events_before_open + 1U && !explore.snapshot().busy; }));
    const auto settled = explore.snapshot();
    CHECK(settled.ready);
    CHECK_FALSE(settled.cancellation_requested);
    CHECK(settled.dataset.class_catalog_identity != first_identity);
    CHECK(settled.filter.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(settled.overlay.class_selection.mode == ExploreClassSelectionMode::All);
    CHECK(stop_result.revision == settled.revision);
    CHECK(stop_result.busy == settled.busy);
    CHECK(stop_result.cancellation_requested == settled.cancellation_requested);
    CHECK(stop_result.dataset.class_catalog_identity == settled.dataset.class_catalog_identity);
    CHECK(changed_events->load(std::memory_order_acquire) == events_before_open + 1U);
    CHECK(changed_revision->load(std::memory_order_acquire) == settled.revision);
    CHECK(settings.system().explore_settings_candidate().preferences.class_catalog_identity == settled.dataset.class_catalog_identity);
    CHECK(commits->load(std::memory_order_acquire) == 3U);
}

TEST_CASE("Explore cancellation during saved-filter preparation discards the unpublished native Open") {
    const auto run = [](const std::size_t blocked_call) {
        LoadedSettings settings;
        auto backend = std::make_shared<FakeImageBackend>();
        auto probe = std::make_shared<OpenPreparationProbe>(blocked_call);
        auto entered = probe->entered.get_future();
        auto cancelled = probe->cancelled.get_future();
        ExploreScenario scenario{settings, backend, [probe] { return std::make_unique<CancellableOpenPreparationAlgorithm>(probe); }};
        auto& explore = scenario.system();
        if (blocked_call != 0U) { scenario.OpenAndWait({.extent = {64U, 64U}}, "/first"); }

        const auto admission = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/cancelled"});
        REQUIRE(entered.wait_for(2s) == std::future_status::ready);
        entered.get();
        const auto stopping = explore.Stop();
        CHECK_FALSE((stopping.busy && !stopping.cancellation_requested));
        REQUIRE(cancelled.wait_for(2s) == std::future_status::ready);
        cancelled.get();
        REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admission.revision; }));
        const auto settled = explore.snapshot();
        CHECK(settled.ready == (blocked_call != 0U));
        CHECK(settled.dataset.image_count == blocked_call);
        CHECK(settled.dataset.class_names.empty() == (blocked_call == 0U));
        CHECK(settled.order.visible_indices.empty() == (blocked_call == 0U));
        CHECK(settled.frame.valid() == (blocked_call != 0U));
        CHECK_FALSE(settled.cancellation_requested);
        CHECK(probe->resets.load(std::memory_order_acquire) == (blocked_call == 0U ? 1U : 0U));
        CHECK(probe->commits.load(std::memory_order_acquire) == blocked_call);

        const auto reopened = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/fresh"});
        REQUIRE(scenario.Wait([&] { return explore.snapshot().ready && explore.snapshot().revision > reopened.revision; }));
        CHECK(explore.snapshot().dataset.image_count == 1U);
        CHECK(explore.snapshot().frame.valid());
        CHECK(probe->commits.load(std::memory_order_acquire) == blocked_call + 1U);
    };

    SECTION("first Open") { run(0U); }
    SECTION("reopen") { run(1U); }
}

TEST_CASE("Annotation peer closure orders accepted input before replacement gestures") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore source{backend, {32U, 32U}};
    EventGate events;
    AnnotationSystem annotation{kDevice,
        RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [] { return std::make_unique<TestAnnotationAlgorithm>(); }),
        borrow_exactly_from(source.system()), [&](AnnotationSystem::event_type) { events.Advance(); }};
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    auto edited = annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}});
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > edited.revision; }));
    std::atomic_uint64_t consumed{0U}, ready{0U};
    annotation.SetInputPeer(1U, [&](AnnotationInputProgress progress) { consumed = progress.consumed_sequence; events.Advance(); });
    const auto epoch = annotation.snapshot().input_document_epoch;
    const auto before = annotation.snapshot().ui.scene.objects.size();
    annotation.Input({.epoch = 1U, .document_epoch = epoch, .sequence = 1U,
        .samples = {{.phase = contracts::AnnotationPointerPhase::Begin, .interaction_id = 1U, .sequence = 1U, .point = {2,3}},
                    {.phase = contracts::AnnotationPointerPhase::Update, .interaction_id = 1U, .sequence = 2U, .point = {4,5}}}});
    annotation.PeerClosed();
    annotation.SetInputPeer(2U, [&](AnnotationInputProgress progress) { ready = progress.epoch; consumed = progress.consumed_sequence; events.Advance(); });
    REQUIRE(events.Wait([&] { return ready.load() == 2U; }));
    CHECK(annotation.snapshot().ui.scene.objects.size() == before);
    annotation.Input({.epoch = 2U, .document_epoch = epoch, .sequence = 1U,
        .samples = {{.phase = contracts::AnnotationPointerPhase::Begin, .interaction_id = 2U, .sequence = 1U, .point = {6,7}},
                    {.phase = contracts::AnnotationPointerPhase::End, .interaction_id = 2U, .sequence = 2U, .point = {8,9}}}});
    REQUIRE(events.Wait([&] { return consumed.load() == 1U && annotation.snapshot().ui.scene.objects.size() == before + 1U; }));
    CHECK(annotation.snapshot().ready);
}

// CLEANUP-IGNORE: This settings-capture scenario and the overlay-retention scenario require independent fixture owners.
TEST_CASE("Explore newest desired work renders the captured current Settings augmentation configuration") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ExploreWorkProbe>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, ExploreScenario::TrackWork(probe)};
    static_cast<void>(scenario.system().UpdateAugmentation({.enabled = true}));
    scenario.OpenAndWait({.extent = {64U, 32U}, .row_count = 1U, .columns = 2U});
    const float original = probe->rendered_copy_paste_probability.load(std::memory_order_acquire);
    const float requested = original == 0.75F ? 0.25F : 0.75F;
    contracts::SettingsUpdateRequest update;
    update.updates.push_back({.path = "workflows.train.request.gpu_augmentation.copy_paste_probability",
                              .value = mmltk::frameworks::serialization::wire::FlatValue{static_cast<double>(requested)}});
    static_cast<void>(settings.system().Update(std::move(update)));
    const auto previous = scenario.system().snapshot().frame;
    SECTION("checked reroll") { static_cast<void>(scenario.system().RerollAugmentation()); }
    SECTION("persisted overlay") {
        auto overlay = scenario.system().snapshot().overlay;
        overlay.show_masks = !overlay.show_masks;
        static_cast<void>(scenario.system().UpdateOverlay(overlay));
    }
    REQUIRE(scenario.Wait([&] {
        return scenario.system().snapshot().frame != previous &&
               probe->rendered_copy_paste_probability.load(std::memory_order_acquire) == requested;
    }));
    CHECK(probe->rendered_copy_paste_probability.load(std::memory_order_acquire) == requested);
    CHECK(settings.system().explore_settings_candidate().augmentation.copy_paste_probability == requested);
}

struct ExploreViewportApplication final {
    ExploreSystem* explore;
};

TEST_CASE("Explore viewport admission rejects malformed grids and publishes native capacity results") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {64U, 32U}, .columns = 2U});
    const auto prior = explore.snapshot();
    for (const auto viewport :
         {ExploreViewport{.extent = {64U, 64U}, .columns = 2U}, ExploreViewport{.extent = {65U, 32U}, .columns = 2U},
          ExploreViewport{.extent = {64U, 65U}, .row_count = 2U, .columns = 2U}, ExploreViewport{.extent = {64U, 64U}, .row_count = 0U},
          ExploreViewport{.extent = {64U, 64U}, .columns = 0U},
          ExploreViewport{.extent = {1U, 1U}, .row_count = UINT32_MAX, .columns = UINT32_MAX}}) {
        CHECK_FALSE(viewport.valid());
        CHECK_THROWS_AS(explore.UpdateViewport({.viewport = viewport}), contracts::InvalidIntentError);
    }
    const auto rows = static_cast<std::uint32_t>(kExploreVisibleItemCapacity + 1U);
    const ExploreViewportUpdate request{.viewport = {.extent = {1U, rows}, .row_count = rows}};
    REQUIRE(request.valid());
    REQUIRE_FALSE(request.viewport.valid());
    explore.UpdateViewport(request);
    auto rejected = explore.snapshot();
    REQUIRE(rejected.viewport_result);
    CHECK(rejected.viewport_result->outcome == ExploreViewportOutcome::VisibleCapacityExceeded);
    CHECK(rejected.viewport_result->request.viewport == request.viewport);
    CHECK(rejected.frame == prior.frame);
    CHECK(rejected.gallery.generation == prior.gallery.generation);
    CHECK(rejected.failure.empty());

    ExploreViewportApplication application{&explore};
    browser::wire::ByteBuffer bytes(mmltk::frameworks::serialization::compact_maximum_cbor_bytes<ExploreViewportUpdate>());
    mmltk::frameworks::serialization::FixedCborEncoder writer(bytes);
    REQUIRE(mmltk::frameworks::serialization::encode_compact(writer, request));
    bytes.resize(writer.size());
    const auto dispatched = browser::dispatch_interaction(
        application,
        browser::Interaction{.endpoint_id = browser::application_stable_id("explore", "UpdateViewport"), .value = std::move(bytes)});
    CHECK(dispatched.disposition == browser::InteractionDispatchDisposition::Accepted);
    rejected = explore.snapshot();
    REQUIRE(rejected.viewport_result);
    CHECK(rejected.viewport_result->outcome == ExploreViewportOutcome::VisibleCapacityExceeded);
    const auto encoded = mmltk::frameworks::serialization::reflected_value(rejected);
    REQUIRE(encoded);
    ExploreSnapshot decoded;
    REQUIRE(mmltk::frameworks::serialization::decode_into(decoded, *encoded));
    REQUIRE(decoded.viewport_result);
    CHECK(decoded.viewport_result->outcome == ExploreViewportOutcome::VisibleCapacityExceeded);
    CHECK(decoded.viewport_result->request.viewport == request.viewport);

    const auto side = std::max(kDevice.maximum_width, kDevice.maximum_height) + 1U;
    explore.UpdateViewport({.viewport = {.extent = {side, side}}});
    REQUIRE(explore.snapshot().viewport_result);
    CHECK(explore.snapshot().viewport_result->outcome == ExploreViewportOutcome::AtlasExtentExceeded);
    CHECK(explore.snapshot().frame == prior.frame);

    explore.UpdateViewport({.viewport = prior.viewport});
    REQUIRE(explore.snapshot().viewport_result);
    CHECK(explore.snapshot().viewport_result->outcome == ExploreViewportOutcome::Ready);
    CHECK(explore.snapshot().viewport.valid());
}

TEST_CASE("Explore end-of-order clamping preserves square native atlas cells") {
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend};
    scenario.OpenAndWait({.extent = {64U, 96U}, .row_count = 3U, .columns = 2U});
    const auto initial = scenario.system().snapshot();
    CHECK(initial.viewport.row_count == 2U);
    CHECK(initial.viewport.extent == VisualExtent{64U, 64U});
    scenario.system().UpdateViewport({.viewport = {.extent = {64U, 96U}, .first_row = 1U, .row_count = 3U, .columns = 2U}});
    REQUIRE(scenario.Wait([&] { return scenario.system().snapshot().viewport.first_row == 1U; }));
    const auto end = scenario.system().snapshot();
    CHECK(end.viewport.row_count == 1U);
    CHECK(end.viewport.extent == VisualExtent{64U, 32U});
    CHECK(end.frame.extent == initial.frame.extent);
    CHECK(end.gallery.layout.row_count == end.viewport.row_count);
}

TEST_CASE("Explore unavailable runtime and selected transport publish distinct typed failures") {
    const bool transport = GENERATE(false, true);
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend, [transport]() -> std::unique_ptr<mmltk::frameworks::gpu::SystemImageModel> {
                                 if (transport) throw mmltk::frameworks::gpu::GdrTransportUnavailable("selected GDR transport unavailable");
                                 throw std::runtime_error("runtime initialization failed");
                             }};
    static_cast<void>(scenario.system().Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}));
    REQUIRE(scenario.Wait([&] { return !scenario.system().snapshot().failure.empty(); }));
    const auto failed = scenario.system().snapshot();
    CHECK_FALSE(failed.busy);
    CHECK_FALSE(failed.ready);
    CHECK(failed.failure_kind ==
          (transport ? ExploreFailureKind::SelectedTransportUnavailable : ExploreFailureKind::RuntimeInitialization));
    CHECK(failed.maximum_atlas_extent.valid());
}

class TransportFailingExploreAlgorithm final : public TestExploreAlgorithm {
   public:
    explicit TransportFailingExploreAlgorithm(bool safe)
        : TestExploreAlgorithm(std::make_shared<std::atomic<std::size_t>>(0U)),
          safe_(safe),
          retirement_(std::make_exception_ptr(std::runtime_error("distinct Explore retirement failure"))) {}
    ExploreOpened Open(std::string_view, std::stop_token) override {
        throw mmltk::frameworks::gpu::GdrTransportUnavailable("selected GDR transport unavailable");
    }
    [[nodiscard]] Release ReleaseResources() noexcept override { return {.all_released = safe_, .failure = retirement_}; }

   private:
    bool safe_;
    std::exception_ptr retirement_;
};

TEST_CASE("Explore preserves selected transport classification through safe and unsafe retirement failures") {
    const bool safe = GENERATE(false, true);
    LoadedSettings settings;
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreScenario scenario{settings, backend, [safe] { return std::make_unique<TransportFailingExploreAlgorithm>(safe); }};
    static_cast<void>(scenario.system().Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}));
    REQUIRE(scenario.Wait([&] { return !scenario.system().snapshot().failure.empty(); }));
    CHECK(scenario.system().snapshot().failure_kind == ExploreFailureKind::SelectedTransportUnavailable);
    CHECK(scenario.system().snapshot().failure == "selected GDR transport unavailable");
    CHECK_FALSE(scenario.system().snapshot().busy);
    CHECK_FALSE(scenario.system().snapshot().ready);
}

TEST_CASE("Explore visibility preserves active augmentation and labels preserve the product") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ExploreWorkProbe>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, ExploreScenario::TrackWork(probe)};
    static_cast<void>(scenario.system().UpdateAugmentation({.enabled = true}));
    scenario.OpenAndWait({.extent = {64U, 32U}, .row_count = 1U, .columns = 2U});
    const auto before = scenario.system().snapshot();
    auto overlay = before.overlay;
    overlay.show_labels = !overlay.show_labels;
    const auto labels = scenario.system().UpdateOverlay(overlay);
    CHECK(labels.frame == before.frame);
    CHECK(labels.gallery.generation == before.gallery.generation);
    CHECK(labels.revision > before.revision);
    CHECK(settings.system().explore_settings_candidate().preferences.policy.overlay.show_labels == overlay.show_labels);
    const auto original = probe->rendered_copy_paste_probability.load(std::memory_order_acquire);
    overlay.show_masks = !overlay.show_masks;
    static_cast<void>(scenario.system().UpdateOverlay(overlay));
    REQUIRE(scenario.Wait([&] { return scenario.system().snapshot().frame != before.frame; }));
    const auto semantic = scenario.system().snapshot();
    CHECK(semantic.frame.revision > before.frame.revision);
    CHECK(semantic.frame.clean_revision == before.frame.clean_revision);
    CHECK(probe->rendered_copy_paste_probability.load(std::memory_order_acquire) == original);
    scenario.OpenAndWait(semantic.viewport);
    const auto reopened = scenario.system().snapshot();
    CHECK(reopened.dataset.identity == semantic.dataset.identity);
    CHECK(reopened.frame.clean_revision > semantic.frame.clean_revision);
}

TEST_CASE("Explore discrete products use the viewport current at execution") {
    auto run = [](const bool select) {
        auto backend = std::make_shared<FakeImageBackend>();
        auto observed_nproc = std::make_shared<std::atomic<std::size_t>>(0U);
        auto gate = std::make_shared<ExploreRenderGate>();
        auto entered = gate->entered.get_future();
        LoadedSettings settings;
        ExploreScenario scenario{settings, backend,
                                 [observed_nproc, gate] { return std::make_unique<TestExploreAlgorithm>(observed_nproc, gate); }};
        auto& explore = scenario.system();
        auto settle_explore = settle_explore_on_exit(explore, gate->release);
        scenario.OpenAndWait({.extent = {63U, 21U}, .row_count = 1U, .columns = 3U});

        explore.UpdateViewport({.viewport = {.extent = {48U, 16U}, .row_count = 1U, .columns = 3U}});
        REQUIRE(entered.wait_for(2s) == std::future_status::ready);
        entered.get();
        explore.UpdateViewport({.viewport = {.extent = {30U, 10U}, .row_count = 1U, .columns = 3U}});
        if (select)
            static_cast<void>(explore.Select({.compiled_index = 1U}));
        else
            static_cast<void>(
                explore.UpdateFilter({.filter = {.minimum_instances = 1U}, .overlay = {.show_boxes = false, .show_masks = true}}));
        gate->release.set_value();

        REQUIRE(scenario.Wait([&] {
            const auto state = explore.snapshot();
            return !state.busy && state.viewport.extent.width == 30U &&
                   (select ? state.mode == ExploreMode::Detail : state.filter.minimum_instances == 1U);
        }));
        const auto state = explore.snapshot();
        CHECK(state.frame.extent == state.viewport.extent);
        CHECK(state.order.visible_indices == std::vector<std::uint32_t>{0U, 1U, 2U});
        CHECK(state.selected_image == (select ? std::optional<std::uint32_t>{1U} : std::nullopt));
        const auto borrowed = explore.BorrowFrame();
        REQUIRE(borrowed.valid());
        CHECK(borrowed.plane(0U).revision() == state.frame.revision);
    };

    SECTION("filter") { run(false); }
    SECTION("selection") { run(true); }
}

TEST_CASE("queued Explore cancellation is finalized by the scheduler callback") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto observed_nproc = std::make_shared<std::atomic<std::size_t>>(0U);
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    auto gate = std::make_shared<ExploreRenderGate>();
    auto entered = gate->entered.get_future();
    LoadedSettings settings;
    (void)settings.system().persist_explore_filter(settings.system().explore_settings_candidate(),
                                                   {
                                                       .filter = {.order = ExploreOrder::Shuffled, .shuffle_seed = 17U},
                                                   });
    ExploreScenario scenario{settings, backend, [observed_nproc, commits, gate] {
                                 return std::make_unique<TestExploreAlgorithm>(observed_nproc, gate, commits);
                             }};
    auto& explore = scenario.system();
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    scenario.OpenAndWait({.extent = {64U, 64U}});
    const auto committed = explore.snapshot();
    explore.UpdateViewport({.viewport = {.extent = {48U, 48U}}});
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    const auto admitted = explore.Reroll();
    CHECK(admitted.busy);

    const auto stopped = explore.Stop();
    CHECK_FALSE(stopped.busy);
    CHECK_FALSE(stopped.cancellation_requested);
    CHECK(stopped.revision > admitted.revision);
    CHECK(commits->load(std::memory_order_acquire) == 1U);

    gate->release.set_value();
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy; }));
    CHECK(explore.snapshot().viewport.extent == committed.viewport.extent);
    CHECK_FALSE(explore.snapshot().busy);
    CHECK(commits->load(std::memory_order_acquire) == 1U);
}

TEST_CASE("post-render Explore cancellation restores the committed gallery product") {
    const auto run = [](const bool selection) {
        auto backend = std::make_shared<FakeImageBackend>();
        auto gate = std::make_shared<ExplorePostRenderGate>(1U);
        auto entered = gate->entered.get_future();
        auto commits = std::make_shared<std::atomic_uint64_t>(0U);
        LoadedSettings settings;
        (void)settings.system().persist_explore_filter(settings.system().explore_settings_candidate(),
                                                       {
                                                           .filter = {.order = ExploreOrder::Shuffled, .shuffle_seed = 19U},
                                                       });
        ExploreScenario scenario{settings, backend, ExploreScenario::GateAfterRender(gate, commits)};
        auto& explore = scenario.system();
        auto settle_explore = settle_explore_on_exit(explore, gate->release);
        scenario.OpenAndWait({.extent = {63U, 21U}, .row_count = 1U, .columns = 3U});
        const auto before = explore.snapshot();
        if (selection)
            static_cast<void>(explore.Select({.compiled_index = 1U}));
        else
            static_cast<void>(explore.Reroll());
        REQUIRE(entered.wait_for(2s) == std::future_status::ready);
        entered.get();
        const auto stopping = explore.Stop();
        CHECK(stopping.busy == !selection);
        CHECK(stopping.cancellation_requested == !selection);
        gate->release.set_value();
        REQUIRE(scenario.Wait([&] {
            const auto snapshot = explore.snapshot();
            return !snapshot.busy && snapshot.revision > stopping.revision;
        }));
        const auto restored = explore.snapshot();
        CHECK(restored.filter == before.filter);
        CHECK(restored.order.visible_indices == before.order.visible_indices);
        CHECK(restored.mode == before.mode);
        CHECK(restored.selected_image == before.selected_image);
        CHECK(restored.frame == before.frame);
        REQUIRE(explore.BorrowFrame().valid());
        CHECK(explore.BorrowFrame().plane(0U).revision() == restored.frame.revision);
        CHECK(commits->load(std::memory_order_acquire) == 1U);
    };

    SECTION("discrete order") { run(false); }
    SECTION("selection") { run(true); }
}

TEST_CASE("Explore focus never changes published target atlas slot identity") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto work = std::make_shared<ExploreWorkProbe>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, ExploreScenario::TrackWork(work)};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 48U}, .row_count = 1U, .columns = 2U});

    const auto before_focus = explore.snapshot().revision;
    explore.UpdateViewport({.viewport = {.extent = {96U, 48U}, .row_count = 1U, .columns = 2U}, .focused_compiled_index = 1U});
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.revision > before_focus && state.focused_image == 1U &&
               state.order.visible_indices == std::vector<std::uint32_t>{0U, 1U};
    }));
    CHECK(work->rendered_count.load(std::memory_order_acquire) == 2U);
    CHECK(work->rendered_slots[0].load(std::memory_order_acquire) == 0U);
    CHECK(work->rendered_slots[1].load(std::memory_order_acquire) == 1U);

    const auto before_scroll = explore.snapshot().revision;
    explore.UpdateViewport(
        {.viewport = {.extent = {96U, 48U}, .first_row = 1U, .row_count = 1U, .columns = 2U}, .focused_compiled_index = 1U});
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.revision > before_scroll && !state.focused_image && state.order.visible_indices == std::vector<std::uint32_t>{2U};
    }));
    CHECK(work->rendered_count.load(std::memory_order_acquire) == 1U);
    CHECK(work->rendered_slots[0].load(std::memory_order_acquire) == 2U);
}

TEST_CASE("Explore selection and detail navigation publish complete bounded snapshots") {
    auto backend = std::make_shared<FakeImageBackend>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 64U}, .row_count = 2U, .columns = 3U});
    explore.UpdateViewport({.viewport = {.extent = {96U, 64U}, .row_count = 2U, .columns = 3U}, .focused_compiled_index = 2U});
    REQUIRE(scenario.Wait([&] { return explore.snapshot().focused_image == 2U; }));
    const auto before_stale_focus = explore.snapshot().revision;
    explore.UpdateViewport({.viewport = {.extent = {96U, 64U}, .row_count = 2U, .columns = 3U}, .focused_compiled_index = 3U});
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.ready && !state.busy && state.revision > before_stale_focus && !state.focused_image;
    }));
    CHECK_THROWS_AS(explore.Select({.compiled_index = 3U}), contracts::InvalidIntentError);
    static_cast<void>(explore.Select({.compiled_index = 1U}));
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return !state.busy && state.mode == ExploreMode::Detail && state.selected_image == 1U;
    }));
    static_cast<void>(explore.Navigate({.direction = ExploreNavigation::Next}));
    REQUIRE(scenario.Wait([&] { return explore.snapshot().selected_image == 2U; }));
    static_cast<void>(explore.CloseDetail());
    REQUIRE(scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.mode == ExploreMode::Gallery && !state.selected_image;
    }));
    CHECK(explore.snapshot().order.visible_indices.size() <= kExploreVisibleItemCapacity);
}

TEST_CASE("Visual workspace retirement resumes queued work only after its delayed physical outcome", "[workspace]") {
    using mmltk::frameworks::gpu::SystemImageRuntime;
    using mmltk::frameworks::gpu::test_support::ImageWorkspaceTestAccess;
    const bool fail_release = GENERATE(false, true);
    ImageWorkspaceTestAccess::Reset();
    auto backend = std::make_shared<FakeImageBackend>();
    std::atomic<std::size_t> constructions{0U};
    std::promise<std::shared_ptr<mmltk::frameworks::gpu::ImageWorkspace>> created_workspace;
    auto workspace_result = created_workspace.get_future();
    std::promise<void> staged_completed;
    auto staged_result = staged_completed.get_future();
    std::promise<void> queued_completed;
    auto queued_result = queued_completed.get_future();
    std::promise<std::exception_ptr> failed;
    auto failure_result = failed.get_future();
    std::atomic<std::size_t> failures{0U};
    mmltk::testsupport::TestGate staged_gate("workspace staged retirement");
    detail::VisualRuntimeOwner owner{[&](auto revisions) {
                                         ++constructions;
                                         auto runtime =
                                             std::make_unique<SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                                                 .device = 0, .backend = backend, .product_revisions = std::move(revisions)});
                                         ImageWorkspaceTestAccess::Install(*runtime);
                                         return runtime;
                                     },
                                     [&](std::exception_ptr failure) {
                                         if (failures.fetch_add(1U) == 0U) failed.set_value(failure);
                                     }};
    REQUIRE(owner.SubmitOrdered([&](auto& runtime, std::stop_token) {
        auto workspace = runtime.CreateWorkspace(ImageWorkspaceTestAccess::Layout());
        runtime.Publish(4U, 3U, [](auto, auto, auto) {});
        return detail::VisualRuntimeOwner::Notification{
            [&, workspace = std::move(workspace)]() mutable { created_workspace.set_value(std::move(workspace)); }};
    }));
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
    REQUIRE(owner.SubmitOrdered(
        [&](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&] { queued_completed.set_value(); }}; }));
    staged_gate.Release();
    mmltk::testsupport::await_test_future(staged_result, "staged workspace retirement handoff");
    CHECK(failures.load() == 0U);
    CHECK(queued_result.wait_for(0s) == std::future_status::timeout);
    CHECK(constructions.load() == 2U);
    const auto cleanup = std::make_exception_ptr(std::runtime_error("delayed visual display release failure"));
    if (fail_release) backend->FailDeviceBinding(1, cleanup);
    workspace.reset();
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
    std::promise<std::shared_ptr<mmltk::frameworks::gpu::ImageWorkspace>> created;
    auto ready = created.get_future();
    auto owner = std::make_unique<detail::VisualRuntimeOwner>(
        [&](auto revisions) {
            auto runtime = std::make_unique<SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                .device = 0, .backend = backend, .product_revisions = std::move(revisions)});
            ImageWorkspaceTestAccess::Install(*runtime);
            return runtime;
        },
        [](std::exception_ptr) { FAIL("healthy delayed workspace unexpectedly failed"); });
    REQUIRE(owner->SubmitOrdered([&](auto& runtime, std::stop_token) {
        auto workspace = runtime.CreateWorkspace(ImageWorkspaceTestAccess::Layout());
        return detail::VisualRuntimeOwner::Notification{
            [&, workspace = std::move(workspace)]() mutable { created.set_value(std::move(workspace)); }};
    }));
    auto workspace = mmltk::testsupport::await_test_future(ready, "workspace before visual owner destruction");
    owner.reset();
    CHECK(backend->contexts_destroyed == 1U);
    workspace.reset();
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(mmltk::frameworks::gpu::test_support::ExportedImageBufferTestAccess::unmaps == 1U);
}

TEST_CASE("a failed visual aggregate publishes once and reconstructs lazily") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto constructions = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto failures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    LoadedSettings settings;
    ExploreScenario scenario{
        settings, backend,
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

TEST_CASE("stopping Explore retains busy until its admitted operation settles") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<CancellationProbe>();
    auto entered = probe->entered.get_future();
    auto cancellation = probe->cancelled.get_future();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, [probe] { return std::make_unique<CancellableExploreAlgorithm>(probe); }};
    auto& explore = scenario.system();
    const auto first_admission = explore.Open({.viewport = {.extent = {32U, 32U}}, .compiled_source = "/test"});
    REQUIRE(scenario.Wait([&] { return explore.snapshot().ready; }));
    const auto completed = explore.snapshot().frame.revision;
    CHECK(explore.snapshot().revision > first_admission.revision);
    const auto second_admission = explore.Open({.viewport = {.extent = {32U, 32U}}, .compiled_source = "/test"});
    CHECK(second_admission.busy);
    CHECK_THROWS_AS(explore.Open({.viewport = {}, .compiled_source = {}}), contracts::BusyError);
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    const auto stopping = explore.Stop();
    CHECK_FALSE((stopping.busy && !stopping.cancellation_requested));
    const auto repeated = explore.Stop();
    CHECK(repeated.revision >= stopping.revision);
    REQUIRE(cancellation.wait_for(2s) == std::future_status::ready);
    cancellation.get();
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy; }));
    CHECK(explore.snapshot().revision > stopping.revision);
    CHECK(explore.snapshot().frame.revision == completed);
}

TEST_CASE("cancelled Explore filter retains one native and public committed order") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<CancellationProbe>();
    auto entered = probe->entered.get_future();
    auto cancelled = probe->cancelled.get_future();
    // CLEANUP-IGNORE: Filter cancellation observes native commit ownership, unlike the discrete viewport execution
    // scenario.
    auto commits = std::make_shared<std::atomic_uint64_t>(0U);
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend,
                             [probe, commits] { return std::make_unique<AtomicFilterExploreAlgorithm>(probe, commits); }};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {32U, 32U}});
    const auto before = explore.snapshot();
    const auto settings_revision = settings.system().snapshot().revision;
    static_cast<void>(explore.UpdateFilter({
        .filter = {.minimum_instances = 1U},
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    static_cast<void>(explore.Stop());
    REQUIRE(cancelled.wait_for(2s) == std::future_status::ready);
    cancelled.get();
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy; }));
    CHECK(commits->load(std::memory_order_acquire) == 1U);
    CHECK(explore.snapshot().filter == before.filter);
    CHECK(explore.snapshot().order.visible_indices == before.order.visible_indices);
    CHECK(settings.system().snapshot().revision == settings_revision);
    REQUIRE(explore.BorrowFrame().valid());
    CHECK(explore.BorrowFrame().plane(0U).revision() == explore.snapshot().frame.revision);
}

// CLEANUP-IGNORE: Finalization-gate cancellation and viewport execution configure distinct algorithm seams.
TEST_CASE("Explore stop at filter finalization cancels before persistence") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto observed_nproc = std::make_shared<std::atomic<std::size_t>>(0U);
    auto gate = std::make_shared<ExploreFinalizationGate>();
    auto entered = gate->entered.get_future();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, [observed_nproc, gate] {
                                 // CLEANUP-IGNORE: Finalization-gate rendering and atomic-filter cancellation configure
                                 // distinct Explore algorithm seams.
                                 return std::make_unique<TestExploreAlgorithm>(observed_nproc, nullptr, nullptr, gate);
                             }};
    auto& explore = scenario.system();
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    scenario.OpenAndWait({.extent = {32U, 32U}});
    const auto before = explore.snapshot();
    const auto settings_revision = settings.system().snapshot().revision;
    gate->Arm();
    static_cast<void>(explore.UpdateFilter({
        .filter = {.minimum_instances = 1U},
        .overlay = {.show_boxes = false, .show_masks = true},
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    const auto stopping = explore.Stop();
    CHECK(stopping.cancellation_requested);
    release_and_wait_for_idle(scenario, gate->release);
    const auto settled = explore.snapshot();
    CHECK(settled.filter == before.filter);
    CHECK(settled.overlay.show_boxes == before.overlay.show_boxes);
    CHECK(settled.overlay.show_masks == before.overlay.show_masks);
    CHECK(settled.frame == before.frame);
    CHECK(settings.system().snapshot().revision == settings_revision);
    REQUIRE(explore.BorrowFrame().valid());
    CHECK(explore.BorrowFrame().plane(0U).revision() == settled.frame.revision);
}

// CLEANUP-IGNORE: Annotation receiver-copy rejection and Upscale high-water publication are distinct system tests.
TEST_CASE("Annotation renderer failure retires resources and allows source restart") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore source{backend, {32U, 32U}};
    auto probe = std::make_shared<AnnotationRenderProbe>();
    EventGate events;
    std::atomic_uint64_t failures{0U};
    AnnotationSystem annotation{kDevice,
        RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [probe] { return std::make_unique<TestAnnotationAlgorithm>(probe); }),
        borrow_exactly_from(source.system()), [&](AnnotationSystem::event_type event) {
            if (std::holds_alternative<AnnotationFailed>(event)) ++failures;
            events.Advance();
        }};
    probe->fail_open = true;
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    REQUIRE(events.Wait([&] { return failures.load() == 1U; }));
    CHECK_FALSE(annotation.snapshot().ready);
    CHECK_FALSE(annotation.BorrowFrame().valid());
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    const auto committed = annotation.snapshot().ui;
    probe->fail_render = true;
    static_cast<void>(annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}}));
    REQUIRE(events.Wait([&] { return failures.load() == 2U; }));
    CHECK_FALSE(annotation.snapshot().ready);
    CHECK_FALSE(annotation.BorrowFrame().valid());
    CHECK(annotation.snapshot().ui.document_revision >= committed.document_revision);
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
}

TEST_CASE("Annotation rejects an oversized incoming document without changing its existing editor or pixels") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {512U, 256U}};
    auto rejected_document = std::make_shared<VisualDocument>();
    rejected_document->scene.document = contracts::WorkspaceResource::From("test://oversized", 1U);
    rejected_document->scene.categories = {{.value = "object"}};
    SECTION("object capacity") { rejected_document->scene.objects.resize(contracts::kAnnotationObjectCapacity + 1U); }
    SECTION("materialized mask capacity") {
        rejected_document->scene.objects.push_back(
            {.shape = contracts::AnnotationShape::Mask, .box = {{0.0F, 0.0F}, {512.0F, 256.0F}}, .mask = {.present = true}});
        rejected_document->mask_contains = [](std::size_t, float x, float) { return static_cast<unsigned>(x * 512.0F) % 2U == 0U; };
    }
    bool reject_incoming = false;
    EventGate events;
    AnnotationSystem annotation{kDevice,
                                RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                                               [] { return std::make_unique<TestAnnotationAlgorithm>(); }),
                                [&](const VisualFrame& frame) {
                                    auto borrowed = source.BorrowExact(frame);
                                    if (reject_incoming) borrowed.document = rejected_document;
                                    return borrowed;
                                },
                                [&events](AnnotationSystem::event_type) { events.Advance(); }};
    static_cast<void>(annotation.Open({.source = source.frame()}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    static_cast<void>(annotation.Edit({.edit = {.value = AnnotationHoldEdit{true}}}));
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
    static_cast<void>(annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}}));
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
    REQUIRE(events.Wait([&] { return annotation.snapshot().rendered.scene_revision == annotation.snapshot().ui.scene_revision; }));
    const auto prior = annotation.snapshot();
    const auto copies = backend->same_copies.load(std::memory_order_acquire);
    const auto pixel = *reinterpret_cast<const std::uint8_t*>(annotation.BorrowFrame().plane(0U).plane().data);
    reject_incoming = true;
    static_cast<void>(annotation.Open({.source = source.frame()}));
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
    CHECK(annotation.snapshot().ready);
    CHECK(annotation.snapshot().ui == prior.ui);
    CHECK(annotation.snapshot().frame == prior.frame);
    CHECK(backend->same_copies.load(std::memory_order_acquire) == copies);
    REQUIRE(annotation.BorrowFrame().valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(annotation.BorrowFrame().plane(0U).plane().data) == pixel);
    static_cast<void>(annotation.Edit({.edit = {.value = AnnotationRedoEdit{}}}));
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
    CHECK(annotation.snapshot().ui.document_revision == prior.ui.document_revision + 1U);
}

TEST_CASE("Annotation reduces input and settles commands while rendering is held") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore source{backend, {32U, 32U}};
    auto probe = std::make_shared<AnnotationRenderProbe>();
    auto hold = std::make_shared<MutationCommitProbe>();
    auto entered = hold->committed.get_future();
    EventGate events;
    std::atomic_uint64_t held_scene{0U};
    std::atomic_bool older_completed{false};
    AnnotationSystem annotation{kDevice,
        RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [probe] { return std::make_unique<TestAnnotationAlgorithm>(probe); }),
        borrow_exactly_from(source.system()), [&](AnnotationSystem::event_type event) {
            if (const auto* full = std::get_if<AnnotationChanged>(&event)) {
                const auto& snapshot = full->snapshot;
                if (held_scene.load() != 0U && snapshot.rendered.scene_revision == held_scene.load() &&
                    snapshot.ui.scene_revision > snapshot.rendered.scene_revision && snapshot.frame.valid())
                    older_completed = true;
            }
            events.Advance();
        }};
    mmltk::testsupport::ScopedTestCleanup release{[&] {
        mmltk::testsupport::release_test_promise(hold->release);
        annotation.Shutdown();
    }};
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    const auto initial = annotation.snapshot();
    { std::scoped_lock lock(probe->mutex); probe->hold = hold; }
    static_cast<void>(annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}}));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    held_scene = annotation.snapshot().ui.scene_revision;
    std::atomic_uint64_t consumed{0U};
    annotation.SetInputPeer(1U, [&](AnnotationInputProgress progress) { consumed = progress.consumed_sequence; events.Advance(); });
    annotation.Input({.epoch = 1U, .document_epoch = initial.input_document_epoch, .sequence = 1U,
        .samples = {{.phase = contracts::AnnotationPointerPhase::Begin, .interaction_id = 1U, .sequence = 1U, .point = {2,3}},
                    {.phase = contracts::AnnotationPointerPhase::Update, .interaction_id = 1U, .sequence = 2U, .point = {4,5}},
                    {.phase = contracts::AnnotationPointerPhase::End, .interaction_id = 1U, .sequence = 3U, .point = {6,7}}}});
    REQUIRE(events.Wait([&] { return consumed.load() == 1U && annotation.snapshot().ui.document_revision > initial.ui.document_revision; }));
    CHECK(annotation.snapshot().frame == initial.frame);
    const auto committed = annotation.snapshot().ui.document_revision;
    const auto command = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > command.revision; }));
    CHECK(annotation.snapshot().ui.document_revision > committed);
    const auto document_epoch = annotation.snapshot().input_document_epoch;
    static_cast<void>(annotation.Open({.source = source.system().snapshot().frame}));
    CHECK(annotation.Stop().cancellation_requested);
    hold->release.set_value();
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy &&
        annotation.snapshot().rendered.scene_revision == annotation.snapshot().ui.scene_revision; }));
    CHECK(annotation.snapshot().input_document_epoch == document_epoch);
    CHECK(annotation.snapshot().frame.revision > initial.frame.revision);
    CHECK(older_completed.load());
}

TEST_CASE("Annotation color sampling completes before following document commands under output pressure") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {32U, 32U}};
    auto semantic = std::make_shared<VisualDocument>();
    semantic->scene.document = contracts::WorkspaceResource::From("test://sample-mask", 1U);
    semantic->scene.categories = {{.value = "object"}};
    semantic->scene.objects = {{.name = contracts::AnnotationText::From("mask"), .shape = contracts::AnnotationShape::Mask,
                                .box = {{1,1}, {16,16}}}};
    auto probe = std::make_shared<AnnotationRenderProbe>();
    auto sample = std::make_shared<MutationCommitProbe>();
    auto entered = sample->committed.get_future();
    EventGate events;
    std::atomic_uint64_t consumed{0U};
    AnnotationSystem annotation{kDevice,
        RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                       [probe] { return std::make_unique<TestAnnotationAlgorithm>(probe); }),
        [&](const VisualFrame& frame) { auto read = source.BorrowExact(frame); read.document = semantic; return read; },
        [&](AnnotationSystem::event_type) { events.Advance(); }};
    mmltk::testsupport::ScopedTestCleanup release{[&] {
        mmltk::testsupport::release_test_promise(sample->release);
        annotation.Shutdown();
    }};
    static_cast<void>(annotation.Open({.source = source.frame()}));
    REQUIRE(events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    auto edit = annotation.Edit({.edit = {.value = AnnotationObjectEdit{0U}}});
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > edit.revision; }));
    edit = annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::ColorSample}}});
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > edit.revision; }));
    REQUIRE(events.Wait([&] { return annotation.snapshot().rendered.scene_revision == annotation.snapshot().ui.scene_revision; }));
    auto held = annotation.BorrowFrame();
    REQUIRE(held.valid());
    { std::scoped_lock lock(probe->mutex); probe->sample_hold = sample; }
    annotation.SetInputPeer(1U, [&](AnnotationInputProgress progress) { consumed = progress.consumed_sequence; events.Advance(); });
    annotation.Input({.epoch = 1U, .document_epoch = annotation.snapshot().input_document_epoch, .sequence = 1U,
        .samples = {{.phase = contracts::AnnotationPointerPhase::Begin, .interaction_id = 1U, .sequence = 1U,
                    .target = {.object = 0U}, .point = {4,5}},
                   {.phase = contracts::AnnotationPointerPhase::End, .interaction_id = 1U, .sequence = 2U,
                    .target = {.object = 0U}, .point = {4,5}}}});
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    CHECK(consumed.load() == 1U);
    const auto undo = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
    CHECK(undo.busy);
    sample->release.set_value();
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > undo.revision; }));
    CHECK_FALSE(annotation.snapshot().ui.scene.objects[0].sup.sampling);
    edit = annotation.Edit({.edit = {.value = AnnotationRedoEdit{}}});
    REQUIRE(events.Wait([&] { return !annotation.snapshot().busy && annotation.snapshot().revision > edit.revision; }));
    CHECK(annotation.snapshot().ui.scene.objects[0].sup.sampling);
    CHECK(annotation.snapshot().ui.scene.objects[0].sup.center.hue == 120.0F);
    held = {};
}

TEST_CASE("Upscale owns and publishes its receiver image") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();

    EventGate upscale_events;
    // CLEANUP-IGNORE: Upscale and Annotation are independently sealed receiver systems with different model factories,
    // intents, snapshots, and output invariants.
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          borrow_exactly_from(explore), [&upscale_events](UpscaleSystem::event_type) { upscale_events.Advance(); }};
    CHECK_FALSE(upscale.BorrowFrame().valid());
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
    })));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(upscale.BorrowFrame().valid());
    CHECK(upscale.snapshot().frame.extent.width == 128U);
    CHECK(upscale.snapshot().frame.extent.height == 128U);
    CHECK(selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::Default);
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::ShiftLut,
    })));
    REQUIRE(upscale_events.Wait([&] { return selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::ShiftLut; }));
    static_cast<void>(upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::RealPlksr,
    })));
    REQUIRE(upscale_events.Wait([&] { return selected_kernel->load(std::memory_order_acquire) == UpscaleKernel::RealPlksr; }));
}

TEST_CASE("Upscale equal extent source changes replace actual receiver pixels") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<UpscaleExtentProbe>();
    UpscaleSourceFixture subject{backend,
                                 {16U, 8U},
                                 3U,
                                 RuntimeFactory(
                                     0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                                     [probe] { return std::make_unique<ExtentUpscaleAlgorithm>(probe); }, 4U)};
    auto& source = subject.source;
    auto& events = subject.events;
    auto& upscale = subject.upscale;
    for (const auto value : {3U, 19U, 3U}) {
        source.Publish({16U, 8U}, static_cast<std::uint8_t>(value));
        const auto input = source.frame();
        static_cast<void>(upscale.Start(test_upscale_request({.source = input})));
        REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == input; }));
        const auto output = upscale.BorrowDocument(upscale.snapshot().frame);
        REQUIRE(output.valid());
        const auto plane = output.pixels.plane(0U).plane();
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(plane.data);
        CHECK(pixels[0] == value);
        CHECK(pixels[(plane.descriptor.height - 1U) * plane.descriptor.pitch_bytes + plane.descriptor.row_bytes() - 1U] == value);
    }
}

TEST_CASE("Upscale exact repeats avoid copying and method switches preserve completed pixels") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    std::atomic_uint32_t copies{0U};
    EventGate events;
    const VisualDiagnosticSink diagnostics{.context = &copies, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
                                               if (fact.operation == VisualDiagnosticOperation::CopyCompleted)
                                                   static_cast<std::atomic_uint32_t*>(context)->fetch_add(1U);
                                           }};
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics};
    for (const auto selected : {UpscaleKernel::Default, UpscaleKernel::ShiftLut, UpscaleKernel::Default}) {
        const auto request = test_upscale_request({.source = source.frame(), .kernel = selected});
        static_cast<void>(upscale.Start(request));
        REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().kernel == selected; }));
        const auto completed = upscale.snapshot().frame;
        CHECK(upscale.ObserveWorkspace().product_revision == completed.revision);
        const auto copied = copies.load();
        const auto inferred = runs->load();
        {
            const auto output = upscale.BorrowDocument(completed);
            REQUIRE(output.valid());
            CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(0U).plane().data) == static_cast<std::uint8_t>(selected) + 1U);
        }
        static_cast<void>(upscale.Start(request));
        REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
        CHECK(upscale.snapshot().frame == completed);
        CHECK(copies.load() == copied);
        CHECK(runs->load() == inferred);
    }
    CHECK(runs->load() == 2U);
    const auto retained = upscale.snapshot().frame;
    upscale.Stop();
    CHECK_FALSE(upscale.snapshot().ready);
    CHECK(upscale.BorrowDocument(retained).valid());
    const auto previous = source.frame();
    source.Publish(previous.extent, 19U);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == source.frame(); }));
    CHECK(runs->load() == 3U);
    auto cropped = source.frame();
    cropped.content = {1U, 1U, 4U, 3U};
    static_cast<void>(upscale.Start(test_upscale_request({.source = cropped})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == cropped; }));
    CHECK(runs->load() == 4U);
}

TEST_CASE("Upscale semantic revisions reuse clean pixels and retain exact input provenance") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    const auto initial = source.frame();
    static_cast<void>(upscale.Start(test_upscale_request({.source = initial})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    const auto clean_revision = upscale.snapshot().frame.clean_revision;
    const auto baseline = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(baseline.valid());
    backend->watched_copy_source.store(baseline.pixels.plane(1U).plane().data);
    CHECK(upscale.snapshot().input == initial);
    source.SetSemantics(55U);
    const auto current = source.frame();
    REQUIRE(current.clean_revision == initial.clean_revision);
    REQUIRE(current.revision != initial.revision);
    const auto copies = backend->same_copies.load(std::memory_order_acquire);
    static_cast<void>(upscale.Start(test_upscale_request({.source = current})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().input == current; }));
    CHECK(runs->load(std::memory_order_acquire) == 1U);
    CHECK(upscale.snapshot().frame.clean_revision == clean_revision);
    CHECK(backend->same_copies.load(std::memory_order_acquire) == copies + 2U);
    CHECK(backend->watched_source_copies.load() == 0U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(baseline.pixels.plane(1U).plane().data) == 23U);
    const auto output = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(output.valid());
    REQUIRE(output.pixels.plane_count() == 2U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(0U).plane().data) == 1U);
    CHECK(*reinterpret_cast<const std::uint8_t*>(output.pixels.plane(1U).plane().data) == 55U);
    CHECK(output.document->scene.categories.size() == 1U);
    CHECK(output.document->facts().resource == test_document({}).document->facts().resource);
    CHECK(output.document->facts().meaning_identity != test_document({}).document->facts().meaning_identity);
}

TEST_CASE("Upscale cached selection cannot redirect an active candidate and all methods retain one input") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}, 3U};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    std::atomic_uint32_t borrows{0U};
    EventGate events;
    DiagnosticCapture diagnostics;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [kernel, gate, runs] { return std::make_unique<TestUpscaleAlgorithm>(kernel, gate, runs, 3U); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              ++borrows;
                              return source.BorrowExact(frame);
                          },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics.sink()};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    std::array<VisualFrame, 2U> cached;
    for (const auto method : {UpscaleKernel::Default, UpscaleKernel::ShiftLut}) {
        static_cast<void>(upscale.Start(test_upscale_request({source.frame(), method})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
        cached[static_cast<std::size_t>(method)] = upscale.snapshot().frame;
    }
    const auto copies = backend->same_copies.load();
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::RealPlksr})));
    const bool running = entered.wait_for(2s) == std::future_status::ready;
    if (!running) gate->release.set_value();
    REQUIRE(running);
    const auto selected = upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::Default}));
    CHECK(selected.busy);
    CHECK(selected.pending->kernel == UpscaleKernel::RealPlksr);
    CHECK(selected.frame == cached[0U]);
    CHECK(selected.frame.revision < cached[1U].revision);
    CHECK(upscale.ObserveWorkspace().product_revision == selected.frame.revision);
    CHECK(diagnostics.reused_revision.load() == selected.frame.revision);
    CHECK(diagnostics.reused_meaning.load() == test_document({}).document->facts().meaning_identity);
    CHECK(diagnostics.reused_observation.load() == selected.revision);
    const auto read = upscale.BorrowDocument(selected.frame);
    REQUIRE(read.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(read.pixels.plane(0U).plane().data) == 1U);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
    CHECK(upscale.snapshot().frame == cached[0U]);
    CHECK(upscale.ObserveWorkspace().product_revision == cached[0U].revision);
    for (const auto& method : upscale.snapshot().methods)
        CHECK(method.available);
    CHECK(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::RealPlksr})).kernel == UpscaleKernel::RealPlksr);
    CHECK(runs->load() == 3U);
    CHECK(borrows.load() == 1U);
    CHECK(backend->same_copies.load() == copies);
}

TEST_CASE("Held output readers prevent obsolete foreground and warm work from preparing a candidate") {
    const bool preempt_warm = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct Admissions {
        std::atomic_uint32_t started{0U}, completed{0U}, prepared{0U};
        std::promise<void> blocked, returned;
    } admissions;
    auto blocked = admissions.blocked.get_future();
    auto returned = admissions.returned.get_future();
    const VisualDiagnosticSink diagnostics{
        .context = &admissions, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
            auto& probe = *static_cast<Admissions*>(context);
            if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAdmissionStarted && ++probe.started == 5U)
                probe.blocked.set_value();
            if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAdmissionCompleted && ++probe.completed == 5U)
                probe.returned.set_value();
            if (fact.operation == VisualDiagnosticOperation::UpscaleOutputAllocation) ++probe.prepared;
        }};
    EventGate events;
    std::atomic<mmltk::frameworks::gpu::SystemImageRuntime*> output_runtime{nullptr};
    const auto factory = TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs);
    UpscaleSystem upscale{kDevice,
                          [&, factory](auto revisions) {
                              auto runtime = factory(std::move(revisions));
                              output_runtime.store(runtime.get(), std::memory_order_release);
                              return runtime;
                          },
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics};
    std::vector<mmltk::frameworks::gpu::BorrowedImageProductReadView> readers;
    for (std::uint8_t value = 1U; value <= 4U; ++value) {
        source.Publish({16U, 8U}, value);
        static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && upscale.snapshot().input == source.frame(); }));
        readers.push_back(upscale.BorrowFrame());
        REQUIRE(readers.back().valid());
    }
    auto* const runtime = output_runtime.load(std::memory_order_acquire);
    REQUIRE(runtime != nullptr);
    // Check custody before starting competing work: an active reservation
    // must not mask an incorrectly writable reader-owned slot.
    REQUIRE(readers.size() == 4U);
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
    auto reservation = std::async(std::launch::async, [&] { return runtime->TryAcquireOutput(baseline).valid(); });
    REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "four held Upscale readers"));
    if (preempt_warm)
        upscale.Warm({16U, 8U});
    else {
        source.Publish({16U, 8U}, 5U);
        static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    }
    REQUIRE(blocked.wait_for(2s) == std::future_status::ready);
    blocked.get();
    const auto copied = backend->same_copies.load();
    const auto allocated = backend->planes_allocated.load();
    const auto prepared = admissions.prepared.load();
    source.Publish({16U, 8U}, 6U);
    const auto latest = test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::ShiftLut});
    static_cast<void>(upscale.Start(latest));
    readers[0U] = {};
    REQUIRE(returned.wait_for(2s) == std::future_status::ready);
    returned.get();
    REQUIRE(events.Wait([&] {
        return !upscale.snapshot().busy && upscale.snapshot().kernel == latest.kernel && upscale.snapshot().input == latest.source;
    }));
    CHECK(runs->load() == 5U);
    CHECK(admissions.prepared.load() == prepared + 1U);
    CHECK(backend->same_copies.load() == copied + 2U);
    CHECK(backend->planes_allocated.load() == allocated);
    readers.clear();
    upscale.Stop();
}

TEST_CASE("Upscale publishes only the newest selected kernel after obsolete device work settles") {
    auto backend = std::make_shared<FakeImageBackend>();
    // CLEANUP-IGNORE: This source feeds a supersession/commit gate; the cached-selection scenario retains three
    // completed methods and validates a different ownership path.
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    std::atomic_uint32_t ready_publications{0U};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [kernel, gate, runs] { return std::make_unique<TestUpscaleAlgorithm>(kernel, gate, runs); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&](UpscaleSystem::event_type event) {
                              if (const auto* changed = std::get_if<UpscaleChanged>(&event); changed && changed->snapshot.ready)
                                  ready_publications.fetch_add(1U, std::memory_order_acq_rel);
                              events.Advance();
                          }};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::ShiftLut})));
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    CHECK_FALSE(upscale.snapshot().ready);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && ready_publications.load(std::memory_order_acquire) == 1U; }));
    CHECK(upscale.snapshot().kernel == UpscaleKernel::RealPlksr);
    CHECK(upscale.snapshot().input == source.frame());
    CHECK(runs->load(std::memory_order_acquire) == 2U);
    CHECK(ready_publications.load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Upscale Stop reaches active latest work after its receiver copy settles") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct CopyGate final {
        std::atomic_bool first{true};
        std::promise<void> copied;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
    } gate;
    const VisualDiagnosticSink diagnostics{
        .context = &gate, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
            auto& current = *static_cast<CopyGate*>(context);
            if (fact.operation == VisualDiagnosticOperation::CopyCompleted && current.first.exchange(false)) {
                current.copied.set_value();
                current.released.wait();
            }
        }};
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate.release);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(gate.copied, "gate.copied", 2s));
    upscale.Stop();
    CHECK_FALSE(upscale.snapshot().busy);
    CHECK_FALSE(upscale.snapshot().ready);
    gate.release.set_value();
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(runs->load(std::memory_order_acquire) == 1U);
    CHECK(upscale.snapshot().input == source.frame());
}

// CLEANUP-IGNORE: This cancellation-at-borrow test has a two-window source-custody gate, unlike held-output
// admission pressure even though both begin with an Upscale fixture.
TEST_CASE("Superseded Upscale demand releases source custody without copying at both borrow windows") {
    const bool after_borrow = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    struct Gate final {
        bool after = false;
        std::atomic_bool first{true};
        std::atomic_uint32_t borrows{0U};
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        void Await() {
            if (!first.exchange(false)) return;
            entered.set_value();
            released.wait();
        }
    } gate;
    gate.after = after_borrow;
    const VisualDiagnosticSink diagnostics{
        .context = &gate, .write = [](void* context, const VisualDiagnosticFact fact) noexcept {
            auto& observed_gate = *static_cast<Gate*>(context);
            if (!observed_gate.after && fact.operation == VisualDiagnosticOperation::UpscaleWorkerStarted) observed_gate.Await();
        }};
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source, &gate](const VisualFrame& frame) {
                              ++gate.borrows;
                              auto borrowed = source.BorrowExact(frame);
                              if (gate.after) gate.Await();
                              return borrowed;
                          },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }, diagnostics};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate.release);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::Default})));
    const auto entered = gate.entered.get_future().wait_for(2s);
    if (entered != std::future_status::ready) gate.release.set_value();
    REQUIRE(entered == std::future_status::ready);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame(), UpscaleKernel::ShiftLut})));
    gate.release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(upscale.snapshot().kernel == UpscaleKernel::ShiftLut);
    CHECK(runs->load() == 1U);
    CHECK(gate.borrows.load() == (after_borrow ? 2U : 1U));
    CHECK(backend->same_copies.load() == 2U);
}

TEST_CASE("Upscale completed and failed facts require exact immutable document meaning") {
    auto backend = std::make_shared<FakeImageBackend>();
    // CLEANUP-IGNORE: This source is mutated to an inconsistent document meaning; the semantic-revision scenario
    // preserves valid document provenance and tests receiver-copy reuse.
    MutableVisualSource source{backend, {16U, 8U}};
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    EventGate events;
    UpscaleSystem upscale{kDevice, TestUpscaleAlgorithm::CreateRuntime(backend, kernel, runs),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    const auto request = test_upscale_request({source.frame()});
    static_cast<void>(upscale.Start(request));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    auto mismatched = request;
    mismatched.document.meaning_identity += 1U;
    static_cast<void>(upscale.Start(mismatched));
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy; }));
    CHECK_FALSE(upscale.snapshot().methods[0U].available);
    CHECK(upscale.snapshot().methods[0U].failure == mismatched);
    CHECK(upscale.snapshot().methods[0U].completed == request);
    static_cast<void>(upscale.Start(request));
    CHECK(upscale.snapshot().methods[0U].available);
    CHECK_FALSE(upscale.snapshot().methods[0U].failed);
    CHECK(runs->load() == 1U);
}

// CLEANUP-IGNORE: Warm idempotence and injected release-failure custody use distinct algorithms and lifecycle
// assertions.
TEST_CASE("Upscale warm activates every mode once and repeated ready signals are idempotent") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto activation = std::make_shared<UpscaleActivationProbe>();
    auto completed = activation->first_warm_completed.get_future();
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [](const VisualFrame&) { return VisualDocumentRead{}; },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};

    upscale.Warm({32U, 32U});
    upscale.Warm({32U, 32U});
    REQUIRE(completed.wait_for(2s) == std::future_status::ready);
    completed.get();
    upscale.Warm({32U, 32U});
    REQUIRE(events.Wait([&] { return std::ranges::all_of(upscale.snapshot().methods, [](const auto& method) { return method.warm; }); }));
    CHECK(activation->warms.load(std::memory_order_acquire) == 1U);
    CHECK(activation->runs.load(std::memory_order_acquire) == 9U);
    for (const auto& ready : activation->ready)
        CHECK(ready.load(std::memory_order_acquire));
}

TEST_CASE("Upscale CUDA tile replay preserves reference pixels and pitched receiver storage", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::frameworks::gpu;
    struct ReferenceEnvironment final {
        std::optional<std::string> prior;
        ReferenceEnvironment() {
            if (const auto* value = std::getenv("MMLTK_UPSCALE_ONNX_REFERENCE")) prior = value;
        }
        ~ReferenceEnvironment() {
            if (prior)
                static_cast<void>(::setenv("MMLTK_UPSCALE_ONNX_REFERENCE", prior->c_str(), 1));
            else
                static_cast<void>(::unsetenv("MMLTK_UPSCALE_ONNX_REFERENCE"));
        }
    } environment;
    constexpr std::uint32_t width = 197U, height = 193U;
    SystemImageRuntime source{{.device = 0, .context_mode = DeviceContextMode::PrimaryInterop}};
    SystemImageRuntime readback{{.device = 0, .context_mode = DeviceContextMode::PrimaryInterop}};
    source.BeginWork();
    auto staging = PinnedHostBuffer::ForCurrentDevice();
    staging->ensure_bytes(width * height * 4U);
    auto* pixels = static_cast<std::uint8_t*>(staging->data());
    std::array<std::vector<std::uint8_t>, 6U> reference;
    for (const bool reference_run : {true, false}) {
        REQUIRE(::setenv("MMLTK_UPSCALE_ONNX_REFERENCE", reference_run ? "1" : "0", 1) == 0);
        using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
        std::size_t tensor_allocations = 0U;
        std::size_t model_constructions = 0U;
        auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
            if (stage == Stage::BuffersAllocated) ++tensor_allocations;
            if (stage == Stage::ContextCreated) ++model_constructions;
        })(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>());
        runtime->BeginWork();
        auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
        REQUIRE(model != nullptr);
        CUdeviceptr previous_input = 0U;
        std::array<CUdeviceptr, 2U> output_slots{};
        std::size_t warm_tensors = 0U, warm_models = 0U;
        for (std::size_t iteration = 0U; iteration < (reference_run ? 6U : 12U); ++iteration) {
            const auto pattern = iteration % 2U;
            const auto method = static_cast<UpscaleKernel>((iteration / 2U) % 3U);
            CAPTURE(reference_run, iteration, method);
            for (std::uint32_t y = 0U; y < height; ++y) {
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const auto pixel = (static_cast<std::size_t>(y) * width + x) * 4U;
                    pixels[pixel] = static_cast<std::uint8_t>((x * 7U + y * 3U + pattern * 73U) % 256U);
                    pixels[pixel + 1U] = static_cast<std::uint8_t>((x ^ y) + pattern * 41U);
                    pixels[pixel + 2U] = static_cast<std::uint8_t>((x * y + pattern * 109U) % 256U);
                    pixels[pixel + 3U] = 255U;
                }
            }
            source.Publish(width, height, [&](const auto clean, const auto, const auto stream) {
                CUDA_MEMCPY2D upload{};
                upload.srcMemoryType = CU_MEMORYTYPE_HOST;
                upload.srcHost = pixels;
                upload.srcPitch = width * 4U;
                upload.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                upload.dstDevice = clean.data;
                upload.dstPitch = clean.descriptor.pitch_bytes;
                upload.WidthInBytes = width * 4U;
                upload.Height = height;
                REQUIRE(cuMemcpy2DAsync(&upload, reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS);
            });
            const auto copied = runtime->CopyInputFrom(
                source.Borrow(), [model](const auto plane, const auto stream) { model->Semantics({}, plane, stream); });
            CHECK(copied[0U] == ImageCopyPath::SameDevice);
            const auto input = runtime->BorrowInput();
            REQUIRE(input.valid());
            const auto input_plane = input.plane(0U).plane();
            if (previous_input != 0U) CHECK(input_plane.data == previous_input);
            previous_input = input_plane.data;
            CHECK(input_plane.descriptor.pitch_bytes >= width * 4U);
            CHECK(input_plane.descriptor.pitch_bytes % 16U == 0U);
            runtime->Publish(width * 4U, height * 4U, [model, &input, method](const auto clean, const auto semantic, const auto stream) {
                model->Run(method, input.plane(0U).plane(), clean, stream);
                model->Semantics({}, semantic, stream);
            });
            // The receiver-copy boundary waits for producer completion before
            // this test-only readback. Product execution never reads through CPU memory.
            CHECK(readback.CopyFrom(runtime->Borrow())[0U] == ImageCopyPath::SameDevice);
            {
                const auto output = runtime->Borrow();
                const auto plane = output.plane(0U).plane();
                const auto slot = iteration % output_slots.size();
                if (output_slots[slot] != 0U) CHECK(plane.data == output_slots[slot]);
                output_slots[slot] = plane.data;
                if (output_slots[1U] != 0U) CHECK(output_slots[0U] != output_slots[1U]);
                CHECK(plane.descriptor.pitch_bytes % 16U == 0U);
            }
            const auto returned = readback.Borrow();
            const auto plane = returned.plane(0U).plane();
            std::vector<std::uint8_t> actual(static_cast<std::size_t>(width) * height * 64U);
            CUDA_MEMCPY2D download{};
            download.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            download.srcDevice = plane.data;
            download.srcPitch = plane.descriptor.pitch_bytes;
            download.dstMemoryType = CU_MEMORYTYPE_HOST;
            download.dstHost = actual.data();
            download.dstPitch = width * 16U;
            download.WidthInBytes = width * 16U;
            download.Height = height * 4U;
            REQUIRE(cuMemcpy2D(&download) == CUDA_SUCCESS);
            if (iteration == 5U) {
                warm_tensors = tensor_allocations;
                warm_models = model_constructions;
            } else if (iteration > 5U) {
                CHECK(tensor_allocations == warm_tensors);
                CHECK(model_constructions == warm_models);
            }
            if (reference_run)
                reference[iteration] = std::move(actual);
            else
                CHECK(actual == reference[iteration % reference.size()]);
        }
        REQUIRE(runtime->Retire().safe_to_destroy);
    }
}

TEST_CASE("Native Upscale warm aggregate retires before model and primary context destruction") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");

    auto runtime = make_native_upscale_runtime_factory(kDevice)(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>());
    runtime->BeginWork();
    auto* const model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
    REQUIRE(model != nullptr);
    model->Warm();

    const auto retirement = runtime->Retire();
    CHECK(retirement.safe_to_destroy);
    CHECK_FALSE(retirement.failure);
    CHECK_FALSE(retirement.custody.valid());
    CHECK_NOTHROW(runtime.reset());
}

void run_native_upscale(mmltk::frameworks::gpu::SystemImageRuntime& runtime, UpscaleKernel kernel,
                        const std::function<bool()>& current = {}) {
    runtime.BeginWork();
    auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime.model());
    REQUIRE(model != nullptr);
    if (!runtime.BorrowInput().valid()) {
        runtime.PublishInput(8U, 8U, [model](auto clean, auto semantic, auto stream) {
            model->Semantics({}, clean, stream);
            model->Semantics({}, semantic, stream);
        });
    }
    const auto input = runtime.BorrowInput();
    runtime.Publish(32U, 32U, [&](auto clean, auto semantic, auto stream) {
        model->Run(kernel, input.plane(0U).plane(), clean, stream, current);
        model->Semantics({}, semantic, stream);
    });
}

class NativeUpscaleSource final {
   public:
    NativeUpscaleSource()
        : runtime_({.device = 0,
                    .context_mode = mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop,
                    .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic}) {
        runtime_.BeginWork();
        runtime_.Publish(8U, 8U, [](auto clean, auto semantic, auto stream) {
            for (auto plane : {clean, semantic})
                REQUIRE(cuMemsetD2D8Async(plane.data, plane.descriptor.pitch_bytes, 0, 32U, 8U, reinterpret_cast<CUstream>(stream)) ==
                        CUDA_SUCCESS);
        });
        frame = visual_frame({PresentationSourceKind::Explore, 1U}, {8U, 8U}, runtime_.OutputFacts().revision);
        frame.clean_revision = frame.revision;
    }
    VisualDocumentRead Borrow(const VisualFrame& requested) const {
        return test_document(borrow_matching_visual_product(requested, runtime_.Borrow()));
    }
    VisualFrame frame{};

   private:
    mmltk::frameworks::gpu::SystemImageRuntime runtime_;
};

class NativeUpscaleFailureScenario final {
   public:
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using Injection = std::function<void(Stage, std::atomic_bool&)>;

    explicit NativeUpscaleFailureScenario(Injection injection)
        : upscale{kDevice,
                  make_native_upscale_runtime_factory(
                      kDevice, [this, injection = std::move(injection)](const Stage reached) { injection(reached, armed); }),
                  [this](const VisualFrame& frame) { return source.Borrow(frame); },
                  [this](UpscaleSystem::event_type event) {
                      if (auto* failure = std::get_if<UpscaleFailed>(&event)) failed.set_value(*failure);
                      events.Advance();
                  }} {}

    NativeUpscaleSource source;
    std::atomic_bool armed{true};
    std::promise<UpscaleFailed> failed;
    EventGate events;
    UpscaleSystem upscale;
};

TEST_CASE("Native Upscale cancellation settles admitted work without recording initialization failure", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const auto stage = GENERATE(Stage::InitializationAdmitted, Stage::ChecksumAdmitted, Stage::BasicAllocationAdmitted,
                                Stage::BasicLaunchAdmitted, Stage::PreprocessAdmitted, Stage::TargetAdmitted, Stage::TilePrepared,
                                Stage::ContextCreated, Stage::WarmInputSubmitted, Stage::WarmSettled, Stage::CaptureBegan,
                                Stage::CaptureEnded, Stage::GraphInstantiated, Stage::ReplaySettled, Stage::CacheLockAdmitted);
    const bool stop = GENERATE(false, true);
    const bool basic = stage == Stage::BasicAllocationAdmitted || stage == Stage::BasicLaunchAdmitted;
    const bool neural_warm = (stage >= Stage::WarmInputSubmitted && stage <= Stage::ReplaySettled) || stage == Stage::CacheLockAdmitted;
    const auto method = basic ? UpscaleKernel::Default : neural_warm ? UpscaleKernel::RealPlksr : UpscaleKernel::ShiftLut;
    CAPTURE(static_cast<std::uint32_t>(stage), stop, static_cast<std::uint32_t>(method));
    NativeUpscaleSource source;
    std::promise<void> admitted, release;
    auto released = release.get_future().share();
    std::atomic_bool armed{true};
    std::array<std::atomic_uint32_t, static_cast<std::size_t>(Stage::Count)> stages{};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          make_native_upscale_runtime_factory(kDevice,
                                                              [&](Stage reached) {
                                                                  ++stages[static_cast<std::size_t>(reached)];
                                                                  if (reached == stage && armed.exchange(false)) {
                                                                      admitted.set_value();
                                                                      released.wait();
                                                                  }
                                                              }),
                          [&source](const VisualFrame& frame) { return source.Borrow(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    auto settle_native = settle_upscale_on_exit(upscale, release);
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, method})));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(admitted, "native Upscale admitted boundary", 120s));
    if (stop)
        static_cast<void>(upscale.Stop());
    else
        static_cast<void>(upscale.Start(
            test_upscale_request({source.frame, method == UpscaleKernel::Default ? UpscaleKernel::ShiftLut : UpscaleKernel::Default})));
    release.set_value();
    if (stop) {
        // A subsequent request is queued behind settlement, proving Stop did
        // not leave an activated owner or an incomplete capture behind.
        static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::Default})));
    }
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && !upscale.snapshot().busy; }, 120s));
    CHECK_FALSE(upscale.snapshot().methods[static_cast<std::size_t>(method)].initialization_failed);
    if (stage == Stage::ReplaySettled) CHECK(stages[static_cast<std::size_t>(Stage::ContextCreated)].load() == 1U);
}

TEST_CASE("Native CUDA failure scope reaches Upscale quarantine or isolated retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto status = GENERATE(cudaErrorDeviceUninitialized, cudaErrorContained, cudaErrorTensorMemoryLeak, cudaErrorSystemNotReady,
                                 cudaErrorMpsClientTerminated, cudaErrorExternalDevice, cudaErrorMemoryAllocation, cudaErrorInvalidValue,
                                 cudaErrorLaunchOutOfResources);
    NativeUpscaleFailureScenario scenario{[status](const Stage reached, std::atomic_bool& armed) {
        if (reached == Stage::ContextCreated && armed.exchange(false)) throw CudaError(status, "injected native CUDA scope");
    }};
    static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame})));
    REQUIRE(scenario.events.Wait([&] { return scenario.upscale.snapshot().ready; }));
    static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut})));
    auto failure = scenario.failed.get_future();
    REQUIRE(failure.wait_for(120s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK((result.kind == UpscaleFailureKind::Physical) == cuda_shared_failure(status));
    if (cuda_shared_failure(status)) {
        CHECK_FALSE(result.snapshot.ready);
        CHECK_FALSE(scenario.upscale.BorrowFrame().valid());
        for (const auto& method : result.snapshot.methods)
            CHECK_FALSE(method.available);
    } else {
        CHECK(result.snapshot.ready);
        CHECK(result.snapshot.methods[0U].available);
        static_cast<void>(scenario.upscale.Start(test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut})));
        REQUIRE(scenario.events.Wait(
            [&] { return scenario.upscale.snapshot().kernel == UpscaleKernel::ShiftLut && !scenario.upscale.snapshot().busy; }, 120s));
        CHECK_FALSE(scenario.upscale.snapshot().methods[1U].initialization_failed);
    }
}

TEST_CASE("Completed native activation survives same-method withdrawal without repeating initialization", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto boundary = GENERATE(Stage::ReplaySettled, Stage::PreprocessAdmitted, Stage::TargetAdmitted, Stage::TilePrepared,
                                   Stage::RuntimeEnqueued, Stage::BindingsReady);
    const auto method = boundary == Stage::BindingsReady ? UpscaleKernel::ShiftLut : UpscaleKernel::RealPlksr;
    std::array<unsigned, static_cast<std::size_t>(Stage::Count)> counts{};
    bool current = true;
    bool armed = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        const auto count = ++counts[static_cast<std::size_t>(reached)];
        // Both TRT lanes must finish; withdrawing after lane one remains a
        // partial activation and intentionally does not establish residency.
        if (armed && reached == boundary && (boundary != Stage::ReplaySettled || count == 2U)) {
            armed = false;
            current = false;
        }
    })(std::make_shared<ImageProductRevisionSequence>());
    CHECK_NOTHROW(run_native_upscale(*runtime, method, [&] { return current; }));
    REQUIRE_FALSE(armed);
    REQUIRE_FALSE(current);
    const auto completed = counts;
    current = true;
    CHECK_NOTHROW(run_native_upscale(*runtime, method, [&] { return current; }));
    const auto check_unchanged = [&](const std::initializer_list<Stage> stages) {
        for (const auto stage : stages) {
            CAPTURE(boundary, stage);
            CHECK(counts[static_cast<std::size_t>(stage)] == completed[static_cast<std::size_t>(stage)]);
        }
    };
    check_unchanged({Stage::InitializationAdmitted, Stage::ChecksumAdmitted, Stage::CacheLockAdmitted, Stage::BuildAdmitted,
                     Stage::ContextCreated, Stage::BuffersAllocated, Stage::StreamCreated, Stage::EventCreated, Stage::BindingsReady});
    if (method == UpscaleKernel::RealPlksr) {
        check_unchanged({Stage::WarmInputSubmitted, Stage::WarmSubmitted, Stage::WarmSettled, Stage::CaptureBegan, Stage::CaptureSubmitted,
                         Stage::CaptureEnded, Stage::GraphInstantiated, Stage::ReplaySubmitted, Stage::ReplaySettled});
        CHECK(counts[static_cast<std::size_t>(Stage::ContextCreated)] == 2U);
        CHECK(counts[static_cast<std::size_t>(Stage::ReplaySettled)] == 2U);
    }
    CHECK(runtime->Retire().safe_to_destroy);
}

TEST_CASE("Native method failures are classified at the actual activation completion boundary", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto boundary =
        GENERATE(Stage::ContextCreated, Stage::PreprocessAdmitted, Stage::TargetAdmitted, Stage::WarmSubmitted, Stage::RuntimeEnqueued);
    NativeUpscaleFailureScenario scenario{[boundary](const Stage reached, std::atomic_bool& armed) {
        if (reached == boundary && armed.exchange(false))
            throw CudaError(cudaErrorMemoryAllocation, "settled activation-boundary allocation failure");
    }};
    const auto request = test_upscale_request({scenario.source.frame, UpscaleKernel::ShiftLut});
    static_cast<void>(scenario.upscale.Start(request));
    auto failure = scenario.failed.get_future();
    REQUIRE(failure.wait_for(120s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK(result.kind == UpscaleFailureKind::Failed);
    CHECK(result.request == request);
    CHECK(result.snapshot.methods[1U].failed);
    CHECK(result.snapshot.methods[1U].initialization_failed == (boundary == Stage::ContextCreated));
    static_cast<void>(scenario.upscale.Start(request));
    REQUIRE(scenario.events.Wait([&] { return scenario.upscale.snapshot().ready && !scenario.upscale.snapshot().busy; }, 120s));
    CHECK_FALSE(scenario.upscale.snapshot().methods[1U].initialization_failed);
}

TEST_CASE("Native withdrawal does not erase a concurrently reported method failure", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const bool physical = GENERATE(false, true);
    bool current = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
        if (stage == Stage::WarmSubmitted && std::exchange(current, false))
            throw CudaError(physical ? cudaErrorContained : cudaErrorMemoryAllocation, "method failure concurrent with withdrawal");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        run_native_upscale(*runtime, UpscaleKernel::ShiftLut, [&] { return current; });
    } catch (...) { failure = std::current_exception(); }
    REQUIRE(failure);
    CHECK_FALSE(current);
    CHECK(is_image_execution_failure(failure) == physical);
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    CHECK(runtime->Retire().safe_to_destroy);
}

TEST_CASE("Native cache lock admission remains cancellable while another process owns the cache", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    {
        auto seed = make_native_upscale_runtime_factory(kDevice)(std::make_shared<ImageProductRevisionSequence>());
        run_native_upscale(*seed, UpscaleKernel::RealPlksr);
        REQUIRE(seed->Retire().safe_to_destroy);
    }
    const auto cache = mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "upscalers";
    std::vector<mmltk::common::io::ScopedFd> locks;
    for (const auto& entry : std::filesystem::directory_iterator(cache)) {
        if (entry.path().extension() != ".lock") continue;
        mmltk::common::io::ScopedFd descriptor{::open(entry.path().c_str(), O_RDWR | O_CLOEXEC)};
        REQUIRE(descriptor.get() >= 0);
        REQUIRE(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0);
        locks.push_back(std::move(descriptor));
    }
    REQUIRE_FALSE(locks.empty());
    const bool stop = GENERATE(false, true);
    std::atomic_bool observed{false};
    std::promise<void> waiting;
    NativeUpscaleSource source;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          make_native_upscale_runtime_factory(kDevice,
                                                              [&](Stage stage) {
                                                                  if (stage == Stage::CacheLockWaiting && !observed.exchange(true))
                                                                      waiting.set_value();
                                                              }),
                          [&source](const VisualFrame& frame) { return source.Borrow(frame); },
                          [&events](UpscaleSystem::event_type) { events.Advance(); }};
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::RealPlksr})));
    const auto reached = waiting.get_future().wait_for(120s);
    if (reached != std::future_status::ready) static_cast<void>(upscale.Stop());
    REQUIRE(reached == std::future_status::ready);
    if (stop) static_cast<void>(upscale.Stop());
    static_cast<void>(upscale.Start(test_upscale_request({source.frame, UpscaleKernel::Default})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && !upscale.snapshot().busy; }));
    CHECK_FALSE(upscale.snapshot().methods[2U].initialization_failed);
    // Cache locks are still owned: completion therefore proves withdrawal,
    // rather than eventual admission after a lock happened to become free.
}

TEST_CASE("TensorRT build progress accepts cancellation without an initialization exception", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::backend::ml::runtime;
    const bool during_build = GENERATE(false, true);
    std::atomic_bool building{false};
    std::atomic_uint32_t observations{0U};
    TensorRtEngineOptions options{};
    options.device = 0;
    options.optimization_profiles = {
        {.input_name = "input", .minimum = {1, 3, 256, 256}, .optimum = {1, 3, 256, 256}, .maximum = {1, 3, 256, 256}}};
    options.log = [&](std::string_view message) {
        if (message == "[trt:build] building serialized engine") building.store(true);
    };
    options.continue_build = [&] {
        if (!during_build) return false;
        if (!building.load()) return true;
        // The first check is the pre-build admission; the second comes from
        // the vendor progress monitor while buildSerializedNetwork is active.
        return ++observations < 2U;
    };
    std::optional<TensorRtEngine> engine;
    try {
        engine.emplace(mmltk::common::system::runtime_paths::repository_root() / "src/backend/imaging/upscale/assets/RealPLKSR_fp16.onnx",
                       std::move(options));
    } catch (const TensorRtOperationError& failure) { FAIL("TensorRT cancellation reported operation error code " << failure.code()); }
    REQUIRE(engine);
    CHECK(engine->cancelled());
    CHECK(engine->native_engine_handle() == 0U);
    if (during_build) CHECK(observations.load() >= 2U);
}

TEST_CASE("Installed neural activation owns every submitted stage through settled failure and retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto stage =
        GENERATE(Stage::StreamCreated, Stage::EventCreated, Stage::ContextCreated, Stage::BuffersAllocated, Stage::WarmInputSubmitted,
                 Stage::WarmSubmitted, Stage::WarmSettled, Stage::CaptureBegan, Stage::CaptureSubmitted, Stage::CaptureEnded,
                 Stage::GraphInstantiated, Stage::ReplaySubmitted, Stage::ReplaySettled);
    const auto method = GENERATE(UpscaleKernel::ShiftLut, UpscaleKernel::RealPlksr);
    const auto occurrence = GENERATE(1U, 2U, 3U, 4U);
    CAPTURE(static_cast<std::uint32_t>(stage), static_cast<std::uint32_t>(method), occurrence);
    if (method == UpscaleKernel::ShiftLut && (stage == Stage::WarmInputSubmitted || stage > Stage::WarmSubmitted)) return;
    const auto maximum = method == UpscaleKernel::ShiftLut ? ((stage == Stage::EventCreated || stage == Stage::WarmSubmitted)       ? 3U
                                                              : (stage == Stage::BuffersAllocated || stage == Stage::StreamCreated) ? 2U
                                                                                                                                    : 1U)
                                                           : ((stage == Stage::BuffersAllocated || stage == Stage::EventCreated) ? 4U
                                                              : stage == Stage::StreamCreated                                    ? 3U
                                                                                                                                 : 2U);
    if (occurrence > maximum) return;
    bool armed = true;
    unsigned reached_count = 0U;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        if (reached == stage && ++reached_count == occurrence && std::exchange(armed, false))
            throw CudaError(cudaErrorMemoryAllocation, "injected settled neural activation allocation failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        for (unsigned replay = 0U; replay < 3U && armed; ++replay)
            run_native_upscale(*runtime, method);
    } catch (...) { failure = std::current_exception(); }
    REQUIRE_FALSE(armed);
    REQUIRE(failure);
    CHECK_FALSE(is_image_execution_failure(failure));
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    CHECK_NOTHROW(run_native_upscale(*runtime, method == UpscaleKernel::ShiftLut ? UpscaleKernel::RealPlksr : UpscaleKernel::ShiftLut));
    CHECK_NOTHROW(run_native_upscale(*runtime, method));
    CHECK(runtime->Retire().safe_to_destroy);
}

TEST_CASE("Neural shared activation and release failures retain typed physical custody", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const bool release_failure = GENERATE(false, true);
    bool armed = true;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        const auto selected = release_failure ? Stage::ContextReleased : Stage::WarmSubmitted;
        if (reached == selected && std::exchange(armed, false))
            throw CudaError(cudaErrorIllegalAddress, "injected shared neural physical failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    if (release_failure) {
        run_native_upscale(*runtime, UpscaleKernel::ShiftLut);
        auto retirement = runtime->Retire();
        CHECK_FALSE(armed);
        CHECK_FALSE(retirement.safe_to_destroy);
        CHECK(retirement.custody.valid());
        CHECK(is_image_execution_failure(retirement.failure));
        CHECK(static_cast<bool>(find_image_failure<CudaError>(retirement.failure)));
    } else {
        std::exception_ptr failure;
        try {
            run_native_upscale(*runtime, UpscaleKernel::ShiftLut);
        } catch (...) { failure = std::current_exception(); }
        CHECK_FALSE(armed);
        CHECK(is_image_execution_failure(failure));
        CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
        CHECK(runtime->Retire().safe_to_destroy);
    }
}

TEST_CASE("Neural cleanup attempts all settled releases after each destruction boundary fails", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    using namespace mmltk::frameworks::gpu;
    const auto stage = GENERATE(Stage::GraphExecutableDestroyed, Stage::GraphDestroyed, Stage::EventDestroyed, Stage::StreamDestroyed,
                                Stage::BufferReleased, Stage::ContextReleased);
    std::array<unsigned, static_cast<std::size_t>(Stage::Count)> released{};
    bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage reached) {
        ++released[static_cast<std::size_t>(reached)];
        if (reached == stage && !std::exchange(injected, true)) throw CudaError(cudaErrorUnknown, "injected neural release failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    run_native_upscale(*runtime, UpscaleKernel::RealPlksr);
    auto retirement = runtime->Retire();
    CHECK(injected);
    CHECK_FALSE(retirement.safe_to_destroy);
    CHECK(retirement.custody.valid());
    CHECK(is_image_execution_failure(retirement.failure));
    CHECK(released[static_cast<std::size_t>(Stage::GraphExecutableDestroyed)] == 2U);
    CHECK(released[static_cast<std::size_t>(Stage::GraphDestroyed)] == 2U);
    CHECK(released[static_cast<std::size_t>(Stage::EventDestroyed)] == 4U);
    CHECK(released[static_cast<std::size_t>(Stage::StreamDestroyed)] == 3U);
    CHECK(released[static_cast<std::size_t>(Stage::BufferReleased)] == 4U);
    CHECK(released[static_cast<std::size_t>(Stage::ContextReleased)] == 2U);
}

TEST_CASE("Valid TensorRT cache survives activation allocation and shared-context failures", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    using namespace mmltk::frameworks::gpu;
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const bool physical = GENERATE(false, true);
    {
        auto seed = make_native_upscale_runtime_factory(kDevice)(std::make_shared<ImageProductRevisionSequence>());
        run_native_upscale(*seed, UpscaleKernel::RealPlksr);
        REQUIRE(seed->Retire().safe_to_destroy);
    }
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> cache;
    const auto directory = mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "upscalers";
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".engine") cache.emplace_back(entry.path(), entry.last_write_time());
    REQUIRE_FALSE(cache.empty());
    bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](Stage stage) {
        if (stage == Stage::ContextCreated && !std::exchange(injected, true))
            throw CudaError(physical ? cudaErrorIllegalAddress : cudaErrorMemoryAllocation, "valid cache activation failure");
    })(std::make_shared<ImageProductRevisionSequence>());
    std::exception_ptr failure;
    try {
        run_native_upscale(*runtime, UpscaleKernel::RealPlksr);
    } catch (...) { failure = std::current_exception(); }
    REQUIRE(injected);
    REQUIRE(failure);
    CHECK(is_image_execution_failure(failure) == physical);
    CHECK(static_cast<bool>(find_image_failure<CudaError>(failure)));
    for (const auto& [path, modified] : cache) {
        REQUIRE(std::filesystem::is_regular_file(path));
        CHECK(std::filesystem::last_write_time(path) == modified);
    }
    CHECK(runtime->Retire().safe_to_destroy);
}

TEST_CASE("Native Basic settled allocation and launch failures permit neural work and lazy Basic retry", "[upscale_gpu]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    const bool allocation_failure = GENERATE(false, true);
    using namespace mmltk::frameworks::gpu;
    using Stage = mmltk::backend::imaging::upscale::ImageUpscalerExecutionStage;
    const Stage failure_stage = allocation_failure ? Stage::BasicAllocationAdmitted : Stage::BasicLaunchAdmitted;
    std::atomic_uint32_t stage_occurrence = 0U;
    std::atomic_bool injected = false;
    auto runtime = make_native_upscale_runtime_factory(kDevice, [&](const Stage stage) {
        if (stage == failure_stage && stage_occurrence.fetch_add(1U, std::memory_order_relaxed) == 1U) {
            injected.store(true, std::memory_order_relaxed);
            throw CudaError(allocation_failure ? cudaErrorMemoryAllocation : cudaErrorInvalidPitchValue,
                            "injected settled Basic operation failure");
        }
    })(std::make_shared<ImageProductRevisionSequence>());
    runtime->BeginWork();
    auto* model = dynamic_cast<UpscaleAlgorithm*>(runtime->model());
    REQUIRE(model != nullptr);
    runtime->PublishInput(8U, 8U, [model](auto clean, auto semantic, auto stream) {
        model->Semantics({}, clean, stream);
        model->Semantics({}, semantic, stream);
    });
    {
        const auto input = runtime->BorrowInput();
        const auto publish = [&](const UpscaleKernel method) {
            runtime->Publish(32U, 32U, [&](auto output, auto semantic, auto stream) {
                auto source = input.plane(0U).plane();
                model->Run(method, source, output, stream);
                model->Semantics({}, semantic, stream);
            });
        };
        try {
            publish(UpscaleKernel::Default);
            FAIL("Basic accepted the failing allocation or launch");
        } catch (const CudaError& failure) {
            CHECK(failure.status() == (allocation_failure ? cudaErrorMemoryAllocation : cudaErrorInvalidPitchValue));
            CHECK_FALSE(failure.shared_failure());
        }
        CHECK(injected.load(std::memory_order_relaxed));
        CHECK_NOTHROW(publish(UpscaleKernel::ShiftLut));
        CHECK_NOTHROW(publish(UpscaleKernel::Default));
    }
    // The local error must not poison the aggregate's checked release.
    CHECK(runtime->Retire().safe_to_destroy);
}

TEST_CASE("Warmed Upscale retains physical custody when model release fails") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto release = std::make_shared<UpscaleReleaseFailureProbe>();
    auto warmed = release->warmed.get_future();
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [release] { return std::make_unique<ReleaseFailingUpscaleAlgorithm>(release); }, 4U),
                          [](const VisualFrame&) { return VisualDocumentRead{}; }};

    upscale.Warm({32U, 32U});
    REQUIRE(warmed.wait_for(2s) == std::future_status::ready);
    warmed.get();
    upscale.Shutdown();
    CHECK(upscale.stopped());
    CHECK(release->releases.load(std::memory_order_acquire) == 1U);
    CHECK_FALSE(release->destroyed.load(std::memory_order_acquire));
    CHECK(backend->contexts_destroyed.load(std::memory_order_acquire) == 0U);
    CHECK(backend->streams_destroyed.load(std::memory_order_acquire) == 0U);
}

TEST_CASE("Upscale requests behind warmup retain identities without queued source leases") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {20U, 12U}, 7U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->hold_warm.store(true, std::memory_order_release);
    auto warm_entered = activation->warm_entered.get_future();
    std::atomic_uint32_t borrows{0U};
    std::atomic_uint32_t failures{0U};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              borrows.fetch_add(1U);
                              return source.BorrowExact(frame);
                          },
                          [&events, &failures](UpscaleSystem::event_type event) {
                              if (std::holds_alternative<UpscaleFailed>(event)) failures.fetch_add(1U);
                              events.Advance();
                          }};
    auto settle_upscale = settle_upscale_on_exit(upscale, activation->warm_release);
    upscale.Warm(source.frame().extent);
    const bool warming = warm_entered.wait_for(2s) == std::future_status::ready;
    if (!warming) activation->warm_release.set_value();
    REQUIRE(warming);
    const auto admitted = upscale.Start(test_upscale_request({.source = source.frame()}));
    CHECK(admitted.busy);
    CHECK(borrows.load() == 0U);
    CHECK(activation->runs.load(std::memory_order_acquire) == 0U);
    source.Publish({20U, 12U}, 19U);
    activation->warm_release.set_value();
    REQUIRE(events.Wait([&] { return failures.load() == 1U; }));
    CHECK_FALSE(upscale.snapshot().ready);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(upscale.snapshot().input == source.frame());
    CHECK(borrows.load() == 2U);
    CHECK(activation->fallback_activations.load(std::memory_order_acquire) == 0U);
}

TEST_CASE("Upscale warm failure is isolated and Start retains first-use activation fallback") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {18U, 10U}, 5U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->fail_warm.store(true, std::memory_order_release);
    std::promise<void> warm_failed;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&warm_failed, &events](UpscaleSystem::event_type event) {
                              if (std::holds_alternative<UpscaleFailed>(event)) warm_failed.set_value();
                              events.Advance();
                          }};

    upscale.Warm({32U, 32U});
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(warm_failed, "warm_failed", 2s));
    upscale.Warm({32U, 32U});
    CHECK(activation->warms.load(std::memory_order_acquire) == 1U);
    CHECK_FALSE(upscale.snapshot().busy);
    CHECK_FALSE(upscale.snapshot().ready);
    REQUIRE(events.Wait([&] { return upscale.snapshot().methods[1U].warm && upscale.snapshot().methods[2U].warm; }));
    CHECK(activation->runs.load(std::memory_order_acquire) == 6U);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(events.Wait([&] { return activation->runs.load(std::memory_order_acquire) == 7U; }));
    CHECK(activation->fallback_activations.load(std::memory_order_acquire) == 1U);
    for (const auto& ready : activation->ready)
        CHECK(ready.load(std::memory_order_acquire));
    CHECK(upscale.snapshot().methods[0U].initialization_failed);
    CHECK_FALSE(upscale.snapshot().methods[2U].initialization_failed);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::Default})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().kernel == UpscaleKernel::Default; }));
    CHECK_FALSE(upscale.snapshot().methods[0U].initialization_failed);
}

TEST_CASE("A preempted warm failure remains method-local through another method completion") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {18U, 10U}, 5U};
    auto activation = std::make_shared<UpscaleActivationProbe>();
    activation->fail_warm = true;
    activation->hold_warm = true;
    auto entered = activation->warm_entered.get_future();
    std::promise<UpscaleSnapshot> completed;
    auto result = completed.get_future();
    std::atomic_bool observed{false};
    UpscaleSystem upscale{
        kDevice,
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            [activation] { return std::make_unique<ActivationUpscaleAlgorithm>(activation); }, 4U),
        [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
        [&](UpscaleSystem::event_type event) {
            if (const auto* changed = std::get_if<UpscaleChanged>(&event);
                changed && changed->snapshot.ready && changed->snapshot.kernel == UpscaleKernel::RealPlksr && !observed.exchange(true))
                completed.set_value(changed->snapshot);
        }};
    auto settle_upscale = settle_upscale_on_exit(upscale, activation->warm_release);
    upscale.Warm({32U, 32U});
    const auto waiting = entered.wait_for(2s);
    if (waiting != std::future_status::ready) activation->warm_release.set_value();
    REQUIRE(waiting == std::future_status::ready);
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.frame(), .kernel = UpscaleKernel::RealPlksr})));
    activation->warm_release.set_value();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto snapshot = result.get();
    CHECK(snapshot.methods[0U].initialization_failed);
    CHECK_FALSE(snapshot.methods[0U].warm);
    CHECK(snapshot.methods[2U].warm);
    CHECK_FALSE(snapshot.methods[2U].initialization_failed);
}

TEST_CASE("Upscale exact-frame admission separates invalid kernels from unavailable sources") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {24U, 15U}};
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    std::atomic<std::size_t> borrows{0U};
    std::atomic<UpscaleFailureKind> failure_kind{UpscaleFailureKind::Failed};
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          [&source, &borrows](const VisualFrame& frame) {
                              borrows.fetch_add(1U, std::memory_order_acq_rel);
                              return source.BorrowExact(frame);
                          },
                          [&events, &failure_kind](UpscaleSystem::event_type event) {
                              if (const auto* failure = std::get_if<UpscaleFailed>(&event)) failure_kind.store(failure->kind);
                              events.Advance();
                          }};
    const auto current = source.frame();

    CHECK_THROWS_AS(upscale.Start(test_upscale_request({.source = current, .kernel = static_cast<UpscaleKernel>(255U)})),
                    contracts::InvalidIntentError);
    CHECK(borrows.load(std::memory_order_acquire) == 0U);
    for (auto invalid : {VisualDocumentFacts{}, test_document({}).document->facts()}) {
        invalid.meaning_identity = 0U;
        CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid}), contracts::InvalidIntentError);
    }
    auto invalid_resource = test_document({}).document->facts();
    invalid_resource.resource = {};
    CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid_resource}), contracts::InvalidIntentError);
    auto invalid_revision = test_document({}).document->facts();
    invalid_revision.resource.revision = 0U;
    CHECK_THROWS_AS(upscale.Start({.source = current, .document = invalid_revision}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(upscale.Start(test_upscale_request({.source = {}})), contracts::InvalidIntentError);
    CHECK(borrows.load(std::memory_order_acquire) == 0U);
    for (const auto& expired : {visual_frame(current.source, current.extent, current.revision + 1U),
                                visual_frame(current.source, {current.extent.width + 1U, current.extent.height}, current.revision),
                                visual_frame({PresentationSourceKind::Explore, 2U}, current.extent, current.revision)}) {
        static_cast<void>(upscale.Start(test_upscale_request({.source = expired})));
        REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && failure_kind.load() == UpscaleFailureKind::Unavailable; }));
        CHECK_FALSE(upscale.snapshot().ready);
        CHECK(upscale.snapshot().methods[0U].failed);
        CHECK(failure_kind.load() == UpscaleFailureKind::Unavailable);
    }
    CHECK(borrows.load(std::memory_order_acquire) == 3U);
}

TEST_CASE("Upscale copies the admitted exact revision before a source update can replace it") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto copy_gate = backend->HoldSameDeviceCopies("Presentation source copy");
    auto race = std::make_shared<UpscaleAdmissionRaceProbe>();
    UpscaleSourceFixture subject{backend,
                                 {24U, 15U},
                                 11U,
                                 RuntimeFactory(
                                     0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                                     [race] { return std::make_unique<RacingUpscaleAlgorithm>(race); }, 4U)};
    auto& source = subject.source;
    auto& events = subject.events;
    auto& upscale = subject.upscale;
    const auto admitted_frame = source.frame();
    mmltk::testsupport::ScopedTestCleanup settle_copy{[&] {
        copy_gate->Release();
        upscale.Stop();
        upscale.Shutdown();
    }};

    static_cast<void>(upscale.Start(test_upscale_request({.source = admitted_frame})));
    const bool copy_entered = copy_gate->WaitEntered(2s);
    if (!copy_entered) copy_gate->Release();
    REQUIRE(copy_entered);
    REQUIRE_FALSE(source.TryReserveOutput().valid());
    auto replacement = std::async(std::launch::async, [&source] { source.Publish({24U, 15U}, 29U); });
    mmltk::testsupport::ScopedTestCleanup release_replacement{[&] { copy_gate->Release(); }};
    copy_gate->Release();
    REQUIRE(replacement.wait_for(2s) == std::future_status::ready);
    replacement.get();
    CHECK(source.frame().revision > admitted_frame.revision);
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK(backend->same_copies.load(std::memory_order_acquire) == 2U);
    CHECK(race->received_value.load(std::memory_order_acquire) == 11U);
    CHECK(race->received_width.load(std::memory_order_acquire) == admitted_frame.extent.width);
    CHECK(race->received_height.load(std::memory_order_acquire) == admitted_frame.extent.height);
    auto product = upscale.BorrowFrame();
    REQUIRE(product.valid());
    CHECK(*reinterpret_cast<const std::uint8_t*>(product.plane(0U).plane().data) == 11U);
}

TEST_CASE("Upscale derives a checked fixed output envelope") {
    CHECK(kUpscaleOutputScale == 4U);
    CHECK((checked_upscale_output_extent({1U, 1U}) == VisualExtent{4U, 4U}));
    CHECK((checked_upscale_output_extent({320U, 180U}) == VisualExtent{1280U, 720U}));
    CHECK_THROWS_AS(checked_upscale_output_extent({}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(checked_upscale_output_extent({std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale + 1U, 1U}),
                    contracts::InvalidIntentError);
    CHECK_THROWS_AS(checked_upscale_output_extent({1U, std::numeric_limits<std::uint32_t>::max() / kUpscaleOutputScale + 1U}),
                    contracts::InvalidIntentError);
}

TEST_CASE("Upscale accepts output beyond the source systems base envelope") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {64U, 64U}};
    auto& explore = opened_explore.system();
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    EventGate events;
    const VisualDeviceSettings base_envelope{
        .device = kDevice.device,
        .maximum_width = 64U,
        .maximum_height = 64U,
    };
    const VisualDeviceSettings output_envelope{
        .device = kDevice.device,
        .maximum_width = 256U,
        .maximum_height = 256U,
    };
    CHECK(explore.snapshot().frame.extent.width <= base_envelope.maximum_width);
    // CLEANUP-IGNORE: The base-envelope assertion and later high-water scenario setup are unrelated test operations.
    CHECK(explore.snapshot().frame.extent.height <= base_envelope.maximum_height);
    UpscaleSystem upscale{output_envelope,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              // CLEANUP-IGNORE: Upscale Start and Annotation Open are distinct typed runtime paths.
                              [selected_kernel] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel); }, 4U),
                          borrow_exactly_from(explore), [&events](UpscaleSystem::event_type) { events.Advance(); }};

    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    CHECK((upscale.snapshot().frame.extent == VisualExtent{256U, 256U}));
    REQUIRE(upscale.BorrowFrame().valid());
    CHECK(upscale.BorrowFrame().plane(0U).plane().descriptor.width == 256U);
    CHECK(upscale.BorrowFrame().plane(0U).plane().descriptor.height == 256U);

    const std::array sources{read_from(explore), read_from(upscale)};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    auto allocation_retired = writer_state->allocation_retired.get_future();
    EventGate presentation_events;
    PresentationSystem presentation{
        output_envelope, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
        std::span{sources}, [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(sources[0].source));
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[0].source; }));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{64U, 64U}));
    const auto initial_generation = presentation.snapshot().capability.generation;

    static_cast<void>(presentation.Select(sources[1].source));
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[1].source; }));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{256U, 256U}));
    CHECK(presentation.snapshot().capability.generation > initial_generation);
    CHECK(presentation.snapshot().completed.revision == upscale.snapshot().frame.revision);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == initial_generation);

    writer_state->AcknowledgeAllocationRetirement(initial_generation);
    REQUIRE(allocation_retired.wait_for(2s) == std::future_status::ready);
    allocation_retired.get();
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == 0U);
    const auto pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(initial_generation);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    upscale.Shutdown();
    CHECK(upscale.stopped());
}

TEST_CASE("Upscale and Presentation preserve complete non-square four-times high-water storage") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {80U, 40U}};
    auto& source = opened_explore.system();
    auto extents = std::make_shared<UpscaleExtentProbe>();
    EventGate upscale_events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [extents] { return std::make_unique<ExtentUpscaleAlgorithm>(extents); }, 4U),
                          borrow_exactly_from(source), [&upscale_events](UpscaleSystem::event_type) { upscale_events.Advance(); }};

    static_cast<void>(upscale.Start(test_upscale_request({.source = source.snapshot().frame})));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE((upscale.snapshot().frame.extent == VisualExtent{320U, 160U}));
    REQUIRE(upscale.BorrowFrame().valid());
    const auto large_output = upscale.BorrowFrame().plane(0U).plane();
    CHECK(large_output.descriptor.width == 320U);
    CHECK(large_output.descriptor.height == 160U);
    REQUIRE(extents->sources.size() == 1U);
    REQUIRE(extents->targets.size() == 1U);
    CHECK(extents->sources[0U].descriptor.width == 80U);
    CHECK(extents->sources[0U].descriptor.height == 40U);
    CHECK(extents->targets[0U].descriptor.width == 320U);
    CHECK(extents->targets[0U].descriptor.height == 160U);
    const auto input_address = extents->sources[0U].data;
    const auto output_address = extents->targets[0U].data;

    const std::array presentation_sources{read_from(upscale), read_from(source)};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    EventGate presentation_events;
    PresentationSystem presentation{
        kDevice, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
        presentation_sources, [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    present_and_wait(presentation, *writer_state, presentation_events, presentation_sources[0U], upscale.snapshot().frame.revision);
    CHECK((presentation.snapshot().completed.extent == VisualExtent{320U, 160U}));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{320U, 160U}));
    const auto allocations_at_high_water = backend->planes_allocated.load(std::memory_order_acquire);
    const auto copies_at_high_water = backend->same_copies.load(std::memory_order_acquire);
    CHECK(backend->staged_downloads.load(std::memory_order_acquire) == 0U);
    CHECK(backend->staged_uploads.load(std::memory_order_acquire) == 0U);

    opened_explore.Reopen({32U, 16U});
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().frame.extent == VisualExtent{128U, 64U}; }));
    REQUIRE(extents->sources.size() == 2U);
    REQUIRE(extents->targets.size() == 2U);
    CHECK(extents->sources[1U].data == input_address);
    CHECK(extents->targets[1U].data != output_address);
    CHECK(backend->planes_allocated.load(std::memory_order_acquire) == allocations_at_high_water + 4U);

    present_and_wait(presentation, *writer_state, presentation_events, presentation_sources[0U], upscale.snapshot().frame.revision);
    CHECK((presentation.snapshot().completed.extent == VisualExtent{128U, 64U}));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{320U, 160U}));
    CHECK(backend->planes_allocated.load(std::memory_order_acquire) == allocations_at_high_water + 4U);
    CHECK(backend->same_copies.load(std::memory_order_acquire) == copies_at_high_water + 3U);
    CHECK(backend->staged_downloads.load(std::memory_order_acquire) == 0U);
    CHECK(backend->staged_uploads.load(std::memory_order_acquire) == 0U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    upscale.Shutdown();
    CHECK(upscale.stopped());
    CHECK(backend->contexts_destroyed.load(std::memory_order_acquire) >= 2U);
}

TEST_CASE("Presentation serializes consecutive growth through exact retirement acknowledgements") {
    ProductPresentationSources products;
    const auto sources = products.sources();
    REQUIRE(sources.size() >= 5U);
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    auto third_submission = writer_state->third_submission.get_future();
    EventGate events;
    PresentationSystem presentation{
        kDevice,
        [backend = products.backend(), writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
        sources, [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};

    const auto source_a = sources[0].source;
    const auto source_b = sources[2].source;
    const auto source_c = sources[4].source;
    static_cast<void>(presentation.Select(source_a));
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_a; }));
    const auto generation_a = presentation.snapshot().capability.generation;

    static_cast<void>(presentation.Select(source_b));
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_b; }));
    const auto generation_b = presentation.snapshot().capability.generation;
    REQUIRE(generation_b > generation_a);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);

    static_cast<void>(presentation.Select(source_c));
    writer_state->SignalReadiness();
    REQUIRE(third_submission.wait_for(2s) == std::future_status::ready);
    third_submission.get();
    CHECK(presentation.snapshot().completed.source == source_b);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);

    auto pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_b);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_a);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 0U);

    writer_state->AcknowledgeAllocationRetirement(generation_a);
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source_c; }));
    CHECK(presentation.snapshot().completed.revision == sources[4].observe().frame.revision);
    CHECK(presentation.snapshot().capability.generation > generation_b);
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_b);

    pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_a);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == generation_b);

    pump_count = writer_state->PumpCount();
    writer_state->AcknowledgeAllocationRetirement(generation_b);
    REQUIRE(writer_state->WaitForPumpAfter(pump_count));
    CHECK(writer_state->allocation_retirements.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->retained_allocation_generation.load(std::memory_order_acquire) == 0U);

    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Upscale invalid exact-source admission preserves the active operation snapshot") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();
    auto selected_kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto gate = std::make_shared<MutationCommitProbe>();
    auto entered = gate->committed.get_future();
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [selected_kernel, gate] { return std::make_unique<TestUpscaleAlgorithm>(selected_kernel, gate); }, 4U),
                          borrow_exactly_from(explore), [&events](UpscaleSystem::event_type) { events.Advance(); }};
    auto settle_upscale = settle_upscale_on_exit(upscale, gate->release);
    const auto admitted = upscale.Start(test_upscale_request({
        .source = explore.snapshot().frame,
        .kernel = UpscaleKernel::ShiftLut,
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    entered.get();
    CHECK_FALSE(upscale.BorrowFrame().valid());
    CHECK_THROWS_AS(upscale.Start(test_upscale_request({
                        .source = {},
                        .kernel = UpscaleKernel::RealPlksr,
                    })),
                    contracts::InvalidIntentError);
    const auto after_duplicate = upscale.snapshot();
    CHECK(after_duplicate.revision == admitted.revision);
    CHECK(after_duplicate.busy);
    CHECK(after_duplicate.pending->kernel == UpscaleKernel::ShiftLut);
    gate->release.set_value();
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
}

TEST_CASE("Presentation forwards every private visual product to the simulated browser arena") {
    ProductPresentationSources products;
    const auto sources = products.sources();
    EventGate events;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{
        kDevice,
        [backend = products.backend(), writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
        sources, [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};

    std::uint64_t previous_presentation_revision = 0U;
    for (auto source = sources.rbegin(); source != sources.rend(); ++source) {
        static_cast<void>(presentation.Select(source->source));
        writer_state->SignalReadiness();
        REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source->source; }));
        const auto published_revision = writer_state->presentation_revision.load(std::memory_order_acquire);
        CHECK(published_revision > previous_presentation_revision);
        CHECK(presentation.snapshot().presentation_revision == published_revision);
        previous_presentation_revision = published_revision;
    }
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == sources.size());
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == sources.size());
    CHECK_THROWS_AS(presentation.Select({PresentationSourceKind::Explore, 2U}), contracts::InvalidIntentError);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Source admission writes retain complete packet and frame provenance", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    const bool deferred = GENERATE(false, true);
    enum class Diagnostics { Absent, Disabled, Enabled };
    const auto diagnostics = GENERATE(Diagnostics::Absent, Diagnostics::Disabled, Diagnostics::Enabled);
    CAPTURE(deferred, static_cast<int>(diagnostics));
    mmltk::testsupport::ScopedTempDir root{"presentation-admission-diagnostics"};
    const auto path = root.path() / "import.sock";
    struct Capture final {
        std::array<VisualDiagnosticFact, 2> facts{};
        std::size_t count = 0U;
        bool enabled = false;
    } capture;
    capture.enabled = diagnostics == Diagnostics::Enabled;
    const VisualDiagnosticSink sink{.context = &capture,
                                    .write =
                                        [](void* context, const VisualDiagnosticFact fact) noexcept {
                                            auto& value = *static_cast<Capture*>(context);
                                            if (value.count < value.facts.size()) value.facts[value.count] = fact;
                                            ++value.count;
                                        },
                                    .enabled = [](void* context) noexcept { return static_cast<Capture*>(context)->enabled; }};
    presentation::WorkspaceSurfaceImportChannel channel{path, diagnostics == Diagnostics::Absent ? VisualDiagnosticSink{} : sink};
    auto peer = mmltk::testsupport::connect_workspace_surface_shell(path);
    channel.pump();
    REQUIRE(channel.connected());
    std::size_t queued_fillers = 0U;
    const abi::Record filler{.opcode = abi::Opcode::Drop, .id_high = 101U, .id_low = 102U};
    if (deferred) {
        const int send_bytes = 4096;
        REQUIRE(::setsockopt(channel.poll_fd(), SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) == 0);
        // Fill the real outbound socket before admitting the source. The bounded
        // loop stops on EAGAIN; no sleeps or assumed kernel queue length.
        while (queued_fillers < 256U && send_workspace_record(channel.poll_fd(), filler))
            ++queued_fillers;
        REQUIRE(queued_fillers > 0U);
        REQUIRE(queued_fillers < 256U);
    }
    auto memory = mmltk::testsupport::workspace_surface_event_descriptor();
    auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
    auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
    const presentation::WorkspaceSurfaceImportId id{11U, 12U};
    abi::Record record{.id_high = id.high,
                       .id_low = id.low,
                       .width = 64U,
                       .height = 32U,
                       .stride = 512U,
                       .size = 32768U,
                       .descriptors = abi::kImportDescriptorCount,
                       .arena_high = 1U,
                       .arena_low = 2U,
                       .allocation_identity = 3U,
                       .device_incarnation = 4U,
                       .offset = 256U,
                       .alignment = 256U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    const auto admitted = record;
    REQUIRE(channel.admit_source(record, 7U, std::move(memory), edge.get(), signal.descriptor(), 19U, 23U));
    CHECK(channel.claimable(id) == !deferred);
    CHECK(channel.wants_write() == deferred);
    REQUIRE(capture.count == (capture.enabled ? (deferred ? 1U : 2U) : 0U));
    record = {};
    if (deferred) {
        for (std::size_t index = 0U; index < queued_fillers; ++index) {
            abi::Record received{};
            CHECK(receive_workspace_record(peer.get(), received).descriptor_count == 0U);
            CHECK(received.opcode == filler.opcode);
            CHECK(received.id_high == filler.id_high);
            CHECK(received.id_low == filler.id_low);
        }
        channel.pump();
    }
    REQUIRE(channel.claimable(id));
    CHECK_FALSE(channel.wants_write());
    CHECK_FALSE(channel.terminal_error());
    abi::Record sent{};
    const auto descriptors = receive_workspace_record(peer.get(), sent);
    REQUIRE(descriptors.descriptor_count == abi::kImportDescriptorCount);
    for (std::size_t index = 0U; index < descriptors.descriptor_count; ++index)
        CHECK(::fcntl(descriptors.descriptors[index].get(), F_GETFD) >= 0);
    CHECK(abi::valid(sent));
    // Record is the frozen, padding-free 144-byte wire layout.
    CHECK(std::memcmp(&sent, &admitted, sizeof(sent)) == 0);
    REQUIRE(capture.count == (capture.enabled ? 2U : 0U));
    if (!capture.enabled) return;
    CHECK(capture.facts[0].operation == VisualDiagnosticOperation::PresentationSourceAdmissionEnqueued);
    CHECK(capture.facts[1].operation == VisualDiagnosticOperation::PresentationSourceAdmissionWritten);
    for (const auto& fact : capture.facts) {
        CHECK(fact.context.surface_high == id.high);
        CHECK(fact.context.surface_low == id.low);
        CHECK(fact.generation == 7U);
        CHECK(fact.context.selection_generation == 19U);
        CHECK(fact.context.frame_revision == 23U);
        CHECK(fact.context.condition == static_cast<std::uint64_t>(PresentationCapabilityCondition::Admitted));
        CHECK(fact.context.outcome == 1U);
        CHECK(fact.context.allocation.allocation_generation == 7U);
        CHECK(fact.context.capacity_width == sent.width);
        CHECK(fact.context.capacity_height == sent.height);
        const auto& workspace = fact.context.workspace;
        CHECK(workspace.workspace_source_high == sent.id_high);
        CHECK(workspace.workspace_source_low == sent.id_low);
        CHECK(workspace.workspace_allocation == sent.allocation_identity);
        CHECK(workspace.workspace_arena_high == sent.arena_high);
        CHECK(workspace.workspace_arena_low == sent.arena_low);
        CHECK(workspace.workspace_bytes == sent.size);
        CHECK(workspace.workspace_pitch == sent.stride);
        CHECK(workspace.workspace_width == sent.width);
        CHECK(workspace.workspace_height == sent.height);
    }
}

class WorkspaceChannelFixture final {
    using Record = presentation::detail::workspace_surface_import::Record;
    using Opcode = presentation::detail::workspace_surface_import::Opcode;
    using Id = presentation::WorkspaceSurfaceImportId;
    mmltk::testsupport::ScopedTempDir root_{"workspace-channel"};

   public:
    presentation::WorkspaceSurfaceImportChannel channel{root_.path() / "import.sock"};
    mmltk::common::io::ScopedFd peer = mmltk::testsupport::connect_workspace_surface_shell(root_.path() / "import.sock");

    WorkspaceChannelFixture() {
        channel.pump();
        REQUIRE(channel.connected());
    }

    [[nodiscard]] static Record Layout(const Id id) {
        return {.opcode = Opcode::ArenaReady,
                .id_high = id.high,
                .id_low = id.low,
                .width = 4U,
                .height = 3U,
                .stride = 32U,
                .size = 96U,
                .device_incarnation = 5U,
                .alignment = 32U,
                .device_uuid = {1U},
                .memory_type_bits = 1U};
    }

    [[nodiscard]] static Record Sample(const Id arena) {
        return {.opcode = Opcode::Presented,
                .id_high = arena.high,
                .id_low = arena.low,
                .stride = 7U,
                .size = 8U,
                .code = 1U,
                .presentation_revision = 9U};
    }

    Record AdmitArena(const Id id, const std::uint64_t generation) {
        const auto layout = Layout(id);
        REQUIRE(channel.admit_arena(id, generation, layout.width, layout.height));
        ExpectRecord(id, Opcode::Arena);
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), layout));
        channel.pump();
        const auto outcome = channel.take_outcome();
        REQUIRE(outcome.has_value());
        CHECK(outcome->id == id);
        REQUIRE(outcome->imported);
        return layout;
    }

    void Withdraw(const Id id) {
        REQUIRE(channel.withdraw(id).progress == presentation::WorkspaceSurfaceWithdrawalProgress::Submitted);
        ExpectRecord(id, Opcode::Drop);
    }

    void Retire(const Id id, const std::uint64_t generation) {
        REQUIRE(mmltk::testsupport::send_workspace_record(peer.get(), {.opcode = Opcode::Retired, .id_high = id.high, .id_low = id.low}));
        channel.pump();
        const auto retired = channel.take_retirement();
        REQUIRE(retired.has_value());
        CHECK(retired->id == id);
        CHECK(retired->generation == generation);
    }

   private:
    void ExpectRecord(const Id id, const Opcode opcode) {
        Record received{};
        CHECK(mmltk::testsupport::receive_workspace_record(peer.get(), received).descriptor_count == 0U);
        CHECK(received.opcode == opcode);
        CHECK(received.id_high == id.high);
        CHECK(received.id_low == id.low);
    }
};

TEST_CASE("Retired source admission does not retire its occupied sample arena", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    const presentation::WorkspaceSurfaceImportId source{3U, 4U};
    abi::Record received{};
    const auto layout = fixture.AdmitArena(arena, 1U);
    auto memory = mmltk::testsupport::workspace_surface_event_descriptor();
    auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
    auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
    auto imported = layout;
    imported.id_high = source.high;
    imported.id_low = source.low;
    imported.arena_high = arena.high;
    imported.arena_low = arena.low;
    imported.allocation_identity = 6U;
    REQUIRE(channel.admit_source(imported, 2U, std::move(memory), edge.get(), signal.descriptor()));
    CHECK(receive_workspace_record(peer.get(), received).descriptor_count == abi::kImportDescriptorCount);
    auto timeline = mmltk::testsupport::workspace_surface_event_descriptor();
    const std::array descriptors{timeline.get()};
    REQUIRE(send_workspace_record(
        peer.get(), {.opcode = abi::Opcode::Ready, .id_high = source.high, .id_low = source.low, .descriptors = abi::kReadyDescriptorCount},
        descriptors));
    channel.pump();
    const auto source_outcome = channel.take_outcome();
    REQUIRE(source_outcome.has_value());
    REQUIRE(source_outcome->imported);
    // A capacity retry may repeat content and presentation while representing
    // a new physical source transfer. The wire must preserve that distinction.
    for (const auto transfer : {1U, 2U}) {
        REQUIRE(channel.copy_completed(source, {7U, 8U}, 9U, transfer));
        static_cast<void>(receive_workspace_record(peer.get(), received));
        CHECK(received.opcode == abi::Opcode::CopyCompleted);
        CHECK(received.offset == transfer);
        CHECK(received.stride == 7U);
        CHECK(received.size == 8U);
        CHECK(received.presentation_revision == 9U);
        CHECK(abi::valid(received));
        auto malformed = received;
        malformed.offset = 0U;
        CHECK_FALSE(abi::valid(malformed));
        malformed = received;
        malformed.opcode = abi::Opcode::Presented;
        malformed.code = 1U;
        CHECK_FALSE(abi::valid(malformed));
    }
    CHECK_FALSE(channel.copy_completed(source, {7U, 8U}, 9U, 0U));
    auto sample = WorkspaceChannelFixture::Sample(arena);
    REQUIRE(send_workspace_record(peer.get(), sample));
    channel.pump();
    fixture.Withdraw(source);
    fixture.Retire(source, 2U);
    CHECK(channel.claimable(arena));
    CHECK_FALSE(channel.terminal_error());
    fixture.Withdraw(arena);
    sample.opcode = abi::Opcode::Completed;
    REQUIRE(send_workspace_record(peer.get(), sample));
    fixture.Retire(arena, 1U);
    CHECK_FALSE(channel.terminal_error());
}

TEST_CASE("Arena capacity tickets remain exact until consumed or withdrawn", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    const presentation::WorkspaceSurfaceImportId arena{1U, 2U};
    fixture.AdmitArena(arena, arena.high);
    auto sample = WorkspaceChannelFixture::Sample(arena);
    REQUIRE(send_workspace_record(peer.get(), sample));
    sample.opcode = abi::Opcode::Completed;
    REQUIRE(send_workspace_record(peer.get(), sample));
    auto available = sample;
    available.opcode = abi::Opcode::Available;
    REQUIRE(send_workspace_record(peer.get(), available));
    channel.pump();

    SECTION("one original availability survives unrelated outcomes and retirement") {
        // Model an ineligible consumer by leaving the ticket untouched. This
        // proves channel retention, not the native writer's GPU interleaving.
        for (std::uint64_t iteration = 2U; iteration <= 301U; ++iteration) {
            const presentation::WorkspaceSurfaceImportId unrelated{iteration, 3U};
            fixture.AdmitArena(unrelated, unrelated.high);
            fixture.Withdraw(unrelated);
            fixture.Retire(unrelated, unrelated.high);
            channel.pump();
            REQUIRE_FALSE(channel.terminal_error());
        }
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
    }
    SECTION("repeated availability coalesces into one exact ticket") {
        for (std::uint64_t iteration = 0U; iteration < 300U; ++iteration) {
            REQUIRE(send_workspace_record(peer.get(), available));
            channel.pump();
            REQUIRE_FALSE(channel.terminal_error());
        }
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
    }
    SECTION("withdrawal clears its own ticket and ignores queued stale availability") {
        fixture.Withdraw(arena);
        REQUIRE(send_workspace_record(peer.get(), available));
        fixture.Retire(arena, arena.high);
    }
    SECTION("a replacement outcome cannot relabel a stale arena ticket") {
        const presentation::WorkspaceSurfaceImportId replacement{2U, 3U};
        fixture.AdmitArena(replacement, replacement.high);
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == arena);
        CHECK(*ticket != replacement);
    }
    SECTION("old arena withdrawal cannot erase the replacement ticket") {
        const presentation::WorkspaceSurfaceImportId replacement{2U, 3U};
        fixture.AdmitArena(replacement, replacement.high);
        auto replacement_available = available;
        replacement_available.id_high = replacement.high;
        replacement_available.id_low = replacement.low;
        REQUIRE(send_workspace_record(peer.get(), replacement_available));
        channel.pump();
        fixture.Withdraw(arena);
        REQUIRE(send_workspace_record(peer.get(), available));
        fixture.Retire(arena, arena.high);
        const auto ticket = channel.take_capacity_wake();
        REQUIRE(ticket.has_value());
        CHECK(*ticket == replacement);
    }
    CHECK_FALSE(channel.take_capacity_wake().has_value());
    CHECK_FALSE(channel.take_outcome().has_value());
    CHECK_FALSE(channel.take_retirement().has_value());
    CHECK_FALSE(channel.terminal_error());
    CHECK(channel.connected());
}

TEST_CASE("Workspace capability ledgers survive sequential retirement beyond concurrent capacity", "[workspace][protocol]") {
    namespace abi = presentation::detail::workspace_surface_import;
    using mmltk::testsupport::receive_workspace_record;
    using mmltk::testsupport::send_workspace_record;
    WorkspaceChannelFixture fixture;
    auto& channel = fixture.channel;
    auto& peer = fixture.peer;
    auto edge = mmltk::testsupport::workspace_surface_event_descriptor();
    auto signal = presentation::WorkspaceSurfaceFrameSignal::create();
    auto timeline = mmltk::testsupport::workspace_surface_event_descriptor();
    const std::array descriptors{timeline.get()};
    for (std::uint64_t iteration = 1U; iteration <= 300U; ++iteration) {
        for (const bool source : {false, true}) {
            for (const bool ready : {false, true}) {
                const presentation::WorkspaceSurfaceImportId id{iteration, 1U + 2U * source + ready};
                auto layout = WorkspaceChannelFixture::Layout(id);
                if (source) {
                    layout.arena_high = iteration;
                    layout.arena_low = 9U;
                    layout.allocation_identity = iteration;
                    REQUIRE(channel.admit_source(layout, iteration, mmltk::testsupport::workspace_surface_event_descriptor(), edge.get(),
                                                 signal.descriptor()));
                } else {
                    REQUIRE(channel.admit_arena(id, iteration, 4U, 3U));
                }
                abi::Record received{};
                static_cast<void>(receive_workspace_record(peer.get(), received));
                if (ready) {
                    if (source) {
                        REQUIRE(send_workspace_record(
                            peer.get(),
                            {.opcode = abi::Opcode::Ready, .id_high = id.high, .id_low = id.low, .descriptors = abi::kReadyDescriptorCount},
                            descriptors));
                    } else {
                        REQUIRE(send_workspace_record(peer.get(), layout));
                    }
                    channel.pump();
                    REQUIRE(channel.take_outcome().has_value());
                }
                fixture.Withdraw(id);
                REQUIRE(
                    send_workspace_record(peer.get(), {.opcode = ready ? abi::Opcode::Retired : abi::Opcode::Failed,
                                                       .id_high = id.high,
                                                       .id_low = id.low,
                                                       .code = ready ? 0U : static_cast<std::uint32_t>(abi::FailureCode::NotAdmitted)}));
                channel.pump();
                REQUIRE(channel.take_retirement().has_value());
                CHECK_FALSE(channel.claimable(id));
                CHECK(channel.withdraw(id).progress == presentation::WorkspaceSurfaceWithdrawalProgress::Invalid);
                CHECK_FALSE(channel.take_outcome().has_value());
                CHECK_FALSE(channel.terminal_error());
                CHECK(channel.connected());
            }
        }
    }
    // A still-live admission remains unique even after all the preceding churn.
    const presentation::WorkspaceSurfaceImportId active{301U, 1U};
    REQUIRE(channel.admit_arena(active, 301U, 4U, 3U));
    CHECK_FALSE(channel.admit_arena(active, 301U, 4U, 3U));
    CHECK(channel.terminal_error().has_value());
}

void publish_first_presentation(PresentationSystem& presentation, const PresentationSourceFixture& source,
                                const TestPresentationWriterState& writer, EventGate& events) {
    static_cast<void>(presentation.Select(source.identity()));
    writer.SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    CHECK(presentation.snapshot().completed_source_revision == 1U);
}

TEST_CASE("Published frames retain their physical allocation while a waiting candidate is advertised") {
    PresentationSourceFixture source;
    EventGate events;
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->advertise_waiting_candidate.store(true);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer};
    publish_first_presentation(presentation, source, *writer, events);
    const auto completed = presentation.snapshot();
    CHECK(completed.capability.surface_low == 1U);
    CHECK(completed.capability.generation == 1U);
    CHECK(completed.capability.condition == PresentationCapabilityCondition::Ready);
    writer->allow_publication.store(false);
    source.Advance();
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.surface_low == 101U; }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted);
    CHECK(presentation.snapshot().completed == completed.completed);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Presentation diagnostics join admitted and completed snapshots to one surface identity") {
    PresentationSourceFixture source;
    EventGate events;
    struct Capture final {
        std::mutex mutex;
        std::vector<VisualDiagnosticFact> facts;
    } capture;
    capture.facts.reserve(16U);
    const VisualDiagnosticSink diagnostics{.context = &capture, .write = [](void* context, const VisualDiagnosticFact fact) noexcept {
                                               if (fact.operation != VisualDiagnosticOperation::PresentationCapabilityPublished &&
                                                   fact.operation != VisualDiagnosticOperation::TimelineReady)
                                                   return;
                                               auto& result = *static_cast<Capture*>(context);
                                               std::scoped_lock lock(result.mutex);
                                               result.facts.push_back(fact);
                                           }};
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->allow_publication.store(false, std::memory_order_release);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }, diagnostics};
    PresentationScenario scenario{presentation, writer};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    const auto admitted = presentation.snapshot().capability;
    writer->allow_publication.store(true, std::memory_order_release);
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    const auto completed = presentation.snapshot();
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    REQUIRE_FALSE(capture.facts.empty());
    for (const auto& fact : capture.facts) {
        CHECK(fact.context.surface_high == admitted.surface_high);
        CHECK(fact.context.surface_low == admitted.surface_low);
        CHECK(fact.generation == admitted.generation);
        CHECK(fact.context.selection_generation != 0U);
        CHECK(fact.context.frame_revision == completed.completed.revision);
    }
}

TEST_CASE("Presentation directly refreshes a selected private product") {
    PresentationSourceFixture source;
    EventGate events;
    std::atomic<std::uint64_t> completed_events{0U};
    std::atomic<std::uint64_t> last_completed_revision{0U};
    std::atomic<std::uint64_t> last_completed_product{0U};
    std::atomic<std::uint64_t> last_completed_sample{0U};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources(),
                                    [&events, &completed_events, &last_completed_revision, &last_completed_sample,
                                     &last_completed_product](PresentationSystem::event_type event) {
                                        if (const auto* completed = std::get_if<PresentationCompleted>(&event)) {
                                            last_completed_revision.store(completed->snapshot.revision, std::memory_order_release);
                                            last_completed_sample.store(completed->snapshot.browser_completed_sample,
                                                                        std::memory_order_release);
                                            completed_events.fetch_add(1U, std::memory_order_acq_rel);
                                            last_completed_product.store(completed->snapshot.completed.revision, std::memory_order_release);
                                        }
                                        events.Advance();
                                    }};
    PresentationScenario scenario{presentation, writer_state};

    publish_first_presentation(presentation, source, *writer_state, events);
    REQUIRE(events.Wait([&] { return last_completed_product.load(std::memory_order_acquire) == 1U; }));

    const auto before_acknowledgement = presentation.snapshot();
    const auto before_acknowledgement_events = completed_events.load(std::memory_order_acquire);
    presentation.Observe({
        .completed_sample = 7U,
        .redraw_requested = false,
    });
    const auto acknowledged = presentation.snapshot();
    CHECK(acknowledged.revision > before_acknowledgement.revision);
    CHECK(acknowledged.browser_completed_sample == 7U);
    CHECK(completed_events.load(std::memory_order_acquire) == before_acknowledgement_events + 1U);
    CHECK(last_completed_revision.load(std::memory_order_acquire) == acknowledged.revision);
    CHECK(last_completed_sample.load(std::memory_order_acquire) == 7U);

    presentation.Observe({
        .completed_sample = 7U,
        .redraw_requested = false,
    });
    presentation.Observe({
        .completed_sample = 6U,
        .redraw_requested = false,
    });
    CHECK(presentation.snapshot().revision == acknowledged.revision);
    CHECK(presentation.snapshot().browser_completed_sample == 7U);
    CHECK(completed_events.load(std::memory_order_acquire) == before_acknowledgement_events + 1U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 1U);

    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 2U; }));
    CHECK(presentation.snapshot().completed_source_revision == 2U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 2U);
    CHECK(presentation.snapshot().browser_completed_sample == 7U);
    REQUIRE(events.Wait([&] { return last_completed_product.load(std::memory_order_acquire) == 2U; }));

    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 4U; }));
    CHECK(presentation.snapshot().completed_source_revision == 4U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 4U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 3U);

    const auto paused_step = writer_state->ArmWaiting();
    presentation.SetApplicationPeerConnected(false);
    source.Advance();
    presentation.SourceChanged(source.identity());
    writer_state->SignalReadiness();
    REQUIRE(writer_state->WaitDisconnected(paused_step, false, 4U));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 4U);
    presentation.SetApplicationPeerConnected(true);
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 5U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 5U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 4U);

    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    presentation.SetApplicationPeerConnected(false);
    writer_state->allow_publication.store(true, std::memory_order_release);
    const auto disconnected_step = writer_state->ArmWaiting();
    writer_state->SignalReadiness();
    REQUIRE(writer_state->WaitDisconnected(disconnected_step, true, 6U));
    CHECK(presentation.snapshot().completed.revision == 5U);
    presentation.SetApplicationPeerConnected(true);
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 6U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 6U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 5U);

    // A producer can retire its old storage before newer metadata arrives.
    // The stale advertised frame must not resubmit itself on every pump.
    source.PublishUnobserved();
    const auto before_rejected_borrow = writer_state->PumpCount();
    presentation.Observe({.redraw_requested = true});
    REQUIRE(writer_state->WaitForPumpAfter(before_rejected_borrow));
    const auto rejected_borrow = writer_state->PumpCount();
    writer_state->SignalReadiness();
    REQUIRE(writer_state->WaitForPumpAfter(rejected_borrow));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 7U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 5U);
    CHECK(presentation.snapshot().completed.revision == 6U);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 8U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 8U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 6U);

    // If the newer observation is already available at rejection, catch up
    // immediately even before its separate source notification is delivered.
    writer_state->allow_publication.store(false, std::memory_order_release);
    source.Advance();
    presentation.SourceChanged(source.identity());
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    source.Advance();
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 10U; }));
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 10U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == 7U);

    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Presentation carries its submitted observation through metadata changes and reselection") {
    PresentationSourceFixture source;
    EventGate events;
    auto writer = std::make_shared<TestPresentationWriterState>();
    writer->allow_publication.store(false);
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(events.Wait([&] { return presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted; }));
    source.AdvanceObservation();
    writer->allow_publication.store(true);
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == 1U; }));
    const auto completed = presentation.snapshot();
    CHECK(completed.completed_source_revision == 1U);
    CHECK(source.sources().front().observe().snapshot_revision == 2U);
    static_cast<void>(presentation.Select(source.identity()));
    writer->SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().presentation_revision > completed.presentation_revision; }));
    CHECK(presentation.snapshot().completed == completed.completed);
    CHECK(presentation.snapshot().completed_source_revision == 2U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Presentation diagnostics retain exact observations across supersession cache reuse reconnect and reconstruction") {
    PresentationSourceFixture source{2U};
    EventGate events;
    struct Capture final {
        std::array<VisualDiagnosticFact, 6U> completed{};
        std::size_t count = 0U;
    } capture;
    const VisualDiagnosticSink diagnostics{
        .context = &capture, .write = [](void* context, VisualDiagnosticFact fact) noexcept {
            auto& output = *static_cast<Capture*>(context);
            if (fact.operation == VisualDiagnosticOperation::TimelineReady && output.count < output.completed.size())
                output.completed[output.count++] = fact;
        }};
    auto writer = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer), source.sources(),
                                    [&events](PresentationSystem::event_type) { events.Advance(); }, diagnostics};
    PresentationScenario scenario{presentation, writer};
    const auto await_publication = [&](std::uint64_t revision) {
        writer->SignalReadiness();
        REQUIRE(events.Wait([&] { return presentation.snapshot().presentation_revision == revision; }));
    };
    auto first = source.Completed();
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(1U);
    writer->allow_publication.store(false);
    source.Advance();
    auto second = source.Completed();
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(writer->second_submission, "writer->second_submission", 2s));
    source.SelectCompleted(first);
    static_cast<void>(presentation.Select(source.identity()));
    writer->allow_publication.store(true);
    await_publication(2U);
    source.SelectCompleted(second);
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(3U);
    source.SelectCompleted(first);
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(4U);
    presentation.Observe({.redraw_requested = true});  // Reconnected renderer requests a new physical receipt.
    await_publication(5U);
    first = {};
    second = {};
    source.Reconstruct();
    static_cast<void>(presentation.Select(source.identity()));
    await_publication(6U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    REQUIRE(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    REQUIRE(capture.count == 6U);
    constexpr std::array products{1U, 1U, 2U, 1U, 1U, 3U};
    constexpr std::array observations{1U, 3U, 4U, 5U, 5U, 6U};
    for (std::size_t index = 0U; index < capture.count; ++index) {
        const auto& fact = capture.completed[index];
        CHECK(fact.context.frame_revision == products[index]);
        CHECK(fact.context.source.source_revision == products[index]);
        CHECK(fact.context.source.source_observation_revision == observations[index]);
        CHECK(fact.context.source.clean_revision == products[index]);
        CHECK(fact.context.publication.presentation_revision == index + 1U);
        CHECK(fact.context.transfer.transfer_sequence == index + 1U);
        CHECK(fact.context.transfer.timeline_ready == fact.value);
        CHECK(fact.context.allocation.allocation_generation == 1U);
        CHECK(fact.context.surface_high == capture.completed[0U].context.surface_high);
        CHECK(fact.context.surface_low == capture.completed[0U].context.surface_low);
    }
    CHECK(capture.completed[4U].context.selection_generation == capture.completed[3U].context.selection_generation);
}

TEST_CASE("Presentation operation projection never combines a new borrow with an incumbent publication") {
    const PresentationCapability allocation{.surface_high = 10U,
                                            .surface_low = 11U,
                                            .extent = {32U, 32U},
                                            .generation = 7U,
                                            .condition = PresentationCapabilityCondition::Ready};
    const PresentationSubmittedSource first{
        {.frame = {.source = {PresentationSourceKind::Explore, 1U}, .extent = {16U, 16U}, .revision = 9U, .clean_revision = 5U},
         .snapshot_revision = 20U},
        30U};
    const PresentationSubmittedSource second{
        {.frame = {.source = {PresentationSourceKind::Explore, 1U}, .extent = {16U, 16U}, .revision = 12U, .clean_revision = 8U},
         .snapshot_revision = 21U},
        31U};
    const PresentationDiagnosticRecord incumbent{
        first, {.capability = allocation, .timeline_ready = 17U, .presentation_revision = 40U, .transfer_sequence = 9U}};
    const auto released = presentation_diagnostic_fact(VisualDiagnosticOperation::PresentationReleaseWaitCompleted, incumbent, 0, 1U);
    CHECK(released.context.selection_generation == first.selection_generation);
    CHECK(released.context.frame_revision == released.context.source.source_revision);
    CHECK(released.context.source.source_revision == 9U);
    CHECK(released.value == released.context.publication.presentation_revision);
    CHECK(released.value == 40U);
    CHECK(released.context.transfer.transfer_sequence == 9U);
    const auto borrowed =
        presentation_diagnostic_fact(VisualDiagnosticOperation::PresentationSourceBorrowStarted, {second, {.capability = allocation}}, 0);
    CHECK(borrowed.context.selection_generation == second.selection_generation);
    CHECK(borrowed.context.frame_revision == borrowed.context.source.source_revision);
    CHECK(borrowed.context.source.source_revision == 12U);
    CHECK(borrowed.value == 0U);
    CHECK(borrowed.context.publication.presentation_revision == 0U);
    CHECK(borrowed.context.transfer.transfer_sequence == 0U);
    CHECK(borrowed.context.transfer.timeline_ready == 0U);
}

TEST_CASE("Presentation switches sources while Live advances in background") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {40U, 20U}};
    auto& explore = opened_explore.system();

    EventGate live_events;
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto token_changed = std::make_shared<std::atomic_bool>(false);
    LiveSystem live{kDevice, test_live_runtime_factory(backend, captures, token_changed),
                    [&live_events](LiveSystem::event_type) { live_events.Advance(); }};
    CHECK_FALSE(live.BorrowFrame().valid());
    const auto admitted_live = live.Start({.extent = {80U, 45U}, .frames_per_second = 120U});
    CHECK(admitted_live.revision != 0U);
    CHECK_THROWS_AS(live.Start({.extent = {80U, 45U}, .frames_per_second = 120U}), contracts::BusyError);
    REQUIRE(live_events.Wait([&] { return live.snapshot().completed_frames >= 2U; }));
    REQUIRE(live.BorrowFrame().valid());
    CHECK(live.snapshot().revision > admitted_live.revision);
    const auto before_switch = live.snapshot().completed_frames;

    const std::array sources{read_from(explore), read_from(live)};
    EventGate presentation_events;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->allow_publication.store(false, std::memory_order_release);
    auto first_submission = writer_state->first_submission.get_future();
    PresentationSystem presentation{
        kDevice, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); }, std::span{sources},
        [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    CHECK(writer_state->constructions.load(std::memory_order_acquire) == 1U);
    const auto first_selection = presentation.Select(sources[0].source);
    CHECK(first_selection.revision != 0U);
    REQUIRE(first_submission.wait_for(2s) == std::future_status::ready);
    first_submission.get();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().capability.valid(); }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted);
    const auto second_selection = presentation.Select(sources[1].source);
    CHECK(second_selection.revision > first_selection.revision);
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[1].source; }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Ready);
    REQUIRE(live_events.Wait([&] { return live.snapshot().completed_frames > before_switch; }));
    CHECK(presentation.snapshot().capability.extent.width == 80U);
    CHECK(presentation.snapshot().capability.extent.height == 45U);
    CHECK(presentation.snapshot().capability.generation != 0U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == presentation.snapshot().timeline_ready);
    CHECK(presentation.Shutdown() == PresentationShutdownResult::BrowserTerminalRequired);
    CHECK_FALSE(presentation.stopped());
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
    const auto stopping_live = live.Stop();
    CHECK(stopping_live.running);
    CHECK(stopping_live.cancellation_requested);
    const auto repeated_stop = live.Stop();
    CHECK(repeated_stop.revision >= stopping_live.revision);
    CHECK(repeated_stop.completed_frames >= stopping_live.completed_frames);
    if (repeated_stop.running) {
        CHECK(repeated_stop.revision == stopping_live.revision);
        CHECK(repeated_stop.cancellation_requested);
    } else {
        CHECK(repeated_stop.revision > stopping_live.revision);
        CHECK_FALSE(repeated_stop.cancellation_requested);
    }
    REQUIRE(live_events.Wait([&] { return !live.snapshot().running; }));
    REQUIRE(live.BorrowFrame().valid());
    CHECK_FALSE(live.snapshot().cancellation_requested);
    CHECK(live.snapshot().revision > stopping_live.revision);
    CHECK_FALSE(token_changed->load(std::memory_order_acquire));
}

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
    owner.SubmitLatest(
        [&latest_entered, &active_cancelled, release](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token stop) mutable {
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
        [&rejected](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&rejected] { rejected.set_value(); }}; }, {},
        true));
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
    detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [](std::exception_ptr) {},
                                     [&](detail::VisualRuntimeOwner::ActivityStage stage, std::uint64_t completed) noexcept {
                                         if (!producer_claims_completion &&
                                             stage == detail::VisualRuntimeOwner::ActivityStage::StagedCompletionLatched &&
                                             completed != 0U) {
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
            if (producer_claims_completion && !owner.TryCompleteActiveWork())
                throw std::runtime_error("producer completion unexpectedly cancelled");
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
    REQUIRE(
        owner.SubmitOrdered([&](auto&, std::stop_token) { return detail::VisualRuntimeOwner::Notification{[&] { next.set_value(); }}; }));
    REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(next, "next", 2s));
    owner.StopAndWait();
}

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
    CHECK_FALSE(entered.load());
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
        for (unsigned index = 0U; index != 2U; ++index) {
            auto temporary = runtime.Completed();
        }
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
            return detail::VisualRuntimeOwner::Notification{
                [&, observed_stop, completed] { cancelled.set_value(observed_stop && !completed); }};
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
    REQUIRE(
        failing.SubmitOrdered([](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
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
    RetainedConstructionModel(std::shared_ptr<std::atomic_bool> destroyed, std::shared_ptr<std::atomic_uint64_t> release_calls,
                              std::exception_ptr failure = {}, bool all_released = false)
        : destroyed_(std::move(destroyed)),
          release_calls_(std::move(release_calls)),
          failure_(failure ? std::move(failure)
                           : std::make_exception_ptr(std::runtime_error("deterministic retained construction resource"))),
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
    for (const auto& expected : {initiating, settlement, retirement})
        CHECK(mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, expected));
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
                    const bool identities = mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, construction_failure) &&
                                            (expected_release_calls == 0U ||
                                             mmltk::frameworks::gpu::test_support::ContainsImageFailure(failure, release_failure));
                    reported_typed_failure.store(std::string_view{error.what()} == expected_failure && identities,
                                                 std::memory_order_release);
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
    detail::VisualRuntimeOwner owner{
        [&](auto revisions) {
            constructions.fetch_add(1U, std::memory_order_acq_rel);
            return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                .device = 0, .backend = backend, .product_revisions = std::move(revisions)});
        },
        [&failed](std::exception_ptr) { failed.set_value(); }};
    REQUIRE(owner.SubmitOrdered(
        [backend](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
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

TEST_CASE("Live failure publishes its newer settled snapshot") {
    auto backend = std::make_shared<FakeImageBackend>();
    std::promise<LiveFailed> failure;
    LiveSystem live{kDevice,
                    RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean,
                                   [] { return std::make_unique<FailingLiveAlgorithm>(); }),
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

TEST_CASE("Upscale method failure preserves healthy resident products and permits isolated retry") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {32U, 32U}};
    auto& explore = opened_explore.system();
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    std::promise<UpscaleFailed> failure;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [runs] { return std::make_unique<FailingUpscaleAlgorithm>(runs); }, 4U),
                          borrow_exactly_from(explore), [&failure, &events](UpscaleSystem::event_type event) {
                              events.Advance();
                              if (auto* failed = std::get_if<UpscaleFailed>(&event)) failure.set_value(std::move(*failed));
                          }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE(upscale.BorrowFrame().valid());
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    auto failed_event = failure.get_future();
    REQUIRE(failed_event.wait_for(2s) == std::future_status::ready);
    const auto failed = failed_event.get();
    CHECK(failed.snapshot.ready);
    CHECK(failed.snapshot.frame.valid());
    CHECK(upscale.BorrowFrame().valid());
    CHECK(failed.snapshot.methods[0U].available);
    CHECK(failed.snapshot.methods[1U].failed);
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    REQUIRE(events.Wait([&] { return !upscale.snapshot().busy && upscale.snapshot().kernel == UpscaleKernel::ShiftLut; }));
    CHECK(runs->load(std::memory_order_acquire) == 3U);
    const auto recovered = upscale.BorrowDocument(upscale.snapshot().frame);
    REQUIRE(recovered.valid());
    CHECK(upscale.snapshot().input == explore.snapshot().frame);
    CHECK(*reinterpret_cast<const std::uint8_t*>(recovered.pixels.plane(0U).plane().data) == 1U);
}

TEST_CASE("Shared Upscale execution failure invalidates every resident product and preserves typed failure authority") {
    auto backend = std::make_shared<FakeImageBackend>();
    MutableVisualSource source{backend, {16U, 8U}};
    auto runs = std::make_shared<std::atomic_uint32_t>(0U);
    std::promise<UpscaleFailed> failed;
    EventGate events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [runs] { return std::make_unique<FailingUpscaleAlgorithm>(runs, true); }, 4U),
                          [&source](const VisualFrame& frame) { return source.BorrowExact(frame); },
                          [&events, &failed](UpscaleSystem::event_type event) {
                              if (auto* failure = std::get_if<UpscaleFailed>(&event)) failed.set_value(*failure);
                              events.Advance();
                          }};
    static_cast<void>(upscale.Start(test_upscale_request({source.frame()})));
    REQUIRE(events.Wait([&] { return upscale.snapshot().ready; }));
    const auto request = test_upscale_request({source.frame(), UpscaleKernel::ShiftLut});
    static_cast<void>(upscale.Start(request));
    auto failure = failed.get_future();
    REQUIRE(failure.wait_for(2s) == std::future_status::ready);
    const auto result = failure.get();
    CHECK(result.kind == UpscaleFailureKind::Physical);
    CHECK(result.request == request);
    CHECK_FALSE(result.snapshot.ready);
    CHECK_FALSE(upscale.BorrowFrame().valid());
    for (const auto& method : result.snapshot.methods)
        CHECK_FALSE(method.available);
    CHECK(mmltk::frameworks::gpu::CudaError(cudaErrorIllegalAddress, "shared").shared_failure());
    CHECK_FALSE(mmltk::frameworks::gpu::CudaError(cudaErrorMemoryAllocation, "local").shared_failure());
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
        for (std::size_t index = 0U; index < malformed.size(); ++index)
            expected += "\xef\xbf\xbd";
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

TEST_CASE("lifecycle diagnostics preserve Explore rendering work and never retain its GPU owner") {
    struct Result final {
        std::array<std::vector<std::uint8_t>, 2U> pixels;
        std::size_t renders = 0U;
        std::size_t allocations = 0U;
        std::size_t copies = 0U;
        bool operator==(const Result&) const = default;
    };
    const auto run = [](const bool enabled) {
        mmltk::testsupport::ScopedTempDir temporary{"mmltk-explore-lifecycle"};
        services::DiagnosticsClient diagnostics;
        if (enabled) diagnostics = services::DiagnosticsClient{temporary.path() / "trace.jsonl"};
        services::RuntimeDiagnostics runtime{diagnostics.producer()};
        auto target = runtime.target();
        CHECK_FALSE(target.pixel_probes_enabled());
        const auto sink = visual_diagnostic_sink(target);
        LoadedSettings settings;
        auto backend = std::make_shared<FakeImageBackend>();
        auto work = std::make_shared<ExploreWorkProbe>();
        EventGate events;
        Result result;
        {
            // CLEANUP-IGNORE: This scoped system measures diagnostic enablement without changing pixels or
            // allocations; the settings-guard fixture owns different work probes and failure expectations.
            ExploreSystem explore{settings.system(),
                                  kDevice,
                                  2U,
                                  RuntimeFactory(
                                      0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                                      [work] {
                                          return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U),
                                                                                        nullptr, nullptr, nullptr, work);
                                      },
                                      2U),
                                  [&events](ExploreSystem::event_type) { events.Advance(); },
                                  sink};
            static_cast<void>(explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"}));
            REQUIRE(events.Wait([&] { return explore.snapshot().ready; }));
            {
                const auto product = explore.BorrowFrame();
                REQUIRE(product.valid());
                for (std::size_t index = 0U; index < result.pixels.size(); ++index) {
                    const auto plane = product.plane(index).plane();
                    const auto row_bytes = plane.descriptor.row_bytes();
                    result.pixels[index].resize(row_bytes * plane.descriptor.height);
                    for (std::uint32_t row = 0U; row < plane.descriptor.height; ++row)
                        std::memcpy(result.pixels[index].data() + row * row_bytes,
                                    reinterpret_cast<const std::uint8_t*>(plane.data) + row * plane.descriptor.pitch_bytes, row_bytes);
                }
            }
            diagnostics.close(services::DiagnosticsCloseMode::Discard);
            CHECK_FALSE(target.valid());
            explore.Shutdown();
            result.renders = work->renders.load();
            result.allocations = backend->planes_allocated.load();
            result.copies = backend->same_copies.load() + backend->peer_copies.load() + backend->staged_downloads.load() +
                            backend->staged_uploads.load();
        }
        CHECK(backend->contexts_created == backend->contexts_destroyed);
        CHECK(backend->planes_allocated == backend->planes_freed);
        return result;
    };
    const auto disabled = run(false);
    const auto enabled = run(true);
    CHECK(disabled == enabled);
}

TEST_CASE("Presentation control remains available during a native wait") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->block_pump.store(true, std::memory_order_release);
    auto pump_entered = writer_state->pump_entered.get_future();
    PresentationSystem presentation{kDevice, TestPresentationWriter::Factory(source.backend(), writer_state), source.sources()};
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(pump_entered.wait_for(2s) == std::future_status::ready);
    pump_entered.get();

    auto admission_closed = std::async(std::launch::async, [&presentation] { presentation.CloseAdmission(); });
    mmltk::testsupport::ScopedTestCleanup release_wait{[&] {
        try {
            writer_state->release_pump.set_value();
        } catch (...) {}
    }};
    CHECK(admission_closed.wait_for(2s) == std::future_status::ready);
    writer_state->release_pump.set_value();
    mmltk::testsupport::await_test_future(admission_closed, "released Presentation admission close");
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}

TEST_CASE("Presentation release failure publishes once and still stops") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->terminal_release_succeeds.store(false, std::memory_order_release);
    PresentationFailureProbe failures;
    auto presentation = make_failure_presentation(source, writer_state, failures);
    PresentationScenario scenario{presentation, writer_state};
    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(failures.events().Wait([&] { return presentation.snapshot().completed.valid(); }));

    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);

    CHECK(presentation.stopped());
    CHECK(failures.failures() == 1U);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
}

TEST_CASE("Presentation retains an unprovable native source read through terminal shutdown") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->terminal_release_succeeds.store(false, std::memory_order_release);
    writer_state->terminal_source_settled.store(false, std::memory_order_release);
    PresentationFailureProbe failures;
    {
        auto presentation = make_failure_presentation(source, writer_state, failures);
        PresentationScenario scenario{presentation, writer_state};
        presentation.CloseAdmission();
        presentation.BrowserPeerLost();
        CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
        CHECK(failures.failures() == 1U);
        CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
        CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    }
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
}

TEST_CASE("Presentation worker failure rejects later direct selection") {
    PresentationSourceFixture source;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->fail_pump.store(true, std::memory_order_release);
    PresentationFailureProbe failures;
    auto presentation = make_failure_presentation(source, writer_state, failures);
    PresentationScenario scenario{presentation, writer_state};

    static_cast<void>(presentation.Select(source.identity()));
    REQUIRE(failures.events().Wait([&] { return failures.failures() == 1U; }));
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 0U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    CHECK_THROWS_AS(presentation.Select(source.identity()), contracts::FailedError);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
}

}  // namespace
}  // namespace mmltk::controller

namespace mmltk::controller {
namespace {
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
    detail::VisualRuntimeOwner owner{
        [&](auto revisions) {
            construction_affinities.push_back(allowed_cpu_set());
            return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                .device = 0, .backend = backend, .execution = execution, .product_revisions = std::move(revisions)});
        },
        [&](std::exception_ptr) { failed.set_value(); }};
    REQUIRE(owner.SubmitOrdered([](auto&, std::stop_token) -> detail::VisualRuntimeOwner::Notification {
        throw std::runtime_error("recoverable operation failure");
    }));
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

TEST_CASE("saved Explore transport changes stage the prior runtime while reopening") {
    auto backend = std::make_shared<FakeImageBackend>();
    ExploreSystem* active = nullptr;
    LoadedSettings settings{[&](SettingsSystem::event_type) {
        if (active) active->ExecutionSettingsChanged();
    }};
    const bool initial_h2d = settings.system().explore_settings_candidate().loading.h2d_dataloader;
    ExploreScenario scenario{settings, backend};
    active = &scenario.system();
    scenario.OpenAndWait({.extent = {63U, 21U}, .row_count = 1U, .columns = 3U});
    const auto before_frame = active->snapshot().frame;
    const auto before = active->snapshot().revision;
    contracts::SettingsUpdateRequest update;
    update.updates.push_back(
        {.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
    static_cast<void>(settings.system().Update(std::move(update)));
    REQUIRE(scenario.Wait([&] {
        const auto snapshot = active->snapshot();
        return snapshot.ready && !snapshot.busy && snapshot.revision > before;
    }));
    CHECK(active->snapshot().frame.clean_revision > before_frame.clean_revision);
    active->Shutdown();
    CHECK(backend->contexts_created == 2U);
    CHECK(backend->contexts_destroyed == 2U);
    active = nullptr;
}

TEST_CASE("Explore applies a transport change after a pending settings mutation rolls back") {
    const bool initial_h2d = GENERATE(false, true);
    auto backend = std::make_shared<FakeImageBackend>();
    auto gate = std::make_shared<ExplorePostRenderGate>(1U);
    auto work = std::make_shared<ExploreWorkProbe>();
    auto failures = std::make_shared<std::atomic_uint64_t>(0U);
    ExploreSystem* active = nullptr;
    LoadedSettings settings{[&](SettingsSystem::event_type) {
        if (active) active->ExecutionSettingsChanged();
    }};
    contracts::SettingsUpdateRequest initial;
    initial.updates.push_back(
        {.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{initial_h2d}});
    static_cast<void>(settings.system().Update(std::move(initial)));
    ExploreScenario scenario{settings, backend, ExploreScenario::GateAfterRender(gate, {}, work), count_explore_failures(failures)};
    auto& explore = scenario.system();
    active = &explore;
    auto settle_explore = settle_explore_on_exit(explore, gate->release);
    scenario.OpenAndWait({.extent = {63U, 21U}, .row_count = 1U, .columns = 3U});
    const auto current = active->snapshot();
    auto overlay = current.overlay;
    overlay.show_masks = !overlay.show_masks;
    const auto admitted = active->UpdateFilter({.filter = current.filter, .overlay = overlay});
    mmltk::testsupport::await_test_promise(gate->entered, "gate->entered");
    contracts::SettingsUpdateRequest update;
    update.updates.push_back(
        {.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
    static_cast<void>(settings.system().Update(std::move(update)));
    gate->release.set_value();
    const bool reopened = scenario.Wait([&] {
        const auto snapshot = active->snapshot();
        return work->opens.load(std::memory_order_acquire) == 2U && snapshot.ready && !snapshot.busy && snapshot.failure.empty() &&
               snapshot.revision > admitted.revision;
    });
    const auto observed = active->snapshot();
    INFO("opens=" << work->opens.load(std::memory_order_acquire) << " ready=" << observed.ready << " busy=" << observed.busy
                  << " failure=" << observed.failure << " revision=" << observed.revision << " admitted=" << admitted.revision
                  << " contexts-created=" << backend->contexts_created << " contexts-destroyed=" << backend->contexts_destroyed
                  << " failures=" << failures->load(std::memory_order_acquire));
    REQUIRE(reopened);
    active->Shutdown();
    CHECK(backend->contexts_created == 2U);
    CHECK(backend->contexts_destroyed == 2U);
    CHECK(failures->load(std::memory_order_acquire) == 1U);
    active = nullptr;
}

TEST_CASE("Explore locality selects automatic budget and preserves explicit overlapping budget") {
    const mmltk::common::system::ExecutionPlacement placement{.numa_node = 3, .cpus = {17, 25}};
    CHECK(normalize_explore_parallelism(0, placement) == 2U);
    CHECK(normalize_explore_parallelism(8, placement) == 8U);
}

}  // namespace
}  // namespace mmltk::controller
