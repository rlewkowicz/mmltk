#include "src/backend/data/compiled_dataset.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/backend/data/dataset_loader.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#include <cuda_runtime_api.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/imaging/explore/tests/explore_dataset_fixture.h"
#include "src/backend/models/rfdetr/augmentation/tests/copy_paste_fixture.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/browser/client_record.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/frameworks/serialization/serialization.h"
import mmltk.backend.imaging.explore.compiled_explore_store;
namespace {
namespace data = mmltk::backend::data;
namespace explore = mmltk::backend::imaging::explore;
namespace controller = mmltk::controller;
class NativeExploreAudit final {
    struct PublicationObservation final {
        void Observe(const std::uint64_t sequence, const controller::VisualDiagnosticFact& fact) noexcept {
            std::uint64_t unset = 0U;
            static_cast<void>(first_sequence.compare_exchange_strong(unset, sequence));
            last_generation.store(fact.generation, std::memory_order_release);
            staging.store(fact.context.staging_bytes, std::memory_order_release);
            count.fetch_add(1U, std::memory_order_acq_rel);
        }
        std::atomic_uint64_t first_sequence{0U};
        std::atomic_uint64_t last_generation{0U};
        std::atomic_uint64_t count{0U};
        std::atomic_size_t staging{0U};
    };

