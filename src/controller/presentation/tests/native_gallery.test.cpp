#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cuda_runtime_api.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <bit>
#include "src/controller/subsystems/explore/detail/gallery_atlas.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
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
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "filesystem_test_utils.hpp"
#include "src/backend/data/compiled_format.h"
#include "src/acceptance/tests/async_test_utils.hpp"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
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
    [[nodiscard]] std::vector<std::uint64_t> ReadAdmissions(const std::uint64_t generation) {
        std::scoped_lock lock(mutex);
        std::vector<std::uint64_t> images;
        for (const auto& fact : facts)
            if (fact.operation == VisualDiagnosticOperation::GalleryReadScheduled && fact.generation == generation)
                images.push_back(fact.detail);
        return images;
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

class GalleryReadPause final {
   public:
    explicit GalleryReadPause(const std::uint32_t image, const bool fail = false) : image_(image), fail_(fail) {}
    struct ReleaseGuard final {
        GalleryReadPause& pause;
        ~ReleaseGuard() { pause.Release(); }
    };
    void Bind(ExploreAcceptanceGate& gate) {
        gate.SetReadObserver(this, [](void* context, std::uint64_t, const std::uint32_t index) {
            auto& pause = *static_cast<GalleryReadPause*>(context);
            if (index != pause.image_) return;
            std::unique_lock lock(pause.mutex_);
            pause.entered_ = true;
            pause.changed_.notify_all();
            pause.changed_.wait(lock, [&] { return pause.released_; });
            if (std::exchange(pause.fail_, false)) throw std::runtime_error("deterministic native source read failure");
        });
    }
    void Wait() {
        std::unique_lock lock(mutex_);
        REQUIRE(changed_.wait_for(lock, std::chrono::seconds{10}, [&] { return entered_; }));
    }
    void Release() {
        std::scoped_lock lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    ~GalleryReadPause() { Release(); }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint32_t image_;
    bool fail_;
    bool entered_ = false;
    bool released_ = false;
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

[[nodiscard]] std::vector<std::uint8_t> copy_atlas_plane(mmltk::frameworks::gpu::BorrowedImageProductReadView product,
                                                         const std::size_t plane_index, const ExploreAtlasLayout& layout) {
    REQUIRE(product.valid());
    REQUIRE(layout.row_capacity != 0U);
    REQUIRE(layout.row_count <= layout.row_capacity);
    REQUIRE(layout.row_origin < layout.row_capacity);
    const auto descriptor = product.plane(plane_index).plane().descriptor;
    const auto row_bytes = static_cast<std::size_t>(layout.columns) * layout.card_extent * 4U;
    REQUIRE(descriptor.row_bytes() == row_bytes);
    REQUIRE(descriptor.height >= layout.row_capacity * layout.card_extent);
    const auto physical = copy_product_plane(std::move(product), plane_index);
    const auto row_size = row_bytes * layout.card_extent;
    std::vector<std::uint8_t> logical(row_size * layout.row_count);
    for (std::size_t row = 0U; row < layout.row_count; ++row)
        std::copy_n(physical.data() + (layout.row_origin + row) % layout.row_capacity * row_size, row_size,
                    logical.data() + row * row_size);
    return logical;
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
    mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput gallery_product, detail_product;
    ExploreAtlasLayout displayed_layout{};
    ExploreMode displayed_mode = ExploreMode::Gallery;
    std::uint32_t atlas_rows = 0U, atlas_side = 0U, atlas_columns = 0U;
    VisualExtent GalleryExtent() {
        const auto side = explore_atlas_card_extent(plan.viewport);
        if (atlas_side != side || atlas_columns != plan.viewport.columns) atlas_rows = 0U;
        atlas_side = side;
        atlas_columns = plan.viewport.columns;
        atlas_rows = std::max(atlas_rows, std::min(std::bit_ceil(plan.viewport.row_count),
                                                   static_cast<std::uint32_t>(kExploreVisibleItemCapacity / plan.viewport.columns)));
        return {plan.viewport.extent.width, atlas_rows * side};
    }
    std::size_t publications = 0U;
    std::size_t acquisitions = 0U;
    ExploreOverlay issued_semantics{};
    ExploreStorageFootprint transaction_storage{};
    GalleryGpuPause* pause = nullptr;
    GalleryGpuPause* probe_pause = nullptr;
    std::optional<ExploreAcceptanceGate::SubmissionStage> supersede_at;
    std::atomic_bool superseded{false};
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
                        if (gallery.supersede_at == stage) {
                            gallery.demand->store(0U);
                            gallery.superseded.store(true);
                        }
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
        algorithm->PrepareOutputPublication(change, plan.mode);
        ExploreGalleryPublication publication;
        if (change == ExploreOutputChange::Unchanged) {
            if (plan.mode == ExploreMode::Gallery)
                publication = algorithm->BeginGallery(plan, order, 2U, {}, {}, 0U);
            else
                algorithm->RenderDetail(plan, 2U, {}, {}, 0U);
            if (commit) runtime->SelectOutput(plan.mode == ExploreMode::Gallery ? gallery_product : detail_product);
        } else {
            ++acquisitions;
            auto baseline = plan.mode == ExploreMode::Detail && change == ExploreOutputChange::Semantic
                                ? detail_product
                                : mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput{};
            auto output = runtime->TryAcquireOutput(baseline);
            REQUIRE(output.valid());
            const auto extent = plan.mode == ExploreMode::Gallery ? GalleryExtent() : algorithm->DetailExtent(plan);
            const auto submit = [&](auto clean, auto semantic, auto stream) {
                if (plan.mode == ExploreMode::Gallery)
                    publication = algorithm->BeginGallery(plan, order, 2U, clean, semantic, stream);
                else
                    algorithm->RenderDetail(plan, 2U, clean, semantic, stream);
            };
            if (plan.mode == ExploreMode::Gallery)
                runtime->PublishRetained(output, extent.width, extent.height, submit);
            else {
                algorithm->PrepareDetailOutput(output.allocations()[0U]);
                runtime->Publish(output, extent.width, extent.height, submit);
            }
            if (evidence.enabled.load()) transaction_storage = algorithm->StorageFootprint();
            if (commit) {
                (plan.mode == ExploreMode::Gallery ? gallery_product : detail_product) = runtime->CommitOutput(std::move(output));
                ++publications;
            }
        }
        if (commit) {
            algorithm->CommitOutputPublication();
            displayed_mode = plan.mode;
            if (plan.mode == ExploreMode::Gallery) displayed_layout = publication.layout;
        } else
            REQUIRE(algorithm->RollbackOutputPublication());
        return publication;
    }
    ExploreGalleryPublication Step(const bool commit = true) {
        const auto advanced = algorithm->AdvanceGallery();
        if (algorithm->HasGalleryTiles()) {
            algorithm->PrepareOutputPublication(ExploreOutputChange::Semantic);
            ++acquisitions;
            mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
            auto output = runtime->TryAcquireOutput(baseline);
            REQUIRE(output.valid());
            const auto extent = GalleryExtent();
            runtime->PublishRetained(output, extent.width, extent.height, [&](auto clean, auto semantic, auto stream) {
                if (pause) pause->Submit(stream);
                static_cast<void>(algorithm->PublishGalleryTiles(clean, semantic, stream));
            });
            if (evidence.enabled.load()) transaction_storage = algorithm->StorageFootprint();
            if (commit && demand->load() == plan.generation) {
                gallery_product = runtime->CommitOutput(std::move(output));
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
        if (displayed_mode != ExploreMode::Gallery) return copy_product_plane(runtime->Borrow(), plane_index);
        return copy_atlas_plane(runtime->Borrow(), plane_index, displayed_layout);
    }
};

TEST_CASE("Atlas directories reconcile exact allocations and invalidate only touched physical cells", "[explore][atlas]") {
    using namespace explore_detail;
    using namespace mmltk::frameworks::gpu;
    GalleryAtlas directory;
    const auto plane = [](const std::uint64_t owner, const std::uint64_t identity, const ImagePlaneKind kind) {
        return ImagePlaneView{.data = 1U,
                              .descriptor = {.kind = kind, .width = 16U, .height = 64U, .pitch_bytes = 64U},
                              .allocation = {identity, 16U, 64U, owner}};
    };
    auto clean = plane(1U, 11U, ImagePlaneKind::Clean);
    auto semantic = plane(2U, 12U, ImagePlaneKind::Semantic);
    const auto other_clean = plane(3U, 13U, ImagePlaneKind::Clean);
    const auto other_semantic = plane(4U, 14U, ImagePlaneKind::Semantic);
    GalleryThumbnailCache::Identity pixels{.dataset = 3U, .seed = 5U, .extent = 8U};
    ExploreViewport viewport{.extent = {16U, 40U}, .first_row = 6U, .row_count = 5U, .columns = 2U};
    auto first = std::make_shared<const GalleryTileMeaning>();
    auto overlap = std::make_shared<const GalleryTileMeaning>();
    auto layout = directory.Begin(clean, semantic, viewport, pixels);
    CHECK(layout.row_capacity == 8U);
    CHECK(layout.row_origin == 6U);
    CHECK(directory.Physical(4U) == 0U);
    const auto first_cell = directory.Physical(0U);
    const auto overlap_cell = directory.Physical(2U);
    directory.Touch(first_cell);
    directory.Stage(first_cell, first, 7U);
    directory.Touch(overlap_cell);
    directory.Stage(overlap_cell, overlap, 7U);
    directory.Commit();
    ++viewport.first_row;
    viewport.row_count = 6U;
    viewport.extent.height = 48U;
    static_cast<void>(directory.Begin(other_clean, other_semantic, viewport, pixels));
    CHECK_FALSE(directory.Contains(directory.Physical(0U), overlap, 7U));
    directory.Rollback();
    static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
    CHECK(directory.Physical(0U) == overlap_cell);
    CHECK(directory.Contains(overlap_cell, overlap, 7U));
    CHECK(directory.ContainsClean(overlap_cell, overlap));
    CHECK_FALSE(directory.Contains(overlap_cell, overlap, 8U));
    // A partially submitted or cancelled write cannot resurrect its old tag.
    directory.Touch(overlap_cell);
    directory.Stage(overlap_cell, overlap, 8U);
    directory.Rollback();
    viewport.row_count = 5U;
    viewport.extent.height = 40U;
    static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
    CHECK_FALSE(directory.Contains(overlap_cell, overlap, 7U));
    CHECK_FALSE(directory.Contains(overlap_cell, overlap, 8U));
    CHECK(directory.Contains(first_cell, first, 7U));
    directory.Touch(overlap_cell);
    directory.Stage(overlap_cell, {}, 0U, true);
    directory.Commit();
    static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
    CHECK(directory.Empty(overlap_cell, true));
    CHECK_FALSE(directory.Empty(overlap_cell, false));
    directory.Commit();
    directory.Invalidate(clean.allocation);
    static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
    CHECK_FALSE(directory.Contains(first_cell, first, 7U));
    directory.Touch(first_cell);
    directory.Stage(first_cell, first, 7U);
    directory.Commit();
    // Recycled addresses still have a new allocation identity after growth.
    ++clean.allocation.identity;
    static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
    CHECK_FALSE(directory.Contains(first_cell, first, 7U));
}

TEST_CASE("Detail framework prewrite failure invalidates its atlas before entering the renderer", "[explore][atlas]") {
    using namespace explore_detail;
    using namespace mmltk::frameworks::gpu;
    auto backend = std::make_shared<test_support::FakeImageBackend>();
    SystemImageRuntime runtime{
        {.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 3U}};
    GalleryAtlas directory;
    const ExploreViewport viewport{.extent = {24U, 8U}, .columns = 3U};
    const GalleryThumbnailCache::Identity pixels{.dataset = 1U, .extent = 8U};
    const auto meaning = std::make_shared<const GalleryTileMeaning>();
    const auto fill = [](const ImagePlaneView plane, const unsigned char value) {
        for (std::uint32_t row = 0U; row != plane.descriptor.height; ++row)
            std::memset(reinterpret_cast<void*>(plane.data + row * plane.descriptor.pitch_bytes), value, plane.descriptor.row_bytes());
    };
    const auto publish = [&](const unsigned char value) {
        SystemImageRuntime::CompletedOutput baseline;
        auto output = runtime.TryAcquireOutput(baseline);
        REQUIRE(output.valid());
        runtime.PublishRetained(output, 24U, 8U, [&](auto clean, auto semantic, auto) {
            static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
            fill(clean, value);
            fill(semantic, value);
            for (std::size_t cell = 0U; cell != 3U; ++cell) {
                directory.Touch(cell);
                directory.Stage(cell, cell == 0U ? meaning : nullptr, 0U, cell == 1U);
            }
        });
        directory.Commit();
        return runtime.CommitOutput(std::move(output));
    };
    const auto check_selected = [&](const unsigned char value) {
        auto selected = runtime.Borrow();
        REQUIRE(selected.valid());
        CHECK(*reinterpret_cast<const unsigned char*>(selected.plane(0U).plane().data) == value);
        static_cast<void>(directory.Begin(selected.plane(0U).plane(), selected.plane(1U).plane(), viewport, pixels));
        CHECK(directory.Contains(0U, meaning, 0U));
        CHECK(directory.Empty(1U, true));
        CHECK(directory.Empty(2U, false));
        directory.Commit();
    };
    auto first = publish(17U);
    auto second = publish(33U);
    auto incumbent = publish(49U);
    first = {};
    SystemImageRuntime::CompletedOutput baseline;
    auto candidate = runtime.TryAcquireOutput(baseline);
    REQUIRE(candidate.valid());
    const auto allocation = candidate.allocations()[0U];
    directory.Invalidate(allocation);
    bool entered = false;
    // Clean is cleared, then semantic clear fails before the callback.
    backend->FailAfter(test_support::FakeImageBackend::FailurePoint::Clear, 1U);
    CHECK_THROWS(runtime.Publish(candidate, 24U, 8U, [&](auto, auto, auto) { entered = true; }));
    CHECK_FALSE(entered);
    CHECK(runtime.Completed().revision() == incumbent.revision());
    check_selected(49U);
    directory.Rollback();
    candidate = {};
    candidate = runtime.TryAcquireOutput(baseline);
    REQUIRE(candidate.valid());
    CHECK(candidate.allocations()[0U] == allocation);
    runtime.PublishRetained(candidate, 24U, 8U, [&](auto clean, auto semantic, auto) {
        CHECK(*reinterpret_cast<const unsigned char*>(clean.data) == 0U);
        CHECK(*reinterpret_cast<const unsigned char*>(semantic.data) == 17U);
        static_cast<void>(directory.Begin(clean, semantic, viewport, pixels));
        CHECK_FALSE(directory.Contains(0U, meaning, 0U));
        CHECK_FALSE(directory.Empty(1U, true));
        CHECK_FALSE(directory.Empty(2U, false));
        fill(clean, 71U);
        fill(semantic, 71U);
        for (std::size_t cell = 0U; cell != 3U; ++cell) {
            directory.Touch(cell);
            directory.Stage(cell, cell == 0U ? meaning : nullptr, 0U, cell == 1U);
        }
        // Submission alone cannot install the new tags.
        CHECK_FALSE(directory.Contains(0U, meaning, 0U));
    });
    directory.Commit();
    auto replacement = runtime.CommitOutput(std::move(candidate));
    CHECK(replacement.valid());
    CHECK(second.valid());
    CHECK(runtime.Completed().revision() == replacement.revision());
    check_selected(71U);
}

// CLEANUP-IGNORE: These independent gallery-return and initialization-rollback cases share only CUDA availability and dataset fixture
// setup.
TEST_CASE("Native gallery return selects its actual completed product and resumes partial demand", "[explore][native][atlas][detail]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const bool complete = GENERATE(false, true);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-return"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{4U};
    gallery.plan.viewport.first_row = 3U;
    gallery.plan.viewport.row_count = 5U;
    gallery.plan.viewport.extent.height = 40U;
    gallery.Open(path);
    if (complete) {
        gallery.Drain();
    } else {
        gallery.WaitForReadyTiles();
        static_cast<void>(gallery.Step());
    }
    const auto revision = gallery.runtime->Completed().revision();
    const auto before = gallery.Pixels(0U);
    const auto layout = gallery.displayed_layout;
    const auto readiness = gallery.algorithm->AdvanceGallery().ready_slots;
    REQUIRE((complete || std::ranges::any_of(readiness, [](bool value) { return !value; })));
    gallery.plan.mode = ExploreMode::Detail;
    gallery.plan.selected_image = 12U;
    gallery.demand->store(++gallery.plan.generation);
    gallery.Begin();
    REQUIRE(gallery.algorithm->Document());
    const auto detail_revision = gallery.runtime->Completed().revision();
    REQUIRE(detail_revision != revision);
    gallery.plan.mode = ExploreMode::Gallery;
    gallery.plan.selected_image.reset();
    gallery.demand->store(++gallery.plan.generation);
    const auto acquisitions = gallery.acquisitions;
    gallery.Begin();
    CHECK(gallery.acquisitions == acquisitions);
    CHECK(gallery.runtime->Completed().revision() == revision);
    CHECK(gallery.displayed_layout == layout);
    CHECK(gallery.Pixels(0U) == before);
    CHECK(gallery.algorithm->AdvanceGallery().ready_slots == readiness);
    gallery.Drain();
    const auto ready_pixels = gallery.Pixels(0U);
    gallery.plan.viewport.row_count = 6U;
    gallery.plan.viewport.extent.height = 48U;
    gallery.demand->store(++gallery.plan.generation);
    gallery.Begin();
    const auto six = gallery.Pixels(0U);
    CHECK(std::equal(ready_pixels.begin(), ready_pixels.end(), six.begin()));
    gallery.plan.viewport.row_count = 5U;
    gallery.plan.viewport.extent.height = 40U;
    gallery.demand->store(++gallery.plan.generation);
    gallery.Begin();
    CHECK(gallery.Pixels(0U) == ready_pixels);
    gallery.plan.mode = ExploreMode::Detail;
    gallery.plan.selected_image = 12U;
    gallery.demand->store(++gallery.plan.generation);
    gallery.Begin();
    ++gallery.plan.augmentation.seed;
    gallery.plan.mode = ExploreMode::Gallery;
    gallery.plan.selected_image.reset();
    gallery.demand->store(++gallery.plan.generation);
    const auto changed = gallery.Begin();
    CHECK(changed.remaining_tiles == 20U);
    CHECK(gallery.runtime->Completed().revision() != revision);
    gallery.Drain();
}

TEST_CASE("Native gallery rollback and detail retain an unfinished independent image read",
          "[explore][native][atlas][transaction][detail]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const bool change_geometry = GENERATE(false, true);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-held-return"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    GalleryReadPause pause{0U};
    NativeGallery gallery{4U, false, true};
    pause.Bind(*gallery.gate);
    std::future<void> progress;
    GalleryReadPause::ReleaseGuard release{pause};
    gallery.Open(path);
    static_cast<void>(gallery.Step());
    pause.Wait();
    auto ready = gallery.algorithm->AdvanceGallery().ready_slots;
    std::array pixels{gallery.Pixels(0U), gallery.Pixels(1U)};
    const auto check_visible_continuity = [&] {
        const auto current = gallery.algorithm->AdvanceGallery().ready_slots;
        REQUIRE(current.size() == ready.size());
        const auto& layout = gallery.displayed_layout;
        for (std::size_t plane = 0U; plane < pixels.size(); ++plane) {
            auto next = gallery.Pixels(plane);
            for (std::size_t slot = 0U; slot < ready.size(); ++slot) {
                if (!ready[slot]) continue;
                REQUIRE(current[slot]);
                for (std::size_t row = 0U; row < layout.card_extent; ++row) {
                    const auto offset = ((slot / layout.columns * layout.card_extent + row) * layout.columns + slot % layout.columns) *
                                        layout.card_extent * 4U;
                    CHECK(std::ranges::equal(std::span{pixels[plane]}.subspan(offset, layout.card_extent * 4U),
                                             std::span{next}.subspan(offset, layout.card_extent * 4U)));
                }
            }
            pixels[plane] = std::move(next);
        }
        ready = current;
    };
    for (;;) {
        const auto observed = gallery.evidence.Epoch();
        const auto partial = gallery.Step();
        check_visible_continuity();
        if (partial.remaining_tiles == 1U) break;
        gallery.evidence.Wait(observed);
    }
    const auto prior_plan = gallery.plan;
    const auto before = gallery.Pixels(0U);
    progress = std::async(std::launch::async, [&] {
        gallery.runtime->BindContext();
        if (change_geometry) {
            gallery.plan.viewport.row_count = 3U;
            gallery.plan.viewport.extent.height = 24U;
        } else {
            gallery.plan.overlay.show_boxes = !gallery.plan.overlay.show_boxes;
        }
        gallery.demand->store(++gallery.plan.generation);
        gallery.Begin(nullptr, false);
        const auto next_generation = gallery.plan.generation + 1U;
        gallery.plan = prior_plan;
        gallery.plan.generation = next_generation;
        gallery.demand->store(next_generation);
        gallery.Begin();
        CHECK(gallery.Pixels(0U) == before);
        const auto retained_revision = gallery.runtime->Completed().revision();
        const auto retained_allocation = gallery.runtime->Borrow().plane(0U).plane().allocation;
        gallery.plan.mode = ExploreMode::Detail;
        gallery.plan.selected_image = 0U;
        gallery.demand->store(++gallery.plan.generation);
        gallery.Begin();
        REQUIRE(gallery.algorithm->Document());
        auto detail_read = gallery.runtime->Borrow();
        const auto detail_revision = detail_read.plane(0U).revision();
        gallery.plan.mode = ExploreMode::Gallery;
        gallery.plan.selected_image.reset();
        gallery.demand->store(++gallery.plan.generation);
        gallery.Begin();
        CHECK(gallery.runtime->Completed().revision() == retained_revision);
        CHECK(gallery.runtime->Borrow().plane(0U).plane().allocation == retained_allocation);
        CHECK(detail_read.plane(0U).revision() == detail_revision);
        CHECK(detail_revision > retained_revision);
        CHECK(gallery.Pixels(0U) == before);
        check_visible_continuity();
        const auto returned = gallery.algorithm->AdvanceGallery();
        REQUIRE(returned.ready_slots.size() == 8U);
        CHECK_FALSE(returned.ready_slots[0U]);
        CHECK(std::ranges::count(returned.ready_slots, true) == 7);
    });
    mmltk::testsupport::await_test_future(progress, "gallery rollback and detail while one disk read remains held");
    gallery.runtime->BindContext();
    pause.Release();
    gallery.Drain();
    check_visible_continuity();
    CHECK(std::ranges::all_of(gallery.algorithm->AdvanceGallery().ready_slots, [](bool slot_ready) { return slot_ready; }));
}

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
        CHECK(background_tiles == explore_detail::GalleryThumbnailCache::WindowCount(203U, gallery.plan.viewport) - columns * 2U);
    }
    const auto storage = gallery.algorithm->StorageFootprint();
    const auto capacity = std::min<std::size_t>(203U, kExploreVisibleItemCapacity + 8U * columns);
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
    REQUIRE(reads == explore_detail::GalleryThumbnailCache::WindowCount(203U, gallery.plan.viewport));
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
        std::size_t restored_tiles = 0U;
        constexpr std::size_t tile_bytes = 8U * 8U * 4U;
        for (const auto& fact : gallery.evidence.facts)
            if (fact.operation == VisualDiagnosticOperation::ExploreCacheTransfer && fact.generation == gallery.plan.generation) {
                REQUIRE(fact.detail < columns * 2U);
                if (fact.context.condition == 1U) {
                    CHECK(fact.value == tile_bytes);
                    semantic_copies += fact.value;
                } else {
                    CHECK(fact.context.condition == 0U);
                    CHECK(fact.value == 2U * tile_bytes);
                    ++restored_tiles;
                }
            }
        // Each physical candidate reconciles its own older cells before the
        // semantic update. Every visible tile still gets one semantic copy.
        CHECK(semantic_copies == columns * 2U * tile_bytes);
        CHECK(restored_tiles <= columns * 2U);
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

TEST_CASE("Native five six five row demand immediately retains pixel identity through reorder", "[explore][native][cache]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-row-count"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U, 30U);
    NativeGallery gallery{5U};
    gallery.plan.viewport = {.extent = {40U, 40U}, .row_count = 5U, .columns = 5U};
    gallery.Open(path);
    gallery.Drain();
    const auto initial_pixels = gallery.Pixels(0U);
    const auto initial_semantics = gallery.Pixels(1U);
    const auto storage = gallery.algorithm->StorageFootprint();
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadScheduled);
    const auto renders = gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted);
    std::vector<std::uint8_t> all_pixels;
    for (const auto rows : {6U, 5U, 6U, 5U}) {
        gallery.plan.viewport.row_count = rows;
        gallery.plan.viewport.extent.height = rows * 8U;
        ++gallery.plan.generation;
        gallery.demand->store(gallery.plan.generation);
        const auto publication = gallery.Begin();
        CHECK(publication.remaining_tiles == 0U);
        CHECK(std::ranges::all_of(publication.ready_slots, [](const bool ready) { return ready; }));
        const auto clean = gallery.Pixels(0U);
        const auto semantic = gallery.Pixels(1U);
        if (rows == 6U) all_pixels = clean;
        REQUIRE(clean.size() >= initial_pixels.size());
        CHECK(std::ranges::equal(std::span{clean}.first(initial_pixels.size()), initial_pixels));
        CHECK(std::ranges::equal(std::span{semantic}.first(initial_semantics.size()), initial_semantics));
        CHECK(gallery.algorithm->StorageFootprint().cache_cards == storage.cache_cards);
        CHECK(gallery.algorithm->StorageFootprint().cache_device_bytes == storage.cache_device_bytes);
        CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadScheduled) == reads);
        CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted) == renders);
    }
    auto reordered = gallery.algorithm->PrepareFilter({}, 83U, 2U, {});
    const auto order = gallery.algorithm->Visible(gallery.plan.viewport, &reordered).visible_indices;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin(&reordered).remaining_tiles == 0U);
    gallery.algorithm->Commit(std::move(reordered));
    const auto shuffled = gallery.Pixels(0U);
    for (std::size_t slot = 0U; slot < order.size(); ++slot)
        for (std::size_t row = 0U; row < 8U; ++row) {
            const auto destination = ((slot / 5U * 8U + row) * 40U + slot % 5U * 8U) * 4U;
            const auto source = ((order[slot] / 5U * 8U + row) * 40U + order[slot] % 5U * 8U) * 4U;
            CHECK(std::ranges::equal(std::span{shuffled}.subspan(destination, 32U), std::span{all_pixels}.subspan(source, 32U)));
        }
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadScheduled) == reads);
}

