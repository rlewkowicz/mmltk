#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <stb_image.h>
namespace mmltk::backend::data::benchmark_internal {
namespace {
[[noreturn]] void invalid(std::string_view detail) { throw std::runtime_error("COCONut: " + std::string(detail)); }
}
const CategoryLookup& coconut_categories() {
 static const auto lookup = make_numeric_lookup(coco_category_mappings());
 return lookup;
}
CoconutNativeImage::Read CoconutNativeImage::read(std::shared_ptr<const CoconutNativeImage> owner) {
 if (!owner) throw std::invalid_argument("missing native image custody");
 NormalizedAnnotationIndex index;
 static_cast<NormalizedAnnotationMetadata&>(index) = owner->lineage().index;
 index.rejected = owner->rejected();
 index.images = {&owner->image_, 1}; index.boxes = owner->boxes_; index.mask_rle_pairs = owner->runs_;
 return {std::move(owner), NormalizedAnnotationReadView(std::move(index))};
}
BenchmarkLabelInput CoconutNativeImage::labels(std::shared_ptr<const CoconutNativeImage> owner) {
 auto input = read(std::move(owner));
 return BenchmarkLabelInput(std::move(input.view), std::move(input.owner));
}
CoconutNativeWorkspace::CoconutNativeWorkspace(const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation)
 : limits_(limits), cancellation_(cancellation) {}
void CoconutNativeWorkspace::originals(std::shared_ptr<const CoconutRecoveryOriginals> value) {
 if (originals_ == value && (value || !recovery_)) return;
 recovery_.reset(); originals_ = std::move(value);
 if (originals_) recovery_ = std::make_unique<CoconutMaskRecovery>(*originals_);
}
void CoconutNativeWorkspace::recovery(const CoconutMaskRecovery* value) {
 recovery_ = value ? value->make_workspace() : nullptr;
}
void CoconutNativeWorkspace::retire() noexcept {
 active_record_ = nullptr; active_dimensions_ = {};
 std::vector<SegmentEntry>().swap(segment_by_id_);
 std::vector<CoconutSegmentSupport>().swap(support_);
 if (recovery_) recovery_->retire_scratch();
}
std::uint64_t CoconutNativeWorkspace::retained_bytes() const noexcept {
 return run_bytes_ + support_.capacity() * sizeof(CoconutSegmentSupport) + segment_by_id_.capacity() * sizeof(SegmentEntry) + (recovery_ ? recovery_->retained_bytes() : 0);
}
CoconutNativeWorkspace::SegmentEntry& CoconutNativeWorkspace::segment_entry(std::uint32_t id) {
 auto slot = (static_cast<std::size_t>(id) * 2654435761U) & (segment_by_id_.size() - 1);
 while (segment_by_id_[slot].id && segment_by_id_[slot].id != id) slot = (slot + 1) & (segment_by_id_.size() - 1);
 return segment_by_id_[slot];
}
std::vector<CoconutSegmentSupport> CoconutNativeWorkspace::take_support(std::size_t count) {
 if (!active_record_ || count != active_record_->segments.size()) invalid("invalid native support transfer");
 active_record_ = nullptr;
 std::vector<CoconutSegmentSupport> result; result.reserve(count);
 for (std::size_t i = 0; i < count; ++i) {
  support_[i].runs.account(nullptr);
  result.push_back(std::move(support_[i]));
 }
 return result;
}
void CoconutNativeWorkspace::borrow_support(const CoconutRecord& record, std::span<const CoconutSegmentSupport> input, dataset::MaskDimensions dimensions, std::shared_ptr<const void> owner) {
 if (input.size() != record.segments.size() || !owner || !dimensions.width || !dimensions.height) invalid("invalid native support custody");
 active_record_ = nullptr;
 if (support_.size() < input.size()) support_.resize(input.size());
 for (std::size_t i = 0; i < input.size(); ++i) {
  auto& target = support_[i];
  target.runs.account(&run_bytes_);
  target.area = input[i].area; target.bounds = input[i].bounds; target.recovered.reset(); target.carved = false;
  target.runs.borrow(input[i].runs.view(), owner);
 }
 active_record_ = &record; active_dimensions_ = dimensions;
}
std::uint64_t CoconutNativeWorkspace::workspace_bytes(const CoconutRecord& record, std::uint64_t pixels, std::uint64_t segments, std::uint64_t encoded, bool recover) const {
  using mmltk::common::math::checked_add;
  using mmltk::common::math::checked_multiply;
  // stb's encoded inflate input and scanlines/output; native run growth and
  // carving; support, open-address lookup, recovery grouping and ordinals.
  auto bytes = checked_add(checked_multiply(pixels, 96U, "COCONut pixel workspace overflow"), checked_multiply(encoded, 2U, "COCONut encoded workspace overflow"), "COCONut decode workspace overflow");
  bytes = checked_add(bytes, checked_multiply(segments, 1024U, "COCONut segment workspace overflow"), "COCONut normalize workspace overflow");
  return checked_add(bytes, recover && recovery_ ? recovery_->workspace_bytes(record) : 0, "COCONut recovery workspace overflow");
 }
dataset::MaskDimensions CoconutNativeWorkspace::admit_png(const CoconutRecord& record, std::span<const std::uint8_t> png) const {
  const auto& limits = limits_;
  if (png.size() > limits.max_png_bytes || png.size() > INT_MAX || png.size() < 26U || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0 || png[24] != 8 || png[25] != 2)
   invalid("expected bounded 8-bit RGB panoptic PNG");
  int width = 0, height = 0, channels = 0;
  if (!stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels) || width <= 0 || height <= 0 || channels != 3 ||
      static_cast<unsigned>(width) > limits.max_dimension || static_cast<unsigned>(height) > limits.max_dimension || static_cast<std::uint64_t>(width) * height > limits.max_pixels ||
      static_cast<std::uint64_t>(width) * height > UINT32_MAX)
   invalid("PNG dimensions exceed admission");
  if ((record.width && record.width != static_cast<unsigned>(width)) || (record.height && record.height != static_cast<unsigned>(height))) invalid("declared and PNG dimensions disagree");
  if (record.segments.size() > limits.max_segments) invalid("segment count exceeds admission");
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
 }