   public:
    void Observe(controller::ExploreSystem::event_type event) noexcept {
        if (const auto* changed = std::get_if<controller::ExploreChanged>(&event); changed && changed->snapshot.ready) {
            if (require_retained_ready_.load(std::memory_order_acquire) && changed->snapshot.mode == controller::ExploreMode::Gallery &&
                std::ranges::any_of(changed->snapshot.gallery.slots, [](const bool ready) { return !ready; }))
                lost_retained_ready_.store(true, std::memory_order_release);
            const auto frame_revision = changed->snapshot.frame.revision;
            std::uint64_t unset = 0U;
            static_cast<void>(first_ready_frame_.compare_exchange_strong(unset, frame_revision));
            last_ready_frame_.store(frame_revision, std::memory_order_release);
            if (changed->snapshot.mode == controller::ExploreMode::Gallery && !changed->snapshot.gallery.slots.empty() &&
                std::ranges::all_of(changed->snapshot.gallery.slots, [](auto ready) { return ready == 0U; })) {
                std::scoped_lock lock(mutex_);
                pending_snapshot_ = changed->snapshot;
            }
        } else if (const auto* failed = std::get_if<controller::ExploreFailed>(&event)) {
            {
                std::scoped_lock lock(mutex_);
                failure_detail_ = failed->detail;
            }
            failed_.store(true, std::memory_order_release);
        }
        Wake();
    }
    [[nodiscard]] std::optional<controller::ExploreSnapshot> pending_snapshot() {
        std::scoped_lock lock(mutex_);
        return pending_snapshot_;
    }
    [[nodiscard]] controller::VisualDiagnosticSink diagnostics() noexcept {
        return {
            .context = this,
            .write =
                [](void* context, const controller::VisualDiagnosticFact fact) noexcept {
                    auto& audit = *static_cast<NativeExploreAudit*>(context);
                    const auto sequence = audit.sequence_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                    if (fact.operation == controller::VisualDiagnosticOperation::GalleryReadScheduled) {
                        audit.read_admissions_.fetch_add(1U, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::PlaceholderPublished) {
                        audit.placeholder_.Observe(sequence, fact);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExplorePrefetchReady) {
                        if (fact.value < 64U) audit.prefetched_indices_.fetch_or(std::uint64_t{1U} << fact.value, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::TileBatchPublished) {
                        audit.tile_.Observe(sequence, fact);
                        audit.last_tile_cumulative_.store(fact.value, std::memory_order_release);
                        audit.reused_tiles_.fetch_add(fact.detail, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreAugmentationBatchPrepared) {
                        audit.augmentation_count_.fetch_add(1U, std::memory_order_acq_rel);
                        audit.last_augmentation_seed_.store(fact.detail, std::memory_order_release);
                        audit.last_valid_donors_.store(fact.context.capacity_width, std::memory_order_release);
                        audit.last_planned_pastes_.store(fact.context.capacity_height, std::memory_order_release);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared) {
                        if (audit.last_descriptor_generation_.exchange(fact.generation, std::memory_order_acq_rel) != fact.generation) {
                            audit.last_descriptor_annotations_.store(0U, std::memory_order_release);
                            audit.last_descriptor_rle_.store(0U, std::memory_order_release);
                        }
                        audit.descriptor_count_.fetch_add(1U, std::memory_order_acq_rel);
                        audit.last_descriptor_annotations_.fetch_add(fact.value, std::memory_order_acq_rel);
                        audit.last_descriptor_rle_.fetch_add(fact.detail, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreTransformedBounds) {
                        audit.bounds_count_.fetch_add(1U, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreSemanticPixels) {
                        if (audit.last_semantic_generation_.exchange(fact.generation, std::memory_order_acq_rel) != fact.generation)
                            audit.semantic_nonzero_cards_.store(0U, std::memory_order_release);
                        audit.semantic_count_.fetch_add(1U, std::memory_order_acq_rel);
                        audit.last_semantic_pixels_.store(fact.detail, std::memory_order_release);
                        if (fact.detail != 0U) audit.semantic_nonzero_cards_.fetch_add(1U, std::memory_order_acq_rel);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreImagePixels) {
                        if ((fact.context.staging_bytes & 4U) == 0U) audit.detail_checksum_.store(fact.detail, std::memory_order_release);
                        if (fact.value >= 10U && fact.value <= 11U && (fact.context.staging_bytes & 7U) == 7U) {
                            const auto image = static_cast<std::size_t>(fact.value - 10U);
                            audit.image_seeds_[image].store(fact.context.capacity_width, std::memory_order_release);
                            audit.image_checksums_[image].store(fact.detail, std::memory_order_release);
                            audit.image_pixel_count_.fetch_add(1U, std::memory_order_acq_rel);
                        }
                    } else if (fact.operation == controller::VisualDiagnosticOperation::ExploreDonorDescriptorsPrepared) {
                        audit.donor_descriptor_count_.fetch_add(1U, std::memory_order_acq_rel);
                        audit.donor_count_.store(fact.value, std::memory_order_release);
                        audit.donor_rle_.store(fact.detail, std::memory_order_release);
                    } else if (fact.operation == controller::VisualDiagnosticOperation::RenderCompleted) {
                        audit.last_render_flags_.store(fact.detail, std::memory_order_release);
                    }
                    audit.Wake();
                },
            .pixel_probes = true,
        };
    }
    template <class Predicate>
    [[nodiscard]] bool Wait(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        std::unique_lock lock(mutex_);
        auto observed = wake_revision_;
        while (true) {
            lock.unlock();
            if (predicate()) return true;
            lock.lock();
            if (!changed_.wait_until(lock, deadline, [&] { return wake_revision_ != observed; })) {
                lock.unlock();
                return predicate();
            }
            observed = wake_revision_;
        }
    }  // CLEANUP-IGNORE: The wait terminator and independent scalar observation getters are not a repeated operation.
    void RequireRetainedReady(const bool enabled) noexcept { require_retained_ready_.store(enabled, std::memory_order_release); }
    // CLEANUP-IGNORE: Retained-ready loss is an independent atomic observation, not a repeated multi-field mapping.
    [[nodiscard]] bool LostRetainedReady() const noexcept { return lost_retained_ready_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t ReadAdmissions() const noexcept { return read_admissions_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t first_ready_frame() const noexcept { return first_ready_frame_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_ready_frame() const noexcept { return last_ready_frame_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t placeholder_count() const noexcept { return placeholder_.count.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t tile_count() const noexcept {
        // CLEANUP-IGNORE: Each scalar audit getter observes a distinct atomic fact; there is no repeated mapping or
        // multi-step operation.
        return tile_.count.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t reused_tiles() const noexcept { return reused_tiles_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t prefetched_indices() const noexcept { return prefetched_indices_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_placeholder_generation() const noexcept { return placeholder_.last_generation.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_tile_generation() const noexcept { return tile_.last_generation.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_tile_cumulative() const noexcept { return last_tile_cumulative_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t last_tile_staging() const noexcept { return tile_.staging.load(std::memory_order_acquire); }
    // CLEANUP-IGNORE: Failure state and semantic/image diagnostic counters are independent observations.
    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);  // CLEANUP-IGNORE: Failure status and indexed image checksums
                                                         // expose different atomic facts.
    }  // CLEANUP-IGNORE: Failure and augmentation getters have independent result meanings, not duplicated mapping
       // logic.
    [[nodiscard]] std::string failure_detail() const {
        std::scoped_lock lock(mutex_);
        // CLEANUP-IGNORE: Locked failure text and adjacent atomic counters expose distinct audit facts.
        return failure_detail_;
        // CLEANUP-IGNORE: This accessor boundary does not begin a repeated operation across the following getters.
    }
    // CLEANUP-IGNORE: Augmentation counters are named atomic observations with independent meaning and storage.
    [[nodiscard]] std::uint64_t augmentation_count() const noexcept {
        // CLEANUP-IGNORE: Scalar audit getters observe distinct facts rather than repeating a field mapping.
        return augmentation_count_.load(std::memory_order_acquire);
    }
    // CLEANUP-IGNORE: Seed and donor observations remain direct named getters rather than a runtime field registry.
    [[nodiscard]] std::uint64_t last_augmentation_seed() const noexcept { return last_augmentation_seed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_valid_donors() const noexcept { return last_valid_donors_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_planned_pastes() const noexcept { return last_planned_pastes_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_render_flags() const noexcept { return last_render_flags_.load(std::memory_order_acquire); }
    [[nodiscard]] bool observed_semantic_diagnostics() const noexcept {
        return descriptor_count_.load(std::memory_order_acquire) != 0U && bounds_count_.load(std::memory_order_acquire) != 0U &&
               // CLEANUP-IGNORE: This completes one compound predicate rather than exposing another scalar counter.
               semantic_count_.load(std::memory_order_acquire) != 0U;
    }
    [[nodiscard]] std::uint64_t descriptor_count() const noexcept {
        // CLEANUP-IGNORE: Each named audit getter exposes a distinct fact without a runtime field registry.
        return descriptor_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t last_descriptor_annotations() const noexcept { return last_descriptor_annotations_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t last_descriptor_rle() const noexcept { return last_descriptor_rle_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t semantic_count() const noexcept {
        // CLEANUP-IGNORE: Semantic and image/checksum facts retain independent names and storage.
        return semantic_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t last_semantic_pixels() const noexcept { return last_semantic_pixels_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t semantic_nonzero_cards() const noexcept { return semantic_nonzero_cards_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t image_pixel_count() const noexcept { return image_pixel_count_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t image_seed(const std::size_t image) const noexcept { return image_seeds_[image].load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t image_checksum(const std::size_t image) const noexcept {
        // CLEANUP-IGNORE: These atomic getters expose different image, semantic, checksum, and donor facts; their storage is not interchangeable.
        return image_checksums_[image].load(std::memory_order_acquire);
    }  // CLEANUP-IGNORE: Indexed image checksums and donor counters are independent observations with distinct storage.
    [[nodiscard]] std::uint64_t donor_count() const noexcept { return donor_count_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t detail_checksum() const noexcept { return detail_checksum_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t donor_descriptor_count() const noexcept { return donor_descriptor_count_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t donor_rle() const noexcept { return donor_rle_.load(std::memory_order_acquire); }
    [[nodiscard]] bool GalleryPublishedAfter(const controller::ExploreSystem& system, const std::uint64_t admission_revision, const std::uint64_t prior_images,
                                             const std::uint64_t prior_placeholders) const {
        const auto snapshot = system.snapshot();
        return !snapshot.busy && snapshot.revision > admission_revision && image_pixel_count() >= prior_images + 2U &&
               last_tile_generation() == last_placeholder_generation() && placeholder_count() > prior_placeholders && last_tile_cumulative() == 2U;
    }
    void CheckInitialOrdering() const {
        CHECK(first_ready_frame() != 0U);
        CHECK(last_ready_frame() > first_ready_frame());
        CHECK(placeholder_.first_sequence.load(std::memory_order_acquire) != 0U);
        CHECK(placeholder_.first_sequence.load(std::memory_order_acquire) < tile_.first_sequence.load(std::memory_order_acquire));
        CHECK(placeholder_.staging.load(std::memory_order_acquire) == 0U);
        CHECK(tile_.staging.load(std::memory_order_acquire) != 0U);
    }

   private:
    void Wake() noexcept {
        {
            std::scoped_lock lock(mutex_);
            ++wake_revision_;
        }
        changed_.notify_all();
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string failure_detail_;
    std::optional<controller::ExploreSnapshot> pending_snapshot_;
    std::uint64_t wake_revision_ = 0U;
    std::atomic_uint64_t sequence_{0U};
    std::atomic_uint64_t read_admissions_{0U};
    std::atomic_bool require_retained_ready_{false};
    std::atomic_bool lost_retained_ready_{false};
    std::atomic_uint64_t reused_tiles_{0U};
    std::atomic_uint64_t prefetched_indices_{0U};
    std::atomic_uint64_t first_ready_frame_{0U};
    std::atomic_uint64_t last_ready_frame_{0U};
    PublicationObservation placeholder_;
    PublicationObservation tile_;
    std::atomic_uint64_t last_tile_cumulative_{0U};
    std::atomic_uint64_t augmentation_count_{0U};
    std::atomic_uint64_t last_augmentation_seed_{0U};
    std::atomic_uint64_t last_valid_donors_{0U};
    std::atomic_uint64_t last_planned_pastes_{0U};
    std::atomic_uint64_t last_render_flags_{0U};
    std::atomic_uint64_t descriptor_count_{0U};
    std::atomic_uint64_t last_descriptor_generation_{0U};
    // CLEANUP-OFF: These independent opt-in acceptance counters preserve named evidence for separate rendered
    // content assertions; they do not repeat the preceding augmentation and descriptor observations.
    std::atomic_uint64_t bounds_count_{0U};
    std::atomic_uint64_t semantic_count_{0U};
    std::atomic_uint64_t last_semantic_generation_{0U};
    std::atomic_uint64_t semantic_nonzero_cards_{0U};
    std::atomic_uint64_t last_descriptor_annotations_{0U};
    std::atomic_uint64_t last_descriptor_rle_{0U};
    std::atomic_uint64_t last_semantic_pixels_{0U};
    std::atomic_uint64_t image_pixel_count_{0U};
    std::atomic_uint64_t detail_checksum_{0U};
    std::array<std::atomic_uint64_t, 2U> image_seeds_{};
    // CLEANUP-ON
    std::array<std::atomic_uint64_t, 2U> image_checksums_{};
    std::atomic_uint64_t donor_descriptor_count_{0U};
    std::atomic_uint64_t donor_count_{0U};
    std::atomic_uint64_t donor_rle_{0U};
    std::atomic_bool failed_{false};
};
struct NativeExploreFixture final {
    static constexpr controller::VisualDeviceSettings device{.device = 0, .maximum_width = 256U, .maximum_height = 256U};
    NativeExploreAudit audit;
    const std::size_t parallelism = controller::normalize_explore_parallelism(2U);
    controller::ExploreSystem system;
    NativeExploreFixture(controller::SettingsSystem& settings, bool h2d)
        : system{settings,
                 device,
                 parallelism,
                 controller::make_native_explore_runtime_factory(device, parallelism,
                                                                 {.loading = data::data_loading_options(h2d), .diagnostics = audit.diagnostics()}),
                 [this](controller::ExploreSystem::event_type event) { audit.Observe(std::move(event)); },
                 audit.diagnostics()} {}
};
void wait_for_native_gallery(NativeExploreAudit& audit, const controller::ExploreSystem& system, const std::uint64_t placeholder_count,
                             const std::uint64_t tile_count, const std::uint32_t ready_tiles = 2U) {
    REQUIRE(audit.Wait([&] { return audit.placeholder_count() > placeholder_count; }));
    const auto generation = audit.last_placeholder_generation();
    const bool completed = audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return audit.tile_count() > tile_count && audit.last_tile_generation() == generation && audit.last_tile_cumulative() == ready_tiles &&
               snapshot.mode == controller::ExploreMode::Gallery && snapshot.gallery.generation == generation && !snapshot.busy &&
               audit.last_ready_frame() == snapshot.frame.revision;
    });
    if (!completed) {
        const auto snapshot = system.snapshot();
        INFO("gallery wait generation=" << generation << " placeholder=" << audit.placeholder_count() << " tiles=" << audit.tile_count()
                                        << " tile generation=" << audit.last_tile_generation() << " cumulative=" << audit.last_tile_cumulative()
                                        << " last ready frame=" << audit.last_ready_frame() << " frame=" << snapshot.frame.revision << " busy=" << snapshot.busy
                                        << " ready=" << snapshot.ready << " failed=" << audit.failed() << " failure=" << audit.failure_detail());
        REQUIRE(completed);
    }
}
void check_published_frame(const controller::ExploreSystem& system) {
    const auto snapshot = system.snapshot();
    const auto product = system.BorrowFrame();
    REQUIRE(snapshot.frame.valid());
    REQUIRE(product.valid());
    REQUIRE(product.plane_count() == 2U);
    CHECK(product.plane(0U).revision() == snapshot.frame.revision);
    CHECK(product.plane(1U).revision() == snapshot.frame.revision);
    CHECK(product.plane(0U).plane().descriptor.width == snapshot.frame.extent.width);
    CHECK(product.plane(0U).plane().descriptor.height == snapshot.frame.extent.height);
    CHECK(product.plane(1U).plane().descriptor.width == snapshot.frame.extent.width);
    CHECK(product.plane(1U).plane().descriptor.height == snapshot.frame.extent.height);
    if (snapshot.mode == controller::ExploreMode::Gallery) {
        const auto& layout = snapshot.gallery.layout;
        CHECK(layout.first_row == snapshot.viewport.first_row);
        CHECK(layout.row_count == snapshot.viewport.row_count);
        CHECK(layout.columns == snapshot.viewport.columns);
        REQUIRE(layout.row_capacity >= layout.row_count);
        CHECK(layout.row_origin < layout.row_capacity);
        CHECK(layout.columns * layout.card_extent == snapshot.frame.extent.width);
        CHECK(layout.row_capacity * layout.card_extent == snapshot.frame.extent.height);
        CHECK(snapshot.gallery.slots.size() == snapshot.order.visible_indices.size());
    }
}
void load_explore_transport(controller::SettingsSystem& settings, const std::filesystem::path& settings_path, const bool h2d) {
    REQUIRE(settings.Load(controller::services::SettingsLocation{settings_path.string()}).applied());
    controller::contracts::SettingsUpdateRequest loading;
    loading.updates.push_back({.path = "workflows.explore.h2d_dataloader", .value = mmltk::frameworks::serialization::wire::FlatValue{h2d}});
    static_cast<void>(settings.Update(std::move(loading)));
}
void require_explore_transport(const bool h2d) {
    if (h2d) return;
    int dmabuf = 0;
    const auto status = cuDeviceGetAttribute(&dmabuf, static_cast<CUdevice_attribute>(152), 0);
    if (!(status == CUDA_SUCCESS && dmabuf != 0) && ::access("/dev/gdrdrv", R_OK | W_OK) != 0)
        SKIP("GDR hardware unavailable; Explore GDR acceptance remains unverified");
}
void test_compiled_dataset_explore_projection_navigation_and_streaming() {
    const bool h2d = GENERATE(true, false);
    INFO("h2d_dataloader=" << h2d);
    require_explore_transport(h2d);
    mmltk::testsupport::ScopedTempDir root{"mmltk-compiled-dataset-explore"};
    const auto compiled = mmltk::testsupport::compile_explore_fixture(root.path(), "fixture", 12);
    const auto resize_mode =
        GENERATE(mmltk::backend::imaging::resample::ImageResizeMode::Stretch, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
    const auto non_square_compiled = mmltk::testsupport::compile_explore_fixture(
        root.path(), "non-square-fixture", 12,
        {.source_width = 48, .source_height = 24, .compiled_width = 32U, .compiled_height = 32U, .resize_mode = resize_mode});
    std::filesystem::remove_all(root.path() / "non-square-fixture" / "dataset");
    const data::CompiledDatasetInfo info = data::inspect_compiled_dataset(compiled);
    REQUIRE(info.image_count == 12U);
    REQUIRE(info.width == 32U);
    REQUIRE(info.height == 32U);
    REQUIRE(info.class_names().front() == "person");
    const mmltk::backend::data::CompiledDataset store = mmltk::backend::data::CompiledDataset::open(compiled);
    const mmltk::backend::data::CompiledDataset non_square_store = mmltk::backend::data::CompiledDataset::open(non_square_compiled);
    const std::vector<explore::ExploreImageSummary> summaries = explore::build_explore_summaries(store);
    REQUIRE(summaries.size() == 12U);
    CHECK(summaries[0].instance_count == 0U);
    CHECK(summaries[10].original_width == 32U);
    CHECK(summaries[10].original_height == 32U);
    CHECK(summaries[10].instance_count == 1U);
    CHECK(summaries[10].has_masks);
    CHECK((summaries[10].classes[0] & 1U) != 0U);
    REQUIRE(non_square_store.class_names().size() >= 2U);
    CHECK(non_square_store.class_names()[0] == "person");
    CHECK(non_square_store.class_names()[1] == "ret");
    const auto letterbox = non_square_store.geometry(10U);
    CHECK(letterbox.resized_width == 32U);
    CHECK(letterbox.resized_height == (resize_mode == mmltk::backend::imaging::resample::ImageResizeMode::Letterbox ? 16U : 32U));
    CHECK(letterbox.offset_x == 0U);
    CHECK(letterbox.offset_y == (resize_mode == mmltk::backend::imaging::resample::ImageResizeMode::Letterbox ? 8U : 0U));
    for (const std::uint32_t image : {10U, 11U}) {
        const auto labels = non_square_store.image_labels(image);
        REQUIRE(labels.size() == 1U);
        CHECK(labels.front().class_id == image - 10U);
        CHECK(std::abs(labels.front().bbox_x1 - 20.0F / 3.0F) < 1e-5F);
        CHECK(std::abs(labels.front().bbox_y1 - (10.0F * static_cast<float>(letterbox.resized_height) / 24.0F + static_cast<float>(letterbox.offset_y))) <
              1e-5F);
        CHECK(labels.front().bbox_x2 == 20.0F);
        CHECK(std::abs(labels.front().bbox_y2 - (23.0F * static_cast<float>(letterbox.resized_height) / 24.0F + static_cast<float>(letterbox.offset_y))) <
              1e-5F);
        const auto runs = non_square_store.instance_rle(labels.front());
        REQUIRE_FALSE(runs.empty());
        CHECK(runs.front().start >= letterbox.offset_y * 32U);
        CHECK(runs.back().start + runs.back().length <= (letterbox.offset_y + letterbox.resized_height) * 32U);
    }
    explore::ExploreSampleFilter filter;
    filter.require_boxes = true;
    std::vector<std::uint32_t> first_order;
    std::vector<std::uint32_t> first_scratch;
    std::vector<std::uint32_t> second_order;
    std::vector<std::uint32_t> second_scratch;
    REQUIRE(explore::rebuild_explore_order(summaries, filter, true, 0x1234U, first_order, first_scratch));
    REQUIRE(explore::rebuild_explore_order(summaries, filter, true, 0x1234U, second_order, second_scratch));
    CHECK(first_order == second_order);
    REQUIRE(first_order.size() == 2U);
    std::vector<std::uint32_t> sorted_order = first_order;
    std::ranges::sort(sorted_order);
    CHECK(sorted_order == std::vector<std::uint32_t>{10U, 11U});
    const explore::ExploreViewport gallery_viewport{
        .first_row = 0U,
        .row_count = 1U,
        .columns = 2U,
    };
    const auto layout = explore::make_explore_atlas_layout(first_order.size(), gallery_viewport, 32U);
    REQUIRE(layout);
    CHECK(layout->width == 64U);
    CHECK(layout->height == 32U);
    std::array<std::uint32_t, 2U> visible_work{};
    REQUIRE(explore::prioritize_explore_work(first_order, gallery_viewport, first_order.front(), 0U, visible_work) == visible_work.size());
    CHECK(visible_work.front() == first_order.front());
    const auto adjacent = explore::adjacent_explore_index(first_order, first_order.front(), true);
    REQUIRE(adjacent);
    CHECK(store.image_pixels(*adjacent) != nullptr);
    CHECK(store.image_entry(*adjacent).pixel_offset != 0U);
    constexpr std::array<std::uint32_t, 3U> navigation{7U, 11U, 19U};
    CHECK(explore::adjacent_explore_index(navigation, 7U, false) == 19U);
    CHECK(explore::adjacent_explore_index(navigation, 19U, true) == 7U);
    CHECK_FALSE(explore::adjacent_explore_index(navigation, 3U, true).has_value());
    const std::vector<std::uint32_t> retained_order = first_order;
    std::atomic<bool> cancelled{true};
    CHECK_FALSE(explore::rebuild_explore_order(summaries, filter, false, 0U, first_order, first_scratch, &cancelled));
    CHECK(first_order == retained_order);
    controller::SettingsSystem settings;
    load_explore_transport(settings, root.path() / "gui.json", h2d);
    controller::contracts::SettingsUpdateRequest deterministic_augmentation;
    deterministic_augmentation.updates.push_back({
        .path = "workflows.train.request.gpu_augmentation.copy_paste_probability",
        .value = mmltk::frameworks::serialization::wire::FlatValue{1.0},
    });
    static_cast<void>(settings.Update(std::move(deterministic_augmentation)));
    NativeExploreFixture fixture(settings, h2d);
    auto& audit = fixture.audit;
    auto& system = fixture.system;
    const controller::ExploreViewport first_view{
        .extent = {64U, 32U},
        .row_count = 1U,
        .columns = 2U,
    };
    static_cast<void>(system.Open({.viewport = first_view, .compiled_source = compiled.string()}));
    wait_for_native_gallery(audit, system, 0U, 0U);
    REQUIRE(audit.Wait([&] { return audit.last_ready_frame() > audit.first_ready_frame(); }));
    audit.CheckInitialOrdering();
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{0U, 1U});
    CHECK(system.snapshot().gallery.generation != 0U);
    CHECK(system.snapshot().gallery.slots == std::vector<bool>{true, true});
    REQUIRE(audit.Wait([&] { return (audit.prefetched_indices() & ((1U << 2U) | (1U << 3U))) == ((1U << 2U) | (1U << 3U)); }));
    { check_published_frame(system); }
    const auto enlarged_placeholders = audit.placeholder_count();
    const auto enlarged_tiles = audit.tile_count();
    system.UpdateViewport({.viewport = {.extent = {64U, 64U}, .row_count = 2U, .columns = 2U}});
    wait_for_native_gallery(audit, system, enlarged_placeholders, enlarged_tiles, 4U);
    const auto reused_before_overlap = audit.reused_tiles();
    const auto overlap_placeholders = audit.placeholder_count();
    const auto overlap_tiles = audit.tile_count();
    system.UpdateViewport({.viewport = {.extent = {64U, 64U}, .first_row = 1U, .row_count = 2U, .columns = 2U}});
    wait_for_native_gallery(audit, system, overlap_placeholders, overlap_tiles, 4U);
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{2U, 3U, 4U, 5U});
    CHECK(audit.reused_tiles() >= reused_before_overlap + 2U);
    check_published_frame(system);
    const auto restore_placeholders = audit.placeholder_count();
    const auto restore_tiles = audit.tile_count();
    system.UpdateViewport({.viewport = first_view});
    wait_for_native_gallery(audit, system, restore_placeholders, restore_tiles);
    // Materialize all twelve fixture images, then challenge every published
    // readiness set during five/six/five row oscillation at unchanged extent.
    const auto warm_placeholders = audit.placeholder_count();
    const auto warm_tiles = audit.tile_count();
    system.UpdateViewport({.viewport = {.extent = {64U, 192U}, .row_count = 6U, .columns = 2U}});
    wait_for_native_gallery(audit, system, warm_placeholders, warm_tiles, 12U);
    const auto warm_reads = audit.ReadAdmissions();
    audit.RequireRetainedReady(true);
    for (const auto rows : {5U, 6U, 5U}) {
        const auto previous_placeholders = audit.placeholder_count();
        const auto previous_tiles = audit.tile_count();
        system.UpdateViewport({.viewport = {.extent = {64U, rows * 32U}, .row_count = rows, .columns = 2U}});
        wait_for_native_gallery(audit, system, previous_placeholders, previous_tiles, rows * 2U);
        CHECK_FALSE(audit.LostRetainedReady());
        CHECK(audit.ReadAdmissions() == warm_reads);
        check_published_frame(system);
    }
    audit.RequireRetainedReady(false);
    auto placeholder_count = audit.placeholder_count();
    auto tile_count = audit.tile_count();
    system.UpdateViewport({.viewport = {.extent = {64U, 32U}, .first_row = 1U, .row_count = 1U, .columns = 2U}});
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{2U, 3U});
    CHECK(audit.augmentation_count() == 0U);
    const auto stable_order = system.snapshot().order.visible_indices;
    const auto stable_shuffle_seed = system.snapshot().order.shuffle_seed;
    auto preview_admission = system.UpdateAugmentation({.enabled = true});
    const bool preview_completed =
        audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > preview_admission.revision && audit.augmentation_count() != 0U; });
    const auto preview_state = system.snapshot();
    INFO("preview busy=" << preview_state.busy << " revision=" << preview_state.revision << " admission=" << preview_admission.revision
                         << " enabled=" << preview_state.augmentation.enabled << " failure=" << preview_state.failure
                         << " placeholders=" << audit.placeholder_count() << " tiles=" << audit.tile_count() << " augmentations=" << audit.augmentation_count()
                         << " render_flags=" << audit.last_render_flags() << " configured=" << settings.explore_settings_candidate().augmentation.enabled);
    REQUIRE(preview_completed);
    const auto first_preview_count = audit.augmentation_count();
    preview_admission = system.RerollAugmentation();
    REQUIRE(audit.Wait([&] {
        return !system.snapshot().busy && system.snapshot().revision > preview_admission.revision && audit.augmentation_count() > first_preview_count;
    }));
    CHECK(system.snapshot().augmentation.seed == 1U);
    CHECK(audit.last_augmentation_seed() == 1U);
    CHECK(system.snapshot().order.visible_indices == stable_order);
    CHECK(system.snapshot().order.shuffle_seed == stable_shuffle_seed);
    // Submit the final replacement sequence, then provide no further input.
    // Native completion events alone must settle the exact visible generation.
    static_cast<void>(system.UpdateAugmentation({.enabled = false}));
    static_cast<void>(system.UpdateAugmentation({.enabled = true}));
    const auto final_preview = system.RerollAugmentation();
    REQUIRE(audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return snapshot.revision > final_preview.revision && snapshot.augmentation.enabled && snapshot.augmentation.seed == 2U &&
               snapshot.gallery.generation == system.LastInteractionGeneration() && snapshot.gallery.slots.size() == snapshot.order.visible_indices.size() &&
               std::ranges::all_of(snapshot.gallery.slots, [](auto ready) { return ready == 1U; });
    }));
    check_published_frame(system);
    // CLEANUP-IGNORE: Augmentation disable must be observed at this native-gallery admission boundary before checking pixel work.
    preview_admission = system.UpdateAugmentation({.enabled = false});
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > preview_admission.revision; }));
    const auto disabled_augmentation_count = audit.augmentation_count();
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    system.UpdateViewport({.viewport = {.extent = {64U, 32U}, .first_row = 0U, .row_count = 1U, .columns = 2U}});
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(audit.augmentation_count() == disabled_augmentation_count);
    REQUIRE_FALSE(system.snapshot().order.visible_indices.empty());
    const auto retained_gallery = system.snapshot();
    const auto selected_image = system.snapshot().order.visible_indices.front();
    const auto detail_admission = system.Select({.compiled_index = selected_image});
    REQUIRE(audit.Wait([&] { return audit.last_ready_frame() > detail_admission.frame.revision; }));
    const auto detail = system.snapshot();
    CHECK_FALSE(detail.busy);
    CHECK(detail.revision > detail_admission.revision);
    CHECK(detail.mode == controller::ExploreMode::Detail);
    CHECK(detail.selected_image == selected_image);
    CHECK(detail.scene.objects.empty());
    check_published_frame(system);
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    const auto returned = system.CloseDetail();
    REQUIRE(audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return snapshot.revision > returned.revision && snapshot.mode == controller::ExploreMode::Gallery && snapshot.frame == retained_gallery.frame &&
               snapshot.gallery.slots == retained_gallery.gallery.slots;
    }));
    CHECK(system.snapshot().gallery.layout == retained_gallery.gallery.layout);
    CHECK(audit.placeholder_count() == placeholder_count);
    CHECK(audit.tile_count() == tile_count);
    check_published_frame(system);
    CHECK(system.snapshot().mode == controller::ExploreMode::Gallery);
    CHECK_FALSE(audit.failed());
    CHECK(system.snapshot().failure.empty());
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    static_cast<void>(system.UpdateFilter({
        .filter = {.require_boxes = true},
        .overlay = {},
    }));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{10U, 11U});
    CHECK(audit.observed_semantic_diagnostics());
    const auto retained_staging = audit.last_tile_staging();
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    static_cast<void>(system.Open({.viewport = first_view, .compiled_source = compiled.string()}));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(system.snapshot().ready);
    CHECK(audit.last_tile_staging() >= retained_staging);
    CHECK_FALSE(audit.failed());
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    static_cast<void>(system.Open({.viewport = first_view, .compiled_source = non_square_compiled.string()}));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(system.snapshot().augmentation.seed == 0U);
    CHECK_FALSE(system.snapshot().augmentation.enabled);
    check_published_frame(system);
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    auto descriptor_count = audit.descriptor_count();
    auto semantic_count = audit.semantic_count();
    static_cast<void>(system.UpdateFilter({
        .filter = {.minimum_compiled_index = 10U, .maximum_compiled_index = 11U, .require_boxes = true, .require_masks = true},
        .overlay = {.class_selection = {.mode = controller::ExploreClassSelectionMode::Subset, .classes = {0U}}, .show_boxes = true, .show_masks = true},
    }));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    const bool semantic_ready = audit.Wait(
        [&] { return audit.descriptor_count() > descriptor_count && audit.semantic_count() > semantic_count && audit.semantic_nonzero_cards() != 0U; });
    INFO("descriptor count=" << audit.descriptor_count() << " before=" << descriptor_count << " annotations=" << audit.last_descriptor_annotations()
                             << " rle=" << audit.last_descriptor_rle() << " semantic count=" << audit.semantic_count() << " before=" << semantic_count
                             << " pixels=" << audit.last_semantic_pixels() << " nonzero cards=" << audit.semantic_nonzero_cards()
                             << " failed=" << audit.failed() << " placeholder generation=" << audit.last_placeholder_generation()
                             << " tile generation=" << audit.last_tile_generation());
    REQUIRE(semantic_ready);
    CHECK(system.snapshot().order.matching_count == 2U);
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{10U, 11U});
    CHECK(audit.last_descriptor_annotations() == 2U);
    CHECK(audit.last_descriptor_rle() != 0U);
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    semantic_count = audit.semantic_count();
    static_cast<void>(system.UpdateFilter({
        .filter = system.snapshot().filter,
        .overlay = {.class_selection = {.mode = controller::ExploreClassSelectionMode::None}, .show_boxes = true, .show_masks = true},
    }));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    REQUIRE(audit.Wait([&] { return audit.semantic_count() > semantic_count; }));
    CHECK(audit.semantic_nonzero_cards() == 0U);
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    semantic_count = audit.semantic_count();
    static_cast<void>(system.UpdateFilter({
        .filter = system.snapshot().filter,
        .overlay = {.show_boxes = true, .show_masks = true},
    }));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    const bool restored_semantics = audit.Wait([&] { return audit.semantic_count() > semantic_count && audit.semantic_nonzero_cards() != 0U; });
    INFO("restored semantic count=" << audit.semantic_count() << " before=" << semantic_count << " nonzero cards=" << audit.semantic_nonzero_cards()
                                    << " generation=" << audit.last_tile_generation() << " failure=" << audit.failure_detail()
                                    << " snapshot failure=" << system.snapshot().failure);
    REQUIRE(restored_semantics);
    const auto source_annotation_count = audit.last_descriptor_annotations();
    const auto source_rle_count = audit.last_descriptor_rle();
    const auto seed_zero_image_count = audit.image_pixel_count();
    const auto donor_descriptor_count = audit.donor_descriptor_count();
    const auto seed_zero_augmentation_count = audit.augmentation_count();
    auto augmented = system.UpdateAugmentation({.enabled = true});
    const bool seed_zero_ready = audit.Wait([&] {
        return !system.snapshot().busy && system.snapshot().revision > augmented.revision && audit.last_augmentation_seed() == 0U &&
               audit.image_pixel_count() >= seed_zero_image_count + 2U && audit.donor_descriptor_count() > donor_descriptor_count &&
               audit.last_tile_generation() == audit.last_placeholder_generation() && audit.last_tile_cumulative() == 2U;
    });
    const auto seed_zero_state = system.snapshot();
    INFO("seed-zero busy=" << seed_zero_state.busy << " revision=" << seed_zero_state.revision << " admission=" << augmented.revision
                           << " failure=" << seed_zero_state.failure << " placeholders=" << audit.placeholder_count() << " tiles=" << audit.tile_count()
                           << " placeholder generation=" << audit.last_placeholder_generation() << " tile generation=" << audit.last_tile_generation()
                           << " cumulative tiles=" << audit.last_tile_cumulative() << " augmentations=" << audit.augmentation_count()
                           << " before=" << seed_zero_augmentation_count << " images=" << audit.image_pixel_count() << " before=" << seed_zero_image_count
                           << " donors=" << audit.donor_descriptor_count() << " before=" << donor_descriptor_count << " donor annotations="
                           << audit.donor_count() << " donor rle=" << audit.donor_rle() << " render flags=" << audit.last_render_flags()
                           << " valid donors=" << audit.last_valid_donors() << " planned pastes=" << audit.last_planned_pastes()
                           << " copy-paste probability=" << settings.explore_settings_candidate().augmentation.copy_paste_probability);
    REQUIRE(seed_zero_ready);
    const auto augmented_frame = system.snapshot().frame.revision;
    const std::array augmented_checksums{audit.image_checksum(0U), audit.image_checksum(1U)};
    REQUIRE(std::ranges::all_of(augmented_checksums, [](const auto checksum) { return checksum != 0U; }));
    CHECK(audit.image_seed(0U) == 0U);
    CHECK(audit.image_seed(1U) == 0U);
    CHECK(audit.donor_count() != 0U);
    CHECK(audit.donor_rle() != 0U);
    CHECK(audit.last_descriptor_annotations() > source_annotation_count);
    CHECK(audit.last_descriptor_rle() > source_rle_count);
    check_published_frame(system);
    REQUIRE_FALSE(system.snapshot().labels.empty());
    const auto augmented_labels = system.snapshot().labels;
    const auto image_pixel_count = audit.image_pixel_count();
    placeholder_count = audit.placeholder_count();
    augmented = system.RerollAugmentation();
    REQUIRE(audit.Wait(
        [&] { return audit.GalleryPublishedAfter(system, augmented.revision, image_pixel_count, placeholder_count) && audit.last_augmentation_seed() == 1U; }));
    const auto pending_replacement = audit.pending_snapshot();
    REQUIRE(pending_replacement.has_value());
    CHECK(pending_replacement->gallery.generation == system.snapshot().gallery.generation);
    // The next seed is pending, while completed pixels and their exact labels
    // remain drawable until each replacement tile is ready.
    CHECK(std::ranges::equal(pending_replacement->labels, augmented_labels, [](const auto& actual, const auto& expected) {
        return actual.box == expected.box && actual.category == expected.category && actual.compiled_index == expected.compiled_index;
    }));
    CHECK(system.snapshot().gallery.slots == std::vector<bool>{true, true});
    CHECK_FALSE(system.snapshot().labels.empty());
    CHECK(system.snapshot().frame.revision > augmented_frame);
    CHECK(system.snapshot().augmentation.seed == 1U);
    CHECK(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{10U, 11U});
    CHECK(audit.image_seed(0U) == 1U);
    CHECK(audit.image_seed(1U) == 1U);
    const std::array rerolled_checksums{audit.image_checksum(0U), audit.image_checksum(1U)};
    CHECK(rerolled_checksums[0] != augmented_checksums[0]);
    CHECK(rerolled_checksums[1] != augmented_checksums[1]);
    check_published_frame(system);
    const auto augmentation_seed = system.snapshot().augmentation.seed;
    const auto shuffled = system.UpdateFilter({
        .filter = {.minimum_compiled_index = 10U,
                   .maximum_compiled_index = 11U,
                   .order = controller::ExploreOrder::Shuffled,
                   .require_boxes = true,
                   .require_masks = true},
        .overlay = system.snapshot().overlay,
    });
    REQUIRE(audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return !snapshot.busy && snapshot.revision > shuffled.revision && snapshot.gallery.slots.size() == snapshot.order.visible_indices.size() &&
               std::ranges::all_of(snapshot.gallery.slots, [](const bool ready) { return ready; });
    }));
    const auto order_seed = system.snapshot().order.shuffle_seed;
    const auto shuffled_order = system.snapshot().order.visible_indices;
    placeholder_count = audit.placeholder_count();
    const auto rerolled = system.Reroll();
    REQUIRE(audit.Wait([&] {
        return !system.snapshot().busy && system.snapshot().revision > rerolled.revision && audit.placeholder_count() > placeholder_count &&
               audit.last_tile_generation() == audit.last_placeholder_generation() && audit.last_tile_cumulative() == 2U;
    }));
    // Reordered indices may miss the position-keyed cache. Their product
    // identity is proved by the unchanged seed and exact per-image checksums.
    CHECK(system.snapshot().order.shuffle_seed > order_seed);
    CHECK(system.snapshot().order.visible_indices != shuffled_order);
    CHECK(system.snapshot().augmentation.seed == augmentation_seed);
    CHECK(audit.image_seed(0U) == augmentation_seed);
    CHECK(audit.image_seed(1U) == augmentation_seed);
    CHECK(audit.image_checksum(0U) == rerolled_checksums[0]);
    CHECK(audit.image_checksum(1U) == rerolled_checksums[1]);
    check_published_frame(system);
    const auto gallery = system.snapshot();
    const auto source_slot = std::ranges::find(gallery.order.visible_indices, 10U) - gallery.order.visible_indices.begin();
    std::vector<controller::ExploreLabel> gallery_labels;
    for (auto label : gallery.labels) {
        if (label.compiled_index != 10U) continue;
        label.box.first.x -= static_cast<float>(source_slot * 32U);
        label.box.second.x -= static_cast<float>(source_slot * 32U);
        gallery_labels.push_back(label);
    }
    auto detail_extent_admission = system.Select({.compiled_index = 10U});
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > detail_extent_admission.revision; }));
    CHECK((system.snapshot().frame.extent == controller::VisualExtent{.width = 32U, .height = 32U}));
    CHECK(audit.detail_checksum() == rerolled_checksums[0]);
    const auto detail_scene = system.snapshot().scene;
    REQUIRE(detail_scene.objects.size() == gallery_labels.size());
    for (std::size_t index = 0U; index < gallery_labels.size(); ++index) {
        const auto& actual = detail_scene.objects[index].box;
        const auto& expected = gallery_labels[index].box;
        CHECK(std::abs(actual.first.x - expected.first.x) < 0.00001F);
        CHECK(std::abs(actual.first.y - expected.first.y) < 0.00001F);
        CHECK(std::abs(actual.second.x - expected.second.x) < 0.00001F);
        CHECK(std::abs(actual.second.y - expected.second.y) < 0.00001F);
        CHECK(detail_scene.objects[index].category == gallery_labels[index].category);
    }
    check_published_frame(system);
    const auto full_detail = system.snapshot().frame;
    const auto clean_detail = full_detail.clean_revision;
    detail_extent_admission = system.UpdateDetail({.show_original_dimensions = true});
    CHECK(detail_extent_admission.detail.show_original_dimensions);
    CHECK(system.snapshot().frame == full_detail);
    CHECK(full_detail.source_extent == controller::VisualExtent{48U, 24U});
    const auto expected_content = resize_mode == mmltk::backend::imaging::resample::ImageResizeMode::Letterbox ? controller::VisualRegion{0U, 8U, 32U, 16U}
                                                                                                               : controller::VisualRegion{0U, 0U, 32U, 32U};
    CHECK(full_detail.content == expected_content);
    static_cast<void>(system.UpdateDetail({.show_original_dimensions = false}));
    CHECK(system.snapshot().frame == full_detail);
    static_cast<void>(system.UpdateDetail({.show_original_dimensions = true}));
    auto borrowed_document = system.BorrowDocument(full_detail);
    REQUIRE(borrowed_document.valid());
    CHECK(borrowed_document.document->scene.objects == detail_scene.objects);
    for (const auto& object : borrowed_document.document->scene.objects) CHECK(object.mask.runs.empty());
    const auto document = controller::materialize_visual_document(*borrowed_document.document, full_detail.extent, full_detail.content,
                                                                  controller::visual_materialized_extent(full_detail, true));
    CHECK(document.frame_width == 32U);
    CHECK(document.frame_height == 16U);
    CHECK(document.categories.size() == system.snapshot().dataset.class_names.size());
    borrowed_document = {};
    const auto before_semantics = audit.augmentation_count();
    const auto overlay = system.UpdateOverlay({.show_boxes = false, .show_masks = true, .show_labels = true});
    REQUIRE(audit.Wait([&] { return system.snapshot().revision > overlay.revision; }));
    CHECK(audit.augmentation_count() == before_semantics);
    CHECK(system.snapshot().frame.clean_revision == clean_detail);
    check_published_frame(system);
    const auto reset_filter = system.UpdateFilter({});
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > reset_filter.revision; }));
    const auto empty_compiled = mmltk::testsupport::compile_explore_fixture(root.path(), "empty-catalog", 2);
    placeholder_count = audit.placeholder_count();
    tile_count = audit.tile_count();
    const auto empty_augmentation_count = audit.augmentation_count();
    static_cast<void>(system.Open({.viewport = first_view, .compiled_source = empty_compiled.string()}));
    wait_for_native_gallery(audit, system, placeholder_count, tile_count);
    CHECK(system.snapshot().augmentation.enabled);
    CHECK(audit.augmentation_count() > empty_augmentation_count);
    CHECK_FALSE(audit.failed());
    CHECK(audit.last_valid_donors() == 0U);
    CHECK(audit.last_planned_pastes() == 0U);
    const auto empty_selected = system.Select({.compiled_index = 0U});
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > empty_selected.revision; }));
    CHECK(system.snapshot().scene.objects.empty());
    check_published_frame(system);
    system.Shutdown();
    CHECK(system.stopped());
}
void test_compiled_explore_magnified_tiny_mask_and_transfer() {
    mmltk::testsupport::ScopedTempDir root{"mmltk-explore-tiny-support"};
    const auto compiled =
        mmltk::testsupport::compile_explore_fixture(root.path(), "tiny", 11, {.source_width = 8, .source_height = 4, .compiled_width = 8, .compiled_height = 4},
                                                    {.objects = 1, .runs_per_object = 1, .derive_boxes_from_masks = true});
    const auto store = data::CompiledDataset::open(compiled);
    const auto labels = store.image_labels(10U);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].class_id == 0);
    CHECK(labels[0].bbox_x1 == 0.0F);
    CHECK(labels[0].bbox_y1 == 0.0F);
    CHECK(labels[0].bbox_x2 == 1.0F);
    CHECK(labels[0].bbox_y2 == 1.0F);
    const auto runs = store.instance_rle(labels[0]);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].start == 0);
    CHECK(runs[0].length == 1);
    controller::SettingsSystem settings;
    load_explore_transport(settings, root.path() / "gui.json", true);
    const auto candidate = settings.explore_settings_candidate();
    auto policy = candidate.preferences.policy;
    policy.overlay.show_boxes = true;
    policy.overlay.show_masks = true;
    static_cast<void>(settings.Update(candidate, {.preferences = policy}));
    NativeExploreFixture fixture(settings, true);
    auto& audit = fixture.audit;
    auto& system = fixture.system;
    static_cast<void>(
        system.Open({.viewport = {.extent = {32U, 32U}, .first_row = 10U, .row_count = 1U, .columns = 1U}, .compiled_source = compiled.string()}));
    wait_for_native_gallery(audit, system, 0U, 0U, 1U);
    REQUIRE(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{10U});
    namespace gpu = mmltk::frameworks::gpu;
    const auto backend = gpu::cuda_image_copy_backend();
    gpu::DeviceContext receiver_context{0, backend};
    gpu::ImageStream receiver_stream{receiver_context};
    gpu::ImageProductBuffer receiver{receiver_context, gpu::ImageProductLayout::CleanAndSemantic};
    const auto check_semantics = [&](const bool atlas) {
        const auto frame = system.snapshot().frame;
        auto source = system.BorrowFrame();
        REQUIRE(controller::visual_product_matches_frame(frame, source));
        // CopyFrom waits for source completion and settles the receiver before releasing the borrow.
        static_cast<void>(receiver.CopyFrom(receiver_stream, std::move(source)));
        const auto completed = receiver.Borrow();
        const auto plane = completed.plane(1).plane();
        const auto width = atlas ? 32U : 8U;
        const auto height = atlas ? 32U : 4U;
        REQUIRE(plane.descriptor.width == width);
        REQUIRE(plane.descriptor.height == height);
        std::vector<std::array<std::uint8_t, 4>> pixels(width * height);
        receiver_context.Bind();
        CUcontext context = nullptr;
        REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
        backend->CopyDeviceToHost(reinterpret_cast<std::uintptr_t>(context), plane, pixels.data(), width * 4U);
        const auto scale = atlas ? 4U : 1U;
        const auto image_top = atlas ? 8U : 0U;
        const auto image_bottom = image_top + 4U * scale;
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                CAPTURE(atlas, x, y);
                const bool supported = x < scale && y >= image_top && y < image_top + scale;
                const bool border = y >= image_top && y < image_bottom && ((x == scale && y <= image_top + scale) || (y == image_top + scale && x <= scale));
                const std::array<std::uint8_t, 4> expected = supported ? std::array<std::uint8_t, 4>{255, 0, 0, 92}
                                                             : border  ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                                                                       : std::array<std::uint8_t, 4>{0, 0, 0, 0};
                CHECK(pixels[y * width + x] == expected);
            }
        CHECK(system.snapshot().frame == frame);
    };
    check_semantics(true);
    const auto previous = system.snapshot().frame;
    static_cast<void>(system.Select({.compiled_index = 10U}));
    REQUIRE(audit.Wait(
        [&] { return !system.snapshot().busy && system.snapshot().mode == controller::ExploreMode::Detail && system.snapshot().frame != previous; }));
    check_semantics(false);
    const auto detail = system.snapshot().frame;
    {
        auto borrowed = system.BorrowDocument(detail);
        REQUIRE(borrowed.valid());
        const auto editable = controller::materialize_visual_document(*borrowed.document, detail.extent, detail.content);
        REQUIRE(editable.objects.size() == 1);
        const auto& object = editable.objects[0];
        CHECK(object.box.first.x == 0.0F);
        CHECK(object.box.first.y == 0.0F);
        CHECK(object.box.second.x == 1.0F);
        CHECK(object.box.second.y == 1.0F);
        REQUIRE(object.mask.runs.size() == 1);
        CHECK(object.mask.runs[0] == controller::contracts::AnnotationMaskRun{0, 0, 0});
        CHECK(system.snapshot().frame == detail);
    }
    CHECK_FALSE(audit.failed());
    system.Shutdown();
    CHECK(system.stopped());
}
void test_compiled_explore_optional_donors_respect_source_capacity() {
    const bool h2d = GENERATE(true, false);
    INFO("h2d_dataloader=" << h2d);
    require_explore_transport(h2d);
    for (const auto annotations : std::array{mmltk::testsupport::ExploreFixtureAnnotations{controller::contracts::kAnnotationObjectCapacity, 1U},
                                             mmltk::testsupport::ExploreFixtureAnnotations{controller::contracts::kAnnotationObjectCapacity - 1U, 1U},
                                             mmltk::testsupport::ExploreFixtureAnnotations{controller::contracts::kAnnotationMaskRunCapacity / 32U, 32U},
                                             mmltk::testsupport::ExploreFixtureAnnotations{controller::contracts::kAnnotationMaskRunCapacity / 32U - 1U, 32U},
                                             mmltk::testsupport::ExploreFixtureAnnotations{.objects = 2U, .runs_per_object = 1U, .crowd_only = true},
                                             mmltk::testsupport::ExploreFixtureAnnotations{.objects = 2U, .runs_per_object = 1U, .mixed_crowd = true},
                                             mmltk::testsupport::ExploreFixtureAnnotations{.objects = 1U, .runs_per_object = 0U}}) {
        CAPTURE(annotations.objects, annotations.runs_per_object);
        mmltk::testsupport::ScopedTempDir root{"mmltk-explore-capacity"};
        const auto compiled = mmltk::testsupport::compile_explore_fixture(root.path(), "dense", 12, {}, annotations);
        const auto store = mmltk::backend::data::CompiledDataset::open(compiled);
        const auto labels = store.image_labels(10U);
        REQUIRE(labels.size() == annotations.objects);
        for (const auto& label : labels) CHECK(label.has_mask());
        if (annotations.crowd_only) CHECK(std::ranges::all_of(labels, [](const auto& label) { return label.is_crowd(); }));
        std::size_t runs = 0U;
        for (const auto& label : labels) runs += store.instance_rle(label).size();
        REQUIRE(runs == annotations.objects * annotations.runs_per_object);
        const bool donor_fits = !annotations.crowd_only && annotations.objects < controller::contracts::kAnnotationObjectCapacity &&
                                annotations.runs_per_object <= controller::contracts::kAnnotationMaskRunCapacity - runs;
        controller::SettingsSystem settings;
        load_explore_transport(settings, root.path() / "gui.json", h2d);
        controller::contracts::SettingsUpdateRequest update;
        update.updates = {
            {.path = "workflows.train.request.gpu_augmentation.enabled", .value = mmltk::frameworks::serialization::wire::FlatValue{true}},
            {.path = "workflows.train.request.gpu_augmentation.copy_paste_probability", .value = mmltk::frameworks::serialization::wire::FlatValue{1.0}},
            {.path = "workflows.explore.show_labels", .value = mmltk::frameworks::serialization::wire::FlatValue{false}},
        };
        static_cast<void>(settings.Update(std::move(update)));
        const auto candidate = settings.explore_settings_candidate();
        auto policy = candidate.preferences.policy;
        policy.filter.minimum_instances = 1U;
        static_cast<void>(settings.Update(candidate, {.preferences = policy}));
        NativeExploreFixture subject{settings, h2d};
        auto& audit = subject.audit;
        auto& system = subject.system;
        static_cast<void>(system.UpdateAugmentation({.enabled = true}));
        static_cast<void>(system.Open({.viewport = {.extent = {64U, 32U}, .row_count = 1U, .columns = 2U}, .compiled_source = compiled.string()}));
        wait_for_native_gallery(audit, system, 0U, 0U);
        REQUIRE(system.snapshot().order.visible_indices == std::vector<std::uint32_t>{10U, 11U});
        REQUIRE(audit.Wait([&] { return audit.augmentation_count() != 0U; }));
        CHECK_FALSE(audit.failed());
        CHECK((audit.last_valid_donors() != 0U) == donor_fits);
        if (!donor_fits) CHECK(audit.last_planned_pastes() == 0U);
        check_published_frame(system);
        const auto previous = system.snapshot().frame;
        static_cast<void>(system.Select({.compiled_index = 10U}));
        REQUIRE(audit.Wait([&] { return system.snapshot().mode == controller::ExploreMode::Detail && system.snapshot().frame != previous; }));
        CHECK_FALSE(audit.failed());
        CHECK(system.snapshot().scene.objects.size() <= annotations.objects + (donor_fits ? 1U : 0U));
        const auto augmented_frame = system.snapshot().frame;
        const auto disabled = system.UpdateAugmentation({.enabled = false});
        REQUIRE(audit.Wait(
            [&] { return !system.snapshot().busy && system.snapshot().revision > disabled.revision && system.snapshot().frame != augmented_frame; }));
        // Identity projection retains the complete source and its compact RLE,
        // including the exact per-image object and run boundaries.
        const auto identity = system.snapshot();
        REQUIRE(identity.scene.objects.size() == annotations.objects);
        auto borrowed = system.BorrowDocument(identity.frame);
        REQUIRE(borrowed.valid());
        const auto editable = controller::materialize_visual_document(*borrowed.document, identity.frame.extent, identity.frame.content);
        REQUIRE(editable.objects.size() == annotations.objects);
        std::size_t editable_runs = 0U;
        for (std::size_t index = 0; index < editable.objects.size(); ++index) {
            const auto& object = editable.objects[index];
            editable_runs += object.mask.runs.size();
            const auto& compiled_label = labels[index];
            CHECK(object.box.first.x == compiled_label.bbox_x1);
            CHECK(object.box.first.y == compiled_label.bbox_y1);
            CHECK(object.box.second.x == compiled_label.bbox_x2);
            CHECK(object.box.second.y == compiled_label.bbox_y2);
        }
        CHECK(editable_runs == runs);
        borrowed = {};
        check_published_frame(system);
        const auto placeholder_count = audit.placeholder_count();
        const auto tile_count = audit.tile_count();
        static_cast<void>(system.CloseDetail());
        wait_for_native_gallery(audit, system, placeholder_count, tile_count);
        const auto gallery = system.snapshot();
        REQUIRE(gallery.order.visible_indices.size() == 2U);
        CHECK(gallery.gallery.slots == std::vector<bool>{true, true});
        REQUIRE(gallery.labels.size() == gallery.order.visible_indices.size() * annotations.objects);
        for (const auto index : gallery.order.visible_indices) {
            const auto count = std::ranges::count_if(gallery.labels, [index](const auto& label) { return label.compiled_index == index; });
            CHECK(std::cmp_equal(count, annotations.objects));
        }
        for (const auto& label : gallery.labels) {
            const float left = label.compiled_index == gallery.order.visible_indices.front() ? 0.0F : 32.0F;
            REQUIRE(label.box.first.x == left + labels.front().bbox_x1);
            REQUIRE(label.box.first.y == labels.front().bbox_y1);
            REQUIRE(label.box.second.x == left + labels.front().bbox_x2);
            REQUIRE(label.box.second.y == labels.front().bbox_y2);
            REQUIRE(label.category == labels.front().class_id);
        }
        namespace cbor = mmltk::frameworks::serialization;
        constexpr cbor::wire::Limits limits{.max_bytes = controller::browser::kMaxOutputValueBytes, .max_items = controller::browser::kMaxOutputValueItems};
        cbor::wire::ByteBuffer encoded;
        REQUIRE(cbor::encode(gallery, encoded, limits));
        const auto decoded = cbor::decode<controller::ExploreSnapshot>({.first = encoded}, limits);
        REQUIRE(decoded);
        CHECK(decoded->labels.size() == gallery.labels.size());
        CHECK(decoded->gallery.slots == gallery.gallery.slots);
        CHECK(decoded->gallery.generation == gallery.gallery.generation);
        CHECK(decoded->frame == gallery.frame);
        CHECK_FALSE(audit.failed());
        system.Shutdown();
    }
}
void test_compiled_explore_ring_holes_survive_hidden_donor_and_transfer() {
    const bool h2d = GENERATE(true, false);
    require_explore_transport(h2d);
    mmltk::testsupport::ScopedTempDir root{"mmltk-explore-ring"};
    const auto compiled = mmltk::testsupport::compile_explore_fixture(
        root.path(), "ring", 2, {.source_width = 8, .source_height = 8, .compiled_width = 8, .compiled_height = 8}, {.ring_and_dots = true});
    const auto store = data::CompiledDataset::open(compiled);
    REQUIRE(store.image_labels(0).size() == 7);
    REQUIRE(store.image_labels(1).size() == 1);
    const auto donor_class = store.image_labels(1).front().class_id;
    const auto source_class = store.image_labels(0).front().class_id;
    namespace fixture = mmltk::backend::models::rfdetr::test_support;
    for (std::size_t i = 0; i < fixture::dot_runs.size(); ++i) {
        const auto compiled_runs = store.instance_rle(store.image_labels(0)[i]);
        REQUIRE(compiled_runs.size() == 1);
        CHECK(compiled_runs[0].start == fixture::dot_runs[i].start);
        CHECK(compiled_runs[0].length == fixture::dot_runs[i].length);
    }
    const auto compiled_ring = store.instance_rle(store.image_labels(1).front());
    REQUIRE(compiled_ring.size() == fixture::ring_runs.size());
    for (std::size_t i = 0; i < compiled_ring.size(); ++i) {
        CHECK(compiled_ring[i].start == fixture::ring_runs[i].start);
        CHECK(compiled_ring[i].length == fixture::ring_runs[i].length);
    }
    controller::SettingsSystem settings;
    load_explore_transport(settings, root.path() / "gui.json", h2d);
    controller::contracts::SettingsUpdateRequest update;
    using Value = mmltk::frameworks::serialization::wire::FlatValue;
    update.updates = {{.path = "workflows.train.request.gpu_augmentation.copy_paste_probability", .value = Value{1.0}}};
    for (const auto group : {"geometry", "resize", "color", "noise", "blur", "occlusion"})
        update.updates.push_back({.path = std::string{"workflows.train.request.gpu_augmentation."} + group + ".probability", .value = Value{0.0}});
    (void)settings.Update(std::move(update));
    NativeExploreFixture subject{settings, h2d};
    auto& audit = subject.audit;
    auto& system = subject.system;
    (void)system.UpdateAugmentation({.enabled = true});
    (void)system.Open({.viewport = {.extent = {16, 8}, .row_count = 1, .columns = 2}, .compiled_source = compiled.string()});
    wait_for_native_gallery(audit, system, 0, 0);
    const auto gallery = system.snapshot();
    const auto selected = system.Select({.compiled_index = 0});
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > selected.revision; }));
    const auto frame = system.snapshot().frame;
    auto borrowed = system.BorrowDocument(frame);
    REQUIRE(borrowed.valid());
    const auto editable = controller::materialize_visual_document(*borrowed.document, frame.extent, frame.content);
    REQUIRE_FALSE(editable.objects.empty());
    REQUIRE(editable.objects.back().category == donor_class);
    const auto mask_bits = [](const controller::contracts::AnnotationObject& object) {
        std::uint64_t bits = 0;
        for (const auto& run : object.mask.runs)
            for (auto x = run.first; x <= run.last; ++x) {
                REQUIRE(run.row < 8);
                REQUIRE(x < 8);
                bits |= 1ULL << (run.row * 8 + x);
            }
        return bits;
    };
    // Physical donor support is observed through the exported mask; source
    // expectations are explicit compiled pixels minus that actual footprint.
    const auto donor_bits = mask_bits(editable.objects.back());
    REQUIRE(donor_bits != 0);
    std::size_t retained = 0;
    for (const auto run : fixture::dot_runs) {
        const auto source_bits = ((1ULL << run.length) - 1) << run.start;
        const auto expected = source_bits & ~donor_bits;
        if (expected == 0) continue;
        REQUIRE(retained < editable.objects.size() - 1);
        const auto& object = editable.objects[retained++];
        CHECK(object.category == source_class);
        CHECK(mask_bits(object) == expected);
        int x0 = 8, y0 = 8, x1 = 0, y1 = 0;
        for (int p = 0; p < 64; ++p)
            if ((expected & (1ULL << p)) != 0) {
                x0 = std::min(x0, p % 8);
                y0 = std::min(y0, p / 8);
                x1 = std::max(x1, p % 8 + 1);
                y1 = std::max(y1, p / 8 + 1);
            }
        CHECK(object.box.first.x == static_cast<float>(x0));
        CHECK(object.box.first.y == static_cast<float>(y0));
        CHECK(object.box.second.x == static_cast<float>(x1));
        CHECK(object.box.second.y == static_cast<float>(y1));
    }
    CHECK(retained + 1 == editable.objects.size());
    std::vector<controller::ExploreLabel> source_labels;
    for (const auto& label : gallery.labels)
        if (label.compiled_index == 0) source_labels.push_back(label);
    REQUIRE(source_labels.size() == editable.objects.size());
    for (std::size_t i = 0; i < source_labels.size(); ++i) CHECK(source_labels[i].box == editable.objects[i].box);
    borrowed = {};
    auto overlay = system.snapshot().overlay;
    overlay.class_selection = {.mode = controller::ExploreClassSelectionMode::Subset, .classes = {source_class}};
    const auto hidden = system.UpdateOverlay(overlay);
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > hidden.revision; }));
    const auto hidden_frame = system.snapshot().frame;
    CHECK(hidden_frame.clean_revision == frame.clean_revision);
    borrowed = system.BorrowDocument(hidden_frame);
    REQUIRE(borrowed.valid());
    const auto hidden_editable = controller::materialize_visual_document(*borrowed.document, hidden_frame.extent, hidden_frame.content);
    CHECK(hidden_editable.objects == editable.objects);
    borrowed = {};
    check_published_frame(system);
    CHECK_FALSE(audit.failed());
    system.Shutdown();
}
void test_compiled_explore_cancelled_lane_preserves_atomic_product() {
    const bool h2d = GENERATE(true, false);
    INFO("h2d_dataloader=" << h2d);
    require_explore_transport(h2d);
    mmltk::testsupport::ScopedTempDir root{"mmltk-explore-cancelled-retained"};
    const auto compiled = mmltk::testsupport::compile_explore_fixture(root.path(), "fixture", 6);
    controller::SettingsSystem settings;
    load_explore_transport(settings, root.path() / "gui.json", h2d);
    std::array<int, 2U> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0]};
    auto gate = std::make_shared<controller::ExploreAcceptanceGate>(sockets[1]);
    NativeExploreAudit audit;
    mmltk::testsupport::TestGate prefetch_lane{"second Explore image before viewport growth"};
    mmltk::testsupport::TestGate stale_lane{"third Explore lane before acceptance wait"};
    struct ReadObservation {
        NativeExploreAudit& audit;
        mmltk::testsupport::TestGate::Receipt stale_lane;
        mmltk::testsupport::TestGate::Receipt prefetch_lane;
        std::atomic_uint64_t started{0U};
        std::atomic_size_t discarded{0U};
        std::atomic_bool hold_third{false};
        std::atomic_uint64_t held_generation{0U};
    } reads{audit, stale_lane.receipt(), prefetch_lane.receipt()};
    gate->SetReadObserver(&reads, [](void* context, std::uint64_t, const std::uint32_t index) {
        if (index == 1U) static_cast<ReadObservation*>(context)->prefetch_lane.ArriveAndWait();
    });
    gate->SetInitialWaitObserver(&reads, [](void* context, const std::uint64_t generation) {
        auto& observed = *static_cast<ReadObservation*>(context);
        if (observed.hold_third.exchange(false)) {
            observed.held_generation.store(generation);
            observed.stale_lane.ArriveAndWait();
        }
    });
    const controller::VisualDiagnosticSink diagnostics{
        .context = &reads, .write = [](void* context, const controller::VisualDiagnosticFact fact) noexcept {
            auto& observed = *static_cast<ReadObservation*>(context);
            if (fact.operation == controller::VisualDiagnosticOperation::AcceptanceLaneStarted && fact.detail < 64U)
                observed.started.fetch_or(std::uint64_t{1U} << fact.detail, std::memory_order_acq_rel);
            if (fact.operation == controller::VisualDiagnosticOperation::AcceptanceStaleReadDiscarded)
                observed.discarded.fetch_add(1U, std::memory_order_acq_rel);
            observed.audit.diagnostics()(fact);
        }};
    const controller::VisualDeviceSettings device{.device = 0, .maximum_width = 256U, .maximum_height = 256U};
    controller::ExploreSystem system{settings,
                                     device,
                                     1U,
                                     controller::make_native_explore_runtime_factory(
                                         device, 1U, {.loading = data::data_loading_options(h2d), .acceptance = gate, .diagnostics = diagnostics}),
                                     [&audit](controller::ExploreSystem::event_type event) { audit.Observe(std::move(event)); },
                                     diagnostics};
    // Stop blocked I/O before system destruction, including assertion unwinding.
    const mmltk::testsupport::ScopedTestCleanup stop{[&] {
        gate->Stop();
        stale_lane.Release();
        prefetch_lane.Release();
    }};
    const auto send = [&](const std::uint8_t command) { REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command)); };
    const auto viewport = [](const std::uint32_t rows) { return controller::ExploreViewport{.extent = {32U, 32U * rows}, .row_count = rows, .columns = 1U}; };
    static_cast<void>(system.Open({.viewport = viewport(1U), .compiled_source = compiled.string()}));
    REQUIRE(audit.Wait([&] { return (reads.started.load(std::memory_order_acquire) & 1U) != 0U; }));
    // Explore owns a blocked source worker. The training loader must still
    // complete an epoch using its independent pool and the same transport.
    data::DatasetLoader training({.compiled_path = compiled.string(),
                                  .batch_size = 2U,
                                  .shuffle = false,
                                  .prefetch_factor = 2,
                                  .gather_workers = 1,
                                  .cpu_affinity = {},
                                  .loading = data::data_loading_options(h2d)});
    training.begin_epoch();
    std::uint32_t next_image = 0U;
    data::Batch batch{};
    struct ReleaseTrainingBatch {
        data::DatasetLoader& loader;
        data::Batch& batch;
        bool leased = false;
        ~ReleaseTrainingBatch() {
            if (leased) loader.release_batch(batch);
        }
        void Release() {
            loader.release_batch(batch);
            leased = false;
        }
    } training_lease{training, batch};
    while (training.next_batch(batch)) {
        training_lease.leased = true;
        training.wait_batch(batch);
        REQUIRE(batch.device_images != nullptr);
        for (std::size_t image = 0U; image < batch.num_images; ++image) CHECK(batch.image_indices[image] == next_image++);
        training_lease.Release();
    }
    training.synchronize();
    CHECK(next_image == 6U);
    CHECK(system.snapshot().gallery.slots == std::vector<bool>{false});
    // Keep another training batch leased across atlas replacement and growth.
    // Explore cancellation cannot recycle this independent mapped allocation.
    training.begin_epoch();
    REQUIRE(training.next_batch(batch));
    training_lease.leased = true;
    training.wait_batch(batch);
    const auto* retained_training_images = batch.device_images;
    std::vector<float> retained_training_pixels(batch.num_images * training.image_stride() / sizeof(float));
    REQUIRE(cudaMemcpy(retained_training_pixels.data(), batch.device_images, retained_training_pixels.size() * sizeof(float), cudaMemcpyDeviceToHost) ==
            cudaSuccess);
    CHECK(std::ranges::equal(retained_training_pixels, training.host_images(batch)));
    send(1U);
    REQUIRE(prefetch_lane.WaitEntered(std::chrono::seconds{2}));
    REQUIRE(audit.Wait([&] { return audit.last_tile_cumulative() == 1U; }));
    // Hold the one physical lane at the second image until viewport growth
    // commits, so speculative reads cannot consume the third image first.
    reads.hold_third.store(true);
    system.UpdateViewport({.viewport = viewport(3U)});
    REQUIRE(audit.Wait([&] { return system.snapshot().gallery.slots == std::vector<bool>{true, false, false}; }));
    prefetch_lane.Release();
    const bool third_lane_entered = stale_lane.WaitEntered(std::chrono::seconds{2});
    INFO("third lane entered=" << third_lane_entered << " failure=" << audit.failure_detail() << " started=" << reads.started.load()
                               << " prefetched=" << audit.prefetched_indices() << " tiles=" << audit.last_tile_cumulative()
                               << " busy=" << system.snapshot().busy << " generation=" << system.snapshot().gallery.generation);
    REQUIRE(third_lane_entered);
    REQUIRE(audit.Wait(
        [&] { return (reads.started.load(std::memory_order_acquire) & 4U) != 0U && system.snapshot().gallery.slots == std::vector<bool>{true, true, false}; }));
    const auto reused = audit.reused_tiles();
    const auto discarded = reads.discarded.load(std::memory_order_acquire);
    const auto placeholders = audit.placeholder_count();
    system.UpdateViewport({.viewport = viewport(4U)});
    REQUIRE(audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return audit.placeholder_count() > placeholders && snapshot.gallery.slots.size() == 4U &&
               snapshot.gallery.generation == audit.last_placeholder_generation();
    }));
    CHECK(reads.held_generation.load() != system.snapshot().gallery.generation);
    // Enter the old generation's wait only after replacement commits. Its stale
    // completion must pass through the continuation, with tiles 0 and 1 retained.
    stale_lane.Release();
    send(1U);
    const bool replacement_ready = audit.Wait([&] {
        const auto snapshot = system.snapshot();
        return reads.discarded.load(std::memory_order_acquire) > discarded && audit.reused_tiles() >= reused + 2U && snapshot.gallery.slots.size() == 4U &&
               snapshot.gallery.slots[0] && snapshot.gallery.slots[1] && audit.last_tile_generation() == audit.last_placeholder_generation();
    });
    INFO("discarded=" << reads.discarded.load() << " baseline=" << discarded << " reused=" << audit.reused_tiles() << " baseline=" << reused
                      << " slots=" << system.snapshot().gallery.slots.size() << " ready slots=" << std::ranges::count(system.snapshot().gallery.slots, true)
                      << " tile generation=" << audit.last_tile_generation() << " placeholder generation=" << audit.last_placeholder_generation());
    REQUIRE(replacement_ready);
    INFO(audit.failure_detail());
    REQUIRE_FALSE(audit.failed());
    send(2U);
    REQUIRE(audit.Wait([&] { return system.snapshot().gallery.slots == std::vector<bool>(4U, true); }));
    system.UpdateViewport({.viewport = viewport(6U)});
    REQUIRE(audit.Wait([&] { return system.snapshot().gallery.slots == std::vector<bool>(6U, true); }));
    CHECK_FALSE(audit.failed());
    check_published_frame(system);
    const auto augmentations = audit.augmentation_count();
    for (const auto mode : {6U, 4U, 5U, 1U, 0U, 2U, 3U, 7U}) {
        const auto before = system.snapshot();
        auto overlay = before.overlay;
        overlay.show_boxes = (mode & 1U) != 0U;
        overlay.show_masks = (mode & 2U) != 0U;
        overlay.show_labels = (mode & 4U) != 0U;
        const bool semantics = overlay.show_boxes != before.overlay.show_boxes || overlay.show_masks != before.overlay.show_masks;
        static_cast<void>(system.UpdateOverlay(overlay));
        REQUIRE(audit.Wait([&] {
            const auto now = system.snapshot();
            return now.overlay == overlay && now.gallery.slots == std::vector<bool>(6U, true) && (!semantics || now.frame.revision > before.frame.revision);
        }));
        if (!semantics) CHECK(system.snapshot().frame == before.frame);
        CHECK(audit.augmentation_count() == augmentations);
        CHECK(batch.device_images == retained_training_images);
    }
    const auto incumbent = system.snapshot();
    const auto reused_before_reopen = audit.reused_tiles();
    static_cast<void>(system.Open({.viewport = incumbent.viewport, .compiled_source = compiled.string()}));
    REQUIRE(audit.Wait([&] {
        const auto reopened = system.snapshot();
        return !reopened.busy && reopened.frame != incumbent.frame && reopened.gallery.slots == std::vector<bool>(6U, true);
    }));
    CHECK(system.snapshot().dataset.identity == incumbent.dataset.identity);
    CHECK(system.snapshot().frame.clean_revision > incumbent.frame.clean_revision);
    CHECK(audit.reused_tiles() == reused_before_reopen);
    CHECK(batch.device_images == retained_training_images);
    gate->Stop();
    system.Shutdown();
    CHECK(batch.device_images == retained_training_images);
    std::vector<float> settled_training_pixels(retained_training_pixels.size());
    REQUIRE(cudaMemcpy(settled_training_pixels.data(), batch.device_images, settled_training_pixels.size() * sizeof(float), cudaMemcpyDeviceToHost) ==
            cudaSuccess);
    CHECK(settled_training_pixels == retained_training_pixels);
    training_lease.Release();
    while (training.next_batch(batch)) {
        training_lease.leased = true;
        training.wait_batch(batch);
        training_lease.Release();
    }
    training.synchronize();
}
void test_native_explore_transaction_faults_and_inactive_release() {
    enum class Outcome { Commit, Descriptors, CompletedProduct, Cancel };
    const auto outcome = GENERATE(Outcome::Commit, Outcome::Descriptors, Outcome::CompletedProduct, Outcome::Cancel);
    INFO("outcome=" << static_cast<unsigned>(outcome));
    require_explore_transport(true);
    mmltk::testsupport::ScopedTempDir root{"mmltk-explore-native-transaction"};
    const auto compiled = mmltk::testsupport::compile_explore_fixture(root.path(), "fixture", 4);
    controller::SettingsSystem settings;
    // CLEANUP-IGNORE: This transaction fixture uses the same transport selector as cancellation coverage but
    // establishes different socket, product-observer, and fault-boundary evidence.
    load_explore_transport(settings, root.path() / "gui.json", true);
    std::array<int, 2U> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0]};
    auto gate = std::make_shared<controller::ExploreAcceptanceGate>(sockets[1]);
    struct Observation {
        std::mutex mutex;
        std::weak_ptr<const void> prepared, released;
        std::size_t remaining = 0U, before = 0U, after = 0U;
        std::atomic_bool block = false;
        std::promise<void> entered, release;
        std::shared_future<void> resumed = release.get_future().share();
    } observation;
    gate->SetProductObserver(&observation, [](void* context, controller::ExploreAcceptanceGate::ProductObservation fact) noexcept {
        auto& observed = *static_cast<Observation*>(context);
        // Copy-count observations have no artifact and precede dataset preparation.
        const bool prepared = !fact.released && !fact.artifact.expired();
        {
            std::scoped_lock lock(observed.mutex);
            if (fact.released) {
                observed.released = std::move(fact.artifact);
                observed.remaining = fact.logical_size;
                observed.before = fact.capacity_before;
                observed.after = fact.capacity_after;
            } else if (prepared) {
                observed.prepared = std::move(fact.artifact);
            }
        }
        if (prepared && observed.block.exchange(false)) {
            observed.entered.set_value();
            observed.resumed.wait();
        }
    });
    NativeExploreAudit audit;
    const controller::VisualDeviceSettings device{.device = 0, .maximum_width = 256U, .maximum_height = 256U};
    controller::ExploreSystem system{
        settings, device, 1U, controller::make_native_explore_runtime_factory(device, 1U, {.loading = data::data_loading_options(true), .acceptance = gate}),
        [&audit](controller::ExploreSystem::event_type event) { audit.Observe(std::move(event)); }};
    const mmltk::testsupport::ScopedTestCleanup stop{[&] { gate->Stop(); }};
    const controller::ExploreViewport viewport{.extent = {64U, 32U}, .columns = 2U};
    static_cast<void>(system.Open({.viewport = viewport, .compiled_source = compiled.string()}));
    REQUIRE(audit.Wait([&] { return system.snapshot().ready && !system.snapshot().busy; }));
    const auto incumbent = system.snapshot();
    // Wake any automatic continuation already waiting for an acceptance command.
    // Terminal reads stay stale, preserving the incumbent placeholders while
    // replacement quiescence reaches the publication controls in every variant.
    gate->Stop();
    std::weak_ptr<const void> incumbent_artifact;
    {
        std::scoped_lock lock(observation.mutex);
        incumbent_artifact = observation.prepared;
    }
    REQUIRE_FALSE(incumbent_artifact.expired());
    auto held = system.BorrowFrame();
    REQUIRE(controller::visual_product_matches_frame(incumbent.frame, held));
    namespace gpu = mmltk::frameworks::gpu;
    const auto backend = gpu::cuda_image_copy_backend();
    gpu::DeviceContext receiver_context{0, backend};
    gpu::ImageStream receiver_stream{receiver_context};
    gpu::ImageProductBuffer receiver{receiver_context, gpu::ImageProductLayout::CleanAndSemantic};
    const auto pixels = [&] {
        static_cast<void>(receiver.CopyFrom(receiver_stream, system.BorrowFrame()));
        const auto read = receiver.Borrow();
        receiver_context.Bind();
        CUcontext context = nullptr;
        REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
        std::array<std::vector<std::uint8_t>, 2U> result;
        for (std::size_t index = 0U; index != result.size(); ++index) {
            const auto plane = read.plane(index).plane();
            result[index].resize(viewport.extent.width * viewport.extent.height * 4U);
            backend->CopyDeviceToHost(reinterpret_cast<std::uintptr_t>(context), plane, result[index].data(), viewport.extent.width * 4U);
        }
        return result;
    };
    const auto incumbent_pixels = pixels();
    using Stage = controller::ExploreAcceptanceGate::PublicationStage;
    if (outcome == Outcome::Descriptors) gate->FailNextPublicationAt(Stage::DescriptorsPrepared);
    if (outcome == Outcome::CompletedProduct) gate->FailNextPublicationAt(Stage::ProductPrepared);
    if (outcome == Outcome::Cancel) observation.block = true;
    const auto admitted = system.Open({.viewport = viewport, .compiled_source = compiled.string()});
    if (outcome == Outcome::Cancel) {
        const auto entered = observation.entered.get_future().wait_for(std::chrono::seconds{2});
        // Always release the worker before an assertion can unwind its owner.
        if (entered == std::future_status::ready) static_cast<void>(system.Stop());
        observation.release.set_value();
        REQUIRE(entered == std::future_status::ready);
    }
    // CLEANUP-IGNORE: This wait proves cancellation and physical settlement; catalog replacement has a separate commit oracle.
    REQUIRE(audit.Wait([&] { return !system.snapshot().busy && system.snapshot().revision > admitted.revision; }));
    const auto settled = system.snapshot();
    {
        std::scoped_lock lock(observation.mutex);
        CHECK(observation.remaining == 0U);
        CHECK(observation.before != 0U);
        CHECK(observation.after == observation.before);
        CHECK(observation.prepared.expired() == (outcome != Outcome::Commit));
    }
    CHECK(held.plane(0U).revision() == incumbent.frame.revision);
    if (outcome == Outcome::Commit) {
        CHECK(settled.frame != incumbent.frame);
    } else {
        CHECK_FALSE(incumbent_artifact.expired());
        // CLEANUP-IGNORE: These end-to-end rollback assertions intentionally restate the public continuity contract
        // across a separate compiled-data acceptance boundary.
        CHECK(settled.frame == incumbent.frame);
        CHECK(settled.dataset.identity == incumbent.dataset.identity);
        CHECK(settled.order.visible_indices == incumbent.order.visible_indices);
        CHECK(settled.viewport == incumbent.viewport);
        CHECK(settled.gallery.generation == incumbent.gallery.generation);
        CHECK(settled.gallery.slots == incumbent.gallery.slots);
        CHECK(pixels() == incumbent_pixels);
        CHECK(settled.failure.empty() == (outcome == Outcome::Cancel));
    }
    held = {};
    gate->Stop();
    system.Shutdown();
    // Clearing the inactive product drops its own references. A shared artifact
    // can still belong to the active product or an unfinished read; shutdown
    // joins those owners before final destruction is observable.
    CHECK(incumbent_artifact.expired());
    {
        std::scoped_lock lock(observation.mutex);
        CHECK(observation.released.expired());
        CHECK(observation.prepared.expired());
    }
}
}  // namespace
TEST_CASE("Compiled Explore imports independent masks through both geometries and upscale", "[acceptance][explore][support]") {
    namespace resize = mmltk::backend::imaging::resample;
    const auto mode = GENERATE(resize::ImageResizeMode::Stretch, resize::ImageResizeMode::Letterbox);
    mmltk::testsupport::ScopedTempDir root{"mmltk-explore-independent-mask"};
    const auto compiled = mmltk::testsupport::compile_explore_fixture(
        root.path(), "independent", 1, {.source_width = 8, .source_height = 4, .compiled_width = 8, .compiled_height = 8, .resize_mode = mode},
        {.independent_masks = true});
    controller::SettingsSystem settings;
    load_explore_transport(settings, root.path() / "gui.json", true);
    NativeExploreFixture fixture(settings, true);
    auto& system = fixture.system;
    static_cast<void>(system.Open({.viewport = {.extent = {32, 32}, .row_count = 1, .columns = 1}, .compiled_source = compiled.string()}));
    wait_for_native_gallery(fixture.audit, system, 0, 0, 1);
    static_cast<void>(system.Select({.compiled_index = 0}));
    REQUIRE(fixture.audit.Wait([&] { return !system.snapshot().busy && system.snapshot().mode == controller::ExploreMode::Detail; }));
    const auto frame = system.snapshot().frame;
    auto borrowed = system.BorrowDocument(frame);
    REQUIRE(borrowed.valid());
    const auto original =
        controller::materialize_visual_document(*borrowed.document, frame.extent, frame.content, controller::visual_materialized_extent(frame, true));
    REQUIRE(original.objects.size() == 3);
    CHECK(original.objects[0].box == controller::contracts::AnnotationBox{{4.25F, 1.25F}, {7.75F, 3.75F}});
    CHECK(original.objects[0].mask.runs == std::vector<controller::contracts::AnnotationMaskRun>{{0, 0, 0}});
    CHECK(original.objects[1].mask.present);
    CHECK(original.objects[1].mask.runs.empty());
    CHECK_FALSE(original.objects[2].mask.present);
    const auto canvas = controller::materialize_visual_document(*borrowed.document, frame.extent, {});
    CHECK(canvas.objects[0].mask.runs.size() == (mode == resize::ImageResizeMode::Stretch ? 2 : 1));
    const auto scaled = controller::scale_visual_document(borrowed.document, 4);
    const controller::VisualRegion crop{frame.content.x * 4, frame.content.y * 4, frame.content.width * 4, frame.content.height * 4};
    const auto enlarged = controller::materialize_visual_document(*scaled, {32, 32}, crop, {32, 16});
    CHECK(enlarged.objects[0].mask.runs == std::vector<controller::contracts::AnnotationMaskRun>{{0, 0, 3}, {1, 0, 3}, {2, 0, 3}, {3, 0, 3}});
    borrowed = {};
    system.Shutdown();
    CHECK_FALSE(fixture.audit.failed());
}
TEST_CASE("test_compiled_dataset_explore_projection_navigation_and_streaming", "[acceptance][backend-data][explore]") {
    test_compiled_dataset_explore_projection_navigation_and_streaming();
}
TEST_CASE("test_compiled_explore_optional_donors_respect_source_capacity", "[acceptance][backend-data][explore][capacity]") {
    test_compiled_explore_optional_donors_respect_source_capacity();
}
TEST_CASE("test_compiled_explore_cancelled_lane_preserves_atomic_product", "[acceptance][backend-data][explore][completion]") {
    test_compiled_explore_cancelled_lane_preserves_atomic_product();
}
TEST_CASE("test_native_explore_transaction_faults_and_inactive_release", "[acceptance][backend-data][explore][transaction]") {
    test_native_explore_transaction_faults_and_inactive_release();
}
TEST_CASE("test_compiled_explore_magnified_tiny_mask_and_transfer", "[acceptance][backend-data][explore][support]") {
    test_compiled_explore_magnified_tiny_mask_and_transfer();
}
TEST_CASE("test_compiled_explore_ring_holes_survive_hidden_donor_and_transfer", "[acceptance][explore][copy_paste]") {
    test_compiled_explore_ring_holes_survive_hidden_donor_and_transfer();
}
