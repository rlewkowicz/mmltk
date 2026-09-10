#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cuda_runtime_api.h>
#include <sys/socket.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "filesystem_test_utils.hpp"
#include "src/backend/data/compiled_format.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/common/io/scoped_fd.h"

namespace native_gallery_allocations {
thread_local bool enabled = false;
thread_local std::size_t count = 0U;
struct Scope final {
    Scope() {
        count = 0U;
        enabled = true;
    }
    ~Scope() { enabled = false; }
};
[[nodiscard]] void* Allocate(std::size_t bytes, const std::size_t alignment) {
    if (enabled) ++count;
    if (bytes == 0U) bytes = 1U;
    void* result = nullptr;
    if (alignment <= alignof(std::max_align_t))
        result = std::malloc(bytes);
    else if (::posix_memalign(&result, alignment, bytes) != 0)
        result = nullptr;
    if (!result) throw std::bad_alloc{};
    return result;
}
}  // namespace native_gallery_allocations
[[gnu::noinline]] void* operator new(std::size_t bytes) { return native_gallery_allocations::Allocate(bytes, alignof(std::max_align_t)); }
[[gnu::noinline]] void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
[[gnu::noinline]] void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return native_gallery_allocations::Allocate(bytes, static_cast<std::size_t>(alignment));
}
[[gnu::noinline]] void* operator new[](std::size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }
[[gnu::noinline]] void operator delete(void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }

namespace mmltk::controller {
namespace {

// A real compiled artifact keeps source reads, RLE projection, augmentation,
// CUDA rendering, cache ownership and runtime publication in the test path.
void write_gallery_artifact(const std::filesystem::path& path, const float red, const std::uint32_t mask_start,
                            const std::uint32_t count = 203U, const std::uint32_t classes = 1U) {
    namespace data = mmltk::backend::data;
    data::FileHeader header{.magic = data::MAGIC,
                            .version = data::FORMAT_VERSION,
                            .num_images = count,
                            .image_width = 8U,
                            .image_height = 8U,
                            .channels = 3U,
                            .num_classes = classes,
                            .index_offset = sizeof(data::FileHeader),
                            .label_offset = 0U,
                            .pixel_offset = 0U,
                            .mask_rle_offset = 0U,
                            .total_file_size = 0U,
                            .image_stride = 8U * 8U * 3U * sizeof(float),
                            .class_names = {},
                            .max_instances_per_image = 1U,
                            ._reserved = {}};
    header.pixel_offset = data::align_up(header.index_offset + count * sizeof(data::ImageEntry), data::HUGE_PAGE_SIZE);
    header.label_offset = header.pixel_offset + count * header.image_stride;
    header.mask_rle_offset = header.label_offset + count * sizeof(data::PackedInstance);
    header.total_file_size = header.mask_rle_offset + count * sizeof(data::RLEPair);
    for (std::uint32_t category = 0U; category < classes; ++category) {
        const auto name = std::string{"object"} + std::to_string(category);
        std::memcpy(header.class_names[category].data(), name.c_str(), name.size() + 1U);
    }
    std::vector<std::byte> bytes(header.total_file_size);
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (std::uint32_t index = 0U; index < count; ++index) {
        const data::ImageEntry entry{.pixel_offset = header.pixel_offset + index * header.image_stride,
                                     .label_offset = static_cast<std::uint32_t>(index * sizeof(data::PackedInstance)),
                                     .num_instances = 1U,
                                     ._pad = 0U,
                                     .label_bytes = sizeof(data::PackedInstance),
                                     .original_width = 8U,
                                     .original_height = 8U,
                                     ._reserved = 0U};
        const data::PackedInstance label{.class_id = 0U,
                                         ._pad = 0U,
                                         .bbox_x1 = 1,
                                         .bbox_y1 = 1,
                                         .bbox_x2 = 7,
                                         .bbox_y2 = 7,
                                         .mask_rle_offset = static_cast<std::uint32_t>(index * sizeof(data::RLEPair)),
                                         .mask_rle_pairs = 1U};
        const data::RLEPair run{mask_start, 4U};
        std::array<float, 8U * 8U * 3U> pixels{};
        for (std::size_t pixel = 0U; pixel < 64U; ++pixel) {
            pixels[pixel] = red;
            pixels[64U + pixel] = static_cast<float>(index) / 255.0F;
        }
        std::memcpy(bytes.data() + header.index_offset + index * sizeof(entry), &entry, sizeof(entry));
        std::memcpy(bytes.data() + entry.pixel_offset, pixels.data(), sizeof(pixels));
        std::memcpy(bytes.data() + header.label_offset + entry.label_offset, &label, sizeof(label));
        std::memcpy(bytes.data() + header.mask_rle_offset + label.mask_rle_offset, &run, sizeof(run));
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

struct GalleryEvidence final {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t epoch = 0U;
    std::vector<VisualDiagnosticFact> facts;
    std::atomic_bool enabled{true};
    void Wake() {
        std::scoped_lock lock(mutex);
        ++epoch;
        changed.notify_all();
    }
    [[nodiscard]] std::uint64_t Epoch() {
        std::scoped_lock lock(mutex);
        return epoch;
    }
    void Wait(const std::uint64_t previous) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds{10}, [&] { return epoch != previous; }))
            throw std::runtime_error("native gallery did not deliver its completion wake");
    }
    [[nodiscard]] std::size_t Count(const VisualDiagnosticOperation operation) {
        std::scoped_lock lock(mutex);
        std::size_t count = 0U;
        for (const auto& fact : facts)
            count += fact.operation == operation;
        return count;
    }
    [[nodiscard]] std::size_t BackgroundSubmissions() {
        std::scoped_lock lock(mutex);
        std::size_t count = 0U;
        for (const auto& fact : facts)
            if (fact.operation == VisualDiagnosticOperation::ExploreRenderSubmitted && fact.context.condition != 0U) ++count;
        return count;
    }
    [[nodiscard]] VisualDiagnosticSink Sink() {
        return {.context = this,
                .write =
                    [](void* context, VisualDiagnosticFact fact) noexcept {
                        auto& evidence = *static_cast<GalleryEvidence*>(context);
                        try {
                            std::scoped_lock lock(evidence.mutex);
                            evidence.facts.push_back(fact);
                        } catch (...) {}
                    },
                .enabled = [](void* context) noexcept { return static_cast<GalleryEvidence*>(context)->enabled.load(); },
                .pixel_probes = true};
    }
};

struct GalleryGpuPause final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    void Submit(const std::uintptr_t stream) {
        const auto status = cudaLaunchHostFunc(
            reinterpret_cast<cudaStream_t>(stream),
            [](void* context) {
                auto& pause = *static_cast<GalleryGpuPause*>(context);
                std::unique_lock lock(pause.mutex);
                pause.entered = true;
                pause.changed.notify_all();
                pause.changed.wait(lock, [&] { return pause.released; });
            },
            this);
        if (status != cudaSuccess) throw std::runtime_error("native gallery GPU pause submission failed");
    }
    [[nodiscard]] bool Wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds{10}, [&] { return entered; });
    }
    void Release() {
        std::scoped_lock lock(mutex);
        released = true;
        changed.notify_all();
    }
    ~GalleryGpuPause() { Release(); }
};

