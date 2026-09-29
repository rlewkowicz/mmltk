#include "src/common/math/deterministic_sampling.h"
#include "src/backend/data/benchmark/detail/benchmark_sampling.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/pch_std.h"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
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
 std::size_t mask_runs = 0;
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
 std::size_t selected_boxes = 0U, selected_runs = 0U;
};
struct ShardSummary {
 std::uint64_t images = 0U;
 std::array<std::uint64_t, kClassCount> class_images{};
};
[[nodiscard]] std::uint64_t selection_workspace_bytes(
 const NormalizedAnnotationIndex& objects365, const NormalizedAnnotationIndex& open_images, const std::size_t shards, const std::uint64_t selected_images) {
 using mmltk::common::math::checked_add;
 using mmltk::common::math::checked_multiply;
 constexpr auto overflow = "supplemental selection workspace overflow";
 // Fixed class arrays, vector/dispatch/view control records and routine call
 // frames. This is a construction allowance, not a process RSS limit.
 std::uint64_t bytes = 64U << 10;
 const auto include = [&](const std::uint64_t count, const std::uint64_t width) { bytes = checked_add(bytes, checked_multiply(count, width, overflow), overflow); };
 for (const auto* source : {&objects365, &open_images}) {
  const auto images = checked_cast<std::uint64_t>(source->images.size(), overflow);
  const auto boxes = checked_cast<std::uint64_t>(source->boxes.size(), overflow);
  const auto incidences = std::min(boxes, checked_multiply(images, std::uint64_t{kClassCount}, overflow));
  include(images, sizeof(ClassMembership) + sizeof(HashedImage) + sizeof(std::uint8_t));
  // GCC's vector push growth doubles capacity. All class queues together have
  // <2C slots, plus <=C old slots during a reallocation, where C <= min(A,80N).
  include(incidences, 3U * sizeof(std::uint32_t));
  // In-place introsort has at most two recursive levels per key-count bit;
  // allow 512 bytes per frame, separately for the concurrent source sorts.
  include(std::bit_width(images) + 1U, 2U * 512U);
 }
 include(checked_cast<std::uint64_t>(shards, overflow), sizeof(ShardSummary) + 2U * sizeof(std::uint8_t) + sizeof(std::uint16_t));
 // Both final position arrays can be constructed concurrently. They and the
 // chosen shard IDs become retained products; immutable source backing is
 // already retained by the inputs and is not charged again here.
 include(selected_images, sizeof(std::size_t));
 return bytes;
}
[[nodiscard]] std::uint64_t sampling_hash(const BenchmarkDatasetSource source, const std::uint64_t image_id) noexcept {
 constexpr std::uint64_t kRevisionSeed = 0xC080BA1A6CED0002ULL;
 const std::uint64_t source_seed = static_cast<std::uint64_t>(source) * 0xD6E8FEB86659FD93ULL;
 return mmltk::common::math::deterministic_mix64(image_id ^ source_seed ^ kRevisionSeed);
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
void build_memberships(const NormalizedAnnotationIndex& source, SupplementalSamplingStats* stats, std::vector<ClassMembership>* memberships, const std::span<ShardSummary> shards,
 mmltk::common::concurrency::CancellationObservation cancel_requested) {
 stats->full_images = source.images.size();
 stats->full_boxes = source.boxes.size();
 if (memberships) memberships->resize(source.images.size());
 for (std::size_t image_index = 0U; image_index < source.images.size(); ++image_index) {
  if ((image_index & 4095U) == 0U) { throw_if_cancelled(cancel_requested); }
  const NormalizedImage& image = source.images[image_index];
  ShardSummary* shard = nullptr;
  if (!shards.empty()) {
   if (image.source_shard >= shards.size()) { throw std::runtime_error("Objects365 annotation references an unknown image shard"); }
   shard = &shards[image.source_shard];
   ++shard->images;
  }
  if (image.box_count == 0U || image.first_box > source.boxes.size() || image.box_count > source.boxes.size() - image.first_box) {
   throw std::runtime_error("supplemental sampler received an invalid image box span");
  }
  ClassMembership membership;
  for (std::uint64_t box_index = image.first_box; box_index < image.first_box + image.box_count; ++box_index) {
   const NormalizedBox& box = source.boxes[checked_cast<std::size_t>(box_index, "supplemental box index overflow")];
   if (box.class_id >= kClassCount) { throw std::runtime_error("supplemental sampler received an invalid class id"); }
   add_class(&membership, box.class_id);
   membership.mask_runs = mmltk::common::math::checked_add(membership.mask_runs, std::size_t{box.mask_rle_pairs}, "supplemental image mask count overflow");
  }
  if (membership.low == 0U && membership.high == 0U) { throw std::runtime_error("supplemental sampler received an image without mapped classes"); }
  if (memberships) (*memberships)[image_index] = membership;
  ClassMembershipCursor classes(membership);
  while (const auto class_id = classes.next()) {
   ++stats->available_class_images[*class_id];
   if (shard) ++shard->class_images[*class_id];
  }
 }
}
[[nodiscard]] std::vector<std::uint16_t> choose_objects365_shards(const std::span<const ShardSummary> summaries, const std::array<std::uint64_t, kClassCount>& full_class_images,
 const std::span<const std::uint64_t> shard_bytes, const std::uint64_t required_images, std::uint64_t* selected_archive_bytes, mmltk::common::concurrency::CancellationObservation cancel_requested) {
 const std::uint64_t headroom_images =
  checked_cast<std::uint64_t>((static_cast<unsigned long long>(required_images) * kShardCandidateHeadroomNumerator + kShardCandidateHeadroomDenominator - 1U) / kShardCandidateHeadroomDenominator,
   "Objects365 sampling headroom overflow");
 std::array<std::uint64_t, kClassCount> coverage_targets{};
 for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) { coverage_targets[class_id] = std::min(full_class_images[class_id], kShardClassCoverageFloor); }
 std::vector<std::uint8_t> selected(summaries.size(), std::uint8_t{0U});
 std::array<std::uint64_t, kClassCount> selected_coverage{};
 std::uint64_t selected_images = 0U;
 *selected_archive_bytes = 0U;
 std::vector<std::uint16_t> result;
 result.reserve(summaries.size());
 while (selected_images < headroom_images || !std::ranges::equal(selected_coverage, coverage_targets, [](const std::uint64_t available, const std::uint64_t target) { return available >= target; })) {
  throw_if_cancelled(cancel_requested);
  std::size_t best = summaries.size();
  long double best_score = -1.0L;
  for (std::size_t shard = 0U; shard < summaries.size(); ++shard) {
   if (selected[shard] != 0U || summaries[shard].images == 0U) { continue; }
   std::uint64_t coverage_gain = 0U;
   for (std::size_t class_id = 0U; class_id < kClassCount; ++class_id) {
    const std::uint64_t deficit = coverage_targets[class_id] > selected_coverage[class_id] ? coverage_targets[class_id] - selected_coverage[class_id] : 0U;
    coverage_gain += std::min(deficit, summaries[shard].class_images[class_id]);
   }
   const std::uint64_t useful_images = selected_images < headroom_images ? std::min(summaries[shard].images, headroom_images - selected_images) : 0U;
   const std::uint64_t gain = useful_images + coverage_gain * 8U;
   const std::uint64_t bytes = std::max<std::uint64_t>(1U, shard_bytes[shard]);
   const long double score = static_cast<long double>(gain) / static_cast<long double>(bytes);
   if (score > best_score || (score == best_score && (best == summaries.size() || bytes < shard_bytes[best] || (bytes == shard_bytes[best] && shard < best)))) {
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
[[nodiscard]] SourceSelection make_source_selection(
 const NormalizedAnnotationIndex& source, std::vector<ClassMembership> memberships, SupplementalSamplingStats stats, const std::span<const std::uint8_t> eligible_shards) {
 SourceSelection result;
 result.source = &source;
 result.memberships = std::move(memberships);
 result.selected.assign(source.images.size(), std::uint8_t{0U});
 result.stats = std::move(stats);
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
 source->selected_boxes = mmltk::common::math::checked_add(source->selected_boxes, std::size_t{image.box_count}, "supplemental selected box count overflow");
 source->selected_runs = mmltk::common::math::checked_add(source->selected_runs, source->memberships[image_index].mask_runs, "supplemental selected mask count overflow");
 ClassMembershipCursor classes(source->memberships[image_index]);
 while (const auto class_id = classes.next()) {
  ++source->stats.selected_class_images[*class_id];
  ++(*combined_counts)[*class_id];
 }
}
[[nodiscard]] std::size_t rarest_available_class(
 SourceSelection* objects, SourceSelection* open_images, const std::array<std::uint64_t, kClassCount>& combined_counts, const bool open_only, const bool open_allowed) {
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
[[nodiscard]] NormalizedAnnotationReadView selected_view(const SourceSelection& source, mmltk::common::concurrency::CancellationObservation cancellation) {
 std::vector<std::size_t> positions;
 positions.reserve(checked_cast<std::size_t>(source.stats.selected_images, "supplemental selected image count overflow"));
 for (std::size_t position = 0; position < source.selected.size(); ++position) {
  if ((position & 4095U) == 0) throw_if_cancelled(cancellation);
  if (source.selected[position]) positions.push_back(position);
 }
 return NormalizedAnnotationReadView(*source.source).select_images(std::move(positions), NormalizedAnnotationReadView::Counts{source.selected_boxes, source.selected_runs}, cancellation);
}
}  // namespace
CombinedSupplementalSamplingResult sample_combined_supplemental_indices(const NormalizedAnnotationIndex& coco_train, const NormalizedAnnotationIndex& objects365,
 const NormalizedAnnotationIndex& open_images, const std::span<const std::uint64_t> objects365_shard_bytes, mmltk::common::concurrency::CancellationObservation cancel_requested,
 BenchmarkCompilePipeline* execution) {
 if (coco_train.source != BenchmarkDatasetSource::kCoco2017 || objects365.source != BenchmarkDatasetSource::kObjects365V2 || open_images.source != BenchmarkDatasetSource::kOpenImagesV7) {
  throw std::runtime_error("combined supplemental sampler received the wrong sources");
 }
 if (objects365.images.size() < kSupplementalBudgetDenominator || open_images.images.size() < kSupplementalBudgetDenominator) {
  throw std::runtime_error("supplemental sources are too small for benchmark sampling");
 }
 if (objects365_shard_bytes.empty()) { throw std::runtime_error("Objects365 sampler requires archive byte identities"); }
 const auto target_images = objects365.images.size() / kSupplementalBudgetDenominator + open_images.images.size() / kSupplementalBudgetDenominator;
 // Reserve once on the source controller, before any CPU job or allocation.
 // Declaration order keeps credits until every transient vector and borrowed
 // callback below has retired, including exceptional exits. Child jobs draw
 // no additional credits and remain independently runnable when oversized.
 const auto workspace = execution ? execution->reserve({selection_workspace_bytes(objects365, open_images, objects365_shard_bytes.size(), target_images), 0}) : BenchmarkAllowance{};
 CombinedSupplementalSamplingResult result;
 result.target_images = target_images;
 result.open_images_floor = std::max<std::uint64_t>(1U, result.target_images / kOpenImagesDiversityDenominator);
 result.open_images_ceiling = std::max(result.open_images_floor, result.target_images / kOpenImagesMaximumDenominator);
 const std::uint64_t required_objects = result.target_images - result.open_images_floor;
 // Source-local membership, class counts, sorting and candidate construction
 // share the existing CPU owner; the greedy cross-source choice stays serial.
 const auto parallel = [&](std::size_t count, const std::function<void(std::size_t)>& work) {
  if (execution)
   execution->for_each(BenchmarkStage::Metadata, count, BenchmarkResources{}, work);
  else
   for (std::size_t i = 0; i < count; ++i) work(i);
 };
 SourceSelection object_selection, open_selection;
 SupplementalSamplingStats coco_stats;
 parallel(3, [&](std::size_t source) {
  if (source == 2) {
   build_memberships(coco_train, &coco_stats, nullptr, {}, cancel_requested);
   return;
  }
  SupplementalSamplingStats stats;
  std::vector<ClassMembership> memberships;
  std::vector<ShardSummary> shards(source == 0 ? objects365_shard_bytes.size() : 0);
  const auto& index = source == 0 ? objects365 : open_images;
  build_memberships(index, &stats, &memberships, shards, cancel_requested);
  std::vector<std::uint8_t> eligible;
  if (source == 0) {
   result.objects365_shards = choose_objects365_shards(shards, stats.available_class_images, objects365_shard_bytes, required_objects, &result.objects365_archive_bytes, cancel_requested);
   eligible.resize(objects365_shard_bytes.size());
   for (const auto shard : result.objects365_shards) eligible[shard] = 1;
  }
  (source == 0 ? object_selection : open_selection) = make_source_selection(index, std::move(memberships), std::move(stats), eligible);
 });
 auto combined_counts = coco_stats.available_class_images;
 std::uint64_t selected_total = 0U;
 while (selected_total < result.target_images) {
  if ((selected_total & 4095U) == 0U) { throw_if_cancelled(cancel_requested); }
  const bool open_allowed = open_selection.stats.selected_images < result.open_images_ceiling;
  const bool force_open = open_selection.stats.selected_images < result.open_images_floor && open_selection.stats.selected_images * result.target_images <= selected_total * result.open_images_floor;
  std::size_t chosen_class = rarest_available_class(&object_selection, &open_selection, combined_counts, force_open, open_allowed);
  if (chosen_class == kClassCount && force_open) { chosen_class = rarest_available_class(&object_selection, &open_selection, combined_counts, false, open_allowed); }
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
 if (selected_total != result.target_images || open_selection.stats.selected_images < result.open_images_floor || open_selection.stats.selected_images > result.open_images_ceiling) {
  throw std::runtime_error("combined supplemental sampler could not fill its exact target");
 }
 object_selection.stats.selected_boxes = object_selection.selected_boxes;
 open_selection.stats.selected_boxes = open_selection.selected_boxes;
 result.objects365.stats = object_selection.stats;
 result.open_images.stats = open_selection.stats;
 parallel(2, [&](std::size_t source) { (source == 0 ? result.objects365 : result.open_images).view = selected_view(source == 0 ? object_selection : open_selection, cancel_requested); });
 return result;
}
}  // namespace mmltk::backend::data::benchmark_internal