TEST_CASE("Native disk admission follows immediate forward four then backward four", "[explore][native][priority]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    const auto reverse = GENERATE(false, true);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-admission"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    NativeGallery gallery{2U};
    gallery.plan.viewport.first_row = 10U;
    gallery.plan.focused_image = 22U;
    gallery.plan.scroll_direction = reverse ? ExploreScrollDirection::Backward : ExploreScrollDirection::Forward;
    gallery.Open(path);
    gallery.Drain();
    std::vector<std::uint32_t> admitted;
    {
        std::scoped_lock lock(gallery.evidence.mutex);
        for (const auto& fact : gallery.evidence.facts)
            if (fact.operation == VisualDiagnosticOperation::GalleryReadScheduled)
                admitted.push_back(static_cast<std::uint32_t>(fact.detail));
    }
    std::vector<std::uint32_t> expected{22U, 20U, 21U, 23U};
    const auto ahead = [&] {
        for (std::uint32_t image = 24U; image < 32U; ++image)
            expected.push_back(image);
    };
    const auto behind = [&] {
        for (std::uint32_t row = 10U; row != 6U;) {
            --row;
            expected.push_back(row * 2U);
            expected.push_back(row * 2U + 1U);
        }
    };
    if (reverse) {
        behind();
        ahead();
    } else {
        ahead();
        behind();
    }
    CHECK(admitted == expected);
}

