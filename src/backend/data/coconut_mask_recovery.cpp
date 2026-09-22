#include "detail/coconut_mask_recovery.h"
#include "detail/benchmark_cache.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
namespace mmltk::backend::data::benchmark_internal {
namespace {
using Cancellation = mmltk::common::concurrency::CancellationObservation;
using GroupKey = std::tuple<std::uint64_t, bool, bool>;
struct Candidate {
 const NormalizedBox* box;
 std::span<const RLEPair> runs;
 std::uint32_t min_x = UINT32_MAX, min_y = UINT32_MAX, max_x = 0, max_y = 0;
};
template <class Bounds>
void include_run(Bounds& bounds, RLEPair run, std::uint32_t width) {
 const auto last = run.start + run.length - 1U;
 const auto first_row = run.start / width;
 const auto last_row = last / width;
 bounds.min_x = std::min(bounds.min_x, first_row == last_row ? run.start % width : 0U);
 bounds.max_x = std::max(bounds.max_x, first_row == last_row ? last % width + 1U : width);
 bounds.min_y = std::min(bounds.min_y, first_row);
 bounds.max_y = std::max(bounds.max_y, last_row + 1U);
}
bool candidate_mask(const NormalizedAnnotationIndex& index, const NormalizedBox& box, std::uint32_t width, std::uint32_t height,
                    Candidate& candidate, Cancellation cancellation) {
 if ((box.flags & (kAnnotationMask | kAnnotationId | kAnnotationCategory)) != (kAnnotationMask | kAnnotationId | kAnnotationCategory) ||
     !std::isfinite(box.original_area) || box.original_area < 0 || !std::isfinite(box.x1) || !std::isfinite(box.y1) ||
     !std::isfinite(box.x2) || !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1 || box.mask_rle_pairs == 0 ||
     box.mask_rle_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - box.mask_rle_offset)
  return false;
 candidate.runs = std::span(index.mask_rle_pairs).subspan(box.mask_rle_offset, box.mask_rle_pairs);
 const auto pixels = static_cast<std::uint64_t>(width) * height;
 std::uint64_t end = 0;
 for (const auto run : candidate.runs) {
  throw_if_benchmark_cancelled(cancellation);
  const auto next = static_cast<std::uint64_t>(run.start) + run.length;
  if (run.length == 0 || run.start < end || next > pixels) return false;
  include_run(candidate, run, width);
  end = next;
 }
 return true;
}
bool intersects(const CoconutSegmentSupport& support, const Candidate& candidate, Cancellation cancellation) {
 if (support.max_x <= candidate.min_x || candidate.max_x <= support.min_x || support.max_y <= candidate.min_y || candidate.max_y <= support.min_y)
  return false;
 std::size_t left = 0, right = 0;
 while (left < support.runs.size() && right < candidate.runs.size()) {
  throw_if_benchmark_cancelled(cancellation);
  const auto a = support.runs[left], b = candidate.runs[right];
  if (a.start < b.start + b.length && b.start < a.start + a.length) return true;
  if (a.start + a.length <= b.start) ++left;
  else ++right;
 }
 return false;
}
}  // namespace
CoconutMaskRecovery::CoconutMaskRecovery(const NormalizedAnnotationIndex* train, const NormalizedAnnotationIndex* validation, Cancellation cancellation) {
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
const CoconutMaskRecovery::Originals* CoconutMaskRecovery::originals(CoconutImageNamespace source) const noexcept {
 if (source == CoconutImageNamespace::CocoTrain) return &train_;
 if (source == CoconutImageNamespace::CocoValidation) return &validation_;
 return nullptr;
}
std::string_view CoconutMaskRecovery::original_identity(CoconutImageNamespace source) const noexcept {
 const auto* selected = originals(source);
 return selected && selected->index ? std::string_view(selected->index->annotation_sha256) : std::string_view{};
}
void CoconutMaskRecovery::apply(CoconutImageNamespace source, const CoconutRecord& record, std::uint32_t width, std::uint32_t height,
                                std::span<CoconutSegmentSupport> support, CoconutRecoveryImage& facts, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 const auto* selected = originals(source);
 if (!selected || !selected->index) return;
 const auto found = selected->images.find(record.image_id);
 if (found == selected->images.end() || !found->second) return;
 const auto& image = *found->second;
 const auto& index = *selected->index;
 if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > UINT32_MAX || image.width != width || image.height != height ||
     image.first_box > index.boxes.size() || image.box_count > index.boxes.size() - image.first_box || support.size() != record.segments.size()) return;
 struct Group {
  std::vector<std::size_t> dropped, surviving;
  std::vector<Candidate> candidates;
  bool valid = true;
 };
 std::map<GroupKey, Group> groups;
 for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& segment = record.segments[ordinal];
  if (!segment.isthing) continue;
  auto& group = groups[{segment.category_id, segment.crowd, segment.ignore}];
  (support[ordinal].area == 0 && !segment.bbox ? group.dropped : group.surviving).push_back(ordinal);
 }
 if (std::ranges::none_of(groups, [](const auto& entry) { return !entry.second.dropped.empty(); })) return;
 for (const auto& box : std::span(index.boxes).subspan(image.first_box, image.box_count)) {
  throw_if_benchmark_cancelled(cancellation);
  const auto group = groups.find({box.source_category_id, (box.flags & kAnnotationCrowd) != 0, (box.flags & kAnnotationIgnore) != 0});
  if (group == groups.end() || group->second.dropped.empty()) continue;
  Candidate candidate{&box, {}};
  if (!candidate_mask(index, box, width, height, candidate, cancellation)) group->second.valid = false;
  group->second.candidates.push_back(candidate);
 }
 union_.clear();
 for (auto& [key, group] : groups) {
  throw_if_benchmark_cancelled(cancellation);
  if (!group.valid || group.dropped.empty() || group.candidates.size() != group.dropped.size() + group.surviving.size()) continue;
  std::unordered_set<std::uint64_t> identities;
  for (const auto& candidate : group.candidates) {
   if (!identities.insert(candidate.box->annotation_id).second) group.valid = false;
  }
  if (!group.valid) continue;
  std::vector<bool> represented(group.candidates.size(), false);
  for (const auto ordinal : group.surviving) {
   if (support[ordinal].runs.empty()) { group.valid = false; break; }
   std::size_t match = group.candidates.size();
   for (std::size_t i = 0; i < group.candidates.size(); ++i) {
    if (!intersects(support[ordinal], group.candidates[i], cancellation)) continue;
    if (match != group.candidates.size()) { group.valid = false; break; }
    match = i;
   }
   if (!group.valid || match == group.candidates.size() || represented[match]) { group.valid = false; break; }
   represented[match] = true;
  }
  if (!group.valid) continue;
  std::vector<const Candidate*> remaining;
  remaining.reserve(group.dropped.size());
  for (std::size_t i = 0; i < group.candidates.size(); ++i) if (!represented[i]) remaining.push_back(&group.candidates[i]);
  if (remaining.size() != group.dropped.size()) continue;
  std::ranges::sort(group.dropped, [&](auto a, auto b) { return record.segments[a].id < record.segments[b].id; });
  std::ranges::sort(remaining, {}, [](const Candidate* candidate) { return candidate->box->annotation_id; });
  for (std::size_t i = 0; i < remaining.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto ordinal = group.dropped[i];
   const auto& candidate = *remaining[i];
   auto& target = support[ordinal];
   target.recovered = *candidate.box;
   target.runs.assign(candidate.runs.begin(), candidate.runs.end());
   target.area = 0;
   for (const auto run : target.runs) { throw_if_benchmark_cancelled(cancellation); target.area += run.length; }
   target.min_x = candidate.min_x; target.min_y = candidate.min_y;
   target.max_x = candidate.max_x; target.max_y = candidate.max_y;
   union_.insert(union_.end(), candidate.runs.begin(), candidate.runs.end());
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) throw std::runtime_error("COCONut: source ordinal overflow");
   facts.objects.push_back({record.segments[ordinal].id, record.first_segment_ordinal + ordinal, record.segments[ordinal].category_id,
                            candidate.box->annotation_id});
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
  } else union_[used++] = run;
 }
 union_.resize(used);
 Candidate recovered_bounds{nullptr, union_};
 for (const auto run : union_) {
  throw_if_benchmark_cancelled(cancellation);
  include_run(recovered_bounds, run, width);
 }
 for (std::size_t ordinal = 0; ordinal < support.size(); ++ordinal) {
  auto& target = support[ordinal];
  if (!record.segments[ordinal].isthing || target.recovered || target.runs.empty() || target.max_x <= recovered_bounds.min_x ||
      recovered_bounds.max_x <= target.min_x || target.max_y <= recovered_bounds.min_y || recovered_bounds.max_y <= target.min_y) continue;
  scratch_.clear();
  std::uint64_t area = 0;
  for (const auto run : target.runs) {
   throw_if_benchmark_cancelled(cancellation);
   auto cursor = run.start;
   const auto end = run.start + run.length;
   auto cut = std::lower_bound(union_.begin(), union_.end(), cursor, [](RLEPair value, std::uint32_t position) { return value.start + value.length <= position; });
   for (; cut != union_.end() && cut->start < end; ++cut) {
    throw_if_benchmark_cancelled(cancellation);
    if (cursor < cut->start) scratch_.push_back({cursor, cut->start - cursor});
    cursor = std::min(end, cut->start + cut->length);
   }
   if (cursor < end) scratch_.push_back({cursor, end - cursor});
  }
  for (const auto run : scratch_) { throw_if_benchmark_cancelled(cancellation); area += run.length; }
  if (area == target.area) continue;
  target.runs.swap(scratch_);
  target.area = area;
  target.carved = true;
  target.min_x = target.min_y = UINT32_MAX;
  target.max_x = target.max_y = 0;
  for (const auto run : target.runs) { throw_if_benchmark_cancelled(cancellation); include_run(target, run, width); }
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
