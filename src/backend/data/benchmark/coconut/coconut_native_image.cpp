#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/common/io/file_digest.h"
#include <nlohmann/json.hpp>
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <stb_image.h>
#include "src/common/system/numa_topology.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
namespace mmltk::backend::data::benchmark_internal {
namespace {
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
}  // namespace
std::string coconut_image_input_identity(std::string_view annotations, const CoconutPhysicalImage& physical, std::string_view originals) {
 const auto material = nlohmann::json{
  {"domain", "coconut-image-labels-v1"}, {"annotations", annotations},
  {"physical", {physical.source == CoconutImageNamespace::CocoUnlabeled ? CoconutImageNamespace::CocoTrain : physical.source, physical.image_id}},
  {"recovery_policy", originals.empty() ? 0 : kCoconutRecoveryPolicy}, {"originals", originals}
 }.dump();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
const CategoryLookup& coconut_categories() {
 static const auto lookup = make_numeric_lookup(coco_category_mappings());
 return lookup;
}
CoconutNativeImage::Read CoconutNativeImage::read(std::shared_ptr<const CoconutNativeImage> owner) {
 if (!owner) throw std::invalid_argument("missing native image custody");
 auto index = owner->admitted_.value_or(NormalizedAnnotationIndex{});
 if (!owner->admitted_) index = seal_normalized_annotation_image(owner->lineage().index.source, owner->image_, owner->boxes(), owner->runs(), owner);
 static_cast<NormalizedAnnotationMetadata&>(index) = owner->lineage().index;
 index.rejected = owner->rejected();
 return {std::move(owner), NormalizedAnnotationReadView(std::move(index))};
}
BenchmarkLabelInput CoconutNativeImage::labels(std::shared_ptr<const CoconutNativeImage> owner) {
 auto input = read(std::move(owner));
 return BenchmarkLabelInput(std::move(input.view), std::move(input.owner));
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeImage::rejected(const CoconutRecord& record, const CoconutPhysicalImage& physical, std::shared_ptr<const CoconutNativeLineage> lineage) {
 auto result = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(std::move(lineage)));
 result->inventory_ = {physical, record.image_id, record.source_ordinal};
 result->image_ = {physical.image_id, 0, 0, std::max(1U, record.width), std::max(1U, record.height), physical.shard, 0};
 result->segment_begin_ = record.first_segment_ordinal;
 result->rejected_.malformed_records = record.segments.size();
 result->rejected_.raw_records = record.segments.size();
 result->required_labels_valid_ = record.width && record.height && std::ranges::none_of(record.segments, [](const auto& segment) { return segment.isthing || !segment.valid; });
 result->input_identity_ = coconut_image_input_identity(result->lineage().component.annotation_input_identity, physical, result->lineage().component.original_annotation_identity);
 return result;
}
namespace {
std::size_t aligned_native_extent(std::size_t bytes, std::size_t alignment) { return common::math::checked_add(bytes, alignment - 1, "COCONut native alignment overflow") & ~(alignment - 1); }
constexpr auto native_alignment = std::max({alignof(NormalizedBox), alignof(RLEPair), alignof(CoconutNativeSupport::Segment)});
}  // namespace
CoconutNativeOutput::CoconutNativeOutput(std::shared_ptr<CoconutNativeImage> product, std::span<const NormalizedBox> boxes, std::span<const RLEPair> runs)
    : product_(std::move(product)),
      boxes_(boxes),
      runs_(runs),
      run_offset_(aligned_native_extent(boxes.size_bytes(), alignof(RLEPair))),
      request_(common::math::checked_add(run_offset_, runs.size_bytes(), "COCONut native extent overflow"), native_alignment) {}
void CoconutNativeOutput::retain_support(std::span<const CoconutSegmentSupport> support, CoconutNativeSupport& target) {
 support_ = support;
 support_target_ = &target;
 support_offset_ = aligned_native_extent(common::math::checked_add(run_offset_, runs_.size_bytes(), "COCONut support extent overflow"), alignof(CoconutNativeSupport::Segment));
 support_run_offset_ =
  aligned_native_extent(common::math::checked_add(
                         support_offset_, common::math::checked_multiply(support.size(), sizeof(CoconutNativeSupport::Segment), "COCONut support extent overflow"), "COCONut support extent overflow"),
   alignof(RLEPair));
 support_runs_ = 0;
 for (const auto& item : support) support_runs_ = common::math::checked_add(support_runs_, item.runs.size(), "COCONut support run count overflow");
 request_ = CoconutStaging::Request(
  common::math::checked_add(support_run_offset_, common::math::checked_multiply(support_runs_, sizeof(RLEPair), "COCONut support extent overflow"), "COCONut support extent overflow"),
  native_alignment);
}
void CoconutNativeOutput::fill(mmltk::common::concurrency::CancellationObservation cancellation) {
 const auto target = request_.target();
 if (!product_) return;
 if (!boxes_.empty()) std::memcpy(target.data(), boxes_.data(), boxes_.size_bytes());
 if (!runs_.empty()) std::memcpy(target.data() + run_offset_, runs_.data(), runs_.size_bytes());
 if (support_target_) {
  auto* headers = support_.empty() ? nullptr : reinterpret_cast<CoconutNativeSupport::Segment*>(target.data() + support_offset_);
  auto* output = support_runs_ ? reinterpret_cast<RLEPair*>(target.data() + support_run_offset_) : nullptr;
  std::size_t offset = 0;
  for (std::size_t i = 0; i < support_.size(); ++i) {
   const auto& item = support_[i];
   headers[i] = {item.area, item.bounds, offset, item.runs.size(), item.valid};
   if (!item.runs.empty()) std::memcpy(output + offset, item.runs.data(), item.runs.size() * sizeof(RLEPair));
   offset += item.runs.size();
  }
  *support_target_ = {request_.bytes().owner, {headers, support_.size()}, {output, support_runs_}};
 }
 product_->backing_ = request_.bytes().owner;
 product_->mapped_boxes_ = {boxes_.empty() ? nullptr : reinterpret_cast<const NormalizedBox*>(target.data()), boxes_.size()};
 product_->mapped_runs_ = {runs_.empty() ? nullptr : reinterpret_cast<const RLEPair*>(target.data() + run_offset_), runs_.size()};
 product_->staged_ = true;
 std::vector<NormalizedBox>().swap(product_->boxes_);
 std::vector<RLEPair>().swap(product_->runs_);
 product_->admitted_ = seal_normalized_annotation_image(product_->lineage().index.source, product_->image_, product_->mapped_boxes_, product_->mapped_runs_, product_->backing_, cancellation);
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeImage::stage(std::shared_ptr<const CoconutNativeImage> source, CoconutStaging& staging) {
 if (source->staged_ || (!source->backing_ && source->boxes().empty() && source->runs().empty())) return source;
 auto result = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(*source));
 CoconutNativeOutput output(result, source->boxes(), source->runs());
 staging.prepare(output.request());
 output.fill();
 return result;
}
void CoconutNativeWorkspace::materialize(const std::shared_ptr<CoconutNativeImage>& product, CoconutNativeOutput* output) {
 if (output)
  *output = CoconutNativeOutput(product, scratch().boxes, scratch().runs);
 else if (scratch().boxes.empty() && scratch().runs.empty())
  product->staged_ = true;
 else {
  product->boxes_.assign(scratch().boxes.begin(), scratch().boxes.end());
  product->runs_.assign(scratch().runs.begin(), scratch().runs.end());
 }
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeImage::bind(std::shared_ptr<const CoconutNativeImage> source, const CoconutPhysicalImage& physical) {
 if (source->inventory_.physical == physical) return source;
 auto lineage = std::make_shared<CoconutNativeLineage>(*source->lineage_);
 lineage->component.source = physical.source;
 lineage->index.source =
  physical.source == CoconutImageNamespace::Objects365V1 || physical.source == CoconutImageNamespace::Objects365V2 ? BenchmarkDatasetSource::kObjects365V2 : BenchmarkDatasetSource::kCoco2017;
 lineage->index.split = "coconut-" + std::to_string(static_cast<unsigned>(lineage->component.edition)) + "-" + std::to_string(static_cast<unsigned>(physical.source));
 auto result = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(*source));
 result->lineage_ = std::move(lineage);
 result->image_.source_shard = physical.shard;
 result->inventory_.physical = physical;
 if (result->admitted_) result->admitted_ = rebind_normalized_annotation_image(*result->admitted_, result->lineage_->index.source, result->image_);
 result->input_identity_ = coconut_image_input_identity(result->lineage().component.annotation_input_identity, physical, result->lineage().component.original_annotation_identity);
 return result;
}
CoconutNativeWorkspace::CoconutNativeWorkspace(const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation) : limits_(limits), cancellation_(cancellation) {}
CoconutNativeWorkspace::~CoconutNativeWorkspace() { retire(); }
void CoconutNativeWorkspace::originals(std::shared_ptr<const CoconutRecoveryOriginals> value) {
 if (originals_ == value && (value || !recovery_)) return;
 recovery_.reset();
 originals_ = std::move(value);
 if (originals_) {
  recovery_ = std::make_unique<CoconutMaskRecovery>(*originals_);
  if (supplied_) recovery_->prepare(supplied_.get());
 }
}
void CoconutNativeWorkspace::recovery(const CoconutMaskRecovery* value) { recovery_ = value ? value->make_workspace() : nullptr; }
CoconutNativeWorkspace::Scratch& CoconutNativeWorkspace::scratch() {
 if (!scratch_) scratch_.emplace(supplied_ ? static_cast<std::pmr::memory_resource*>(supplied_.get()) : std::pmr::get_default_resource());
 return *scratch_;
}
void CoconutNativeWorkspace::supports(std::size_t count) {
 auto& values = scratch().support;
 values.reserve(count);
 while (values.size() < count) values.emplace_back(values.get_allocator().resource());
}
void CoconutNativeWorkspace::prepare_support(std::span<std::byte> memory) {
 retire();
 if (memory.empty()) return;
 supplied_ = std::make_unique<std::pmr::monotonic_buffer_resource>(memory.data(), memory.size(), std::pmr::null_memory_resource());
 if (recovery_) recovery_->prepare(supplied_.get());
}
void CoconutNativeWorkspace::release_supplied() noexcept {
 if (!supplied_) return;
 if (recovery_) recovery_->retire_scratch();
 scratch_.reset();
 supplied_.reset();
 active_record_ = nullptr;
}
void CoconutNativeWorkspace::retire() noexcept {
 if (recovery_) recovery_->retire_scratch();
 scratch_.reset();
 supplied_.reset();
 decode_memory_.reset();
 active_record_ = nullptr;
 active_dimensions_ = {};
 unlisted_segment_ = 0;
}
std::uint64_t CoconutNativeWorkspace::retained_bytes() const noexcept {
 if (supplied_) return 0;  // Actual supplied bytes belong to the task arena lease.
 return (decode_memory_ ? decode_memory_->capacity_bytes() : 0) + run_bytes_ +
        (scratch_ ? scratch_->boxes.capacity() * sizeof(NormalizedBox) + scratch_->runs.capacity() * sizeof(RLEPair) + scratch_->support.capacity() * sizeof(CoconutSegmentSupport) +
                     scratch_->segments.capacity() * sizeof(SegmentEntry)
                  : 0) +
        (recovery_ ? recovery_->retained_bytes() : 0);
}
CoconutNativeWorkspace::SegmentEntry& CoconutNativeWorkspace::segment_entry(std::uint32_t id) {
 auto slot = (static_cast<std::size_t>(id) * 2654435761U) & (scratch().segments.size() - 1);
 while (scratch().segments[slot].id && scratch().segments[slot].id != id) slot = (slot + 1) & (scratch().segments.size() - 1);
 return scratch().segments[slot];
}
std::span<const CoconutSegmentSupport> CoconutNativeWorkspace::support(std::size_t count) const {
 if (!scratch_ || count > scratch_->support.size()) throw std::logic_error("COCONut: invalid native support transfer");
 return std::span(scratch_->support).first(count);
}
void CoconutNativeWorkspace::borrow_support(const CoconutRecord& record, const CoconutNativeSupport& input, dataset::MaskDimensions dimensions, std::uint32_t unlisted_segment) {
 unlisted_segment_ = unlisted_segment;
 if (input.segments.size() != record.segments.size() || (!input.owner && !input.segments.empty()) || !dimensions.width || !dimensions.height)
  throw std::logic_error("COCONut: invalid native support custody");
 active_record_ = nullptr;
 supports(input.segments.size());
 for (std::size_t i = 0; i < input.segments.size(); ++i) {
  auto& target = scratch().support[i];
  target.runs.account(&run_bytes_);
  target.area = input.segments[i].area;
  target.bounds = input.segments[i].bounds;
  target.recovered.reset();
  target.carved = false;
  target.valid = input.segments[i].valid;
  target.runs.borrow(input.runs.subspan(input.segments[i].first_run, input.segments[i].runs), input.owner);
 }
 active_record_ = &record;
 active_dimensions_ = dimensions;
}
std::uint64_t CoconutNativeWorkspace::support_workspace_bytes(const CoconutRecord& record, std::uint64_t pixels, std::uint64_t segments, bool recover) const {
 using mmltk::common::math::checked_add;
 using mmltk::common::math::checked_multiply;
 // Typed capacities for native runs (geometric growth plus immutable output),
 // support records and the open-address table. PNG storage is admitted from its
 // actual header by decoder_workspace_bytes before dispatch, separately.
 auto bytes =
  checked_add(std::uint64_t{256}, checked_multiply(pixels, sizeof(RLEPair) * (recover && recovery_ ? 12U : 6U), "COCONut support workspace overflow"), "COCONut support alignment overflow");
 bytes =
  checked_add(bytes, checked_multiply(segments, sizeof(CoconutSegmentSupport) + sizeof(SegmentEntry) * 4U + sizeof(NormalizedBox) + sizeof(std::uint64_t) * 4U, "COCONut segment workspace overflow"),
   "COCONut normalize workspace overflow");
 return checked_add(bytes, recover && recovery_ ? recovery_->workspace_bytes(record) : 0, "COCONut recovery workspace overflow");
}
std::uint64_t CoconutNativeWorkspace::reuse_workspace_bytes(const CoconutRecord& record, const NormalizedAnnotationReadView& source, std::size_t position) const {
 const auto image = source.image(position);
 auto bytes = common::math::checked_add(
  std::uint64_t{256}, common::math::checked_multiply(std::uint64_t{image.box_count}, sizeof(NormalizedBox), "COCONut reused boxes overflow"), "COCONut reused alignment overflow");
 bytes = common::math::checked_add(bytes,
  common::math::checked_multiply(std::uint64_t{std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2))}, sizeof(SegmentEntry), "COCONut reused table overflow"),
  "COCONut reused workspace overflow");
 for (const auto& box : source.storage().boxes.subspan(image.first_box, image.box_count))
  bytes = common::math::checked_add(bytes, common::math::checked_multiply(std::uint64_t{box.mask_rle_pairs}, sizeof(RLEPair), "COCONut reused runs overflow"), "COCONut reused workspace overflow");
 return bytes;
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
std::size_t CoconutNativeWorkspace::decoder_workspace_bytes(std::span<const std::uint8_t> png) const {
 try {
  return detail::CompileImageDecoder::plan(decoder_.read_header(png, detail::CompileImagePolicy::Directory)).bytes();
 } catch (const detail::CompileImageError&) { invalid("cannot decode panoptic PNG"); }
}
void CoconutNativeWorkspace::decode(const CoconutRecord& record, std::span<const std::uint8_t> png, dataset::MaskDimensions dimensions, std::span<std::byte> supplied) {
 active_record_ = nullptr;
 unlisted_segment_ = 0;
 if (png.size() > limits_.max_png_bytes || png.size() > INT_MAX) invalid("PNG exceeds decode admission");
 int width = static_cast<int>(dimensions.width), height = static_cast<int>(dimensions.height);
 throw_if_benchmark_cancelled(cancellation_);
 detail::CompileImageHeader header;
 try {
  header = decoder_.read_header(png, detail::CompileImagePolicy::Directory);
 } catch (const detail::CompileImageError&) { invalid("cannot decode panoptic PNG"); }
 const auto bytes = detail::CompileImageDecoder::plan(header).bytes();
 if (!supplied.empty()) prepare_support(supplied.subspan(bytes));
 if (supplied.empty()) {
  if (!decode_memory_) decode_memory_ = std::make_unique<common::system::NumaMemory>(common::system::NumaTopology::Capture().permitted_nodes.front(), common::system::NumaMemory::Lock::Pages);
  decode_memory_->ensure_bytes(bytes);
  supplied = {static_cast<std::byte*>(decode_memory_->data()), decode_memory_->capacity_bytes()};
 }
 std::span<const std::uint8_t> pixels;
 try {
  pixels = decoder_.decode_rgb(png, header, supplied);
 } catch (const detail::CompileImageError&) { invalid("cannot decode panoptic PNG"); }
 width = static_cast<int>(header.width);
 height = static_cast<int>(header.height);
 if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != dimensions.width || static_cast<std::uint32_t>(height) != dimensions.height || dimensions.width > limits_.max_dimension ||
     dimensions.height > limits_.max_dimension || std::uint64_t{dimensions.width} * dimensions.height > limits_.max_pixels || std::uint64_t{dimensions.width} * dimensions.height > UINT32_MAX ||
     record.segments.size() > limits_.max_segments)
  invalid("decoded PNG dimensions exceed admission");
 scratch().segments.assign(std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2)), {});
 supports(record.segments.size());
 for (std::size_t i = 0; i < record.segments.size(); ++i) {
  const auto& segment = record.segments[i];
  auto& support = scratch().support[i];
  support.runs.account(&run_bytes_);
  support.area = 0;
  support.bounds = {};
  support.runs.clear();
  support.recovered.reset();
  support.carved = false;
  support.valid = segment.valid && segment.id != 0 && segment.id <= 0xffffffU;
  if (!support.valid) continue;
  auto& entry = segment_entry(segment.id);
  if (entry.id) {
   scratch().support[entry.index].valid = support.valid = false;
   continue;
  }
  entry = {segment.id, i};
 }
 const std::uint32_t count = static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * height);
 for (std::uint32_t begin = 0; begin < count;) {
  if ((begin / static_cast<std::uint32_t>(width)) % 64U == 0) throw_if_benchmark_cancelled(cancellation_);
  const auto id_at = [&](std::uint32_t position) {
   const auto* pixel = pixels.data() + static_cast<std::size_t>(position) * 3U;
   return static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8U) | (static_cast<std::uint32_t>(pixel[2]) << 16U);
  };
  const auto id = id_at(begin);
  auto end = begin + 1U;
  // Row boundaries make bounds O(1) per run; coalesce adjacent support below.
  const auto row_end = std::min(count, (begin / static_cast<std::uint32_t>(width) + 1U) * static_cast<std::uint32_t>(width));
  while (end < row_end && id_at(end) == id) ++end;
  if (id != 0) {
   const auto& found = segment_entry(id);
   if (!found.id) {
    if (!unlisted_segment_) unlisted_segment_ = id;
    begin = end;
    continue;
   }
   if (!scratch().support[found.index].valid) {
    begin = end;
    continue;
   }
   auto& support = scratch().support[found.index];
   support.area += end - begin;
   dataset::include_row_major_mask_run(&support.bounds, begin, end, static_cast<std::uint32_t>(width));
   if (record.segments[found.index].isthing) { support.runs.append(begin, end); }
  }
  begin = end;
 }
 active_record_ = &record;
 active_dimensions_ = dimensions;
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeWorkspace::finish(const CoconutRecord& record, const CoconutPhysicalImage& physical, dataset::MaskDimensions dimensions,
 std::shared_ptr<const CoconutNativeLineage> lineage, const CoconutRejectedObject& rejected_object, CoconutNativeOutput* output) {
 if (active_record_ != &record || active_dimensions_.width != dimensions.width || active_dimensions_.height != dimensions.height || !lineage || lineage->component.source != physical.source)
  throw std::logic_error("COCONut: native image has no matching production workspace");
 const auto width = dimensions.width, height = dimensions.height;
 auto product = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(std::move(lineage)));
 scratch().boxes.clear();
 scratch().runs.clear();
 product->segment_begin_ = record.first_segment_ordinal;
 const auto& component = product->lineage().component;
 CoconutRecoveryImage recovery{physical.image_id, 0, {}};
 if (recovery_)
  recovery_->apply(physical.source, record, static_cast<unsigned>(width), static_cast<unsigned>(height), std::span(scratch().support).first(record.segments.size()), recovery, cancellation_);
 std::size_t output_runs = 0;
 for (const auto& value : std::span(scratch().support).first(record.segments.size())) output_runs = common::math::checked_add(output_runs, value.runs.size(), "COCONut native output overflow");
 scratch().boxes.reserve(record.segments.size());
 scratch().runs.reserve(output_runs);
 NormalizedImage image{physical.image_id, scratch().boxes.size(), 0, static_cast<unsigned>(width), static_cast<unsigned>(height), physical.shard, 0};
 for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
  if (ordinal > UINT64_MAX - record.first_segment_ordinal) invalid("source ordinal overflow");
  const auto& segment = record.segments[ordinal];
  auto& support = scratch().support[ordinal];
  ++product->rejected_.raw_records;
  if (!segment.isthing) continue;
  try {
   if (!segment.valid || !support.valid) invalid("malformed or ambiguous required segment");
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
   box.mask_rle_offset = scratch().runs.size();
   box.mask_rle_pairs = static_cast<std::uint32_t>(support.runs.size());
   box.class_id = static_cast<std::uint8_t>(categories[segment.category_id]);
   box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory | (segment.crowd ? kAnnotationCrowd : 0U) | (segment.ignore ? kAnnotationIgnore : 0U);
   box.original_area = support.recovered ? support.recovered->original_area : support.carved ? static_cast<double>(support.area) : segment.area.value_or(static_cast<double>(support.area));
   box.annotation_id = segment.id;
   box.source_category_id = segment.category_id;
   box.source_ordinal = record.first_segment_ordinal + ordinal;
   scratch().runs.insert(scratch().runs.end(), support.runs.begin(), support.runs.end());
   scratch().boxes.push_back(box);
   ++image.box_count;
  } catch (const CoconutInputError& error) {
   ++product->rejected_.malformed_records;
   if (rejected_object) rejected_object(physical, record, segment, error.what(), component.recovery_policy);
  }
 }
 if (unlisted_segment_) {
  ++product->rejected_.malformed_records;
  if (rejected_object)
   rejected_object(physical, record, {.id = unlisted_segment_, .isthing = true, .valid = false}, "panoptic pixels have unlisted segment IDs; first unlisted ID reported", component.recovery_policy);
 }
 product->required_labels_valid_ = image.box_count != 0 || (!unlisted_segment_ && std::ranges::none_of(record.segments, [](const auto& segment) { return segment.isthing || !segment.valid; }));
 product->image_ = image;
 product->inventory_ = {physical, record.image_id, record.source_ordinal};
 product->recovery_ = std::move(recovery);
 active_record_ = nullptr;
 product->input_identity_ = coconut_image_input_identity(product->lineage().component.annotation_input_identity, physical, product->lineage().component.original_annotation_identity);
 materialize(product, output);
 return product;
}
std::shared_ptr<const CoconutNativeImage> CoconutNativeWorkspace::reuse(const CoconutRecord& record, const CoconutPhysicalImage& physical, const NormalizedAnnotationReadView& source,
 std::size_t position, const CoconutInventoryImage& inventory, const CoconutRecoveryImage* recovered, std::shared_ptr<const CoconutNativeLineage> lineage, std::string_view dependency,
 bool required_labels_valid, CoconutNativeOutput* output) {
 if (!lineage || lineage->component.source != physical.source || inventory.physical != physical) throw std::logic_error("COCONut: reused native image has no matching physical input");
 auto product = std::shared_ptr<CoconutNativeImage>(new CoconutNativeImage(std::move(lineage)));
 scratch().boxes.clear();
 scratch().runs.clear();
 product->segment_begin_ = record.first_segment_ordinal;
 auto image = source.image(position);
 image.first_box = scratch().boxes.size();
 scratch().segments.assign(std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2)), {});
 std::uint64_t things = 0;
 for (std::size_t i = 0; i < record.segments.size(); ++i) {
  const auto id = record.segments[i].id;
  things += record.segments[i].isthing;
  auto& entry = segment_entry(id);
  if (!id || entry.id) throw std::logic_error("COCONut: duplicate/invalid reused segment ID");
  entry = {id, i};
 }
 const auto ordinal = [&](std::uint64_t annotation) {
  const auto& found = segment_entry(mmltk::common::math::checked_cast<std::uint32_t>(annotation, "reused segment ID overflow"));
  if (!found.id) throw std::logic_error("COCONut: reused image contains an undeclared segment");
  return mmltk::common::math::checked_add(record.first_segment_ordinal, std::uint64_t{found.index}, "reused source ordinal overflow");
 };
 const auto& index = source.storage();
 const auto& original = source.image(position);
 std::size_t run_count = 0;
 for (const auto& box : index.boxes.subspan(original.first_box, original.box_count))
  run_count = common::math::checked_add(run_count, std::size_t{box.mask_rle_pairs}, "COCONut reused run count overflow");
 scratch().boxes.reserve(original.box_count);
 scratch().runs.reserve(run_count);
 for (auto box : index.boxes.subspan(original.first_box, original.box_count)) {
  const auto runs = index.mask_rle_pairs.subspan(box.mask_rle_offset, box.mask_rle_pairs);
  box.source_ordinal = ordinal(box.annotation_id);
  box.flags &= ~kAnnotationOrdinalIncomplete;  // The reparsed local row is authoritative; final source prefixes settle independently.
  box.mask_rle_offset = scratch().runs.size();
  scratch().runs.insert(scratch().runs.end(), runs.begin(), runs.end());
  scratch().boxes.push_back(box);
 }
 product->required_labels_valid_ = required_labels_valid;
 product->image_ = image;
 product->inventory_ = inventory;
 product->rejected_.raw_records += record.segments.size();
 product->rejected_.degenerate_boxes += static_cast<std::uint64_t>(things) - image.box_count;
 if (recovered) {
  auto recovery = *recovered;
  for (auto* objects : {&recovery.objects, &recovery.omissions})
   for (auto& object : *objects) object.source_ordinal = ordinal(object.annotation_id);
  product->recovery_ = std::move(recovery);
 }
 product->input_identity_ = dependency;
 materialize(product, output);
 return product;
}
}  // namespace mmltk::backend::data::benchmark_internal