TEST_CASE("Native cold visible admission proceeds while obsolete speculation holds its physical lane", "[explore][native][priority]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-visible-admission"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    GalleryReadPause held{4U};
    NativeGallery gallery{2U, false, true};
    GalleryReadPause::ReleaseGuard release{held};
    held.Bind(*gallery.gate);
    gallery.Open(path);
    for (;;) {
        const auto observed = gallery.evidence.Epoch();
        static_cast<void>(gallery.Step());
        bool scheduled = false;
        {
            std::scoped_lock lock(gallery.evidence.mutex);
            scheduled = std::ranges::any_of(gallery.evidence.facts, [](const auto& fact) {
                return fact.operation == VisualDiagnosticOperation::GalleryReadScheduled && fact.detail == 4U;
            });
        }
        if (scheduled) break;
        gallery.evidence.Wait(observed);
    }
    held.Wait();
    gallery.plan.viewport.first_row = 20U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    CHECK(gallery.Begin().remaining_tiles == 4U);
    for (;;) {
        const auto observed = gallery.evidence.Epoch();
        static_cast<void>(gallery.algorithm->AdvanceGallery());
        const auto admitted = gallery.evidence.ReadAdmissions(gallery.plan.generation);
        if (!admitted.empty()) {
            CHECK(admitted.front() == 40U);
            CHECK(std::ranges::all_of(admitted, [](const auto image) { return image >= 40U && image < 44U; }));
            break;
        }
        // The other physical lane may still be finishing its incumbent read.
        // Its completion must admit current visible work while image 4 stays held.
        gallery.evidence.Wait(observed);
    }
    held.Release();
    gallery.Drain();
    CHECK(gallery.algorithm->Visible(gallery.plan.viewport).visible_indices == std::vector<std::uint32_t>{40U, 41U, 42U, 43U});
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
        const auto admitted = gallery.evidence.ReadAdmissions(gallery.plan.generation);
        REQUIRE_FALSE(admitted.empty());
        CHECK(admitted.front() == *gallery.plan.focused_image);
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
    CAPTURE(hold_probe);
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
        // These immediate rows are already in the four forward cached rows,
        // so the held work can enter the background tier without starving
        // a foreground publication on this same test thread.
        gallery.plan.viewport.first_row = 4U;
        ++gallery.plan.generation;
        gallery.demand->store(gallery.plan.generation);
        const auto visible = gallery.Begin();
        REQUIRE(std::ranges::all_of(visible.ready_slots, [](const bool ready) { return ready; }));
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
    // Useful read, obsolete failure, current failure, adopted failure, and shuffled identity.
    const auto scenario = GENERATE(0U, 1U, 2U, 3U, 4U);
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-commit"};
    const auto path = directory.path() / "compiled.bin";
    write_gallery_artifact(path, 0.25F, 9U);
    GalleryReadPause held{68U, scenario != 0U && scenario != 4U};
    NativeGallery gallery{4U, false, true};
    GalleryReadPause::ReleaseGuard release{held};
    held.Bind(*gallery.gate);
    gallery.plan.viewport = {.extent = {32U, 32U}, .first_row = 17U, .row_count = 4U, .columns = 4U};
    gallery.Open(path);
    static_cast<void>(gallery.algorithm->AdvanceGallery());
    held.Wait();
    if (scenario != 2U) {
        gallery.plan.viewport.row_count = 1U;
        gallery.plan.viewport.extent.height = 8U;
        if (scenario == 1U) gallery.plan.viewport.first_row = 30U;
        std::optional<ExploreOrderCandidate> reordered;
        if (scenario == 4U) {
            auto prepared = std::async(std::launch::async, [&] {
                gallery.runtime->BindContext();
                return gallery.algorithm->PrepareFilter({}, 83U, 2U, {});
            });
            const auto ready = prepared.wait_for(std::chrono::seconds{10});
            if (ready != std::future_status::ready) held.Release();
            REQUIRE(ready == std::future_status::ready);
            reordered = prepared.get();
            const auto found = std::ranges::find(reordered->order.visible_indices, 68U);
            REQUIRE(found != reordered->order.visible_indices.end());
            gallery.plan.viewport.first_row = static_cast<std::uint32_t>(found - reordered->order.visible_indices.begin()) / 4U;
        }
        ++gallery.plan.generation;
        gallery.demand->store(gallery.plan.generation);
        gallery.Begin(reordered ? &*reordered : nullptr);
        if (reordered) gallery.algorithm->Commit(std::move(*reordered));
        CHECK(gallery.algorithm->StorageFootprint().cache_cards == 203U);
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
        if (scenario == 0U || scenario == 4U) {
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
    const auto wait = [&](const char* stage, auto&& ready) {
        for (;;) {
            const auto observed = evidence.Epoch();
            if (ready()) return;
            try {
                evidence.Wait(observed);
            } catch (const std::runtime_error& failure) {
                // Collect context only after the existing completion deadline fails.
                const auto snapshot = system.snapshot();
                std::vector<VisualDiagnosticFact> facts;
                std::uint64_t epoch = 0U;
                {
                    std::scoped_lock lock(evidence.mutex);
                    facts = evidence.facts;
                    epoch = evidence.epoch;
                }
                std::ostringstream context;
                context << failure.what() << "\nrollback stage=" << stage << " empty_candidate=" << empty_candidate
                        << " observed_epoch=" << observed << " current_epoch=" << epoch << " ready=" << snapshot.ready
                        << " busy=" << snapshot.busy << " failure=" << snapshot.failure << " revision=" << snapshot.revision
                        << " frame_revision=" << snapshot.frame.revision << " gallery_generation=" << snapshot.gallery.generation
                        << " first_row=" << snapshot.viewport.first_row << " rows=" << snapshot.viewport.row_count
                        << " columns=" << snapshot.viewport.columns << " matching=" << snapshot.order.matching_count << "\nslots=";
                for (const bool ready_slot : snapshot.gallery.slots)
                    context << (ready_slot ? '1' : '0');
                context << "\nheld="
                        << std::ranges::count(facts, VisualDiagnosticOperation::AcceptanceCompletionHeld, &VisualDiagnosticFact::operation)
                        << " gpu_completed="
                        << std::ranges::count(facts, VisualDiagnosticOperation::GalleryGpuCompleted, &VisualDiagnosticFact::operation)
                        << " facts=" << facts.size();
                const auto first = facts.size() > 128U ? facts.size() - 128U : 0U;
                for (auto index = first; index < facts.size(); ++index) {
                    const auto& fact = facts[index];
                    context << "\n"
                            << index << " " << visual_diagnostic_event_name(fact.operation) << " generation=" << fact.generation
                            << " value=" << fact.value << " detail=" << fact.detail << " condition=" << fact.context.condition;
                }
                throw std::runtime_error(context.str());
            }
        }
    };
    const ExploreViewport viewport{.extent = {32U, 32U}, .first_row = 17U, .row_count = 4U, .columns = 4U};
    static_cast<void>(system.Open({.viewport = viewport, .compiled_source = path.string()}));
    wait("incumbent held input", [&] {
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
        return copy_atlas_plane(system.BorrowFrame(), plane_index, system.snapshot().gallery.layout);
    };
    const auto prior_clean = pixels(0U);
    const auto prior_semantic = pixels(1U);
    gate->FailNextPublicationAt(ExploreAcceptanceGate::PublicationStage::ProductPrepared);
    if (empty_candidate)
        static_cast<void>(system.UpdateFilter({.filter = {.minimum_instances = 2U}, .overlay = incumbent.overlay}));
    else
        system.UpdateViewport({.viewport = {.extent = {32U, 8U}, .first_row = 17U, .row_count = 1U, .columns = 4U}});
    wait("failed candidate rollback and held input continuation", [&] {
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
    const auto cached_tiles = explore_detail::GalleryThumbnailCache::WindowCount(incumbent.order.matching_count, viewport);
    wait("restored cache completion", [&] { return evidence.Count(VisualDiagnosticOperation::GalleryGpuCompleted) == cached_tiles; });
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
    wait("changed semantic overlay", [&] { return system.snapshot().frame.revision > first_revision; });
    CHECK(pixels(0U) == clean);
    // Semantic updates continue using the restored demand's cached tiles after
    // the smaller or empty candidate rolls back.
    const auto semantic_revision = system.snapshot().frame.revision;
    static_cast<void>(system.UpdateOverlay(restored.overlay));
    wait("restored semantic overlay", [&] { return system.snapshot().frame.revision > semantic_revision; });
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

TEST_CASE("Native superseded background work preserves incumbent planes and meaning", "[explore][native][cache][shutdown]") {
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
    // Initial demand retains 0..23. Row four is hot; new speculative rows
    // follow it without discarding the ready retained origin.
    gallery.plan.viewport.first_row = 4U;
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

TEST_CASE("Native cache admission retains only completely submitted obsolete pixels", "[explore][native][cache]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    using Stage = ExploreAcceptanceGate::SubmissionStage;
    const auto checkpoint = GENERATE(Stage::BeforeCacheAdmission, Stage::CacheCleanSubmitted, Stage::Background);
    CAPTURE(checkpoint);
    const auto check_labels = [](const auto& actual, const auto& expected) {
        REQUIRE(actual.size() == expected.size());
        for (std::size_t index = 0U; index != actual.size(); ++index) {
            CHECK(actual[index].box == expected[index].box);
            CHECK(actual[index].category == expected[index].category);
            CHECK(actual[index].compiled_index == expected[index].compiled_index);
        }
    };
    mmltk::testsupport::ScopedTempDir directory{"native-gallery-admission"};
    const auto path = directory.path() / "compiled.bin";
    // One visible image and one speculative image make the affected cache
    // identity independent of asynchronous read completion order.
    write_gallery_artifact(path, 0.25F, 9U, 2U);
    NativeGallery reference{1U};
    reference.plan.viewport = {.extent = {8U, 8U}, .row_count = 1U, .columns = 1U};
    reference.Open(path);
    reference.Drain();
    reference.plan.viewport.first_row = 1U;
    ++reference.plan.generation;
    reference.demand->store(reference.plan.generation);
    const auto expected_ready = reference.Begin().ready_slots;
    const auto expected_clean = reference.Pixels(0U);
    const auto expected_semantic = reference.Pixels(1U);
    const auto expected_labels = reference.algorithm->Labels();
    REQUIRE(expected_ready == std::vector<bool>{true});
    REQUIRE_FALSE(expected_labels.empty());
    REQUIRE(std::ranges::any_of(expected_semantic, [](const auto byte) { return byte != 0U; }));

    GalleryGpuPause pause;
    NativeGallery gallery{1U, false, true};
    gallery.plan.viewport = reference.plan.viewport;
    gallery.plan.viewport.first_row = 0U;
    gallery.Open(path);
    gallery.WaitForReadyTiles();
    static_cast<void>(gallery.Step());
    const auto incumbent_clean = gallery.Pixels(0U);
    const auto incumbent_semantic = gallery.Pixels(1U);
    const auto incumbent_labels = gallery.algorithm->Labels();
    const auto before = gallery.evidence.BackgroundSubmissions();
    gallery.supersede_at = checkpoint;
    if (checkpoint == Stage::Background) gallery.pause = &pause;
    auto submitted = std::async(std::launch::async, [&] {
        gallery.runtime->BindContext();
        while (!gallery.superseded.load()) {
            const auto epoch = gallery.evidence.Epoch();
            static_cast<void>(gallery.algorithm->AdvanceGallery());
            if (!gallery.superseded.load()) gallery.evidence.Wait(epoch);
        }
    });
    if (checkpoint == Stage::Background) {
        const bool entered = pause.Wait();
        pause.Release();
        REQUIRE(entered);
    }
    submitted.get();
    gallery.pause = nullptr;
    gallery.supersede_at.reset();
    gallery.runtime->BindContext();
    const bool admitted = checkpoint != Stage::BeforeCacheAdmission;
    CHECK(gallery.evidence.BackgroundSubmissions() == before + (admitted ? 1U : 0U));
    CHECK(gallery.Pixels(0U) == incumbent_clean);
    CHECK(gallery.Pixels(1U) == incumbent_semantic);
    check_labels(gallery.algorithm->Labels(), incumbent_labels);
    const auto reads = gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted);
    const auto renders = gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted);
    gallery.plan.viewport.first_row = 1U;
    ++gallery.plan.generation;
    gallery.demand->store(gallery.plan.generation);
    const auto revisited = gallery.Begin();
    CHECK(revisited.ready_slots == std::vector<bool>{admitted});
    CHECK(revisited.reused_tiles == (admitted ? 1U : 0U));
    if (!admitted) {
        CHECK(revisited.remaining_tiles == 1U);
        CHECK(gallery.algorithm->Labels().empty());
    }
    gallery.Drain();
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::GalleryReadStarted) == reads);
    CHECK(gallery.evidence.Count(VisualDiagnosticOperation::ExploreRenderSubmitted) == renders + (admitted ? 0U : 1U));
    CHECK(gallery.algorithm->AdvanceGallery().ready_slots == expected_ready);
    CHECK(gallery.Pixels(0U) == expected_clean);
    CHECK(gallery.Pixels(1U) == expected_semantic);
    check_labels(gallery.algorithm->Labels(), expected_labels);
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

class AcceptanceGateFixture final {
   public:
    explicit AcceptanceGateFixture(const int transport) : AcceptanceGateFixture(OpenSockets(transport)) {}
    [[nodiscard]] ExploreAcceptanceGate& gate() noexcept { return gate_; }
    [[nodiscard]] mmltk::common::io::ScopedFd& commands() noexcept { return commands_; }

    template <class T>
    T Await(std::future<T>& result) {
        const auto status = result.wait_for(std::chrono::seconds{2});
        if (status != std::future_status::ready) gate_.Stop();
        REQUIRE(status == std::future_status::ready);
        return result.get();
    }

   private:
    using Sockets = std::array<mmltk::common::io::ScopedFd, 2U>;
    [[nodiscard]] static Sockets OpenSockets(const int transport) {
        std::array<int, 2U> sockets{};
        REQUIRE(::socketpair(AF_UNIX, transport | SOCK_CLOEXEC, 0, sockets.data()) == 0);
        return {mmltk::common::io::ScopedFd{sockets[0]}, mmltk::common::io::ScopedFd{sockets[1]}};
    }
    explicit AcceptanceGateFixture(Sockets sockets) : commands_(std::move(sockets[0])), gate_(sockets[1].release()) {}

    mmltk::common::io::ScopedFd commands_;
    ExploreAcceptanceGate gate_;
};

TEST_CASE("acceptance gate retains one reader across settled frontend workflows", "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    std::promise<contracts::IntegrationControlReceipt> advanced;
    auto delivered = advanced.get_future();
    AcceptanceGateFixture fixture{transport};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    gate.SetFrontendCommand([&advanced](const auto receipt) {
        advanced.set_value(receipt);
        return true;
    });
    const std::uint8_t release_held = 4U;
    REQUIRE(::send(commands.get(), &release_held, sizeof(release_held), MSG_NOSIGNAL) == sizeof(release_held));
    REQUIRE(gate.ClaimHeldCompletion());
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(0U, 4U, 8U, 64U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 2U}));
    for (const auto kind : {Kind::Progress, Kind::Settled, Kind::PressureEntered})
        CHECK_FALSE(gate.ObserveFrontend({.kind = kind, .sequence = 1U, .progress = 1U, .failureline = 123U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Advance, .sequence = 1U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    const std::uint8_t advance = 8U;
    REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    CHECK((fixture.Await(delivered) == contracts::IntegrationControlReceipt{.kind = Kind::Advance, .sequence = 2U}));
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
    AcceptanceGateFixture fixture{transport};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    for (const auto command : std::array<std::uint8_t, 3U>{1U, 2U, 4U})
        REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    gate.AdvanceGeneration(7U);
    auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
    CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
    REQUIRE(gate.ClaimHeldCompletion());
    gate.AdvanceGeneration(7U);
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 4U, 8U, 64U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    gate.AdvanceGeneration(8U);
    CHECK(gate.AwaitInitialRelease(7U) == ExploreAcceptanceGate::WaitResult::Stale);
    gate.Stop();
    CHECK(gate.AwaitInitialRelease(8U) == ExploreAcceptanceGate::WaitResult::Stale);
}

TEST_CASE("acceptance gate rejects premature advancement and unavailable callbacks", "[explore][acceptance][control]") {
    const unsigned terminal_path = GENERATE(0U, 1U, 2U, 3U, 4U);
    std::promise<void> invoked;
    auto invocation = invoked.get_future();
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    gate.SetFrontendCommand([terminal_path, &invoked](auto) {
        invoked.set_value();
        return terminal_path == 4U;
    });
    if (terminal_path == 1U || terminal_path == 4U)
        REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Settled, .sequence = 1U}));
    if (terminal_path == 2U) {
        CHECK_FALSE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Failed, .sequence = 1U}));
        REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Failed, .sequence = 1U, .failureline = 123U}));
        ExploreAcceptanceGate::ControlObservation observation{};
        REQUIRE(::recv(commands.get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation.event == ExploreAcceptanceGate::ControlEvent::Frontend);
        CHECK(observation.generation == 1U);
        CHECK(observation.compiled_index == 0U);
        CHECK(observation.staging_bytes == 123U);
    }
    gate.AdvanceGeneration(1U);
    const std::uint8_t advance = 8U;
    if (terminal_path < 2U || terminal_path == 4U)
        REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 1U || terminal_path == 4U) fixture.Await(invocation);
    if (terminal_path == 4U) REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 3U) commands.reset();
    auto waiting = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(1U); });
    CHECK(fixture.Await(waiting) == ExploreAcceptanceGate::WaitResult::Stale);
    CHECK(gate.ClaimTerminalReport());
}