[[nodiscard]] std::vector<std::uint8_t> copy_product_plane(mmltk::frameworks::gpu::BorrowedImageProductReadView product,
                                                           const std::size_t plane_index) {
    REQUIRE(product.valid());
    const auto plane = product.plane(plane_index).plane();
    std::vector<std::uint8_t> bytes(plane.descriptor.row_bytes() * plane.descriptor.height);
    REQUIRE(cudaMemcpy2D(bytes.data(), plane.descriptor.row_bytes(), reinterpret_cast<const void*>(plane.data),
                         plane.descriptor.pitch_bytes, plane.descriptor.row_bytes(), plane.descriptor.height,
                         cudaMemcpyDeviceToHost) == cudaSuccess);
    return bytes;
}

class NativeGallery final {
   public:
    GalleryEvidence evidence;
    std::shared_ptr<std::atomic<std::uint64_t>> demand = std::make_shared<std::atomic<std::uint64_t>>(1U);
    std::shared_ptr<ExploreAcceptanceGate> gate;
    mmltk::common::io::ScopedFd commands;
    std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime> runtime;
    ExploreAlgorithm* algorithm = nullptr;
    ExploreRenderPlan plan{};
    std::size_t publications = 0U;
    std::size_t acquisitions = 0U;
    ExploreOverlay issued_semantics{};
    ExploreStorageFootprint transaction_storage{};
    GalleryGpuPause* pause = nullptr;
    GalleryGpuPause* probe_pause = nullptr;
    std::atomic<std::size_t> logical_copies{0U};

    explicit NativeGallery(const std::uint32_t columns, const bool delayed = false, const bool observed_submission = false) {
        if (delayed || observed_submission) {
            std::array<int, 2U> sockets{};
            REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0);
            commands = mmltk::common::io::ScopedFd{sockets[0U]};
            gate = std::make_shared<ExploreAcceptanceGate>(sockets[1U]);
            Command(2U);
            if (!delayed) Command(4U);
            gate->SetProductObserver(this, [](void* context, ExploreAcceptanceGate::ProductObservation observation) noexcept {
                static_cast<NativeGallery*>(context)->logical_copies.fetch_add(observation.copied_entries);
            });
            if (observed_submission)
                gate->SetSubmissionObserver(
                    this, [](void* context, const std::uintptr_t stream, const ExploreAcceptanceGate::SubmissionStage stage) {
                        auto& gallery = *static_cast<NativeGallery*>(context);
                        if (stage == ExploreAcceptanceGate::SubmissionStage::Background && gallery.pause) gallery.pause->Submit(stream);
                        if (stage == ExploreAcceptanceGate::SubmissionStage::Probe && gallery.probe_pause)
                            gallery.probe_pause->Submit(stream);
                    });
        }
        runtime = make_native_explore_runtime_factory(
            {.device = 0, .maximum_width = 4096U, .maximum_height = 4096U}, 2U,
            {.acceptance = gate, .diagnostics = evidence.Sink()})(std::make_shared<mmltk::frameworks::gpu::ImageProductRevisionSequence>());
        algorithm = dynamic_cast<ExploreAlgorithm*>(runtime->model());
        REQUIRE(algorithm);
        algorithm->SetCurrentDemand(ExploreDemandCheck{demand});
        runtime->BeginWork();
        algorithm->SetGalleryReadySink(ExploreAlgorithm::GalleryReadySink{[this] { evidence.Wake(); }});
        plan.viewport = {.extent = {columns * 8U, 16U}, .row_count = 2U, .columns = columns};
        plan.augmentation_config.enabled = false;
        plan.generation = 1U;
    }
    void Command(const std::uint8_t command) {
        REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    }
    ~NativeGallery() {
        demand->store(0U);
        if (algorithm) algorithm->StopIngress();
        if (runtime) static_cast<void>(runtime->Retire());
    }
    void Open(const std::filesystem::path& path) {
        const auto opened = algorithm->Open(path.string(), {});
        plan.dataset_identity = opened.dataset_identity;
        auto order = algorithm->PrepareFilter({}, 0U, 2U, {});
        Begin(&order);
        algorithm->Commit(std::move(order));
    }
    ExploreGalleryPublication Begin(const ExploreOrderCandidate* order = nullptr, const bool commit = true) {
        // The fixture stands in for ExploreSystem, the identity issuer. Native
        // rendering itself receives only the issued scalar validity key.
        if (plan.semantic_identity == 0U || plan.overlay.show_boxes != issued_semantics.show_boxes ||
            plan.overlay.show_masks != issued_semantics.show_masks || plan.overlay.class_selection != issued_semantics.class_selection) {
            issued_semantics = plan.overlay;
            ++plan.semantic_identity;
        }
        const auto change = algorithm->OutputChange(plan, order);
        algorithm->PrepareOutputPublication(change);
        ExploreGalleryPublication publication;
        if (change == ExploreOutputChange::Unchanged) {
            if (plan.mode == ExploreMode::Gallery)
                publication = algorithm->BeginGallery(plan, order, 2U, {}, {}, 0U);
            else
                algorithm->RenderDetail(plan, 2U, {}, {}, 0U);
        } else {
            ++acquisitions;
            auto output = runtime->AcquireOutput({}, change == ExploreOutputChange::Semantic
                                                         ? runtime->Completed()
                                                         : mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput{});
            const auto extent = plan.mode == ExploreMode::Gallery ? plan.viewport.extent : algorithm->DetailExtent(plan);
            runtime->Publish(output, extent.width, extent.height, [&](auto clean, auto semantic, auto stream) {
                if (plan.mode == ExploreMode::Gallery)
                    publication = algorithm->BeginGallery(plan, order, 2U, clean, semantic, stream);
                else
                    algorithm->RenderDetail(plan, 2U, clean, semantic, stream);
            });
            if (evidence.enabled.load()) transaction_storage = algorithm->StorageFootprint();
            if (commit) {
                runtime->CommitOutput(std::move(output));
                ++publications;
            }
        }
        if (commit)
            algorithm->CommitOutputPublication();
        else
            REQUIRE(algorithm->RollbackOutputPublication());
        return publication;
    }
    ExploreGalleryPublication Step(const bool commit = true) {
        const auto advanced = algorithm->AdvanceGallery();
        if (algorithm->HasGalleryTiles()) {
            algorithm->PrepareOutputPublication(ExploreOutputChange::Semantic);
            ++acquisitions;
            auto output = runtime->AcquireOutput({}, runtime->Completed());
            runtime->Publish(output, plan.viewport.extent.width, plan.viewport.extent.height, [&](auto clean, auto semantic, auto stream) {
                if (pause) pause->Submit(stream);
                static_cast<void>(algorithm->PublishGalleryTiles(clean, semantic, stream));
            });
            if (evidence.enabled.load()) transaction_storage = algorithm->StorageFootprint();
            if (commit && demand->load() == plan.generation) {
                runtime->CommitOutput(std::move(output));
                algorithm->CommitOutputPublication();
                ++publications;
            } else {
                if (!algorithm->RollbackOutputPublication()) throw std::runtime_error("native gallery rollback did not settle");
            }
            evidence.Wake();
        }
        return advanced;
    }
    void Drain() {
        for (;;) {
            const auto observed = evidence.Epoch();
            const auto advanced = Step();
            if (advanced.remaining_tiles == 0U && advanced.active_pinned_bytes == 0U) return;
            evidence.Wait(observed);
        }
    }
    void WaitForReadyTiles() {
        for (;;) {
            const auto observed = evidence.Epoch();
            static_cast<void>(algorithm->AdvanceGallery());
            if (algorithm->HasGalleryTiles()) return;
            evidence.Wait(observed);
        }
    }
    [[nodiscard]] std::vector<std::uint8_t> Pixels(const std::size_t plane_index) {
        return copy_product_plane(runtime->Borrow(), plane_index);
    }
};

