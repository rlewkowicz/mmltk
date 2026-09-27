#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
using Cancellation = mmltk::common::concurrency::CancellationObservation;
bool CoconutMaskRecovery::candidate_mask(const NormalizedAnnotationIndex& index, std::uint32_t width, std::uint32_t height, Candidate& candidate, Cancellation cancellation) {
 const auto& box = *candidate.box;
 if ((box.flags & (kAnnotationMask | kAnnotationId | kAnnotationCategory)) != (kAnnotationMask | kAnnotationId | kAnnotationCategory) || !std::isfinite(box.original_area) || box.original_area < 0 ||
     !std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1 || box.mask_rle_pairs == 0 ||
     box.mask_rle_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - box.mask_rle_offset)
  return false;
 candidate.runs = std::span(index.mask_rle_pairs).subspan(box.mask_rle_offset, box.mask_rle_pairs);
 const auto pixels = static_cast<std::uint64_t>(width) * height;
 std::uint64_t end = 0;
 for (const auto run : candidate.runs) {
  throw_if_benchmark_cancelled(cancellation);
  const auto next = static_cast<std::uint64_t>(run.start) + run.length;
  if (run.length == 0 || run.start < end || next > pixels) return false;
  dataset::include_row_major_mask_run(&candidate.bounds, run.start, next, width);
  end = next;
 }
 return true;
}
bool CoconutMaskRecovery::intersects(const CoconutSegmentSupport& support, const Candidate& candidate, Cancellation cancellation) {
 if (support.bounds.max_x <= candidate.bounds.min_x || candidate.bounds.max_x <= support.bounds.min_x || support.bounds.max_y <= candidate.bounds.min_y ||
     candidate.bounds.max_y <= support.bounds.min_y)
  return false;
 std::size_t left = 0, right = 0;
 while (left < support.runs.size() && right < candidate.runs.size()) {
  throw_if_benchmark_cancelled(cancellation);
  const auto a = support.runs[left], b = candidate.runs[right];
  if (a.start < b.start + b.length && b.start < a.start + a.length) return true;
  if (a.start + a.length <= b.start)
   ++left;
  else
   ++right;
 }
 return false;
}
CoconutRecoveryOriginals::CoconutRecoveryOriginals(const NormalizedAnnotationIndex* train, const NormalizedAnnotationIndex* validation, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 const auto admit = [&](Originals& target, const NormalizedAnnotationIndex* index, std::string_view split) {
  if (!index || index->source != BenchmarkDatasetSource::kCoco2017 || index->split != split || index->annotation_sha256.empty()) return;
  target.index = index;
  target.images.reserve(index->images.size());
  for (const auto& image : index->images) {
   throw_if_benchmark_cancelled(cancellation);
   const auto [entry, inserted] = target.images.emplace(static_cast<std::uint64_t>(image.source_image_id), &image);
   if (!inserted) entry->second = nullptr;  // Duplicate physical identity is unusable.
  }
 };
 admit(train_, train, "train2017");
 admit(validation_, validation, "val2017");
}
const CoconutRecoveryOriginals::Originals* CoconutRecoveryOriginals::originals(CoconutImageNamespace source) const noexcept {
 if (source == CoconutImageNamespace::CocoTrain) return &train_;
 if (source == CoconutImageNamespace::CocoValidation) return &validation_;
 return nullptr;
}
std::string_view CoconutMaskRecovery::original_identity(CoconutImageNamespace source) const noexcept {
 const auto* selected = originals_.originals(source);
 return selected && selected->index ? std::string_view(selected->index->annotation_sha256) : std::string_view{};
}
void CoconutMaskRecovery::retire_scratch() noexcept { workspace_ = Workspace{}; }
void CoconutMaskRecovery::apply(CoconutImageNamespace source, const CoconutRecord& record, std::uint32_t width, std::uint32_t height, std::span<CoconutSegmentSupport> support,
 CoconutRecoveryImage& facts, Cancellation cancellation) {
 // Reset even after cancellation or an unavailable image; clear retains capacity.
 workspace_.groups.clear();
 workspace_.ordinals.clear();
 workspace_.candidates.clear();
 workspace_.represented.clear();
 workspace_.remaining.clear();
 workspace_.identities.clear();
 workspace_.combined.clear();
 workspace_.scratch.clear();
 throw_if_benchmark_cancelled(cancellation);
 const auto* selected = originals_.originals(source);
 if (!selected || !selected->index) return;
 const auto found = selected->images.find(record.image_id);
 if (found == selected->images.end() || !found->second) return;
 const auto& image = *found->second;
 const auto& index = *selected->index;
 if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > UINT32_MAX || image.width != width || image.height != height || image.first_box > index.boxes.size() ||
     image.box_count > index.boxes.size() - image.first_box || support.size() != record.segments.size())
  return;
 const auto dropped = [&](std::size_t ordinal) { return support[ordinal].area == 0 && !record.segments[ordinal].bbox; };
 bool has_dropped = false;
 for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
  throw_if_benchmark_cancelled(cancellation);
  if (record.segments[ordinal].isthing && dropped(ordinal)) {
   has_dropped = true;
   break;
  }
 }
 if (!has_dropped) return;
 const auto key = [&](std::size_t ordinal) {
  const auto& segment = record.segments[ordinal];
  return GroupKey{segment.category_id, segment.crowd, segment.ignore};
 };
 for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
  throw_if_benchmark_cancelled(cancellation);
  if (record.segments[ordinal].isthing) workspace_.ordinals.push_back(ordinal);
 }
 std::ranges::sort(workspace_.ordinals, [&](auto a, auto b) {
  throw_if_benchmark_cancelled(cancellation);
  if (key(a) != key(b)) return key(a) < key(b);
  if (dropped(a) != dropped(b)) return dropped(a);
  return dropped(a) ? record.segments[a].id < record.segments[b].id : a < b;
 });
 for (std::size_t i = 0; i < workspace_.ordinals.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  const auto group_key = key(workspace_.ordinals[i]);
  if (workspace_.groups.empty() || workspace_.groups.back().key != group_key) workspace_.groups.push_back({.key = group_key, .begin = i});
  auto& group = workspace_.groups.back();
  group.end = i + 1;
  if (dropped(workspace_.ordinals[i])) ++group.dropped;
 }
 for (const auto& box : std::span(index.boxes).subspan(image.first_box, image.box_count)) {
  throw_if_benchmark_cancelled(cancellation);
  const GroupKey group_key{box.source_category_id, (box.flags & kAnnotationCrowd) != 0, (box.flags & kAnnotationIgnore) != 0};
  const auto group = std::ranges::lower_bound(workspace_.groups, group_key, {}, &Group::key);
  if (group == workspace_.groups.end() || group->key != group_key || group->dropped == 0) continue;
  Candidate candidate{.box = &box, .group = static_cast<std::size_t>(group - workspace_.groups.begin())};
  if (!candidate_mask(index, width, height, candidate, cancellation)) group->valid = false;
  workspace_.candidates.push_back(candidate);
  ++group->candidate_count;
 }
 std::ranges::sort(workspace_.candidates, [&](const Candidate& a, const Candidate& b) {
  throw_if_benchmark_cancelled(cancellation);
  return a.group < b.group;
 });
 std::size_t candidate_begin = 0;
 for (const auto& group : workspace_.groups) {
  throw_if_benchmark_cancelled(cancellation);
  const auto candidates = std::span(workspace_.candidates).subspan(candidate_begin, group.candidate_count);
  candidate_begin += group.candidate_count;
  if (!group.valid || group.dropped == 0 || candidates.size() != group.end - group.begin) continue;
  workspace_.identities.clear();
  for (const auto& candidate : candidates) {
   throw_if_benchmark_cancelled(cancellation);
   workspace_.identities.push_back(candidate.box->annotation_id);
  }
  std::ranges::sort(workspace_.identities, [&](auto a, auto b) {
   throw_if_benchmark_cancelled(cancellation);
   return a < b;
  });
  bool valid = true;
  for (std::size_t i = 1; i < workspace_.identities.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   if (workspace_.identities[i - 1] == workspace_.identities[i]) {
    valid = false;
    break;
   }
  }
  if (!valid) continue;
  workspace_.represented.assign(candidates.size(), 0);
  for (std::size_t i = group.begin + group.dropped; i < group.end; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto ordinal = workspace_.ordinals[i];
   if (support[ordinal].runs.empty()) {
    valid = false;
    break;
   }
   std::size_t match = candidates.size();
   for (std::size_t j = 0; j < candidates.size(); ++j) {
    throw_if_benchmark_cancelled(cancellation);
    if (!intersects(support[ordinal], candidates[j], cancellation)) continue;
    if (match != candidates.size()) {
     valid = false;
     break;
    }
    match = j;
   }
   if (!valid || match == candidates.size() || workspace_.represented[match]) {
    valid = false;
    break;
   }
   workspace_.represented[match] = 1;
  }
  if (!valid) continue;
  workspace_.remaining.clear();
  for (std::size_t i = 0; i < candidates.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   if (!workspace_.represented[i]) workspace_.remaining.push_back(&candidates[i]);
  }
  if (workspace_.remaining.size() != group.dropped) continue;
  std::ranges::sort(workspace_.remaining, [&](const Candidate* a, const Candidate* b) {
   throw_if_benchmark_cancelled(cancellation);
   return a->box->annotation_id < b->box->annotation_id;
  });
  for (std::size_t i = 0; i < workspace_.remaining.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto ordinal = workspace_.ordinals[group.begin + i];
   const auto& candidate = *workspace_.remaining[i];
   auto& target = support[ordinal];
   target.recovered = *candidate.box;
   target.runs.assign(candidate.runs.begin(), candidate.runs.end());
   target.area = 0;
   for (const auto run : target.runs) {
    throw_if_benchmark_cancelled(cancellation);
    target.area += run.length;
   }
   target.bounds = candidate.bounds;
   workspace_.combined.insert(workspace_.combined.end(), candidate.runs.begin(), candidate.runs.end());
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) throw std::runtime_error("COCONut: source ordinal overflow");
   facts.objects.push_back({record.segments[ordinal].id, record.first_segment_ordinal + ordinal, record.segments[ordinal].category_id, candidate.box->annotation_id});
  }
 }
 if (workspace_.combined.empty()) return;
 throw_if_benchmark_cancelled(cancellation);
 std::ranges::sort(workspace_.combined, {}, [](RLEPair run) { return run.start; });
 std::size_t used = 0;
 for (const auto run : workspace_.combined) {
  throw_if_benchmark_cancelled(cancellation);
  if (used && run.start <= workspace_.combined[used - 1].start + workspace_.combined[used - 1].length) {
   auto& prior = workspace_.combined[used - 1];
   prior.length = std::max(prior.start + prior.length, run.start + run.length) - prior.start;
  } else
   workspace_.combined[used++] = run;
 }
 workspace_.combined.resize(used);
 dataset::RowMajorMaskBounds recovered_bounds;
 for (const auto run : workspace_.combined) {
  throw_if_benchmark_cancelled(cancellation);
  dataset::include_row_major_mask_run(&recovered_bounds, run.start, run.start + run.length, width);
 }
 for (std::size_t ordinal = 0; ordinal < support.size(); ++ordinal) {
  throw_if_benchmark_cancelled(cancellation);
  auto& target = support[ordinal];
  if (!record.segments[ordinal].isthing || target.recovered || target.runs.empty() || target.bounds.max_x <= recovered_bounds.min_x || recovered_bounds.max_x <= target.bounds.min_x ||
      target.bounds.max_y <= recovered_bounds.min_y || recovered_bounds.max_y <= target.bounds.min_y)
   continue;
  workspace_.scratch.clear();
  std::uint64_t area = 0;
  dataset::RowMajorMaskBounds bounds;
  const auto emit = [&](std::uint32_t begin, std::uint32_t end) {
   if (begin == end) return;
   workspace_.scratch.push_back({begin, end - begin});
   area += end - begin;
   dataset::include_row_major_mask_run(&bounds, begin, end, width);
  };
  auto cut = std::lower_bound(workspace_.combined.begin(), workspace_.combined.end(), target.runs.front().start, [](RLEPair value, std::uint32_t position) { return value.start + value.length <= position; });
  for (const auto run : target.runs) {
   throw_if_benchmark_cancelled(cancellation);
   auto cursor = run.start;
   const auto end = run.start + run.length;
   while (cut != workspace_.combined.end() && cut->start < end) {
    throw_if_benchmark_cancelled(cancellation);
    const auto cut_end = cut->start + cut->length;
    if (cut_end <= cursor) {
     ++cut;
     continue;
    }
    if (cursor < cut->start) emit(cursor, cut->start);
    cursor = std::min(end, cut_end);
    // A cut extending beyond this supporter run must remain for the next run.
    if (cut_end >= end) break;
    ++cut;
   }
   emit(cursor, end);
  }
  if (area == target.area) continue;
  target.runs.swap(workspace_.scratch);
  target.area = area;
  target.carved = true;
  target.bounds = bounds;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