TEST_CASE("acceptance redraw is one shot and preserves the held read boundary", "[explore][acceptance][control]") {
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    // Successful redraw, callback refusal, callback exception, unavailable
    // callback, duplicate command, and redraw after all/one read was released.
    const unsigned path = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    std::promise<void> invoked;
    auto invocation = invoked.get_future();
    AcceptanceGateFixture fixture{transport};
    auto& gate = fixture.gate();
    const auto send = [&](const std::uint8_t command) {
        REQUIRE(::send(fixture.commands().get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    };
    gate.AdvanceGeneration(7U);
    if (path != 3U) {
        gate.SetRedrawCommand([&] {
            // This takes the gate mutex: callbacks must execute outside it.
            gate.AdvanceGeneration(7U);
            invoked.set_value();
            if (path == 2U) throw std::runtime_error("acceptance redraw failure");
            return path != 1U;
        });
    }
    if (path == 5U) send(2U);
    if (path == 6U) {
        send(1U);
        auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
        CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
    }
    send(16U);
    if (path < 3U || path == 4U) fixture.Await(invocation);
    if (path == 0U) {
        auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
        mmltk::testsupport::ScopedTestCleanup cleanup{[&] { gate.Stop(); }};
        // The real initial-wait receipt proves redraw left reads held.
        pollfd waiting{.fd = fixture.commands().get(), .events = POLLIN, .revents = 0};
        int ready = -1;
        do {
            ready = ::poll(&waiting, 1U, 2000);
        } while (ready < 0 && errno == EINTR);
        if (ready != 1) gate.Stop();
        REQUIRE(ready == 1);
        std::uint8_t observation = 0U;
        REQUIRE(::recv(fixture.commands().get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation == static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlEvent::InitialWait));
        CHECK(initial.wait_for(std::chrono::seconds{0}) != std::future_status::ready);
        send(1U);
        CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
        gate.StopAndJoin();
    } else {
        if (path == 4U) send(16U);
        // This wait is independent of release-all, including path 5. Only
        // rejection or shutdown can settle it without command 4.
        auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 0U, 0U, 64U); });
        CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Stale);
        gate.StopAndJoin();
    }
    CHECK(gate.ClaimTerminalReport());
    CHECK(gate.AwaitInitialRelease(7U) == ExploreAcceptanceGate::WaitResult::Stale);
}

}  // namespace
}  // namespace mmltk::controller