TEST_CASE("Native gallery retains slot products across hot reuse semantic changes collisions and artifact replacement",
          "[explore][native][cache]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const auto columns = GENERATE(4U, 10U);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{columns};
    gallery.Open(path);
    gallery.Drain();
    {
        std::scoped_lock lock(gallery.evidence.mutex);
        std::size_t background_tiles = 0U;
        for (const auto& fact : gallery.evidence.facts) {
            if (fact.operation == VisualDiagnosticOperation::ExploreProbeBatchSubmitted) {
                CHECK(fact.value > 0U);
                CHECK(fact.value <= kExploreVisibleItemCapacity);
                CHECK(fact.context.staging_bytes == fact.value * 7U * sizeof(std::uint64_t));
            }
            if (fact.operation != VisualDiagnosticOperation::ExploreRenderSubmitted || fact.context.condition == 0U) continue;
            CHECK(fact.detail == columns * 2U);
            background_tiles += fact.value;
        }
        CHECK(background_tiles == explore_detail::GalleryThumbnailCache::CardCount(203U, 2U, columns) - columns * 2U);
    }
    const auto storage = gallery.algorithm->StorageFootprint();
    const auto capacity = explore_detail::GalleryThumbnailCache::CardCount(203U, 2U, columns);
    CHECK(storage.cache_cards == capacity);
    CHECK(storage.cache_device_bytes == 4U * capacity * 8U * 8U * 4U);
    CHECK(storage.host_bytes >= 2U * capacity * sizeof(explore_detail::GalleryThumbnailCache::Entry));
    CHECK(storage.device_bytes >= storage.cache_device_bytes);
    const auto output_storage = gallery.runtime->OutputStorageFootprint();
    {
        auto output = gallery.runtime->Borrow();
        const auto clean_pitch = output.plane(0U).plane().descriptor.pitch_bytes;
        const auto semantic_pitch = output.plane(1U).plane().descriptor.pitch_bytes;
        // Both output slots have been materialized by progressive publication.
        CHECK(output_storage.device_bytes == 2U * (clean_pitch + semantic_pitch) * 16U);
        CHECK(output_storage.pinned_bytes == 0U);
    }
    const auto original = gallery.Pixels(0U);
    const auto original_semantics = gallery.Pixels(1U);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    const auto published = gallery.publications;
    const auto transfers = gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer);
    const auto allocations = gallery.evidence.Count(VisualDiagnosticOperation::ExploreStorageGrown);
    REQUIRE(reads == explore_detail::GalleryThumbnailCache::CardCount(203U, 2U, columns));
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin().remaining_tiles == 0U);
    gallery.Drain();
    CHECK(gallery.publications == published);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer) == transfers);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreStorageGrown) == allocations);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreAugmentationBatchPrepared) == 0U);
    CHECK(gallery.Pixels(0U) == original);

    gallery.plan.overlay.show_masks = !gallery.plan.overlay.show_masks;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.Pixels(0U) == original);
    CHECK(gallery.Pixels(1U) != original_semantics);
    {
        std::scoped_lock lock(gallery.evidence.mutex);
        std::size_t semantic_copies = 0U;
        for (const auto& fact : gallery.evidence.facts)
            if (fact.operation == VisualDiagnosticOperation::ExploreCacheTransfer && fact.generation == gallery.plan.generation) {
                CHECK(fact.context.condition == 1U);
                semantic_copies += fact.value;
            }
        CHECK(semantic_copies == columns * 2U * 8U * 8U * 4U);
    }

    gallery.plan.viewport.first_row = 18U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.WaitForReadyTiles();
    const auto before_patch = gallery.Pixels(0U);
    const auto before_patch_bytes = gallery.algorithm->StorageFootprint().host_bytes;
    static_cast<void>(gallery.Step(false));
    CHECK(gallery.Pixels(0U) == before_patch);
    CHECK(gallery.transaction_storage.host_bytes > before_patch_bytes);
    gallery.plan.viewport.first_row = 2U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin().remaining_tiles == 0U);
    gallery.plan.viewport.first_row = 0U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();

    gallery.plan.viewport.first_row = 18U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin(nullptr, false);
    CHECK(gallery.Pixels(0U) == original);
    gallery.plan.viewport.first_row = 0U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.Pixels(0U) == original);

    const auto stable_seed_identity = gallery.plan.dataset_identity;
    gallery.plan.overlay.show_masks = true;
    gallery.plan.overlay.show_boxes = false;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    const auto old_masks = gallery.Pixels(1U);
    const auto replacement = directory.path() / "replacement.bin";
    write_gallery_artifact(replacement, 0.75F, 17U);
    std::filesystem::rename(replacement, path);
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Open(path);
    gallery.Drain();
    CHECK(gallery.plan.dataset_identity == stable_seed_identity);
    CHECK(gallery.Pixels(0U) != original);
    CHECK(gallery.Pixels(1U) != old_masks);
    gallery.plan.viewport.first_row = 18U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto before_jump = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) > before_jump);

    gallery.plan.viewport.extent = {columns * 16U, 32U};
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto before_resize = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) > before_resize);
    CHECK(gallery.Pixels(0U).size() == static_cast<std::size_t>(columns) * 16U * 32U * 4U);
    gallery.plan.augmentation.enabled = true;
    gallery.plan.augmentation_config.enabled = true;
    ++gallery.plan.augmentation.seed;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto before_augmentation = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) > before_augmentation);
    const auto augmented_storage = gallery.algorithm->StorageFootprint();
    CHECK(augmented_storage.augmentation_device_bytes > 0U);
    CHECK(augmented_storage.augmentation_pinned_bytes > 0U);
    CHECK(augmented_storage.device_bytes >= augmented_storage.augmentation_device_bytes + augmented_storage.cache_device_bytes);
    CHECK(augmented_storage.pinned_bytes >= augmented_storage.augmentation_pinned_bytes);
    gallery.plan.viewport.first_row = 0U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.WaitForReadyTiles();
    const auto protected_storage = gallery.algorithm->StorageFootprint();
    static_cast<void>(gallery.Step(false));
    const auto candidate_storage = gallery.transaction_storage;
    CHECK(candidate_storage.host_bytes > protected_storage.host_bytes);
    CHECK(candidate_storage.augmentation_device_bytes == protected_storage.augmentation_device_bytes);
    CHECK(candidate_storage.augmentation_pinned_bytes == protected_storage.augmentation_pinned_bytes);
    gallery.plan.viewport.first_row = 203U / columns;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.algorithm->Visible(gallery.plan.viewport).visible_indices.size() == 3U);
    CHECK(gallery.Pixels(0U).back() == 0U);
    CHECK(gallery.Pixels(1U).back() == 0U);

    auto filtered = gallery.algorithm->PrepareFilter({.minimum_instances = 2U}, 0U, 2U, {});
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin(&filtered).remaining_tiles == 0U);
    gallery.algorithm->Commit(std::move(filtered));
    gallery.Drain();
    gallery.evidence.enabled.store(false);
    gallery.demand->store(0U);
    const auto stopped_reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    static_cast<void>(gallery.algorithm->AdvanceGallery());
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == stopped_reads);
}