void CoconutNativeWorkspace::decode(const CoconutRecord& record, std::span<const std::uint8_t> png, dataset::MaskDimensions dimensions) {
  active_record_ = nullptr;
  if (png.size() > limits_.max_png_bytes || png.size() > INT_MAX) invalid("PNG exceeds decode admission");
  int width = static_cast<int>(dimensions.width), height = static_cast<int>(dimensions.height), channels = 3;
  throw_if_benchmark_cancelled(cancellation_);
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 3), stbi_image_free);
  if (!pixels) invalid("cannot decode panoptic PNG");
  if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != dimensions.width || static_cast<std::uint32_t>(height) != dimensions.height ||
   dimensions.width > limits_.max_dimension || dimensions.height > limits_.max_dimension || std::uint64_t{dimensions.width} * dimensions.height > limits_.max_pixels ||
   std::uint64_t{dimensions.width} * dimensions.height > UINT32_MAX || record.segments.size() > limits_.max_segments) invalid("decoded PNG dimensions exceed admission");
  segment_by_id_.assign(std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2)), {});
  if (support_.size() < record.segments.size()) support_.resize(record.segments.size());
  for (std::size_t i = 0; i < record.segments.size(); ++i) {
   const auto& segment = record.segments[i];
   if (segment.id == 0 || segment.id > 0xffffffU) invalid("duplicate/invalid segment ID");
   auto& entry = segment_entry(segment.id);
   if (entry.id) invalid("duplicate/invalid segment ID");
   entry = {segment.id, i};
   auto& support = support_[i];
   support.runs.account(&run_bytes_);
   support.area = 0;
   support.bounds = {};
   support.runs.clear();
   support.recovered.reset();
   support.carved = false;
  }
  const std::uint32_t count = static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * height);
  for (std::uint32_t begin = 0; begin < count;) {
   if ((begin / static_cast<std::uint32_t>(width)) % 64U == 0) throw_if_benchmark_cancelled(cancellation_);
   const auto id_at = [&](std::uint32_t position) {
    const auto* pixel = pixels.get() + static_cast<std::size_t>(position) * 3U;
    return static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8U) | (static_cast<std::uint32_t>(pixel[2]) << 16U);
   };
   const auto id = id_at(begin);
   auto end = begin + 1U;
   // Row boundaries make bounds O(1) per run; coalesce adjacent support below.
   const auto row_end = std::min(count, (begin / static_cast<std::uint32_t>(width) + 1U) * static_cast<std::uint32_t>(width));
   while (end < row_end && id_at(end) == id) ++end;
   if (id != 0) {
    const auto& found = segment_entry(id);
    if (!found.id) invalid("PNG references undeclared segment " + std::to_string(id));
    auto& support = support_[found.index];
    support.area += end - begin;
    dataset::include_row_major_mask_run(&support.bounds, begin, end, static_cast<std::uint32_t>(width));
    if (record.segments[found.index].isthing) {
     support.runs.append(begin, end);
    }
   }
   begin = end;
  }
  active_record_ = &record; active_dimensions_ = dimensions;
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeWorkspace::finish(const CoconutRecord& record, const CoconutPhysicalImage& physical, dataset::MaskDimensions dimensions,
 std::shared_ptr<const CoconutNativeLineage> lineage, const CoconutRejectedObject& rejected_object) {
  if (active_record_ != &record || active_dimensions_.width != dimensions.width || active_dimensions_.height != dimensions.height ||
   !lineage || lineage->component.source != physical.source) invalid("native image has no matching production workspace");
  const auto width = dimensions.width, height = dimensions.height;
  auto product = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(std::move(lineage)));
  product->segment_begin_ = record.first_segment_ordinal;
  const auto& component = product->lineage().component;
  CoconutRecoveryImage recovery{physical.image_id, 0, {}};
  if (recovery_)
   recovery_->apply(physical.source, record, static_cast<unsigned>(width), static_cast<unsigned>(height), std::span(support_).first(record.segments.size()), recovery, cancellation_);

  NormalizedImage image{physical.image_id, product->boxes_.size(), 0, static_cast<unsigned>(width), static_cast<unsigned>(height), physical.shard, 0};
  struct ReleaseSupport { CoconutSupportRuns& runs; ~ReleaseSupport() { runs.clear(); } };
  for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) invalid("source ordinal overflow");
   const auto& segment = record.segments[ordinal];
   auto& support = support_[ordinal];
   ReleaseSupport release{support.runs};
   ++product->rejected_.raw_records;
   if (!segment.isthing) continue;
   const auto& categories = coconut_categories().target_by_id;
   if (segment.category_id >= categories.size() || categories[segment.category_id] < 0) invalid("unknown COCO80 thing category " + std::to_string(segment.category_id));
   if (segment.area && (!std::isfinite(*segment.area) || *segment.area < 0)) invalid("invalid supplied area");
   NormalizedBox box;
   if (support.recovered) {
    box.x1 = support.recovered->x1;
    box.y1 = support.recovered->y1;
    box.x2 = support.recovered->x2;
    box.y2 = support.recovered->y2;
   } else if (segment.bbox) {
    const auto& supplied = *segment.bbox;
    if (!std::ranges::all_of(supplied, [](double value) { return std::isfinite(value); }) || supplied[2] <= 0 || supplied[3] <= 0 || !std::isfinite(supplied[0] + supplied[2]) ||
        !std::isfinite(supplied[1] + supplied[3]))
     invalid("invalid authoritative COCO bbox");
    box.x1 = static_cast<float>(supplied[0] / width);
    box.y1 = static_cast<float>(supplied[1] / height);
    box.x2 = static_cast<float>((supplied[0] + supplied[2]) / width);
    box.y2 = static_cast<float>((supplied[1] + supplied[3]) / height);
   } else {
    if (support.area == 0) {
     ++product->rejected_.degenerate_boxes;
     ++recovery.unresolved;
     if (component.recovery_policy) recovery.omissions.push_back({segment.id, record.first_segment_ordinal + ordinal, segment.category_id, 0});
     if (rejected_object) {
      try {
       rejected_object(physical, record, segment, "thing segment has neither mask pixels nor an authoritative bbox", component.recovery_policy);
      } catch (...) {
       // Reporting a discarded object cannot interrupt compilation.
      }
     }
     continue;
    }
    box.x1 = static_cast<float>(static_cast<double>(support.bounds.min_x) / width);
    box.y1 = static_cast<float>(static_cast<double>(support.bounds.min_y) / height);
    box.x2 = static_cast<float>(static_cast<double>(support.bounds.max_x) / width);
    box.y2 = static_cast<float>(static_cast<double>(support.bounds.max_y) / height);
   }
   if (!std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1)
    invalid("normalized box coordinates are not representable");
   if (support.runs.size() > UINT32_MAX) invalid("normalized RLE count overflow");
   box.mask_rle_offset = product->runs_.size();
   box.mask_rle_pairs = static_cast<std::uint32_t>(support.runs.size());
   box.class_id = static_cast<std::uint8_t>(categories[segment.category_id]);
   box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory | (segment.crowd ? kAnnotationCrowd : 0U) | (segment.ignore ? kAnnotationIgnore : 0U);
   box.original_area = support.recovered ? support.recovered->original_area : support.carved ? static_cast<double>(support.area) : segment.area.value_or(static_cast<double>(support.area));
   box.annotation_id = segment.id;
   box.source_category_id = segment.category_id;
   box.source_ordinal = record.first_segment_ordinal + ordinal;
   product->runs_.insert(product->runs_.end(), support.runs.begin(), support.runs.end());
   product->boxes_.push_back(box);
   ++image.box_count;
  }
  product->image_ = image;
  product->inventory_ = {physical, record.image_id, record.source_ordinal};
  product->recovery_ = std::move(recovery);
  active_record_ = nullptr;
  return product;
 }
