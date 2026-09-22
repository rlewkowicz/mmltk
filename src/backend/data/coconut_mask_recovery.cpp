#include "detail/coconut_mask_recovery.h"
#include "detail/benchmark_cache.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
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
void CoconutMaskRecovery::apply(CoconutImageNamespace source, const CoconutRecord& record, std::uint32_t width, std::uint32_t height, std::span<CoconutSegmentSupport> support,
 CoconutRecoveryImage& facts, Cancellation cancellation) {
 // Reset even after cancellation or an unavailable image; clear retains capacity.
 groups_.clear();
 ordinals_.clear();
 candidates_.clear();
 represented_.clear();
 remaining_.clear();
 identities_.clear();
 union_.clear();
 scratch_.clear();
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
  if (record.segments[ordinal].isthing) ordinals_.push_back(ordinal);
 }
 std::ranges::sort(ordinals_, [&](auto a, auto b) {
  throw_if_benchmark_cancelled(cancellation);
  if (key(a) != key(b)) return key(a) < key(b);
  if (dropped(a) != dropped(b)) return dropped(a);
  return dropped(a) ? record.segments[a].id < record.segments[b].id : a < b;
 });
 for (std::size_t i = 0; i < ordinals_.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  const auto group_key = key(ordinals_[i]);
  if (groups_.empty() || groups_.back().key != group_key) groups_.push_back({.key = group_key, .begin = i});
  auto& group = groups_.back();
  group.end = i + 1;
  if (dropped(ordinals_[i])) ++group.dropped;
 }
 for (const auto& box : std::span(index.boxes).subspan(image.first_box, image.box_count)) {
  throw_if_benchmark_cancelled(cancellation);
  const GroupKey group_key{box.source_category_id, (box.flags & kAnnotationCrowd) != 0, (box.flags & kAnnotationIgnore) != 0};
  const auto group = std::ranges::lower_bound(groups_, group_key, {}, &Group::key);
  if (group == groups_.end() || group->key != group_key || group->dropped == 0) continue;
  Candidate candidate{.box = &box, .group = static_cast<std::size_t>(group - groups_.begin())};
  if (!candidate_mask(index, width, height, candidate, cancellation)) group->valid = false;
  candidates_.push_back(candidate);
  ++group->candidate_count;
 }
 std::ranges::sort(candidates_, [&](const Candidate& a, const Candidate& b) {
  throw_if_benchmark_cancelled(cancellation);
  return a.group < b.group;
 });
 std::size_t candidate_begin = 0;
 for (const auto& group : groups_) {
  throw_if_benchmark_cancelled(cancellation);
  const auto candidates = std::span(candidates_).subspan(candidate_begin, group.candidate_count);
  candidate_begin += group.candidate_count;
  if (!group.valid || group.dropped == 0 || candidates.size() != group.end - group.begin) continue;
  identities_.clear();
  for (const auto& candidate : candidates) {
   throw_if_benchmark_cancelled(cancellation);
   identities_.push_back(candidate.box->annotation_id);
  }
  std::ranges::sort(identities_, [&](auto a, auto b) {
   throw_if_benchmark_cancelled(cancellation);
   return a < b;
  });
  bool valid = true;
  for (std::size_t i = 1; i < identities_.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   if (identities_[i - 1] == identities_[i]) {
    valid = false;
    break;
   }
  }
  if (!valid) continue;
  represented_.assign(candidates.size(), 0);
  for (std::size_t i = group.begin + group.dropped; i < group.end; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto ordinal = ordinals_[i];
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
   if (!valid || match == candidates.size() || represented_[match]) {
    valid = false;
    break;
   }
   represented_[match] = 1;
  }
  if (!valid) continue;
  remaining_.clear();
  for (std::size_t i = 0; i < candidates.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   if (!represented_[i]) remaining_.push_back(&candidates[i]);
  }
  if (remaining_.size() != group.dropped) continue;
  std::ranges::sort(remaining_, [&](const Candidate* a, const Candidate* b) {
   throw_if_benchmark_cancelled(cancellation);
   return a->box->annotation_id < b->box->annotation_id;
  });
  for (std::size_t i = 0; i < remaining_.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto ordinal = ordinals_[group.begin + i];
   const auto& candidate = *remaining_[i];
   auto& target = support[ordinal];
   target.recovered = *candidate.box;
   target.runs.assign(candidate.runs.begin(), candidate.runs.end());
   target.area = 0;
   for (const auto run : target.runs) {
    throw_if_benchmark_cancelled(cancellation);
    target.area += run.length;
   }
   target.bounds = candidate.bounds;
   union_.insert(union_.end(), candidate.runs.begin(), candidate.runs.end());
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) throw std::runtime_error("COCONut: source ordinal overflow");
   facts.objects.push_back({record.segments[ordinal].id, record.first_segment_ordinal + ordinal, record.segments[ordinal].category_id, candidate.box->annotation_id});
  }
 }
 if (union_.empty()) return;
 throw_if_benchmark_cancelled(cancellation);
 std::ranges::sort(union_, {}, [](RLEPair run) { return run.start; });
 std::size_t used = 0;
 for (const auto run : union_) {
  throw_if_benchmark_cancelled(cancellation);
  if (used && run.start <= union_[used - 1].start + union_[used - 1].length) {
   auto& prior = union_[used - 1];
   prior.length = std::max(prior.start + prior.length, run.start + run.length) - prior.start;
  } else
   union_[used++] = run;
 }
 union_.resize(used);
 dataset::RowMajorMaskBounds recovered_bounds;
 for (const auto run : union_) {
  throw_if_benchmark_cancelled(cancellation);
  dataset::include_row_major_mask_run(&recovered_bounds, run.start, run.start + run.length, width);
 }
 for (std::size_t ordinal = 0; ordinal < support.size(); ++ordinal) {
  throw_if_benchmark_cancelled(cancellation);
  auto& target = support[ordinal];
  if (!record.segments[ordinal].isthing || target.recovered || target.runs.empty() || target.bounds.max_x <= recovered_bounds.min_x || recovered_bounds.max_x <= target.bounds.min_x ||
      target.bounds.max_y <= recovered_bounds.min_y || recovered_bounds.max_y <= target.bounds.min_y)
   continue;
  scratch_.clear();
  std::uint64_t area = 0;
  dataset::RowMajorMaskBounds bounds;
  const auto emit = [&](std::uint32_t begin, std::uint32_t end) {
   if (begin == end) return;
   scratch_.push_back({begin, end - begin});
   area += end - begin;
   dataset::include_row_major_mask_run(&bounds, begin, end, width);
  };
  auto cut = std::lower_bound(union_.begin(), union_.end(), target.runs.front().start, [](RLEPair value, std::uint32_t position) { return value.start + value.length <= position; });
  for (const auto run : target.runs) {
   throw_if_benchmark_cancelled(cancellation);
   auto cursor = run.start;
   const auto end = run.start + run.length;
   while (cut != union_.end() && cut->start < end) {
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
  target.runs.swap(scratch_);
  target.area = area;
  target.carved = true;
  target.bounds = bounds;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