TEST_CASE("Native delayed visible completion prevents offscreen GPU and failed replacement resumes retained work",
          "[explore][native][priority]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-priority"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U, true};
    gallery.plan.viewport.first_row = 18U;
    gallery.Open(path);
    for (;;) {
        const auto previous = gallery.evidence.Epoch();
        const auto advanced = gallery.Step();
        if (gallery.evidence.Count(VisualDiagnosticOperation::ExplorePrefetchReady) != 0U) {
            CHECK(advanced.remaining_tiles == 1U);
            break;
        }
        gallery.evidence.Wait(previous);
    }
    {
        std::scoped_lock lock(gallery.evidence.mutex);
        for (const auto& fact : gallery.evidence.facts)
            if (fact.operation == VisualDiagnosticOperation::ExploreRenderSubmitted) CHECK(fact.context.condition == 0U);
    }
    const auto retained = gallery.Pixels(0U);
    gallery.Command(4U);
    gallery.gate->FailNextPublicationAt(ExploreAcceptanceGate::PublicationStage::DescriptorsPrepared);
    gallery.plan.viewport.first_row = 0U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    REQUIRE_THROWS(gallery.Begin());
    REQUIRE(gallery.algorithm->RollbackOutputPublication());
    CHECK(gallery.Pixels(0U) == retained);
    gallery.plan.viewport.first_row = 18U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.Pixels(0U) != retained);
    const auto clean = gallery.Pixels(0U);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    gallery.gate->FailNextProbe();
    gallery.plan.overlay.show_boxes = !gallery.plan.overlay.show_boxes;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreProbeFailed) == 1U);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.Pixels(0U) == clean);
    gallery.evidence.enabled.store(false);
    // CLEANUP-IGNORE: This counter anchors semantic-only cache reuse; the earlier counter anchors viewport patch
    // scheduling and they intentionally assert different operations.
    const auto recorded = gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted);
    gallery.plan.overlay.show_masks = !gallery.plan.overlay.show_masks;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted) == recorded);
    CHECK(gallery.Pixels(0U) == clean);
}

TEST_CASE("Native exact reuse performs no host allocation or logical copy after capacity growth", "[explore][native][cache]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-host"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U, 1003U, static_cast<std::uint32_t>(kExploreClassCapacity));
    const auto columns = GENERATE(4U, 10U);
    NativeGallery gallery{columns, false, true};
    gallery.plan.viewport.row_count = 1U;
    gallery.plan.viewport.extent.height = 8U;
    gallery.Open(path);
    gallery.Drain();
    gallery.plan.overlay.class_selection.mode = ExploreClassSelectionMode::Subset;
    gallery.plan.overlay.class_selection.classes.resize(kExploreClassCapacity);
    std::iota(gallery.plan.overlay.class_selection.classes.begin(), gallery.plan.overlay.class_selection.classes.end(), 0U);
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto before_semantic = gallery.logical_copies.load();
    gallery.Begin();
    CHECK(gallery.logical_copies.load() - before_semantic == columns);
    gallery.plan.viewport.row_count = static_cast<std::uint32_t>(kExploreVisibleItemCapacity / columns);
    gallery.plan.viewport.extent.height = gallery.plan.viewport.row_count * 8U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    gallery.Drain();
    gallery.evidence.enabled.store(false);
    const auto copies = gallery.logical_copies.load();
    const auto publications = gallery.publications;
    const auto acquisitions = gallery.acquisitions;
    const auto revision = gallery.runtime->Completed().revision();
    for (unsigned repeat = 0U; repeat != 4U; ++repeat) {
        ++gallery.plan.generation;
        if (repeat != 0U) gallery.plan.focused_image = repeat;
        gallery.demand->store(gallery.plan.generation);
        std::size_t allocations;
        ExploreGalleryPublication reused;
        {
            native_gallery_allocations::Scope measurement;
            reused = gallery.Begin();
            allocations = native_gallery_allocations::count;
        }
        CHECK(allocations == 0U);
        CHECK(gallery.logical_copies.load() == copies);
        CHECK(gallery.publications == publications);
        CHECK(gallery.acquisitions == acquisitions);
        CHECK(gallery.runtime->Completed().revision() == revision);
        CHECK(reused.generation == gallery.plan.generation);
    }
    gallery.evidence.enabled.store(true);
    const auto descriptors = gallery.evidence.Count(VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared);
    const auto renders = gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted);
    const auto transfers = gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer);
    gallery.plan.focused_image = 7U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared) == descriptors);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted) == renders);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer) == transfers);

    // A cold jump has no issued source reads until Advance. A metadata-only
    // focus commit must therefore put the requested card first in that demand.
    gallery.plan.viewport.first_row = 400U / columns;
    gallery.plan.focused_image.reset();
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    const auto cold_publications = gallery.publications;
    gallery.plan.focused_image = 403U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.publications == cold_publications);
    static_cast<void>(gallery.algorithm->AdvanceGallery());
    {
        std::scoped_lock lock(gallery.evidence.mutex);
        const auto first = std::ranges::find_if(gallery.evidence.facts, [&](const auto& fact) {
            return fact.operation == VisualDiagnosticOperation::GalleryReadScheduled && fact.generation == gallery.plan.generation;
        });
        REQUIRE(first != gallery.evidence.facts.end());
        CHECK(first->detail == *gallery.plan.focused_image);
    }
    gallery.Drain();

    gallery.plan.mode = ExploreMode::Detail;
    gallery.plan.selected_image = 403U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    const auto detail_acquisitions = gallery.acquisitions;
    const auto detail_publications = gallery.publications;
    const auto detail_revision = gallery.runtime->Completed().revision();
    const auto detail_descriptors = gallery.evidence.Count(VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared);
    const auto detail_clean = gallery.Pixels(0U);
    const auto detail_semantic = gallery.Pixels(1U);
    gallery.plan.viewport.extent = {24U, 24U};
    gallery.plan.viewport.row_count = 3U;
    gallery.plan.focused_image = 402U;
    gallery.plan.detail.show_original_dimensions = true;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.evidence.enabled.store(false);
    std::size_t detail_allocations;
    {
        native_gallery_allocations::Scope measurement;
        gallery.Begin();
        detail_allocations = native_gallery_allocations::count;
    }
    CHECK(detail_allocations == 0U);
    CHECK(gallery.acquisitions == detail_acquisitions);
    CHECK(gallery.publications == detail_publications);
    CHECK(gallery.runtime->Completed().revision() == detail_revision);
    gallery.evidence.enabled.store(true);
    gallery.plan.focused_image = 401U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared) == detail_descriptors);
    CHECK(gallery.Pixels(0U) == detail_clean);
    CHECK(gallery.Pixels(1U) == detail_semantic);
}

