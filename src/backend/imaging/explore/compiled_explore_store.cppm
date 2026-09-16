module;
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/image_resize.h"
#include "src/common/io/file_memory.h"
#include "src/common/concurrency/worker_pool.h"
export module mmltk.backend.imaging.explore.compiled_explore_store;
export namespace mmltk::backend::imaging::explore {
inline constexpr std::size_t kExploreClassMaskWords = mmltk::backend::data::MAX_CLASSES / 64U;
inline constexpr std::size_t kExploreWorkCapacity = 256U;
using ExploreRlePair = mmltk::backend::data::RLEPair;
using ExploreClassMask = std::array<std::uint64_t, kExploreClassMaskWords>;
struct ExploreImageSummary {
    ExploreClassMask classes{};
    std::uint32_t original_width = 0U;
    std::uint32_t original_height = 0U;
    std::uint16_t instance_count = 0U;
    bool has_masks = false;
};
struct ExploreSampleFilter {
    ExploreClassMask classes{};
    std::uint32_t min_instances = 0U;
    std::uint32_t max_instances = 10'000U;
    std::uint64_t min_compiled_index = 0U;
    std::uint64_t max_compiled_index = std::numeric_limits<std::uint64_t>::max();
    bool require_boxes = false;
    bool require_masks = false;
    bool restrict_classes = false;
    [[nodiscard]] bool operator==(const ExploreSampleFilter&) const noexcept = default;
};
struct ExploreViewport final {
    std::uint32_t first_row = 0U;
    std::uint32_t row_count = 0U;
    std::uint32_t columns = 0U;
    [[nodiscard]] bool valid() const noexcept { return row_count != 0U && columns != 0U; }
};
struct ExploreAtlasLayout final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t columns = 0U;
    std::uint32_t rows = 0U;
    std::uint32_t card_extent = 0U;
    [[nodiscard]] bool valid() const noexcept { return width != 0U && height != 0U && columns != 0U && rows != 0U && card_extent != 0U; }
};
[[nodiscard]] std::vector<ExploreImageSummary> build_explore_summaries(const mmltk::backend::data::CompiledDataset& store,
                                                                       const std::atomic<bool>* cancel_requested = nullptr,
                                                                       mmltk::common::concurrency::WorkerPool* workers = nullptr);
[[nodiscard]] ExploreClassMask explore_class_mask(std::span<const bool> enabled) noexcept;
[[nodiscard]] bool rebuild_explore_order(std::span<const ExploreImageSummary> summaries, const ExploreSampleFilter& filter, bool shuffled,
                                         std::uint64_t shuffle_seed, std::vector<std::uint32_t>& current, std::vector<std::uint32_t>& scratch,
                                         const std::atomic<bool>* cancel_requested = nullptr, std::uint64_t expected_generation = 0U,
                                         const std::atomic<std::uint64_t>* current_generation = nullptr,
                                         mmltk::common::concurrency::WorkerPool* workers = nullptr);
[[nodiscard]] std::optional<std::uint32_t> adjacent_explore_index(std::span<const std::uint32_t> order, std::uint32_t selected, bool next) noexcept;
[[nodiscard]] std::optional<ExploreAtlasLayout> make_explore_atlas_layout(std::size_t item_count, ExploreViewport viewport, std::uint32_t card_extent) noexcept;
// Produces one fixed-capacity latest-viewport work set. The caller supplies a
// unique current order and either no focus or a focus identity from that
// order. Focus wins, then the visible row range, then stable background
// residency from the supplied cursor. Navigation history is never retained.
[[nodiscard]] std::size_t prioritize_explore_work(std::span<const std::uint32_t> order, ExploreViewport viewport, std::optional<std::uint32_t> focused_index,
                                                  std::uint32_t background_cursor, std::span<std::uint32_t> output) noexcept;
}  // namespace mmltk::backend::imaging::explore
