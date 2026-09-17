#include "src/controller/browser/application_materializer.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/controller/subsystems/explore/tests/support/explore_system_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/frameworks/gpu/image_buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
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
    for (auto image : std::span{images}.first(13U)) cache.Complete(image, image, meaning, 1U, 1U, 0U);
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
    for (std::size_t index = 0U; index < images.size(); ++index) images[index] = static_cast<std::uint32_t>((index + 1U) * 29U);
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
    if (identity.SameSource(incumbent.cache.identity())) {
        const auto* retained = candidate.cache.Retained(7U);
        REQUIRE(retained);
        CHECK(retained->meaning == meaning);
        CHECK(retained->refresh_pending);
        CHECK(retained->bank == 1U);
        const std::array images{7U};
        candidate.cache.Admit(images, 4U);
        candidate.cache.BeginUpdate();
        candidate.cache.UpdateSemantics(4U, 3U, 1U);
        CHECK_FALSE(candidate.cache.Find(7U));
        CHECK(candidate.cache.Retained(7U)->semantic_identity == 3U);
        candidate.cache.RollbackUpdate();
        CHECK(candidate.cache.Retained(7U)->semantic_identity == 2U);
        CHECK(candidate.cache.Retained(7U)->semantic_bank == 0U);
        candidate.cache.Complete(4U, 7U, meaning, 3U, 0U, 1U);
        REQUIRE(candidate.cache.Find(7U));
        CHECK_FALSE(candidate.cache.Find(7U)->refresh_pending);
    } else
        CHECK_FALSE(candidate.cache.Retained(7U));
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
                          product.active_classes.capacity() * sizeof(decltype(product.active_classes)::value_type) + product.completed_slots.capacity() / 8U +
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
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t, const mmltk::frameworks::gpu::ImagePlaneView clean,
                       const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
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
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, const std::uint64_t seed, std::size_t, const std::stop_token stop) override {
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
        return candidate == nullptr ? ExploreOrderFacts{.matching_count = static_cast<std::uint32_t>(order_.size()), .visible_indices = order_}
                                    : candidate->order;
    }
    bool Contains(const std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
    std::optional<std::uint32_t> Adjacent(const std::uint32_t value, std::int64_t) const override {
        return Contains(value) ? std::optional{value} : std::nullopt;
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t, const mmltk::frameworks::gpu::ImagePlaneView clean,
                       const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
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
    ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, std::uint64_t seed, std::size_t,
                                        // CLEANUP-IGNORE: Atomic-filter cancellation and reopen cancellation exercise different algorithm boundaries.
                                        const std::stop_token stop) override {
        if (prepare_count_++ == 0U) return {.filter = filter, .order = {.matching_count = 1U, .shuffle_seed = seed, .visible_indices = {0U}}, .generation = 1U};
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
    std::optional<std::uint32_t> Adjacent(std::uint32_t value, std::int64_t) const override { return Contains(value) ? std::optional{value} : std::nullopt; }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate*, std::size_t, mmltk::frameworks::gpu::ImagePlaneView clean,
                       mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
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
                        fact.detail == 30U + static_cast<std::uint64_t>(detail::VisualRuntimeOwner::ActivityStage::CycleFinalized) && fact.value == 0U) {
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
        : TestExploreAlgorithm(std::make_shared<std::atomic_size_t>(0U), nullptr, nullptr, nullptr, nullptr, probe.render_gate), probe_(probe) {}
    ExploreOutputChange OutputChange(const ExploreRenderPlan& plan, const ExploreOrderCandidate*) const override {
        probe_.attempts.fetch_add(1U, std::memory_order_release);
        probe_.events.Advance();
        return probe_.semantic_detail && plan.mode == ExploreMode::Detail ? ExploreOutputChange::Semantic : ExploreOutputChange::Initialize;
    }
    void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc,
                       mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) override {
        if (probe_.fail_render.exchange(false, std::memory_order_acq_rel)) throw std::runtime_error("Explore pending predecessor failed");
        TestExploreAlgorithm::RenderProduct(plan, candidate, nproc, clean, semantic, stream);
    }

   private:
    ExplorePressureProbe& probe_;
};
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
                       observations.push_back(
                           {std::visit([](auto& value) { return std::move(value.snapshot); }, event), probe.runtimes.load(std::memory_order_acquire), failed});
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
    void RequestViewport(const std::uint32_t row, const bool expect_work = true) {
        auto next = viewport;
        next.first_row = row;
        const auto before = probe.attempts.load(std::memory_order_acquire);
        scenario.system().UpdateViewport({.viewport = next});
        if (expect_work) probe.WaitIdle(before + 1U);
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
    fixture.RequestViewport(1U, false);
    CHECK(explore.LastInteractionGeneration() == generation);
    CHECK(explore.snapshot().frame == held);
    fixture.held[0U] = {};
    REQUIRE(fixture.scenario.Wait([&] {
        const auto state = explore.snapshot();
        return state.frame != held && state.viewport.first_row == 1U;
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
    fixture.RequestViewport(0U);
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
        fixture.held[1U] = {};
        REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().filter == policy.filter; }));
        const auto filtered = explore.snapshot();
        CHECK(filtered.frame != predecessor.frame);
        CHECK(filtered.viewport.first_row == 0U);
        CHECK(filtered.overlay == policy.overlay);
        CHECK_FALSE(filtered.selected_image);
        fixture.RequestViewport(1U);
        const auto later = explore.snapshot();
        CHECK(later.filter == policy.filter);
        CHECK(later.overlay == policy.overlay);
        CHECK(later.viewport.first_row == 1U);
        CHECK(later.order.visible_indices == std::vector<std::uint32_t>{1U});
        CHECK_FALSE(later.selected_image);
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
    fixture.RequestViewport(0U);
    const bool initial_h2d = fixture.settings.system().explore_settings_candidate().loading.h2d_dataloader;
    contracts::SettingsUpdateRequest update;
    update.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
    static_cast<void>(fixture.settings.system().Update(std::move(update)));
    CHECK(explore.Open({.viewport = fixture.viewport, .compiled_source = "/replacement"}).busy);
    CHECK(fixture.probe.runtimes.load(std::memory_order_acquire) == 1U);
    fixture.held = {};
    REQUIRE(fixture.scenario.Wait([&] { return !explore.snapshot().busy; }));
    const auto observations = fixture.Recorded();
    const auto predecessor =
        std::ranges::find_if(observations, [](const auto& event) { return event.snapshot.busy && event.snapshot.viewport.first_row == 0U; });
    REQUIRE(predecessor != observations.end());
    CHECK(predecessor->runtimes == 1U);
    CHECK(predecessor->snapshot.order.visible_indices == std::vector<std::uint32_t>{0U});
    CHECK(fixture.probe.runtimes.load(std::memory_order_acquire) == 2U);
    CHECK(explore.snapshot().frame != predecessor->snapshot.frame);
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
[[nodiscard]] ExploreFilterPreferences select_explore_subset(LoadedSettings& settings, ExploreScenario& scenario, const std::uint32_t class_index) {
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
[[nodiscard]] ExploreScenario tracked_explore_scenario(LoadedSettings& settings, std::shared_ptr<FakeImageBackend> backend,
                                                       std::shared_ptr<std::atomic_uint64_t> commits, ExploreScenario::Observer observer = {}) {
    return ExploreScenario{settings, std::move(backend), ExploreScenario::TrackCommits(std::move(commits)), std::move(observer)};
}
[[nodiscard]] ExploreSnapshot wait_for_explore_failure(ExploreScenario& scenario, const std::shared_ptr<std::atomic_uint64_t>& failures,
                                                       const std::uint64_t admission_revision) {
    auto& explore = scenario.system();
    REQUIRE(scenario.Wait(
        [&] { return failures->load(std::memory_order_acquire) == 1U && !explore.snapshot().busy && explore.snapshot().revision > admission_revision; }));
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
        return assignment.generation == probe.generation && std::ranges::find(visible_indices, assignment.compiled_index) != visible_indices.end();
    });
}
[[nodiscard]] ExploreClassCatalogIdentity test_explore_catalog_identity() {
    const std::array names{
        mmltk::backend::data::catalog::ClassName{.value = "person"},
        mmltk::backend::data::catalog::ClassName{.value = "vehicle"},
    };
    return explore_class_catalog_identity(names);
}
void persist_explore_catalog(SettingsSystem& settings, const ExploreClassCatalogIdentity identity) {
    const auto candidate = settings.explore_settings_candidate();
    auto preferences = candidate.preferences.policy;
    preferences.filter.class_selection = {};
    preferences.overlay.class_selection = {};
    static_cast<void>(settings.Update(candidate, {.preferences = preferences, .class_catalog_identity = identity}));
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
                                  return constructions->fetch_add(1U) == 0U ? first_factory(std::move(revisions)) : replacement_factory(std::move(revisions));
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
        update.updates.push_back(
            {.path = "workflows.explore.h2d_dataloader",
             .value = mmltk::frameworks::serialization::wire::FlatValue{!settings.system().explore_settings_candidate().loading.h2d_dataloader}});
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
    const auto completed = explore.snapshot();
    CHECK(completed.gallery.slots == std::vector<bool>{true, true});
    explore.SetInputPeer(1U);
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{6.25F, 2.0F}});
    CHECK(explore.snapshot().gallery.generation == completed.gallery.generation);
    CHECK(explore.snapshot().gallery.slots == completed.gallery.slots);
    std::scoped_lock lock(probe->mutex);
    CHECK(probe->lane_preparations == 2U);
}
TEST_CASE("Explore owns direction across accepted logical rows and zero-movement updates", "[explore][priority]") {
    StreamingExploreFixture scenario{2U};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {8U, 4U}, .row_count = 1U, .columns = 2U});
    explore.SetInputPeer(1U);
    const auto check = [&](const std::uint32_t row, const ExploreScrollDirection direction) {
        const auto before = explore.snapshot();
        const auto generation = explore.LastInteractionGeneration();
        explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{row % 2U == 0U ? 0.5F : 7.5F, 2.0F}});
        explore.UpdateViewport({.viewport = {.extent = {8U, 4U}, .first_row = row, .row_count = 1U, .columns = 2U}});
        if (before.viewport.first_row != row)
            REQUIRE(scenario.Wait([&] { return explore.snapshot().viewport.first_row == row && explore.snapshot().revision > before.revision; }));
        else
            CHECK(explore.LastInteractionGeneration() == generation);
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
    REQUIRE(scenario.Wait([&] { return scenario.failure_count() == 1U && !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
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
        explore.Shutdown();
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
        update.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{h2d}});
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
    static_cast<void>(explore.Open({.viewport = {.extent = {8U, 4U}, .first_row = 1U, .row_count = 1U, .columns = 2U}, .compiled_source = "/rejected"}));
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
                case Mutation::Augmentation: return explore.UpdateAugmentation({.enabled = true});
                case Mutation::Reroll: return explore.RerollAugmentation();
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
    CHECK(scenario.backend().streams_destroyed == 1U);
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
    const VisualDiagnosticSink diagnostics{.context = &capture,
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
        0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [calls] { return std::make_unique<StorageAlgorithm>(calls); }, 3U);
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
    ExploreSystem explore{
        settings.system(), kDevice, 2U,
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            [work] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr, work); }, 3U),
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
TEST_CASE("Explore installs and atomically persists typed live filter preferences") {
    auto settings_events = std::make_shared<std::atomic_uint64_t>(0U);
    LoadedSettings settings{[settings_events](SettingsSystem::event_type) { settings_events->fetch_add(1U, std::memory_order_acq_rel); }};
    persist_explore_catalog(settings.system(), test_explore_catalog_identity());
    const auto initial_events = settings_events->load(std::memory_order_acquire);
    (void)settings.system().Update(
        settings.system().explore_settings_candidate(),
        {.preferences = mmltk::controller::ExploreFilterUpdate{
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
             .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {1U}}, .show_boxes = false, .show_masks = true},
         }});
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
        .overlay = {.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {0U}}, .show_boxes = true, .show_masks = false},
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
    class DocumentExploreAlgorithm final : public TestExploreAlgorithm {
       public:
        using TestExploreAlgorithm::TestExploreAlgorithm;
        std::shared_ptr<const VisualDocument> Document() const override { return test_document({}).document; }
    };
    auto backend = std::make_shared<FakeImageBackend>();
    auto extents = std::make_shared<ExploreDetailExtentProbe>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend, [extents] {
                                 return std::make_unique<DocumentExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr,
                                                                                   nullptr, nullptr, extents);
                             }};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 48U}, .row_count = 1U, .columns = 2U});
    const auto gallery_frame = explore.snapshot().frame;
    const auto gallery_metadata = explore.ImageSnapshot(gallery_frame);
    REQUIRE(gallery_metadata);
    auto admitted = explore.Select({.compiled_index = 1U});
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().revision > admitted.revision; }));
    CHECK(explore.snapshot().frame.extent == extents->padded);
    const auto full_frame = explore.snapshot().frame;
    CHECK((full_frame.content == VisualRegion{8U, 16U, 48U, 32U}));
    const auto detail_metadata = explore.ImageSnapshot(full_frame);
    REQUIRE(detail_metadata);
    const auto raw = explore.BorrowFrame();
    const auto captured = explore.BorrowDocument(full_frame);
    REQUIRE(raw.valid());
    REQUIRE(captured.valid());
    REQUIRE(captured.image_metadata);
    const auto captured_metadata = *captured.image_metadata;
    const auto raw_data = raw.plane(0U).plane().data;
    for (const bool original : {true, false}) {
        admitted = explore.UpdateDetail({.show_original_dimensions = original});
        CHECK(admitted.detail.show_original_dimensions == original);
        CHECK(explore.snapshot().frame == full_frame);
        for (const auto* baseline : {&*gallery_metadata, &*detail_metadata}) {
            auto expected = *baseline;
            expected.detail.show_original_dimensions = original;
            const auto refreshed = explore.ImageSnapshot(baseline->frame);
            REQUIRE(refreshed);
            const auto actual_value = mmltk::frameworks::serialization::reflected_transport_value(*refreshed);
            const auto expected_value = mmltk::frameworks::serialization::reflected_transport_value(expected);
            REQUIRE(actual_value);
            REQUIRE(expected_value);
            CHECK(*actual_value == *expected_value);
        }
        const auto current = explore.BorrowDocument(full_frame);
        REQUIRE(current.valid());
        CHECK(current.document == captured.document);
        CHECK(current.pixels.plane(0U).plane().data == raw_data);
        CHECK(raw.valid());
        CHECK(captured.pixels.valid());
        CHECK(*captured.image_metadata == captured_metadata);
        CHECK(settings.system().explore_settings_candidate().show_original_dimensions == original);
    }
}
TEST_CASE("Explore Open commits the effective pending filter with a different catalog") {
    auto settings_events = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_revision = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_catalog = std::make_shared<std::atomic_uint64_t>(0U);
    auto emitted_minimum_instances = std::make_shared<std::atomic_uint32_t>(0U);
    auto emitted_show_boxes = std::make_shared<std::atomic_bool>(true);
    LoadedSettings settings{
        [settings_events, emitted_revision, emitted_catalog, emitted_minimum_instances, emitted_show_boxes](SettingsSystem::event_type event) {
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
                                     return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, gate);
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
    (void)settings.system().Update(settings.system().explore_settings_candidate(), {.preferences = saved});
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
    ExploreScenario scenario{settings, backend,
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
                                 return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits, gate);
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
    auto scenario = tracked_explore_scenario(settings, backend, commits, [changed_events, changed_revision](ExploreSystem::event_type event) {
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
    REQUIRE(scenario.Wait([&] { return changed_events->load(std::memory_order_acquire) == events_before_open + 1U && !explore.snapshot().busy; }));
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
        std::atomic_uint64_t settled_revision{0U};
        ExploreScenario scenario{settings, backend, [probe] { return std::make_unique<CancellableOpenPreparationAlgorithm>(probe); },
                                 [&](ExploreSystem::event_type event) {
                                     if (const auto* changed = std::get_if<ExploreChanged>(&event); changed && !changed->snapshot.busy)
                                         settled_revision.store(changed->snapshot.revision, std::memory_order_release);
                                 }};
        auto& explore = scenario.system();
        const auto wait_settled = [&](const ExploreSnapshot& admitted) {
            // The typed completion is delivered after worker admission is released.
            // A snapshot can expose the result while that worker is still returning.
            REQUIRE(scenario.Wait([&] { return settled_revision.load(std::memory_order_acquire) > admitted.revision; }));
            CHECK_FALSE(explore.snapshot().busy);
            CHECK(explore.snapshot().revision > admitted.revision);
        };
        if (blocked_call != 0U) {
            const auto opened = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/first"});
            wait_settled(opened);
            REQUIRE(explore.snapshot().ready);
        }
        const auto admission = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/cancelled"});
        REQUIRE(entered.wait_for(2s) == std::future_status::ready);
        entered.get();
        const auto stopping = explore.Stop();
        CHECK_FALSE((stopping.busy && !stopping.cancellation_requested));
        REQUIRE(cancelled.wait_for(2s) == std::future_status::ready);
        cancelled.get();
        wait_settled(admission);
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
        wait_settled(reopened);
        REQUIRE(explore.snapshot().ready);
        CHECK(explore.snapshot().dataset.image_count == 1U);
        CHECK(explore.snapshot().frame.valid());
        CHECK(probe->commits.load(std::memory_order_acquire) == blocked_call + 1U);
    };
    SECTION("first Open") { run(0U); }
    SECTION("reopen") { run(1U); }
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
    REQUIRE(scenario.Wait(
        [&] { return scenario.system().snapshot().frame != previous && probe->rendered_copy_paste_probability.load(std::memory_order_acquire) == requested; }));
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
          ExploreViewport{.extent = {64U, 64U}, .columns = 0U}, ExploreViewport{.extent = {1U, 1U}, .row_count = UINT32_MAX, .columns = UINT32_MAX}}) {
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
        application, browser::Interaction{.endpoint_id = browser::application_stable_id("explore", "UpdateViewport"), .value = std::move(bytes)});
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
    CHECK(failed.failure_kind == (transport ? ExploreFailureKind::SelectedTransportUnavailable : ExploreFailureKind::RuntimeInitialization));
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
        ExploreScenario scenario{settings, backend, [observed_nproc, gate] { return std::make_unique<TestExploreAlgorithm>(observed_nproc, gate); }};
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
            static_cast<void>(explore.UpdateFilter({.filter = {.minimum_instances = 1U}, .overlay = {.show_boxes = false, .show_masks = true}}));
        gate->release.set_value();
        REQUIRE(scenario.Wait([&] {
            const auto state = explore.snapshot();
            return !state.busy && state.viewport.extent.width == 30U && (select ? state.mode == ExploreMode::Detail : state.filter.minimum_instances == 1U);
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
    (void)settings.system().Update(settings.system().explore_settings_candidate(), {.preferences = mmltk::controller::ExploreFilterUpdate{
                                                                                        .filter = {.order = ExploreOrder::Shuffled, .shuffle_seed = 17U},
                                                                                    }});
    ExploreScenario scenario{settings, backend,
                             [observed_nproc, commits, gate] { return std::make_unique<TestExploreAlgorithm>(observed_nproc, gate, commits); }};
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
        (void)settings.system().Update(settings.system().explore_settings_candidate(), {.preferences = mmltk::controller::ExploreFilterUpdate{
                                                                                            .filter = {.order = ExploreOrder::Shuffled, .shuffle_seed = 19U},
                                                                                        }});
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
TEST_CASE("Explore shared input lifecycles preserve atlas content through scrolling and selection", "[explore][priority]") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto work = std::make_shared<ExploreWorkProbe>();
    LoadedSettings settings;
    // The old borrowed atlas, current atlas and detail remain independently
    // retained while the fake algorithm initializes the closing-gallery output.
    ExploreScenario scenario{settings, 2U,
                             RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, ExploreScenario::TrackWork(work), 4U)};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 96U}, .row_count = 2U, .columns = 2U});
    explore.SetInputPeer(1U);
    const auto before = explore.snapshot();
    auto pixels = explore.BorrowFrame();
    const auto renders = work->renders.load();
    const auto motion = [&](const WorkspacePoint point, WorkspaceMouseKind kind = WorkspaceMouseKind::Motion) {
        explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .kind = kind, .point = point});
    };
    motion({48.0F, 0.0F}, WorkspaceMouseKind::Enter);
    motion({95.75F, 47.875F});
    CHECK(explore.snapshot().frame == before.frame);
    CHECK(work->renders.load() == renders);
    motion({0.25F, 48.0F});
    CHECK_THROWS_AS(explore.Input({.source = PresentationSourceKind::Predict, .peer_epoch = 1U, .point = WorkspacePoint{1.0F, 1.0F}}),
                    contracts::InvalidIntentError);
    CHECK_THROWS_AS(motion({std::numeric_limits<float>::infinity(), 0.0F}), contracts::InvalidIntentError);
    for (const auto point : {WorkspacePoint{48.0F, 48.0F}, WorkspacePoint{96.0F, 0.0F}, WorkspacePoint{0.0F, 96.0F}, WorkspacePoint{-0.125F, 0.0F}}) {
        motion(point);
    }
    for (const auto kind : {WorkspaceMouseKind::Press, WorkspaceMouseKind::Release, WorkspaceMouseKind::Wheel}) { motion({1.5F, 1.5F}, kind); }
    for (const auto kind : {WorkspaceMouseKind::Leave, WorkspaceMouseKind::Cancel}) {
        motion({1.5F, 1.5F}, kind);
        motion({1.5F, 1.5F});
    }
    explore.SetInputPeer(2U);
    CHECK_THROWS_AS(motion({1.5F, 1.5F}), contracts::InvalidIntentError);
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 2U, .point = WorkspacePoint{1.5F, 1.5F}});
    explore.UpdateViewport({.viewport = {.extent = {96U, 48U}, .first_row = 1U, .row_count = 1U, .columns = 2U}});
    REQUIRE(scenario.Wait([&] { return explore.snapshot().viewport.first_row == 1U; }));
    CHECK(work->rendered_count.load() == 1U);
    CHECK(work->rendered_slots[0].load() == 2U);
    static_cast<void>(explore.Select({.compiled_index = 2U}));
    REQUIRE(scenario.Wait([&] { return explore.snapshot().mode == ExploreMode::Detail; }));
    static_cast<void>(explore.CloseDetail());
    REQUIRE(scenario.Wait([&] { return explore.snapshot().mode == ExploreMode::Gallery; }));
    CHECK(pixels.valid());
}
TEST_CASE("Explore selection and detail navigation publish complete bounded snapshots") {
    auto backend = std::make_shared<FakeImageBackend>();
    LoadedSettings settings;
    ExploreScenario scenario{settings, backend};
    auto& explore = scenario.system();
    scenario.OpenAndWait({.extent = {96U, 64U}, .row_count = 2U, .columns = 3U});
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
    ExploreScenario scenario{settings, backend, [probe, commits] { return std::make_unique<AtomicFilterExploreAlgorithm>(probe, commits); }};
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
            ExploreSystem explore{
                settings.system(),
                kDevice,
                2U,
                RuntimeFactory(
                    0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                    [work] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr, work); },
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
            result.copies = backend->same_copies.load() + backend->peer_copies.load() + backend->staged_downloads.load() + backend->staged_uploads.load();
        }
        CHECK(backend->contexts_created == backend->contexts_destroyed);
        CHECK(backend->planes_allocated == backend->planes_freed);
        return result;
    };
    const auto disabled = run(false);
    const auto enabled = run(true);
    CHECK(disabled == enabled);
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
    update.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
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
    initial.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{initial_h2d}});
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
    update.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{!initial_h2d}});
    static_cast<void>(settings.system().Update(std::move(update)));
    gate->release.set_value();
    const bool reopened = scenario.Wait([&] {
        const auto snapshot = active->snapshot();
        return work->opens.load(std::memory_order_acquire) == 2U && snapshot.ready && !snapshot.busy && snapshot.failure.empty() &&
               snapshot.revision > admitted.revision;
    });
    const auto observed = active->snapshot();
    INFO("opens=" << work->opens.load(std::memory_order_acquire) << " ready=" << observed.ready << " busy=" << observed.busy << " failure=" << observed.failure
                  << " revision=" << observed.revision << " admitted=" << admitted.revision << " contexts-created=" << backend->contexts_created
                  << " contexts-destroyed=" << backend->contexts_destroyed << " failures=" << failures->load(std::memory_order_acquire));
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