TEST_CASE("Native retirement settles held GPU and probe callbacks before checked release", "[explore][native][shutdown]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-retirement"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    const bool hold_probe = GENERATE(false, true);
    NativeGallery gallery{4U, false, true};
    GalleryGpuPause held;
    if (hold_probe) gallery.probe_pause = &held;
    gallery.Open(path);
    if (hold_probe) {
        while (gallery.evidence.Count(VisualDiagnosticOperation::ExploreProbeBatchSubmitted) == 0U) {
            const auto previous = gallery.evidence.Epoch();
            static_cast<void>(gallery.Step());
            if (gallery.evidence.Count(VisualDiagnosticOperation::ExploreProbeBatchSubmitted) == 0U) gallery.evidence.Wait(previous);
        }
    } else {
        gallery.Drain();
        gallery.plan.viewport.first_row = 8U;
        ++gallery.plan.generation;
        gallery.demand->store(gallery.plan.generation);
        gallery.Begin();
        gallery.pause = &held;
        const auto before = gallery.evidence.BackgroundSubmissions();
        for (;;) {
            const auto previous = gallery.evidence.Epoch();
            static_cast<void>(gallery.algorithm->AdvanceGallery());
            if (gallery.evidence.BackgroundSubmissions() != before) break;
            gallery.evidence.Wait(previous);
        }
    }
    const bool entered = held.Wait();
    if (!entered) held.Release();
    REQUIRE(entered);
    const auto publications = gallery.publications;
    std::promise<void> stopped;
    auto stopped_wait = stopped.get_future();
    auto retirement = std::async(std::launch::async, [&] {
        gallery.runtime->BindContext();
        gallery.demand->store(0U);
        gallery.algorithm->StopIngress();
        stopped.set_value();
        return gallery.runtime->Retire();
    });
    stopped_wait.get();
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    held.Release();
    const auto result = retirement.get();
    gallery.algorithm = nullptr;
    gallery.pause = nullptr;
    gallery.probe_pause = nullptr;
    CHECK(result.safe_to_destroy);
    CHECK_FALSE(result.failure);
    CHECK_FALSE(result.custody.valid());
    CHECK(gallery.publications == publications);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreProbeResourcesReleased) == 1U);
}

TEST_CASE("Native initialization commit reserves useful reads and isolates obsolete failures", "[explore][native][transaction]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    // Useful read, obsolete failure, current failure, adopted current failure.
    const auto scenario = GENERATE(0U, 1U, 2U, 3U);
    struct HeldRead final {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
        bool fail = false;
        void Release() {
            std::scoped_lock lock(mutex);
            released = true;
            changed.notify_all();
        }
    } held;
    held.fail = scenario != 0U;
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-commit"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U, false, true};
    struct ReleaseOnExit final {
        HeldRead& held;
        ~ReleaseOnExit() { held.Release(); }
    } release_on_exit{held};
    gallery.gate->SetReadObserver(&held, [](void* context, std::uint64_t, const std::uint32_t index) {
        if (index != 68U) return;
        auto& held_read = *static_cast<HeldRead*>(context);
        std::unique_lock lock(held_read.mutex);
        held_read.entered = true;
        held_read.changed.notify_all();
        held_read.changed.wait(lock, [&] { return held_read.released; });
        if (std::exchange(held_read.fail, false)) throw std::runtime_error("deterministic native source read failure");
    });
    gallery.plan.viewport = {.extent = {32U, 32U}, .first_row = 17U, .row_count = 4U, .columns = 4U};
    gallery.Open(path);
    static_cast<void>(gallery.algorithm->AdvanceGallery());
    {
        std::unique_lock lock(held.mutex);
        REQUIRE(held.changed.wait_for(lock, std::chrono::seconds{10}, [&] { return held.entered; }));
    }
    if (scenario != 2U) {
        gallery.plan.viewport.row_count = 1U;
        gallery.plan.viewport.extent.height = 8U;
        if (scenario == 1U) gallery.plan.viewport.first_row = 30U;
        ++gallery.plan.generation;
        gallery.demand->store(gallery.plan.generation);
        gallery.Begin();
        CHECK(gallery.algorithm->StorageFootprint().cache_cards == 60U);
        // Exercise scheduling while the old read is still physically held.
        // A useful reservation must prevent another lane from reading slot 68.
        static_cast<void>(gallery.algorithm->AdvanceGallery());
    }
    held.Release();
    if (scenario == 2U || scenario == 3U) {
        REQUIRE_THROWS_WITH(gallery.Drain(), "deterministic native source read failure");
        std::scoped_lock lock(gallery.evidence.mutex);
        CHECK(std::ranges::count_if(gallery.evidence.facts, [](const auto& fact) {
                  return fact.operation == VisualDiagnosticOperation::GalleryReadStarted && fact.detail == 68U;
              }) == 1);
        CHECK(std::ranges::count_if(gallery.evidence.facts, [&](const auto& fact) {
                  return fact.operation == VisualDiagnosticOperation::GalleryReadCompleted && fact.detail == 68U &&
                         fact.context.capacity_height == 1U && fact.context.demand.demand_generation == gallery.plan.generation;
              }) == 1);
    } else {
        REQUIRE_NOTHROW(gallery.Drain());
        if (scenario == 0U) {
            // CLEANUP-IGNORE: Successful GPU completion evidence differs from the read-failure branch even though
            // both inspect the same diagnostic collection under its lock.
            std::scoped_lock lock(gallery.evidence.mutex);
            CHECK(std::ranges::count_if(gallery.evidence.facts, [](const auto& fact) {
                      return fact.operation == VisualDiagnosticOperation::GalleryReadStarted && fact.detail == 68U;
                  }) == 1);
            CHECK(std::ranges::count_if(gallery.evidence.facts, [](const auto& fact) {
                      return fact.operation == VisualDiagnosticOperation::GalleryGpuCompleted && fact.detail == 68U;
                  }) == 1);
        }
        CHECK(gallery.Pixels(0U).size() == 32U * 8U * 4U);
    }
}

