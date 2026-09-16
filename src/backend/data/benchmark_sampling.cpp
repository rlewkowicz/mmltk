#include "detail/benchmark_sampling.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::math::checked_cast;
namespace {
constexpr std::size_t kClassCount = 80U;
constexpr std::uint64_t kShardClassCoverageFloor = 512U;
constexpr std::uint64_t kShardCandidateHeadroomNumerator = 11U;
constexpr std::uint64_t kShardCandidateHeadroomDenominator = 10U;
struct ClassMembership {
    std::uint64_t low = 0U;
    std::uint64_t high = 0U;
};
struct HashedImage {
    std::uint64_t hash = 0U;
    std::uint64_t source_image_id = 0U;
    std::uint32_t image_index = 0U;
    // Deterministic selection order is the declaration order of these fields; the type owns it so no
    // call site spells the comparison out again.
    auto operator<=>(const HashedImage&) const = default;
};
struct SourceSelection {
    const NormalizedAnnotationIndex* source = nullptr;
    std::vector<ClassMembership> memberships;
    std::vector<HashedImage> hash_order;
    std::array<std::vector<std::uint32_t>, kClassCount> candidates;
    std::array<std::size_t, kClassCount> cursors{};
    std::vector<std::uint8_t> selected;
    SupplementalSamplingStats stats;
    std::size_t selected_boxes = 0U;
};
struct ShardSummary {
    std::uint64_t images = 0U;
    std::array<std::uint64_t, kClassCount> class_images{};
};
[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}
[[nodiscard]] std::uint64_t sampling_hash(const BenchmarkDatasetSource source, const std::uint64_t image_id) noexcept {
    constexpr std::uint64_t kRevisionSeed = 0xC080BA1A6CED0002ULL;
    const std::uint64_t source_seed = static_cast<std::uint64_t>(source) * 0xD6E8FEB86659FD93ULL;
    return mix64(image_id ^ source_seed ^ kRevisionSeed);
}
void throw_if_cancelled(mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (cancel_requested.requested()) { throw std::runtime_error("benchmark dataset compilation cancelled"); }
}
void add_class(ClassMembership* membership, const std::uint8_t class_id) {
    if (class_id < 64U) {
        membership->low |= std::uint64_t{1} << class_id;
    } else {
        membership->high |= std::uint64_t{1} << (class_id - 64U);
    }
}
class ClassMembershipCursor {
   public:
    explicit ClassMembershipCursor(const ClassMembership membership) noexcept : low_(membership.low), high_(membership.high) {}
    [[nodiscard]] std::optional<std::uint8_t> next() noexcept {
        if (low_ != 0U) {
            const auto class_id = static_cast<std::uint8_t>(std::countr_zero(low_));
            low_ &= low_ - 1U;
            return class_id;
        }
        if (high_ != 0U) {
            const auto class_id = static_cast<std::uint8_t>(64U + std::countr_zero(high_));
            high_ &= high_ - 1U;
            return class_id;
        }
        return std::nullopt;
    }