std::shared_ptr<const CoconutNativeImage> CoconutNativeWorkspace::reuse(const CoconutRecord& record, const CoconutPhysicalImage& physical,
 const NormalizedAnnotationReadView& source, std::size_t position, const CoconutInventoryImage& inventory, const CoconutRecoveryImage* recovered,
 std::shared_ptr<const CoconutNativeLineage> lineage) {
  if (!lineage || lineage->component.source != physical.source || inventory.physical != physical) invalid("reused native image has no matching physical input");
  auto product = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(std::move(lineage)));
  product->segment_begin_ = record.first_segment_ordinal;
  auto image = source.image(position);
  image.first_box = product->boxes_.size();
  segment_by_id_.assign(std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2)), {});
  std::uint64_t things = 0;
  for (std::size_t i = 0; i < record.segments.size(); ++i) {
   const auto id = record.segments[i].id;
   things += record.segments[i].isthing;
   auto& entry = segment_entry(id);
   if (!id || entry.id) invalid("duplicate/invalid reused segment ID");
   entry = {id, i};
  }
  const auto ordinal = [&](std::uint64_t annotation) {
   const auto& found = segment_entry(mmltk::common::math::checked_cast<std::uint32_t>(annotation, "reused segment ID overflow"));
   if (!found.id) invalid("reused image contains an undeclared segment");
   return mmltk::common::math::checked_add(record.first_segment_ordinal, std::uint64_t{found.index}, "reused source ordinal overflow");
  };
  const auto& index = source.storage();
  const auto& original = source.image(position);
  for (auto box : index.boxes.subspan(original.first_box, original.box_count)) {
   const auto runs = index.mask_rle_pairs.subspan(box.mask_rle_offset, box.mask_rle_pairs);
   box.source_ordinal = ordinal(box.annotation_id);
   box.mask_rle_offset = product->runs_.size();
   product->runs_.insert(product->runs_.end(), runs.begin(), runs.end());
   product->boxes_.push_back(box);
  }
  product->image_ = image;
  product->inventory_ = inventory;
  product->rejected_.raw_records += record.segments.size();
  product->rejected_.degenerate_boxes += static_cast<std::uint64_t>(things) - image.box_count;
  if (recovered) {
   auto recovery = *recovered;
   for (auto* objects : {&recovery.objects, &recovery.omissions}) for (auto& object : *objects) object.source_ordinal = ordinal(object.annotation_id);
   product->recovery_ = std::move(recovery);
  }
  return product;
 }
} // namespace mmltk::backend::data::benchmark_internal
