module;
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/image_resize.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"

module mmltk.backend.imaging.explore.compiled_explore_store;

namespace mmltk::backend::imaging::explore {

namespace {

[[nodiscard]] bool cancellation_requested(const std::atomic<bool>* cancel_requested) noexcept {
    return cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed);
}

[[nodiscard]] bool generation_is_current(const std::uint64_t expected_generation,
                                         const std::atomic<std::uint64_t>* current_generation) noexcept {
    return current_generation == nullptr || current_generation->load(std::memory_order_acquire) == expected_generation;
}

void require_not_cancelled(const std::atomic<bool>* cancel_requested) {
    if (cancellation_requested(cancel_requested)) { throw std::runtime_error("dataset explore cancelled"); }
}

[[nodiscard]] bool class_masks_intersect(const ExploreClassMask& left, const ExploreClassMask& right) noexcept {
    std::uint64_t intersection = 0U;
    for (std::size_t word = 0U; word < left.size(); ++word) {
        intersection |= left[word] & right[word];
    }
    return intersection != 0U;
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] bool deterministic_shuffle(std::vector<std::uint32_t>& values, const std::uint64_t seed,
                                         const std::atomic<bool>* cancel_requested, const std::uint64_t expected_generation,
                                         const std::atomic<std::uint64_t>* current_generation) noexcept {
    std::uint64_t state = seed;
    for (std::size_t remaining = values.size(); remaining > 1U; --remaining) {
        if ((remaining & 4095U) == 0U &&
            (cancellation_requested(cancel_requested) || !generation_is_current(expected_generation, current_generation))) {
            return false;
        }
        const std::size_t selected = static_cast<std::size_t>(splitmix64(state) % remaining);
        std::swap(values[remaining - 1U], values[selected]);
    }
    return !cancellation_requested(cancel_requested) && generation_is_current(expected_generation, current_generation);
}

}  // namespace

std::vector<ExploreImageSummary> build_explore_summaries(const mmltk::backend::data::CompiledDataset& store,
                                                         const std::atomic<bool>* cancel_requested,
                                                         mmltk::common::concurrency::WorkerPool* workers) {
    const auto image_entries_ = store.image_entries();
    const auto labels_ = store.labels();
    std::vector<ExploreImageSummary> summaries(image_entries_.size());
    const auto summarize = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t image_index = begin; image_index < end; ++image_index) {
            if ((image_index & 4095U) == 0U && cancellation_requested(cancel_requested)) return;
            const mmltk::backend::data::ImageEntry& entry = image_entries_[image_index];
            ExploreImageSummary& summary = summaries[image_index];
            summary.original_width = entry.original_width;
            summary.original_height = entry.original_height;
            summary.instance_count = entry.num_instances;
            const std::size_t label_begin = entry.label_offset / sizeof(mmltk::backend::data::PackedInstance);
            const mmltk::backend::data::PackedInstance* image_labels = labels_.data() + label_begin;
            for (std::size_t instance_index = 0U; instance_index < entry.num_instances; ++instance_index) {
                const mmltk::backend::data::PackedInstance& instance = image_labels[instance_index];
                summary.classes[instance.class_id >> 6U] |= std::uint64_t{1} << (instance.class_id & 63U);
                summary.has_masks |= instance.mask_rle_pairs != 0U;
            }
        }
    };
    if (workers != nullptr && image_entries_.size() > 4096U)
        workers->parallel_for<std::size_t>(0U, image_entries_.size(), -1, summarize);
    else
        summarize(0U, image_entries_.size());
    require_not_cancelled(cancel_requested);
    return summaries;
}

ExploreClassMask explore_class_mask(const std::span<const bool> enabled) noexcept {
    ExploreClassMask mask{};
    const std::size_t count = std::min<std::size_t>(enabled.size(), mmltk::backend::data::MAX_CLASSES);
    for (std::size_t class_id = 0U; class_id < count; ++class_id) {
        if (enabled[class_id]) { mask[class_id >> 6U] |= std::uint64_t{1} << (class_id & 63U); }
    }
    return mask;
}