   private:
    std::uint64_t low_ = 0U;
    std::uint64_t high_ = 0U;
};
[[nodiscard]] std::vector<ClassMembership> build_memberships(const NormalizedAnnotationIndex& source, SupplementalSamplingStats* stats,
                                                             mmltk::common::concurrency::CancellationObservation cancel_requested) {
    stats->full_images = source.images.size();
    stats->full_boxes = source.boxes.size();
    std::vector<ClassMembership> memberships(source.images.size());
    for (std::size_t image_index = 0U; image_index < source.images.size(); ++image_index) {
        if ((image_index & 4095U) == 0U) { throw_if_cancelled(cancel_requested); }
        const NormalizedImage& image = source.images[image_index];
        if (image.box_count == 0U || image.first_box > source.boxes.size() || image.box_count > source.boxes.size() - image.first_box) {
            throw std::runtime_error("supplemental sampler received an invalid image box span");
        }
        ClassMembership membership;
        for (std::uint64_t box_index = image.first_box; box_index < image.first_box + image.box_count; ++box_index) {
            const NormalizedBox& box = source.boxes[checked_cast<std::size_t>(box_index, "supplemental box index overflow")];
            if (box.class_id >= kClassCount) { throw std::runtime_error("supplemental sampler received an invalid class id"); }
            add_class(&membership, box.class_id);
        }
        if (membership.low == 0U && membership.high == 0U) { throw std::runtime_error("supplemental sampler received an image without mapped classes"); }
        memberships[image_index] = membership;
        ClassMembershipCursor classes(membership);
        while (const auto class_id = classes.next()) { ++stats->available_class_images[*class_id]; }
    }
    return memberships;
}
[[nodiscard]] std::array<std::uint64_t, kClassCount> coco_class_counts(const NormalizedAnnotationIndex& coco_train,
                                                                       mmltk::common::concurrency::CancellationObservation cancel_requested) {
    SupplementalSamplingStats ignored;
    const std::vector<ClassMembership> memberships = build_memberships(coco_train, &ignored, cancel_requested);
    return ignored.available_class_images;
}
[[nodiscard]] std::vector<std::uint16_t> choose_objects365_shards(const NormalizedAnnotationIndex& objects365,
                                                                  const std::span<const ClassMembership> memberships,
                                                                  const std::span<const std::uint64_t> shard_bytes, const std::uint64_t required_images,
                                                                  std::uint64_t* selected_archive_bytes,
                                                                  mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (shard_bytes.empty()) { throw std::runtime_error("Objects365 sampler requires archive byte identities"); }
    std::vector<ShardSummary> summaries(shard_bytes.size());
    std::array<std::uint64_t, kClassCount> full_class_images{};
    for (std::size_t image_index = 0U; image_index < objects365.images.size(); ++image_index) {
        if ((image_index & 4095U) == 0U) { throw_if_cancelled(cancel_requested); }
        const std::uint16_t shard = objects365.images[image_index].source_shard;
        if (shard >= summaries.size()) { throw std::runtime_error("Objects365 annotation references an unknown image shard"); }
        ShardSummary& summary = summaries[shard];
        ++summary.images;
        ClassMembershipCursor classes(memberships[image_index]);
        while (const auto class_id = classes.next()) {
            ++summary.class_images[*class_id];
            ++full_class_images[*class_id];
        }
    }
    const std::uint64_t headroom_images = checked_cast<std::uint64_t>(
        (static_cast<unsigned long long>(required_images) * kShardCandidateHeadroomNumerator + kShardCandidateHeadroomDenominator - 1U) /
            kShardCandidateHeadroomDenominator,
        "Objects365 sampling headroom overflow");
    std::array<std::uint64_t, kClassCount> coverage_targets{};
    for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) {
        coverage_targets[class_id] = std::min(full_class_images[class_id], kShardClassCoverageFloor);
    }
    std::vector<std::uint8_t> selected(summaries.size(), std::uint8_t{0U});
    std::array<std::uint64_t, kClassCount> selected_coverage{};
    std::uint64_t selected_images = 0U;
    *selected_archive_bytes = 0U;
    std::vector<std::uint16_t> result;
    result.reserve(summaries.size());
    while (selected_images < headroom_images ||
           !std::ranges::equal(
               selected_coverage, coverage_targets, [](const std::uint64_t available, const std::uint64_t target) { return available >= target; })) {
        throw_if_cancelled(cancel_requested);
        std::size_t best = summaries.size();
        long double best_score = -1.0L;
        for (std::size_t shard = 0U; shard < summaries.size(); ++shard) {
            if (selected[shard] != 0U || summaries[shard].images == 0U) { continue; }
            std::uint64_t coverage_gain = 0U;
            for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) {
                const std::uint64_t deficit =
                    coverage_targets[class_id] > selected_coverage[class_id] ? coverage_targets[class_id] - selected_coverage[class_id] : 0U;
                coverage_gain += std::min(deficit, summaries[shard].class_images[class_id]);
            }
            const std::uint64_t useful_images = selected_images < headroom_images ? std::min(summaries[shard].images, headroom_images - selected_images) : 0U;
            const std::uint64_t gain = useful_images + coverage_gain * 8U;
            const std::uint64_t bytes = std::max<std::uint64_t>(1U, shard_bytes[shard]);
            const long double score = static_cast<long double>(gain) / static_cast<long double>(bytes);
            if (score > best_score ||
                (score == best_score && (best == summaries.size() || bytes < shard_bytes[best] || (bytes == shard_bytes[best] && shard < best)))) {
                best = shard;
                best_score = score;
            }
        }
        if (best == summaries.size()) { throw std::runtime_error("Objects365 shard selector cannot satisfy the sample budget"); }
        selected[best] = 1U;
        result.push_back(checked_cast<std::uint16_t>(best, "Objects365 shard index overflow"));
        selected_images += summaries[best].images;
        *selected_archive_bytes = checked_cast<std::uint64_t>(*selected_archive_bytes + shard_bytes[best], "Objects365 selected archive byte total overflow");
        for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) { selected_coverage[class_id] += summaries[best].class_images[class_id]; }
    }
    for (std::size_t position = result.size(); position > 0U; --position) {
        throw_if_cancelled(cancel_requested);
        const std::size_t shard = result[position - 1U];
        const ShardSummary& summary = summaries[shard];
        if (selected_images - summary.images < headroom_images) { continue; }
        bool preserves_coverage = true;
        for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) {
            if (selected_coverage[class_id] - summary.class_images[class_id] < coverage_targets[class_id]) {
                preserves_coverage = false;
                break;
            }
        }
        if (!preserves_coverage) { continue; }
        selected[shard] = 0U;
        selected_images -= summary.images;
        *selected_archive_bytes -= shard_bytes[shard];
        for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) { selected_coverage[class_id] -= summary.class_images[class_id]; }
        result.erase(result.begin() + checked_cast<std::ptrdiff_t>(position - 1U, "Objects365 shard prune position overflow"));
    }
    std::ranges::sort(result);
    return result;
}
[[nodiscard]] SourceSelection make_source_selection(const NormalizedAnnotationIndex& source, std::vector<ClassMembership> memberships,
                                                    const std::span<const std::uint8_t> eligible_shards) {
    SourceSelection result;
    result.source = &source;
    result.memberships = std::move(memberships);
    result.selected.assign(source.images.size(), std::uint8_t{0U});
    result.stats.full_images = source.images.size();
    result.stats.full_boxes = source.boxes.size();
    for (const ClassMembership membership : result.memberships) {
        ClassMembershipCursor classes(membership);
        while (const auto class_id = classes.next()) { ++result.stats.available_class_images[*class_id]; }
    }
    result.hash_order.reserve(source.images.size());
    for (std::size_t image_index = 0U; image_index < source.images.size(); ++image_index) {
        const std::uint16_t shard = source.images[image_index].source_shard;
        if (!eligible_shards.empty() && (shard >= eligible_shards.size() || eligible_shards[shard] == 0U)) { continue; }
        result.hash_order.push_back(HashedImage{
            sampling_hash(source.source, source.images[image_index].source_image_id),
            source.images[image_index].source_image_id,
            checked_cast<std::uint32_t>(image_index, "supplemental image index overflow"),
        });
    }
    std::ranges::sort(result.hash_order);
    for (const HashedImage& image : result.hash_order) {
        ClassMembershipCursor classes(result.memberships[image.image_index]);
        while (const auto class_id = classes.next()) { result.candidates[*class_id].push_back(image.image_index); }
    }
    return result;
}
[[nodiscard]] bool class_candidate_available(SourceSelection* source, const std::size_t class_id) {
    std::size_t& cursor = source->cursors[class_id];
    const std::vector<std::uint32_t>& candidates = source->candidates[class_id];
    while (cursor < candidates.size() && source->selected[candidates[cursor]] != 0U) { ++cursor; }
    return cursor < candidates.size();
}
void select_image(SourceSelection* source, const std::uint32_t image_index, std::array<std::uint64_t, kClassCount>* combined_counts) {
    if (source->selected[image_index] != 0U) { throw std::runtime_error("supplemental sampler selected an image twice"); }
    const NormalizedImage& image = source->source->images[image_index];
    source->selected[image_index] = 1U;
    ++source->stats.selected_images;
    source->selected_boxes += image.box_count;
    ClassMembershipCursor classes(source->memberships[image_index]);
    while (const auto class_id = classes.next()) {
        ++source->stats.selected_class_images[*class_id];
        ++(*combined_counts)[*class_id];
    }
}
[[nodiscard]] std::size_t rarest_available_class(SourceSelection* objects, SourceSelection* open_images,
                                                 const std::array<std::uint64_t, kClassCount>& combined_counts, const bool open_only, const bool open_allowed) {
    std::size_t chosen = kClassCount;
    std::uint64_t chosen_count = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) {
        const bool object_available = !open_only && class_candidate_available(objects, class_id);
        const bool open_available = open_allowed && class_candidate_available(open_images, class_id);
        if ((object_available || open_available) && combined_counts[class_id] < chosen_count) {
            chosen = class_id;
            chosen_count = combined_counts[class_id];
        }
    }
    return chosen;
}
[[nodiscard]] std::uint32_t take_class_candidate(SourceSelection* source, const std::size_t class_id) {
    std::size_t& cursor = source->cursors[class_id];
    if (!class_candidate_available(source, class_id)) { throw std::runtime_error("supplemental class candidate unexpectedly exhausted"); }
    return source->candidates[class_id][cursor++];
}
[[nodiscard]] NormalizedAnnotationIndex materialize_selection(const SourceSelection& source) {
    NormalizedAnnotationIndex result;
    result.source = source.source->source;
    result.split = source.source->split;
    result.annotation_sha256 = source.source->annotation_sha256;
    result.rejected = source.source->rejected;
    result.images.reserve(checked_cast<std::size_t>(source.stats.selected_images, "supplemental selected image count overflow"));
    result.boxes.reserve(source.selected_boxes);
    for (std::size_t image_index = 0U; image_index < source.source->images.size(); ++image_index) {
        if (source.selected[image_index] == 0U) { continue; }
        const NormalizedImage& input = source.source->images[image_index];
        NormalizedImage output = input;
        output.first_box = result.boxes.size();
        result.images.push_back(output);
        using BoxDifference = std::vector<NormalizedBox>::difference_type;
        const auto first = source.source->boxes.begin() + checked_cast<BoxDifference>(input.first_box, "supplemental box offset exceeds iterator range");
        const auto last = first + checked_cast<BoxDifference>(input.box_count, "supplemental box count exceeds iterator range");
        for (auto box = first; box != last; ++box) {
            if (box->mask_rle_offset > source.source->mask_rle_pairs.size() ||
                box->mask_rle_pairs > source.source->mask_rle_pairs.size() - box->mask_rle_offset) {
                throw std::runtime_error("supplemental mask range is invalid");
            }
            NormalizedBox output_box = *box;
            output_box.mask_rle_offset = result.mask_rle_pairs.size();
            const auto mask = std::span(source.source->mask_rle_pairs).subspan(static_cast<std::size_t>(box->mask_rle_offset), box->mask_rle_pairs);
            result.mask_rle_pairs.insert(result.mask_rle_pairs.end(), mask.begin(), mask.end());
            result.boxes.push_back(output_box);
        }
    }
    return result;
}
}  // namespace
CombinedSupplementalSamplingResult sample_combined_supplemental_indices(const NormalizedAnnotationIndex& coco_train,
                                                                        const NormalizedAnnotationIndex& objects365,
                                                                        const NormalizedAnnotationIndex& open_images,
                                                                        const std::span<const std::uint64_t> objects365_shard_bytes,
                                                                        mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (coco_train.source != BenchmarkDatasetSource::kCoco2017 || objects365.source != BenchmarkDatasetSource::kObjects365V2 ||
        open_images.source != BenchmarkDatasetSource::kOpenImagesV7) {
        throw std::runtime_error("combined supplemental sampler received the wrong sources");
    }
    if (objects365.images.size() < kSupplementalBudgetDenominator || open_images.images.size() < kSupplementalBudgetDenominator) {
        throw std::runtime_error("supplemental sources are too small for benchmark sampling");
    }
    CombinedSupplementalSamplingResult result;
    result.target_images = objects365.images.size() / kSupplementalBudgetDenominator + open_images.images.size() / kSupplementalBudgetDenominator;
    result.open_images_floor = std::max<std::uint64_t>(1U, result.target_images / kOpenImagesDiversityDenominator);
    result.open_images_ceiling = std::max(result.open_images_floor, result.target_images / kOpenImagesMaximumDenominator);
    const std::uint64_t required_objects = result.target_images - result.open_images_floor;
    SupplementalSamplingStats object_membership_stats;
    std::vector<ClassMembership> object_memberships = build_memberships(objects365, &object_membership_stats, cancel_requested);
    SupplementalSamplingStats open_membership_stats;
    std::vector<ClassMembership> open_memberships = build_memberships(open_images, &open_membership_stats, cancel_requested);
    result.objects365_shards =
        choose_objects365_shards(objects365, object_memberships, objects365_shard_bytes, required_objects, &result.objects365_archive_bytes, cancel_requested);
    std::vector<std::uint8_t> eligible_shards(objects365_shard_bytes.size(), std::uint8_t{0U});
    for (const std::uint16_t shard : result.objects365_shards) { eligible_shards[shard] = 1U; }
    SourceSelection object_selection = make_source_selection(objects365, std::move(object_memberships), eligible_shards);
    SourceSelection open_selection = make_source_selection(open_images, std::move(open_memberships), {});
    std::array<std::uint64_t, kClassCount> combined_counts = coco_class_counts(coco_train, cancel_requested);
    std::uint64_t selected_total = 0U;
    while (selected_total < result.target_images) {
        if ((selected_total & 4095U) == 0U) { throw_if_cancelled(cancel_requested); }
        const bool open_allowed = open_selection.stats.selected_images < result.open_images_ceiling;
        const bool force_open = open_selection.stats.selected_images < result.open_images_floor &&
                                open_selection.stats.selected_images * result.target_images <= selected_total * result.open_images_floor;
        std::size_t chosen_class = rarest_available_class(&object_selection, &open_selection, combined_counts, force_open, open_allowed);
        if (chosen_class == kClassCount && force_open) {
            chosen_class = rarest_available_class(&object_selection, &open_selection, combined_counts, false, open_allowed);
        }
        if (chosen_class == kClassCount) { break; }
        const bool object_available = !force_open && class_candidate_available(&object_selection, chosen_class);
        if (object_available) {
            select_image(&object_selection, take_class_candidate(&object_selection, chosen_class), &combined_counts);
        } else if (open_allowed && class_candidate_available(&open_selection, chosen_class)) {
            select_image(&open_selection, take_class_candidate(&open_selection, chosen_class), &combined_counts);
        } else {
            break;
        }
        ++selected_total;
    }
    const auto fill_from_hash_order = [&](SourceSelection* source, const std::uint64_t maximum, std::uint64_t* total) {
        for (const HashedImage& image : source->hash_order) {
            if (*total == result.target_images || source->stats.selected_images == maximum) { break; }
            if (source->selected[image.image_index] == 0U) {
                select_image(source, image.image_index, &combined_counts);
                ++*total;
            }
        }
    };
    fill_from_hash_order(&object_selection, result.target_images - result.open_images_floor, &selected_total);
    fill_from_hash_order(&open_selection, result.open_images_ceiling, &selected_total);
    fill_from_hash_order(&object_selection, result.target_images, &selected_total);
    if (selected_total != result.target_images || open_selection.stats.selected_images < result.open_images_floor ||
        open_selection.stats.selected_images > result.open_images_ceiling) {
        throw std::runtime_error("combined supplemental sampler could not fill its exact target");
    }
    object_selection.stats.selected_boxes = object_selection.selected_boxes;
    open_selection.stats.selected_boxes = open_selection.selected_boxes;
    result.objects365.stats = object_selection.stats;
    result.open_images.stats = open_selection.stats;
    result.objects365.index = materialize_selection(object_selection);
    result.open_images.index = materialize_selection(open_selection);
    return result;
}
}  // namespace mmltk::backend::data::benchmark_internal