namespace mmltk::controller {
namespace {

TEST_CASE("completed gallery read receipts retain identity through reentrant supersession and terminal callbacks",
          "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    const auto outcome = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& gate = fixture.gate();
    std::optional<contracts::IntegrationControlReceipt> received;
    gate.SetFrontendCommand([&](const auto receipt) {
        CHECK(gate.FrontendSequence() == 1U);
        REQUIRE_FALSE(received.has_value());
        received = receipt;
        if (outcome == 0U) gate.AdvanceGeneration(8U);
        if (outcome == 4U || outcome == 5U) {
            gate.AdvanceGeneration(outcome == 4U ? 8U : 0U);
            gate.AdvanceGeneration(7U);
        }
        if (outcome == 2U) throw std::runtime_error("fixture callback failed");
        if (outcome == 3U) gate.Stop();
        return outcome != 1U;
    });
    gate.AdvanceGeneration(7U);
    REQUIRE(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ClaimHeldCompletion());
    if (outcome == 6U || outcome == 7U) {
        gate.AdvanceGeneration(outcome == 6U ? 8U : 0U);
        gate.AdvanceGeneration(7U);
    }
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 3U, 47U, 1024U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Stale);
    REQUIRE(received.has_value());
    CHECK((*received == contracts::IntegrationControlReceipt{
                            .kind = Kind::GalleryReadCompletionHeld, .sequence = 1U, .read_generation = 7U, .compiled_index = 47U}));
    for (const auto expected : {ExploreAcceptanceGate::ControlEvent::HeldWait, ExploreAcceptanceGate::ControlEvent::HeldStale}) {
        ExploreAcceptanceGate::ControlObservation observation{};
        REQUIRE(::recv(fixture.commands().get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation.event == expected);
        CHECK(observation.generation == 7U);
        CHECK(observation.slot == 3U);
        CHECK(observation.compiled_index == 47U);
        CHECK(observation.staging_bytes == 1024U);
    }
    CHECK(gate.ClaimTerminalReport() == (outcome >= 1U && outcome <= 3U));
    gate.StopAndJoin();
}

TEST_CASE("visible read gate holds the exact image across demand changes and rejects duplicate receipts",
          "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& gate = fixture.gate();
    std::promise<void> armed, observed;
    auto arm = armed.get_future(), held = observed.get_future();
    gate.SetFrontendCommand([&](const auto receipt) {
        // This reentrant owner access proves callbacks run outside the gate mutex.
        CHECK(gate.FrontendSequence() == 1U);
        if (receipt.kind == Kind::VisibleReadArmed) armed.set_value();
        if (receipt.kind == Kind::VisibleReadHeld) {
            CHECK(receipt.read_generation == 7U);
            CHECK(receipt.compiled_index == 0U);
            observed.set_value();
        }
        return true;
    });
    gate.AdvanceGeneration(7U);
    REQUIRE(gate.ObserveFrontend({.kind = Kind::VisibleReadArmRequested, .sequence = 1U, .compiled_index = 0U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadArmRequested, .sequence = 1U}));
    const auto send = [&](const ExploreAcceptanceGate::ControlCommand command) {
        const auto value = static_cast<std::uint8_t>(command);
        REQUIRE(::send(fixture.commands().get(), &value, sizeof(value), MSG_NOSIGNAL) == sizeof(value));
    };
    send(ExploreAcceptanceGate::ControlCommand::ArmVisibleRead);
    fixture.Await(arm);
    gate.AwaitVisibleRead(7U, 1U);  // Another compiled descriptor is not held.
    auto reading = std::async(std::launch::async, [&] { gate.AwaitVisibleRead(7U, 0U); });
    fixture.Await(held);
    gate.AdvanceGeneration(8U);
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 8U}));
    REQUIRE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 7U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 7U}));
    send(ExploreAcceptanceGate::ControlCommand::ReleaseVisibleRead);
    fixture.Await(reading);
    CHECK(gate.ObserveFrontend({.kind = Kind::CapacityArmRequested, .sequence = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::CapacityArmRequested, .sequence = 1U}));
    gate.StopAndJoin();
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
}