TEST_CASE("Native superseded source failure leaves newer system demand queued", "[explore][native][transaction]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const bool before_consumption = GENERATE(true, false);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-stale-failure"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    SettingsSystem settings;
    REQUIRE(settings.Load(services::SettingsLocation{(directory.path() / "gui.json").string()}).applied());
    struct Interleave final {
        GalleryEvidence evidence;
        ExploreSystem* system = nullptr;
        std::atomic_bool fail_once{true};
        std::atomic_bool read_scheduled{false};
        std::atomic_bool failed_ready{false};
        std::atomic_bool superseded{false};
        std::atomic_size_t failures{0U};
        bool before_consumption = false;
        ExploreViewport next{.extent = {32U, 8U}, .first_row = 30U, .row_count = 1U, .columns = 4U};
    } interleave;
    interleave.before_consumption = before_consumption;
    auto diagnostics = interleave.evidence.Sink();
    diagnostics.context = &interleave;
    diagnostics.enabled = [](void* context) noexcept { return static_cast<Interleave*>(context)->evidence.enabled.load(); };
    diagnostics.write = [](void* context, VisualDiagnosticFact fact) noexcept {
        auto& state = *static_cast<Interleave*>(context);
        try {
            {
                std::scoped_lock lock(state.evidence.mutex);
                state.evidence.facts.push_back(fact);
                ++state.evidence.epoch;
                state.evidence.changed.notify_all();
            }
            if (fact.operation == VisualDiagnosticOperation::GalleryReadScheduled && fact.detail == 68U) state.read_scheduled.store(true);
            if (fact.operation == VisualDiagnosticOperation::AcceptanceReadySinkStarted && fact.detail == 68U) {
                state.failed_ready.store(true);
                state.evidence.Wake();
            }
            if (state.before_consumption && state.read_scheduled.load() && !state.superseded.load() &&
                fact.operation == VisualDiagnosticOperation::ExploreContinuationStarted && fact.detail == 10U) {
                std::unique_lock lock(state.evidence.mutex);
                if (!state.evidence.changed.wait_for(lock, std::chrono::seconds{10}, [&] { return state.failed_ready.load(); }))
                    throw std::runtime_error("native failure did not reach its ready sink");
            }
            const bool frontier =
                state.before_consumption ? fact.detail == 10U && state.failed_ready.load() : fact.detail == 11U && fact.value == 1U;
            if (fact.operation == VisualDiagnosticOperation::ExploreContinuationStarted && frontier && !state.superseded.exchange(true))
                state.system->UpdateViewport({.viewport = state.next});
        } catch (...) { state.failures.fetch_add(1U); }
    };
    std::array<int, 2U> sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0U]};
    auto gate = std::make_shared<ExploreAcceptanceGate>(sockets[1U]);
    for (const std::uint8_t command : std::array<std::uint8_t, 2U>{2U, 4U})
        REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    gate->SetReadObserver(&interleave, [](void* context, std::uint64_t, const std::uint32_t index) {
        auto& state = *static_cast<Interleave*>(context);
        if (index != 68U || !state.fail_once.exchange(false)) return;
        // A before-read failure submits no transfer. CompiledImageStream
        // settles its transfer observer before delivering the read failure.
        throw std::runtime_error("superseded native source failure");
    });
    auto factory = make_native_explore_runtime_factory({.device = 0, .maximum_width = 4096U, .maximum_height = 4096U}, 2U,
                                                       {.acceptance = gate, .diagnostics = diagnostics});
    ExploreSystem system{settings,
                         {.device = 0, .maximum_width = 4096U, .maximum_height = 4096U},
                         2U,
                         std::move(factory),
                         [&interleave](ExploreSystem::event_type event) {
                             if (std::holds_alternative<ExploreFailed>(event)) interleave.failures.fetch_add(1U);
                             interleave.evidence.Wake();
                         },
                         diagnostics};
    interleave.system = &system;
    static_cast<void>(system.Open(
        {.viewport = {.extent = {32U, 32U}, .first_row = 17U, .row_count = 4U, .columns = 4U}, .compiled_source = path.string()}));
    for (;;) {
        const auto observed = interleave.evidence.Epoch();
        const auto snapshot = system.snapshot();
        if (snapshot.ready && snapshot.viewport == interleave.next && snapshot.gallery.slots.size() == 4U &&
            std::ranges::all_of(snapshot.gallery.slots, [](bool ready) { return ready; }))
            break;
        interleave.evidence.Wait(observed);
    }
    CHECK(interleave.superseded.load());
    CHECK(interleave.failures.load() == 0U);
    CHECK(system.snapshot().failure.empty());
    system.Shutdown();
    CHECK(system.stopped());
}