bool rebuild_explore_order(const std::span<const ExploreImageSummary> summaries, const ExploreSampleFilter& filter, const bool shuffled,
                           const std::uint64_t shuffle_seed, std::vector<std::uint32_t>& current, std::vector<std::uint32_t>& scratch,
                           const std::atomic<bool>* cancel_requested, const std::uint64_t expected_generation,
                           const std::atomic<std::uint64_t>* current_generation, mmltk::common::concurrency::WorkerPool* workers) {
    if (summaries.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("compiled dataset image count exceeds Explore index capacity");
    }
    constexpr std::uint32_t kRejected = std::numeric_limits<std::uint32_t>::max();
    scratch.assign(summaries.size(), kRejected);
    const auto classify = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t image_index = begin; image_index < end; ++image_index) {
            if ((image_index & 4095U) == 0U &&
                (cancellation_requested(cancel_requested) || !generation_is_current(expected_generation, current_generation)))
                return;
            const ExploreImageSummary& summary = summaries[image_index];
            const std::uint64_t compiled_index = image_index;
            if (compiled_index < filter.min_compiled_index || compiled_index > filter.max_compiled_index ||
                summary.instance_count < filter.min_instances || summary.instance_count > filter.max_instances ||
                (filter.require_boxes && summary.instance_count == 0U) || (filter.require_masks && !summary.has_masks) ||
                (filter.restrict_classes && !class_masks_intersect(summary.classes, filter.classes)))
                continue;
            scratch[image_index] = static_cast<std::uint32_t>(image_index);
        }
    };
    if (workers != nullptr && summaries.size() > 4096U)
        workers->parallel_for<std::size_t>(0U, summaries.size(), -1, classify);
    else
        classify(0U, summaries.size());
    if (cancellation_requested(cancel_requested) || !generation_is_current(expected_generation, current_generation)) { return false; }
    const auto selected_end = std::remove(scratch.begin(), scratch.end(), kRejected);
    scratch.erase(selected_end, scratch.end());
    if (shuffled && !deterministic_shuffle(scratch, shuffle_seed, cancel_requested, expected_generation, current_generation)) {
        return false;
    }
    if (cancellation_requested(cancel_requested) || !generation_is_current(expected_generation, current_generation)) { return false; }
    current.swap(scratch);
    return true;
}

std::optional<std::uint32_t> adjacent_explore_index(const std::span<const std::uint32_t> order, const std::uint32_t selected,
                                                    const bool next) noexcept {
    if (order.empty()) { return std::nullopt; }
    const auto position = std::find(order.begin(), order.end(), selected);
    if (position == order.end()) { return std::nullopt; }
    if (next) {
        const auto following = position + 1;
        return following == order.end() ? order.front() : *following;
    }
    return position == order.begin() ? order.back() : *(position - 1);
}

std::optional<ExploreAtlasLayout> make_explore_atlas_layout(const std::size_t item_count, const ExploreViewport viewport,
                                                            const std::uint32_t card_extent) noexcept {
    if (item_count == 0U || !viewport.valid() || card_extent == 0U) return std::nullopt;
    const std::size_t columns = viewport.columns;
    const std::size_t rows = item_count / columns + (item_count % columns != 0U ? 1U : 0U);
    if (rows > std::numeric_limits<std::uint32_t>::max() || viewport.columns > std::numeric_limits<std::uint32_t>::max() / card_extent ||
        rows > std::numeric_limits<std::uint32_t>::max() / card_extent)
        return std::nullopt;
    return ExploreAtlasLayout{.width = viewport.columns * card_extent,
                              .height = static_cast<std::uint32_t>(rows) * card_extent,
                              .columns = viewport.columns,
                              .rows = static_cast<std::uint32_t>(rows),
                              .card_extent = card_extent};
}

std::size_t prioritize_explore_work(const std::span<const std::uint32_t> order, const ExploreViewport viewport,
                                    const std::optional<std::uint32_t> focused_index, const std::uint32_t background_cursor,
                                    const std::span<std::uint32_t> output) noexcept {
    if (order.empty() || output.empty() || !viewport.valid()) return 0U;
    const std::span<std::uint32_t> bounded_output = output.first(std::min(output.size(), kExploreWorkCapacity));
    std::size_t written = 0U;
    const auto append = [&](const std::uint32_t value) {
        if (written == bounded_output.size()) return;
        bounded_output[written++] = value;
    };
    if (focused_index.has_value()) append(*focused_index);
    const auto bounded_product = [limit = order.size()](const std::size_t lhs, const std::size_t rhs) noexcept {
        return rhs != 0U && lhs > limit / rhs ? limit : std::min(limit, lhs * rhs);
    };
    const std::size_t first = bounded_product(viewport.first_row, viewport.columns);
    const std::size_t visible_count = bounded_product(viewport.row_count, viewport.columns);
    const std::size_t last = first + std::min(visible_count, order.size() - first);
    for (std::size_t index = first; index != last && written != bounded_output.size(); ++index) {
        if (focused_index != order[index]) append(order[index]);
    }
    for (std::size_t offset = 0U; offset != order.size() && written != bounded_output.size(); ++offset) {
        const std::size_t index = (static_cast<std::size_t>(background_cursor) + offset) % order.size();
        if ((index < first || index >= last) && focused_index != order[index]) { append(order[index]); }
    }
    return written;
}

}  // namespace mmltk::backend::imaging::explore