TEST_CASE("native completion gate preserves the capacity-before-consumption wake", "[presentation][acceptance][control]") {
    PresentationAcceptanceGate gate;
    std::vector<PresentationAcceptanceGate::Receipt> receipts;
    unsigned wakes = 0U;
    gate.SetWake([&] {
        ++wakes;
        gate.SetWake({});
    });
    gate.SetObserver([&](const auto receipt) {
        receipts.push_back(receipt);
        gate.SetObserver({});
    });
    CHECK_FALSE(gate.Release());
    REQUIRE(gate.Arm());
    CHECK_FALSE(gate.Arm());
    const PresentationAcceptanceGate::Receipt held{3U, 4U, 5U, 6U};
    REQUIRE(gate.Hold(held));
    REQUIRE(receipts.size() == 1U);
    CHECK_FALSE(receipts.front().capacity_available);
    gate.SetObserver([&](const auto receipt) { receipts.push_back(receipt); });
    gate.ObserveCapacity();
    gate.ObserveCapacity();
    REQUIRE(receipts.size() == 2U);
    CHECK(receipts.back().capacity_available);
    CHECK(receipts.back().publication == held.publication);
    REQUIRE(gate.Release());
    CHECK(wakes == 1U);  // Explicit wake survives an already drained completion edge.
    CHECK_FALSE(gate.Hold(held));
    CHECK_FALSE(gate.Release());
    gate.Stop();
    CHECK_FALSE(gate.Arm());
}

}  // namespace
}  // namespace mmltk::controller