TEST_CASE("Native initialization rollback resumes held incumbent input through automatic continuation", "[explore][native][transaction]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const bool empty_candidate = GENERATE(false, true);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-initialize"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    SettingsSystem settings;
    REQUIRE(settings.Load(services::SettingsLocation{(directory.path() / "gui.json").string()}).applied());
    GalleryEvidence evidence;
    auto diagnostics = evidence.Sink();
    diagnostics.write = [](void* context, VisualDiagnosticFact fact) noexcept {
        auto& observed_evidence = *static_cast<GalleryEvidence*>(context);
        try {
            std::scoped_lock lock(observed_evidence.mutex);
            observed_evidence.facts.push_back(fact);
            ++observed_evidence.epoch;
            observed_evidence.changed.notify_all();
            // CLEANUP-IGNORE: This guarded diagnostic sink is specific to rollback evidence; the earlier sink records
            // supersession state and has different wake and failure ownership.
        } catch (...) {}
    };
    std::array<int, 2U> sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0U]};
    auto gate = std::make_shared<ExploreAcceptanceGate>(sockets[1U]);
    const std::uint8_t release_reads = 2U;
    REQUIRE(::send(commands.get(), &release_reads, sizeof(release_reads), MSG_NOSIGNAL) == sizeof(release_reads));
    auto factory = make_native_explore_runtime_factory({.device = 0, .maximum_width = 4096U, .maximum_height = 4096U}, 2U,
                                                       {.acceptance = gate, .diagnostics = diagnostics});
    mmltk::frameworks::gpu::SystemImageRuntime* runtime = nullptr;
    ExploreSystem system{settings,
                         // CLEANUP-IGNORE: This native rollback fixture captures its runtime for exact product
                         // inspection; the presentation test constructs a fake runtime with different ownership.
                         {.device = 0, .maximum_width = 4096U, .maximum_height = 4096U},
                         2U,
                         [factory = std::move(factory), &runtime](auto revisions) mutable {
                             auto created = factory(std::move(revisions));
                             runtime = created.get();
                             return created;
                         },
                         [&evidence](ExploreSystem::event_type) { evidence.Wake(); },
                         diagnostics};
    const auto wait = [&](auto&& ready) {
        for (;;) {
            const auto observed = evidence.Epoch();
            if (ready()) return;
            evidence.Wait(observed);
        }
    };
    const ExploreViewport viewport{.extent = {32U, 32U}, .first_row = 17U, .row_count = 4U, .columns = 4U};
    static_cast<void>(system.Open({.viewport = viewport, .compiled_source = path.string()}));
    wait([&] {
        const auto snapshot = system.snapshot();
        return snapshot.ready && snapshot.gallery.slots.size() == 16U && std::ranges::count(snapshot.gallery.slots, true) == 15 &&
               evidence.Count(VisualDiagnosticOperation::AcceptanceCompletionHeld) == 1U;
    });
    const auto incumbent = system.snapshot();
    std::uint32_t held_index = 0U;
    {
        std::scoped_lock lock(evidence.mutex);
        const auto held =
            std::ranges::find(evidence.facts, VisualDiagnosticOperation::AcceptanceCompletionHeld, &VisualDiagnosticFact::operation);
        REQUIRE(held != evidence.facts.end());
        held_index = static_cast<std::uint32_t>(held->detail);
    }
    const auto pixels = [&](const std::size_t plane_index) {
        runtime->BindContext();
        return copy_product_plane(system.BorrowFrame(), plane_index);
    };
    const auto prior_clean = pixels(0U);
    const auto prior_semantic = pixels(1U);
    gate->FailNextPublicationAt(ExploreAcceptanceGate::PublicationStage::ProductPrepared);
    if (empty_candidate)
        static_cast<void>(system.UpdateFilter({.filter = {.minimum_instances = 2U}, .overlay = incumbent.overlay}));
    else
        system.UpdateViewport({.viewport = {.extent = {32U, 8U}, .first_row = 17U, .row_count = 1U, .columns = 4U}});
    wait([&] {
        const auto snapshot = system.snapshot();
        return !snapshot.failure.empty() && snapshot.viewport == viewport && snapshot.gallery.slots.size() == 16U &&
               std::ranges::all_of(snapshot.gallery.slots, [](bool ready) { return ready; });
    });
    // No explicit Begin/Advance is issued: the real ExploreSystem failure path
    // restores demand and schedules the continuation that consumes held input.
    const auto restored = system.snapshot();
    CHECK(restored.dataset.identity == incumbent.dataset.identity);
    CHECK(restored.order.visible_indices == incumbent.order.visible_indices);
    CHECK(restored.order.matching_count == incumbent.order.matching_count);
    const auto clean = pixels(0U);
    const auto semantic = pixels(1U);
    for (std::size_t slot = 0U; slot < incumbent.gallery.slots.size(); ++slot) {
        if (!incumbent.gallery.slots[slot]) continue;
        for (std::size_t row = 0U; row < 8U; ++row) {
            const auto offset = ((slot / 4U * 8U + row) * 32U + slot % 4U * 8U) * 4U;
            CHECK(std::equal(clean.begin() + offset, clean.begin() + offset + 32U, prior_clean.begin() + offset));
            CHECK(std::equal(semantic.begin() + offset, semantic.begin() + offset + 32U, prior_semantic.begin() + offset));
        }
    }
    wait([&] { return evidence.Count(VisualDiagnosticOperation::GalleryGpuCompleted) == 72U; });
    const auto reads_of_held = [&] {
        std::scoped_lock lock(evidence.mutex);
        return std::ranges::count_if(evidence.facts, [&](const auto& fact) {
            return fact.operation == VisualDiagnosticOperation::GalleryReadStarted && fact.detail == held_index;
        });
    };
    CHECK(reads_of_held() == 1);
    auto overlay = restored.overlay;
    overlay.show_masks = !overlay.show_masks;
    const auto first_revision = system.snapshot().frame.revision;
    static_cast<void>(system.UpdateOverlay(overlay));
    wait([&] { return system.snapshot().frame.revision > first_revision; });
    CHECK(pixels(0U) == clean);
    // Positions 68..71 address high slots in the restored 72-slot ring, beyond
    // the failed 60-slot (or zero-slot) candidate's scheduler dimensions.
    const auto semantic_revision = system.snapshot().frame.revision;
    static_cast<void>(system.UpdateOverlay(restored.overlay));
    wait([&] { return system.snapshot().frame.revision > semantic_revision; });
    CHECK(pixels(0U) == clean);
    CHECK(pixels(1U) == semantic);
    CHECK(reads_of_held() == 1);
    system.Shutdown();
    CHECK(system.stopped());
}

TEST_CASE("Native detail class selection reuses exact clean pixels and unchanged output", "[explore][native][detail]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-detail"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U};
    gallery.Open(path);
    gallery.Drain();
    gallery.plan.mode = ExploreMode::Detail;
    gallery.plan.selected_image = 0U;
    gallery.plan.overlay.show_boxes = false;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    const auto clean = gallery.Pixels(0U);
    const auto semantic = gallery.Pixels(1U);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    const auto augmented = gallery.evidence.Count(VisualDiagnosticOperation::ExploreAugmentationBatchPrepared);
    const auto allocations = gallery.evidence.Count(VisualDiagnosticOperation::ExploreStorageGrown);
    const auto published = gallery.publications;
    const auto copies = gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer);
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.publications == published);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreStorageGrown) == allocations);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreCacheTransfer) == copies);
    CHECK(gallery.Pixels(0U) == clean);
    CHECK(gallery.Pixels(1U) == semantic);

    gallery.plan.overlay.class_selection.mode = ExploreClassSelectionMode::None;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.Pixels(0U) == clean);
    CHECK(gallery.Pixels(1U) == std::vector<std::uint8_t>(8U * 8U * 4U, 0U));
    gallery.plan.overlay.class_selection = {.mode = ExploreClassSelectionMode::Subset, .classes = {0U}};
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.Pixels(1U) == semantic);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreAugmentationBatchPrepared) == augmented);
    gallery.evidence.enabled.store(false);
    ++gallery.plan.generation;
    gallery.plan.viewport.extent = {80U, 40U};
    gallery.plan.viewport.row_count = 5U;
    gallery.plan.focused_image = 0U;
    gallery.plan.detail.show_original_dimensions = true;
    gallery.demand->store(gallery.plan.generation);
    const auto detail_publications = gallery.publications;
    const auto detail_acquisitions = gallery.acquisitions;
    // CLEANUP-IGNORE: This no-allocation detail rerender checkpoint is distinct from the gallery initialization
    // allocation measurement despite using the same scoped counter.
    const auto detail_revision = gallery.runtime->Completed().revision();
    std::size_t host_allocations;
    {
        native_gallery_allocations::Scope measurement;
        gallery.Begin();
        host_allocations = native_gallery_allocations::count;
    }
    CHECK(host_allocations == 0U);
    CHECK(gallery.publications == detail_publications);
    CHECK(gallery.acquisitions == detail_acquisitions);
    CHECK(gallery.runtime->Completed().revision() == detail_revision);
    CHECK(gallery.Pixels(0U) == clean);
    CHECK(gallery.Pixels(1U) == semantic);
}

TEST_CASE("Native superseded background collision preserves incumbent planes and meaning", "[explore][native][cache][shutdown]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-background"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U, false, true};
    gallery.Open(path);
    gallery.Drain();
    gallery.plan.overlay.show_boxes = false;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    const auto clean = gallery.Pixels(0U);
    const auto semantic = gallery.Pixels(1U);
    // First window retains 0..63. Row eight shifts it to 4..67, so
    // speculative 64..67 collide with incumbent 0..3 after a hot Begin.
    gallery.plan.viewport.first_row = 8U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin().remaining_tiles == 0U);
    GalleryGpuPause pause;
    gallery.pause = &pause;
    const auto submitted_count = gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted);
    auto submitted = std::async(std::launch::async, [&] {
        gallery.runtime->BindContext();
        for (;;) {
            const auto observed = gallery.evidence.Epoch();
            static_cast<void>(gallery.algorithm->AdvanceGallery());
            if (gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted) != submitted_count) return;
            gallery.evidence.Wait(observed);
        }
    });
    const bool entered = pause.Wait();
    gallery.demand->store(0U);
    pause.Release();
    REQUIRE(entered);
    submitted.get();
    gallery.pause = nullptr;
    gallery.runtime->BindContext();
    gallery.plan.viewport.first_row = 0U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    CHECK(gallery.Begin().remaining_tiles == 0U);
    CHECK(gallery.Pixels(0U) == clean);
    CHECK(gallery.Pixels(1U) == semantic);
    // CLEANUP-IGNORE: This background-collision read count proves retained-cache reuse at a separate cancellation
    // boundary from the semantic-selection scenario.
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    gallery.plan.overlay.class_selection.mode = ExploreClassSelectionMode::None;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.Pixels(0U) == clean);
    CHECK(gallery.Pixels(1U) == std::vector<std::uint8_t>(4U * 8U * 16U * 4U, 0U));
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    gallery.plan.overlay.class_selection.mode = ExploreClassSelectionMode::All;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.Pixels(1U) == semantic);
}

TEST_CASE("Native submitted GPU work settles after Stop without publication or new reads", "[explore][native][shutdown]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-gpu"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U};
    gallery.Open(path);
    const auto retained = gallery.Pixels(0U);
    gallery.WaitForReadyTiles();
    GalleryGpuPause pause;
    gallery.pause = &pause;
    auto submitted = std::async(std::launch::async, [&] {
        gallery.runtime->BindContext();
        return gallery.Step();
    });
    const bool entered = pause.Wait();
    gallery.demand->store(0U);
    pause.Release();
    REQUIRE(entered);
    static_cast<void>(submitted.get());
    gallery.pause = nullptr;
    gallery.runtime->BindContext();
    CHECK(gallery.Pixels(0U) == retained);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    static_cast<void>(gallery.algorithm->AdvanceGallery());
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
}

TEST_CASE("acceptance gate retains one reader across settled frontend workflows", "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    std::array<int, 2U> sockets{};
    REQUIRE(::socketpair(AF_UNIX, transport | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0]};
    ExploreAcceptanceGate gate{sockets[1]};
    std::promise<contracts::IntegrationControlReceipt> advanced;
    auto delivered = advanced.get_future();
    gate.SetFrontendCommand([&advanced](const auto receipt) {
        advanced.set_value(receipt);
        return true;
    });
    const std::uint8_t release_held = 4U;
    REQUIRE(::send(commands.get(), &release_held, sizeof(release_held), MSG_NOSIGNAL) == sizeof(release_held));
    REQUIRE(gate.ClaimHeldCompletion());
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(0U, 4U, 8U, 64U); });
    const auto held_status = held.wait_for(std::chrono::seconds{2});
    if (held_status != std::future_status::ready) gate.Stop();
    REQUIRE(held_status == std::future_status::ready);
    CHECK(held.get() == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 2U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Advance, .sequence = 1U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    const std::uint8_t advance = 8U;
    REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    const auto status = delivered.wait_for(std::chrono::seconds{2});
    if (status != std::future_status::ready) gate.Stop();
    REQUIRE(status == std::future_status::ready);
    CHECK((delivered.get() == contracts::IntegrationControlReceipt{.kind = Kind::Advance, .sequence = 2U}));
    CHECK(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 2U, .progress = 1U}));
    gate.SetFrontendCommand({});
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 2U}));
    gate.Stop();
    CHECK(gate.ClaimTerminalReport());
    CHECK_FALSE(gate.ClaimTerminalReport());
}

TEST_CASE("acceptance gate drains queued worker commands and wakes stale waits", "[explore][acceptance][control]") {
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    std::array<int, 2U> sockets{};
    REQUIRE(::socketpair(AF_UNIX, transport | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0]};
    ExploreAcceptanceGate gate{sockets[1]};
    for (const auto command : std::array<std::uint8_t, 3U>{1U, 2U, 4U})
        REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    gate.AdvanceGeneration(7U);
    auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
    const auto status = initial.wait_for(std::chrono::seconds{2});
    if (status != std::future_status::ready) gate.Stop();
    REQUIRE(status == std::future_status::ready);
    CHECK(initial.get() == ExploreAcceptanceGate::WaitResult::Proceed);
    REQUIRE(gate.ClaimHeldCompletion());
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 4U, 8U, 64U); });
    const auto held_status = held.wait_for(std::chrono::seconds{2});
    if (held_status != std::future_status::ready) gate.Stop();
    REQUIRE(held_status == std::future_status::ready);
    CHECK(held.get() == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    gate.AdvanceGeneration(8U);
    CHECK(gate.AwaitInitialRelease(7U) == ExploreAcceptanceGate::WaitResult::Stale);
    gate.Stop();
    CHECK(gate.AwaitInitialRelease(8U) == ExploreAcceptanceGate::WaitResult::Stale);
}

TEST_CASE("acceptance gate rejects premature advancement and unavailable callbacks", "[explore][acceptance][control]") {
    const unsigned terminal_path = GENERATE(0U, 1U, 2U, 3U, 4U);
    std::array<int, 2U> sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    mmltk::common::io::ScopedFd commands{sockets[0]};
    ExploreAcceptanceGate gate{sockets[1]};
    std::promise<void> invoked;
    auto invocation = invoked.get_future();
    gate.SetFrontendCommand([terminal_path, &invoked](auto) {
        invoked.set_value();
        return terminal_path == 4U;
    });
    if (terminal_path == 1U || terminal_path == 4U)
        REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Settled, .sequence = 1U}));
    if (terminal_path == 2U) REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Failed, .sequence = 1U}));
    gate.AdvanceGeneration(1U);
    const std::uint8_t advance = 8U;
    if (terminal_path < 2U || terminal_path == 4U)
        REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 1U || terminal_path == 4U) {
        const auto callback_status = invocation.wait_for(std::chrono::seconds{2});
        if (callback_status != std::future_status::ready) gate.Stop();
        REQUIRE(callback_status == std::future_status::ready);
    }
    if (terminal_path == 4U) REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 3U) commands.reset();
    auto waiting = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(1U); });
    const auto status = waiting.wait_for(std::chrono::seconds{2});
    if (status != std::future_status::ready) gate.Stop();
    REQUIRE(status == std::future_status::ready);
    CHECK(waiting.get() == ExploreAcceptanceGate::WaitResult::Stale);
    CHECK(gate.ClaimTerminalReport());
}

}  // namespace
}  // namespace mmltk::controller
