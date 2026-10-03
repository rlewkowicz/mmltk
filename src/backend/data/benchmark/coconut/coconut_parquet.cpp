#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet_pool.h"
#include "src/common/types/utf8.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include "src/common/math/checked_arithmetic.h"
#include <arrow/api.h>
#include "src/common/io/file_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/common/system/numa_memory.h"
#include <bit>
#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
#include <map>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <tuple>
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileExecution;
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
namespace {
class ParquetFormatError final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
void check(const arrow::Status& status) {
 if (status.IsOutOfMemory()) throw std::bad_alloc();
 if (!status.ok()) throw ParquetFormatError("COCONut Parquet: " + status.ToString());
}
template <class T>
T take(arrow::Result<T> result) {
 check(result.status());
 return std::move(result).ValueOrDie();
}
[[noreturn]] void malformed(std::string_view field) { throw ParquetFormatError("COCONut Parquet invalid required field: " + std::string(field)); }
template <class T>
std::shared_ptr<const T> as(std::shared_ptr<arrow::Array> array, arrow::Type::type type, std::string_view field) {
 if (!array || array->type_id() != type) malformed(field);
 return std::static_pointer_cast<const T>(std::move(array));
}
void present(const arrow::Array& array, std::int64_t row, std::string_view name) {
 if (row < 0 || row >= array.length() || array.IsNull(row)) malformed(name);
}
std::shared_ptr<const arrow::StructArray> structure(std::shared_ptr<arrow::Array> array, std::string_view name) { return as<arrow::StructArray>(std::move(array), arrow::Type::STRUCT, name); }
namespace reflection = mmltk::frameworks::reflection;
template <class T>
struct ArrowColumn {
 using type = arrow::StructArray;
 static constexpr auto id = arrow::Type::STRUCT;
};
template <>
struct ArrowColumn<std::int64_t> {
 using type = arrow::Int64Array;
 static constexpr auto id = arrow::Type::INT64;
};
template <>
struct ArrowColumn<std::string> {
 using type = arrow::StringArray;
 static constexpr auto id = arrow::Type::STRING;
};
template <>
struct ArrowColumn<std::vector<std::uint8_t>> {
 using type = arrow::BinaryArray;
 static constexpr auto id = arrow::Type::BINARY;
};
template <>
struct ArrowColumn<std::vector<CoconutParquetSegment>> {
 using type = arrow::ListArray;
 static constexpr auto id = arrow::Type::LIST;
};
template <>
struct ArrowColumn<std::variant<std::int64_t, double>> {
 using type = arrow::Array;
};
// Resolved once while the existing reader owns its schema manifest. These
// indices are immutable shard facts, not a second external-field inventory.
struct ParquetProjection {
 std::vector<int> images, masks, png;
 int segment_id = -1;
};
void append_leaf_columns(const parquet::arrow::SchemaField& field, std::vector<int>& columns) {
 if (field.is_leaf())
  columns.push_back(field.column_index);
 else
  for (const auto& child : field.children) append_leaf_columns(child, columns);
}
template <class Shape>
void resolve_fields(std::span<const parquet::arrow::SchemaField>, ParquetProjection&, bool);
template <class T>
void resolve_value(const parquet::arrow::SchemaField& field, ParquetProjection& projection, bool image) {
 using Value = reflection::OptionalValueT<T>;
 if (!field.field) malformed("schema field");
 const auto& type = field.field->type();
 const auto& name = field.field->name();
 if constexpr (std::same_as<Value, std::variant<std::int64_t, double>>) {
  if (!type || (type->id() != arrow::Type::INT64 && type->id() != arrow::Type::DOUBLE)) malformed(name);
 } else if (!type || type->id() != ArrowColumn<Value>::id)
  malformed(name);
 if constexpr (std::same_as<Value, std::vector<CoconutParquetSegment>>) {
  // The Arrow manifest already resolves Parquet's standard and legacy list
  // wrappers. Their element names are format mechanics, never native fields.
  if (field.children.size() != 1) malformed(name);
  resolve_value<CoconutParquetSegment>(field.children.front(), projection, image);
 } else if constexpr (std::same_as<typename ArrowColumn<Value>::type, arrow::StructArray>) {
  resolve_fields<Value>(field.children, projection, image);
 } else {
  if (!field.is_leaf()) malformed(name);
  (image ? projection.images : projection.masks).push_back(field.column_index);
 }
}
template <class Shape>
void resolve_fields(std::span<const parquet::arrow::SchemaField> fields, ParquetProjection& projection, bool image) {
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 std::array<std::size_t, Fields::size()> matches{};
 Fields::Visit([&]<class Declaration, std::size_t Index>() {
  constexpr auto name = reflection::field_declarations<Shape>()[Index].member_name;
  matches[Index] = std::ranges::count_if(fields, [&](const auto& candidate) { return candidate.field && candidate.field->name() == name; });
  if constexpr (!reflection::OptionalValue<typename Declaration::member_type>::value)
   if (matches[Index] != 1) malformed(name);
 });
 for (const auto& field : fields) {
  if (!field.field) malformed("schema field");
  bool selected = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   constexpr auto name = reflection::field_declarations<Shape>()[Index].member_name;
   if (field.field->name() != name) return;
   selected = true;
   if constexpr (reflection::OptionalValue<typename Declaration::member_type>::value) {
    // Arrow's existing GetFieldByName binding treats an ambiguous optional
    // name as absent. Keep decoding those columns for structural validation.
    if (matches[Index] != 1) {
     append_leaf_columns(field, image ? projection.images : projection.masks);
     return;
    }
   }
   bool member_image = image;
   if constexpr (std::same_as<Shape, CoconutParquetRow>) member_image = Index == reflection::member_index<&CoconutParquetRow::image_info>(reflection::field_declarations<Shape>());
   resolve_value<typename Declaration::member_type>(field, projection, member_image);
   if constexpr (std::same_as<Shape, CoconutParquetRow>)
    if constexpr (Index == reflection::member_index<&CoconutParquetRow::mask>(reflection::field_declarations<Shape>())) append_leaf_columns(field, projection.png);
   if constexpr (std::same_as<Shape, CoconutParquetSegment>) {
    if constexpr (Index == reflection::member_index<&CoconutParquetSegment::id>(reflection::field_declarations<Shape>())) projection.segment_id = field.column_index;
   }
  });
  // Preserve the previous selected-root projection's structural admission of
  // extra nested columns. Unknown top-level columns remain unselected.
  if constexpr (!std::same_as<Shape, CoconutParquetRow>)
   if (!selected) append_leaf_columns(field, image ? projection.images : projection.masks);
 }
}
ParquetProjection resolve_projection(const parquet::arrow::SchemaManifest& manifest) {
 ParquetProjection result;
 resolve_fields<CoconutParquetRow>(manifest.schema_fields, result, false);
 if (result.images.empty() || result.masks.empty() || result.segment_id < 0 || result.segment_id >= manifest.descr->num_columns()) malformed("required nested columns");
 return result;
}
template <auto Member>
auto top_level_column(const arrow::RecordBatch& batch) {
 using Value = std::remove_cvref_t<decltype(std::declval<CoconutParquetRow>().*Member)>;
 constexpr auto name = reflection::materialized_member_name<Member>();
 return as<typename ArrowColumn<Value>::type>(batch.GetColumnByName(std::string(name)), ArrowColumn<Value>::id, name);
}
template <class Fields>
struct BoundColumnTuple;
template <class Bases, class... Declaration>
struct BoundColumnTuple<reflection::MaterializedFieldPolicyProduct<Bases, Declaration...>> {
 using type = std::tuple<std::shared_ptr<const typename ArrowColumn<reflection::OptionalValueT<typename Declaration::member_type>>::type>...>;
};
template <class Shape>
class BoundColumns {
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 typename BoundColumnTuple<Fields>::type arrays_;

public:
 explicit BoundColumns(const arrow::StructArray& value) {
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   constexpr const auto name = reflection::field_declarations<Shape>()[Index].member_name;
   auto array = value.GetFieldByName(std::string(name));
   using Value = reflection::OptionalValueT<typename Declaration::member_type>;
   if (!array) {
    if constexpr (!reflection::OptionalValue<typename Declaration::member_type>::value) malformed(name);
   } else {
    if constexpr (std::same_as<Value, std::variant<std::int64_t, double>>) {
     if (array->type_id() != arrow::Type::INT64 && array->type_id() != arrow::Type::DOUBLE) malformed(name);
    } else if (array->type_id() != ArrowColumn<Value>::id)
     malformed(name);
   }
   std::get<Index>(arrays_) = std::static_pointer_cast<const typename ArrowColumn<Value>::type>(std::move(array));
  });
 }
 template <auto Member>
 const auto& get() const {
  constexpr auto index = reflection::member_index<Member>(reflection::field_declarations<Shape>());
  static_assert(index < reflection::field_declarations<Shape>().size());
  return std::get<index>(arrays_);
 }
};
std::uint64_t integer(const std::shared_ptr<const arrow::Int64Array>& array, std::int64_t row, std::string_view name) {
 if (!array) malformed(name);
 present(*array, row, name);
 const auto value = array->Value(row);
 if (value < 0) malformed(name);
 return static_cast<std::uint64_t>(value);
}
template <class Array>
std::string_view binary_value(const Array& array, std::int64_t row, std::string_view name) {
 present(array, row, name);
 const auto first = array.value_offset(row), end = array.value_offset(row + 1);
 const auto& data = array.value_data();
 if (first < 0 || end < first || (!data && end) || (data && end > data->size())) malformed(name);
 if (first == end) return {};
 return {reinterpret_cast<const char*>(data->data() + first), static_cast<std::size_t>(end - first)};
}
std::string_view text(const std::shared_ptr<const arrow::StringArray>& array, std::int64_t row, std::string_view name) {
 if (!array) malformed(name);
 const auto value = binary_value(*array, row, name);
 if (value.size() > 4096 || !mmltk::common::types::valid_utf8(value)) malformed(name);
 return value;
}
bool boolean(const std::shared_ptr<const arrow::Int64Array>& array, std::int64_t row, std::string_view name) {
 if (!array) return false;
 const auto value = integer(array, row, name);
 if (value > 1) malformed(name);
 return value != 0;
}
void read_image_record(const BoundColumns<CoconutParquetImage>& images, std::int64_t row, const CoconutImportLimits& limits, std::uint64_t ordinal, CoconutRecord& record) {
 record.image_id = integer(images.get<&CoconutParquetImage::id>(), row, "id");
 record.file_name = text(images.get<&CoconutParquetImage::file_name>(), row, "file_name");
 record.namespace_hint.reset();
 const auto url = text(images.get<&CoconutParquetImage::coco_url>(), row, "coco_url");
 for (const auto& [component, source] : std::array{
       std::pair{std::string_view{"/train2017/"}, CoconutImageNamespace::CocoTrain}, std::pair{std::string_view{"/unlabeled2017/"}, CoconutImageNamespace::CocoUnlabeled},
       std::pair{std::string_view{"/val2017/"}, CoconutImageNamespace::CocoValidation}
      })
  if (url.find(component) != std::string_view::npos) {
   record.namespace_hint = source;
   break;
  }
 (void)text(images.get<&CoconutParquetImage::date_captured>(), row, "date_captured");
 (void)integer(images.get<&CoconutParquetImage::license>(), row, "license");
 const auto width = integer(images.get<&CoconutParquetImage::width>(), row, "width"), height = integer(images.get<&CoconutParquetImage::height>(), row, "height");
 if (width == 0 || height == 0 || width > limits.max_dimension || height > limits.max_dimension || width > limits.max_pixels / height || width * height > UINT32_MAX)
  malformed("dimensions exceed admission");
 record.width = static_cast<std::uint32_t>(width);
 record.height = static_cast<std::uint32_t>(height);
 record.source_ordinal = ordinal;
}
void read_batch(const arrow::RecordBatch& batch, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation,
 const std::function<void(const CoconutRecord&, std::span<const std::uint8_t>)>& consumer, std::uint64_t& row_ordinal, std::uint64_t& segment_ordinal, bool metadata_only,
 CoconutAnnotationRecords& retained, bool annotation_projection) {
 check(batch.Validate());  // Includes nested offsets and value-buffer bounds.
 if (metadata_only) {
  const auto images = top_level_column<&CoconutParquetRow::image_info>(batch);
  const BoundColumns<CoconutParquetImage> image_fields(*images);
  for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
   throw_if_benchmark_cancelled(cancellation);
   if (row_ordinal >= retained.size()) malformed("retained image row join");
   if (!retained.row(row_ordinal).metadata_ready()) {
    CoconutRecord record;
    present(*images, row, "image_info");
    read_image_record(image_fields, row, limits, row_ordinal, record);
    retained.admit_metadata(row_ordinal, std::move(record));
   }
   if (row_ordinal == UINT64_MAX) malformed("ordinal overflow");
   consumer(retained.record(row_ordinal), {});
   ++row_ordinal;
  }
  return;
 }
 const auto masks = top_level_column<&CoconutParquetRow::mask>(batch);
 const auto annotations = annotation_projection ? top_level_column<&CoconutParquetRow::segments_info>(batch) : nullptr;
 const BoundColumns<CoconutParquetMask> mask_fields(*masks);
 const auto annotation_fields = annotations ? std::make_unique<BoundColumns<CoconutParquetAnnotation>>(*annotations) : nullptr;
 const auto bytes = mask_fields.get<&CoconutParquetMask::bytes>();
 const auto mask_paths = mask_fields.get<&CoconutParquetMask::path>();  // HF path is nullable; embedded bytes are authoritative.
 const auto lists = annotation_fields ? annotation_fields->get<&CoconutParquetAnnotation::segments_info>() : nullptr;
 const auto segments = lists ? structure(lists->values(), "segment") : nullptr;
 const auto segment_fields = segments ? std::make_unique<BoundColumns<CoconutParquetSegment>>(*segments) : nullptr;
 const auto areas = segment_fields ? segment_fields->get<&CoconutParquetSegment::area>() : nullptr;
 const auto integer_areas = areas && areas->type_id() == arrow::Type::INT64 ? static_cast<const arrow::Int64Array*>(areas.get()) : nullptr;
 const auto floating_areas = integer_areas ? nullptr : static_cast<const arrow::DoubleArray*>(areas.get());
 for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
  throw_if_benchmark_cancelled(cancellation);
  auto png = !masks->IsNull(row) && !bytes->IsNull(row) ? binary_value(*bytes, row, "mask.bytes") : std::string_view{};
  if (!mask_paths->IsNull(row) && !mmltk::common::types::valid_utf8(binary_value(*mask_paths, row, "mask.path"))) malformed("mask.path");
  if (png.size() > limits.max_png_bytes) png = {};
  if (row_ordinal >= retained.size() || !retained.row(row_ordinal).metadata_ready()) malformed("retained image row join");
  const auto& record = retained.record(row_ordinal);
  if (!retained.row(row_ordinal).complete()) {
   if (!annotations) malformed("missing annotation projection");
   present(*annotations, row, "segments_info");
   present(*lists, row, "segments list");
   const auto begin = lists->value_offset(row), end = lists->value_offset(row + 1);
   if (begin < 0 || end < begin || end > segments->length()) malformed("segment list offsets");
   const auto length = end - begin;
   if (static_cast<std::uint64_t>(length) > limits.max_segments) malformed("record exceeds PNG/segment admission");
   if (integer(annotation_fields->get<&CoconutParquetAnnotation::image_id>(), row, "image_id") != record.image_id) malformed("image_id join");
   const auto annotation_file = text(annotation_fields->get<&CoconutParquetAnnotation::file_name>(), row, "file_name");
   if (std::filesystem::path(record.file_name).stem() != std::filesystem::path(annotation_file).stem() || std::filesystem::path(annotation_file).extension() != ".png")
    malformed("mask/image filename join");
   std::vector<CoconutSegment> parsed;
   parsed.reserve(static_cast<std::size_t>(length));
   for (std::int64_t offset = 0; offset < length; ++offset) {
    const auto index = begin + offset;
    CoconutSegment segment;
    try {
     present(*segments, index, "segment");
     const auto id = integer(segment_fields->get<&CoconutParquetSegment::id>(), index, "id");
     if (id == 0 || id > 0xffffffU) malformed("RGB segment id");
     segment.id = static_cast<std::uint32_t>(id);
     segment.category_id = integer(segment_fields->get<&CoconutParquetSegment::category_id>(), index, "category_id");
     segment.isthing = boolean(segment_fields->get<&CoconutParquetSegment::isthing>(), index, "isthing");
     segment.crowd = boolean(segment_fields->get<&CoconutParquetSegment::iscrowd>(), index, "iscrowd");
     segment.ignore = boolean(segment_fields->get<&CoconutParquetSegment::ignore>(), index, "ignore");
     if (!areas->IsNull(index)) {
      segment.area = integer_areas ? static_cast<double>(integer_areas->Value(index)) : floating_areas->Value(index);
      if (!std::isfinite(*segment.area) || *segment.area < 0) malformed("area");
     }
    } catch (const ParquetFormatError&) {
     segment.valid = false;
     segment.isthing = true;
    }
    parsed.push_back(segment);
   }
   retained.admit_segments(row_ordinal, std::move(parsed), segment_ordinal);
  }
  if (record.first_segment_ordinal != segment_ordinal) malformed("retained local segment offset");
  if (row_ordinal == UINT64_MAX || record.segments.size() > UINT64_MAX - segment_ordinal) malformed("ordinal overflow");
  throw_if_benchmark_cancelled(cancellation);
  segment_ordinal += record.segments.size();
  consumer(record, {reinterpret_cast<const std::uint8_t*>(png.data()), png.size()});
  ++row_ordinal;
 }
}
}  // namespace
struct CoconutParquetPool::Window {
 CompileAllowance backing;
 std::unique_ptr<common::system::NumaMemory> standalone;
 std::span<std::byte> memory;
 std::map<std::size_t, std::size_t> free;
 std::size_t high_water = 0, allocations = 0;
};
CoconutParquetPool::CoconutParquetPool(CompileAllowance value, std::size_t standalone_bytes) { resume(std::move(value), standalone_bytes); }
void CoconutParquetPool::resume(CompileAllowance value, std::size_t standalone_bytes) {
 const std::lock_guard lock(mutex_);
 if (active_) return;
 auto window = std::make_shared<Window>();
 window->backing = std::move(value);
 if (window->backing)
  window->memory = window->backing.workspace();
 else {
  window->standalone = std::make_unique<common::system::NumaMemory>(common::system::NumaTopology::Capture().permitted_nodes.front(), common::system::NumaMemory::Lock::Pages);
  window->standalone->ensure_bytes(standalone_bytes);
  window->memory = {static_cast<std::byte*>(window->standalone->data()), standalone_bytes};
 }
 if (window->memory.size() < kParquetPoolBytes) throw std::logic_error("COCONut Parquet allocation requires its complete source window");
 window->free.emplace(0, window->memory.size());
 windows_.push_back(window);
 active_ = std::move(window);
}
void CoconutParquetPool::quiesce() {
 const std::lock_guard lock(mutex_);
 if (!active_) return;
 auto window = std::exchange(active_, {});
 if (window->backing) {
  // The used prefix describes immutable bytes. The parent suffix and this
  // prefix share one fixed physical bank lease until the last allocation;
  // quiescence never advertises that suffix as separately lendable capacity.
  auto backing = window->allocations ? window->backing.split_storage(window->high_water) : CompileAllowance{};
  window->backing.retire_workspace();
  window->backing = std::move(backing);
 }
 if (!window->allocations) std::erase(windows_, window);
}
arrow::Status CoconutParquetPool::allocate(std::int64_t size, std::int64_t alignment, std::uint8_t** out, std::int64_t replaced) {
 if (!active_) return arrow::Status::Invalid("quiesced COCONut Parquet source");
 if (size < 0 || alignment <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(alignment))) return arrow::Status::Invalid("invalid Parquet allocation");
 if (size > static_cast<std::int64_t>(kParquetPoolBytes) - (live_ - replaced)) return arrow::Status::OutOfMemory("COCONut Parquet payload exceeds 256 MiB");
 auto& window = *active_;
 for (auto i = window.free.begin(); i != window.free.end(); ++i) {
  const auto start = i->first, extent = i->second;
  const auto address = reinterpret_cast<std::uintptr_t>(window.memory.data() + start);
  const auto padding = (static_cast<std::uintptr_t>(alignment) - (address % alignment)) % alignment;
  const auto count = std::max<std::size_t>(1, static_cast<std::size_t>(size));
  if (padding > extent || count > extent - padding) continue;
  const auto offset = start + padding;
  window.free.erase(i);
  if (padding) window.free.emplace(start, padding);
  if (extent - padding > count) window.free.emplace(offset + count, extent - padding - count);
  *out = reinterpret_cast<std::uint8_t*>(window.memory.data() + offset);
  ++window.allocations;
  window.high_water = std::max(window.high_water, offset + count);
  live_ += size;
  total_ += size;
  ++allocations_;
  peak_ = std::max(peak_, live_ - replaced);
  return arrow::Status::OK();
 }
 // Logical headroom is available, but live addresses cannot move. Record an
 // envelope that can hold this failed physical request after the current
 // window. The reader alone decides when every borrower has retired and the
 // allocation sequence can be replayed in an exclusive larger arena.
 const auto request = common::math::checked_add(std::max<std::size_t>(1, static_cast<std::size_t>(size)), static_cast<std::size_t>(alignment) - 1, "Parquet allocation envelope overflow");
 fragmented_extent_ = common::math::checked_add(window.memory.size(), request, "Parquet allocation envelope overflow");
 return arrow::Status::CapacityError("COCONut Parquet source window has no contiguous fit");
}
void CoconutParquetPool::free(std::uint8_t* buffer, std::int64_t size) {
 if (!buffer) return;
 const auto address = reinterpret_cast<std::uintptr_t>(buffer);
 const auto found = std::ranges::find_if(windows_, [&](const auto& window) {
  const auto begin = reinterpret_cast<std::uintptr_t>(window->memory.data());
  return address >= begin && address - begin < window->memory.size();
 });
 if (found == windows_.end()) std::terminate();
 auto& window = **found;
 auto offset = address - reinterpret_cast<std::uintptr_t>(window.memory.data());
 auto count = std::max<std::size_t>(1, static_cast<std::size_t>(size));
 auto next = window.free.lower_bound(offset);
 if (next != window.free.end() && offset + count == next->first) {
  count += next->second;
  next = window.free.erase(next);
 }
 if (next != window.free.begin()) {
  auto previous = std::prev(next);
  if (previous->first + previous->second == offset) {
   offset = previous->first;
   count += previous->second;
   window.free.erase(previous);
  }
 }
 window.free.emplace(offset, count);
 live_ -= size;
 if (!--window.allocations && found->get() != active_.get()) windows_.erase(found);
}
arrow::Status CoconutParquetPool::Allocate(std::int64_t size, std::int64_t alignment, std::uint8_t** out) {
 const std::lock_guard lock(mutex_);
 fragmented_extent_ = 0;
 return allocate(size, alignment, out);
}
arrow::Status CoconutParquetPool::Reallocate(std::int64_t old_size, std::int64_t size, std::int64_t alignment, std::uint8_t** out) {
 const std::lock_guard lock(mutex_);
 fragmented_extent_ = 0;
 if (old_size < 0 || size < 0 || alignment <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(alignment))) return arrow::Status::Invalid("invalid Parquet reallocation");
 if (!active_) return arrow::Status::Invalid("quiesced COCONut Parquet source");
 if (size > static_cast<std::int64_t>(kParquetPoolBytes) - (live_ - old_size)) return arrow::Status::OutOfMemory("COCONut Parquet payload exceeds 256 MiB");
 const auto address = reinterpret_cast<std::uintptr_t>(*out);
 const auto begin = reinterpret_cast<std::uintptr_t>(active_->memory.data());
 if (*out && address >= begin && address - begin < active_->memory.size() && address % alignment == 0) {
  auto& window = *active_;
  const auto offset = address - begin;
  const auto old_count = std::max<std::size_t>(1, static_cast<std::size_t>(old_size));
  const auto count = std::max<std::size_t>(1, static_cast<std::size_t>(size));
  auto next = window.free.lower_bound(offset + old_count);
  const auto tail = next != window.free.end() && next->first == offset + old_count ? next->second : 0;
  if (count <= old_count + tail) {
   if (tail) window.free.erase(next);
   if (old_count + tail > count) window.free.emplace(offset + count, old_count + tail - count);
   window.high_water = std::max(window.high_water, offset + count);
   live_ += size - old_size;
   total_ += std::max<std::int64_t>(0, size - old_size);
   peak_ = std::max(peak_, live_);
   return arrow::Status::OK();
  }
 }
 // Reallocation admits the final payload size, as Arrow's capped pool does.
 // The old immutable window remains physically charged through this copy;
 // future allocation still occurs only in the fully admitted active window.
 std::uint8_t* next = nullptr;
 auto status = allocate(size, alignment, &next, old_size);
 if (!status.ok()) return status;
 if (old_size && size) std::memcpy(next, *out, static_cast<std::size_t>(std::min(old_size, size)));
 free(*out, old_size);
 *out = next;
 return status;
}
void CoconutParquetPool::Free(std::uint8_t* buffer, std::int64_t size, std::int64_t) {
 const std::lock_guard lock(mutex_);
 free(buffer, size);
}
std::uint64_t CoconutParquetPool::retained_capacity() const {
 const std::lock_guard lock(mutex_);
 std::uint64_t bytes = 0;
 for (const auto& window : windows_) bytes = common::math::checked_add(bytes, std::uint64_t{window->memory.size()}, "Parquet retained capacity overflow");
 return bytes;
}
std::int64_t CoconutParquetPool::bytes_allocated() const {
 const std::lock_guard lock(mutex_);
 return live_;
}
std::int64_t CoconutParquetPool::max_memory() const {
 const std::lock_guard lock(mutex_);
 return peak_;
}
std::int64_t CoconutParquetPool::total_bytes_allocated() const {
 const std::lock_guard lock(mutex_);
 return total_;
}
std::int64_t CoconutParquetPool::num_allocations() const {
 const std::lock_guard lock(mutex_);
 return allocations_;
}
std::string CoconutParquetPool::backend_name() const { return "mmltk-pinned-source"; }
void CoconutParquetPool::begin_call() {
 const std::lock_guard lock(mutex_);
 fragmented_extent_ = 0;
}
std::size_t CoconutParquetPool::fragmented_extent() const {
 const std::lock_guard lock(mutex_);
 return fragmented_extent_;
}
namespace {
struct ParquetContiguousFit final : std::exception {
 std::size_t envelope;
 explicit ParquetContiguousFit(std::size_t value) : envelope(value) {}
 const char* what() const noexcept override { return "COCONut Parquet source window has no contiguous fit"; }
};
std::size_t replay_envelope(std::size_t observed) {
 // Leave room for the bank's page rounding as well as Arrow's signed sizes.
 const auto page = common::system::host_page_size();
 const auto rounded = common::math::checked_add(observed, page - 1, "Parquet exceptional alignment overflow") / page * page;
 (void)common::math::checked_cast<std::int64_t>(rounded, "Parquet exceptional extent overflow");
 return rounded;
}
CompileResources parquet_window(std::size_t bytes = kParquetPoolBytes) {
 return {
  .bytes = bytes, .supplied_workspace = true, .workspace_class = detail::CompileWorkClass::Extraction, .opaque_bytes = 16U << 20, .workspace_window = detail::CompileWorkspaceWindow::SharedSource
 };
}
std::unique_ptr<parquet::arrow::FileReader> open_reader(const std::filesystem::path& path, CoconutParquetPool& pool, std::shared_ptr<parquet::FileMetaData> metadata = {}) {
 parquet::ReaderProperties input(&pool);
 input.enable_buffered_stream();
 input.set_buffer_size(128U * 1024U);
 input.set_thrift_string_size_limit(16U * 1024U * 1024U);
 input.set_thrift_container_size_limit(1000000);
 parquet::ArrowReaderProperties properties;
 properties.set_pre_buffer(false);
 properties.set_use_threads(false);
 properties.set_batch_size(8);
 parquet::arrow::FileReaderBuilder builder;
 check(builder.OpenFile(path.string(), false, input, std::move(metadata)));
 builder.memory_pool(&pool);
 builder.properties(properties);
 return take(builder.Build());
}
struct ParquetBatch {
 std::shared_ptr<CoconutParquetPool> pool;
 std::shared_ptr<arrow::RecordBatch> batch;
};
}  // namespace
class CoconutParquetMetadata final {
public:
 struct Shard {
  std::filesystem::path path;
  CompileAllowance descriptor;
  common::io::FileHandle file;
  std::shared_ptr<CoconutParquetPool> footer_pool;
  std::shared_ptr<parquet::FileMetaData> footer;
  ParquetProjection projection;
  std::size_t first_group = 0;
 };
 std::vector<Shard> shards;
};
void CoconutAnnotationRecords::rebase_parquet_segments() {
 if (prefixes_sealed_) return;
 std::optional<std::uint64_t> prefix = 0;
 for (auto& group : groups) {
  if (group.first_row > size() || group.rows > size() - group.first_row) malformed("retained group row extent");
  group.first_segment = prefix.value_or(0);
  for (auto& row : rows_ | std::views::drop(static_cast<std::size_t>(group.first_row)) | std::views::take(static_cast<std::size_t>(group.rows))) {
   if (!row.complete()) continue;
   row.segment_ordinal_complete_ = prefix.has_value();
   row.segment_ordinal_ = prefix ? mmltk::common::math::checked_add(row.record_.first_segment_ordinal, *prefix, "COCONut Parquet record ordinal overflow") : row.record_.first_segment_ordinal;
  }
  const auto rows = rows_ | std::views::drop(static_cast<std::size_t>(group.first_row)) | std::views::take(static_cast<std::size_t>(group.rows));
  if (std::ranges::any_of(rows, [](const auto& row) { return !row.complete(); }))
   prefix.reset();
  else if (prefix)
   *prefix = mmltk::common::math::checked_add(*prefix, group.segments, "COCONut Parquet segment prefix overflow");
 }
 prefixes_sealed_ = true;
}
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits, common::concurrency::CancellationObservation cancellation,
 const CoconutRecordConsumer& consumer, bool metadata_only, CompileExecution* execution, detail::FileWorkQueue* files, const std::function<void()>& settle_consumers, const CompileAllowance& parent,
 CoconutAnnotationRecords* retained, const std::function<bool(const CoconutRecord&)>& reusable_native, const std::function<bool(const CoconutRecord&, std::span<const std::uint8_t>)>& stage_input,
 const BenchmarkTraceSink& trace) {
 if (execution && !files) throw std::invalid_argument("COCONut Parquet execution requires its file service");
 auto producer = execution ? execution->open_class(detail::CompileWorkClass::Extraction) : CompileExecution::Producer{};
 CoconutAnnotationRecords local;
 if (!retained) retained = &local;
 if (!retained->staging) retained->staging = std::make_shared<CoconutStaging>(retained->staging_directory, retained->storage);
 const auto cpu = [&](CoconutParquetPool& pool, const CompileAllowance& allowance, const std::function<void()>& work) {
  pool.begin_call();
  try {
   if (execution)
    execution->run(CompileTaskKind::Projection, {.workspace_window = detail::CompileWorkspaceWindow::SharedSource}, [&](std::size_t) { work(); }, allowance);
   else
    work();
  } catch (...) {
   // Parquet may wrap Arrow's status in its own exception. Only this pool's
   // distinct failed fit authorizes replay; a logical cap, malformed value,
   // pin failure or cancellation has no such signal.
   if (execution)
    if (auto interrupted = execution->interruption()) std::rethrow_exception(interrupted);
   throw_if_benchmark_cancelled(cancellation);
   if (const auto envelope = pool.fragmented_extent()) throw ParquetContiguousFit(envelope);
   throw;
  }
 };
 if (!retained->parquet) {
  retained->groups.clear();
  auto catalog = std::make_shared<CoconutParquetMetadata>();
  std::uint64_t rows = 0;
  std::vector<CoconutRecordGroup> groups;
  for (const auto& path : shards) {
   throw_if_benchmark_cancelled(cancellation);
   auto descriptor = execution ? execution->reserve(CompileResources::handles(1), parent) : CompileAllowance{};
   auto file = common::io::FileHandle::open_readonly(path.string());
   auto reader_descriptor = execution ? execution->reserve(CompileResources::handles(1), parent) : CompileAllowance{};
   CompileAllowance allowance;
   std::shared_ptr<CoconutParquetPool> pool;
   std::shared_ptr<parquet::FileMetaData> footer;
   ParquetProjection projection;
   const auto first_group = groups.size();
   std::size_t window_bytes = kParquetPoolBytes;
   for (;;) {
    throw_if_benchmark_cancelled(cancellation);
    allowance = execution ? execution->reserve(parquet_window(window_bytes), parent) : CompileAllowance{};
    pool = std::make_shared<CoconutParquetPool>(allowance, window_bytes);
    try {
     cpu(*pool, allowance, [&] {
      auto reader = open_reader("/proc/self/fd/" + std::to_string(file.get()), *pool);
      projection = resolve_projection(reader->manifest());
      footer = reader->parquet_reader()->metadata();
     });
     break;
    } catch (const ParquetContiguousFit& fit) {
     footer.reset();
     pool->close();
     pool.reset();
     allowance = {};
     window_bytes = replay_envelope(fit.envelope);
    } catch (const ParquetFormatError& error) { throw CoconutInputError("COCONut Parquet " + path.string() + ": " + error.what()); }
   }
   try {
    cpu(*pool, allowance, [&] {
     for (int group = 0; group < footer->num_row_groups(); ++group) {
      const auto count = footer->RowGroup(group)->num_rows();
      if (count < 0) malformed("negative row group count");
      groups.push_back({rows, static_cast<std::uint64_t>(count), 0, 0});
      rows = common::math::checked_add(rows, static_cast<std::uint64_t>(count), "COCONut Parquet row prefix overflow");
     }
    });
   } catch (const ParquetFormatError& error) { throw CoconutInputError("COCONut Parquet " + path.string() + ": " + error.what()); }
   pool->close();
   catalog->shards.push_back({path, std::move(descriptor), std::move(file), std::move(pool), std::move(footer), std::move(projection), first_group});
  }
  retained->resize(common::math::checked_cast<std::size_t>(rows, "COCONut Parquet row count overflow"));
  retained->groups = std::move(groups);
  retained->parquet = std::move(catalog);
 }
 auto catalog = retained->parquet;
 if (catalog->shards.size() != shards.size()) malformed("retained shard generation mismatch");
 for (std::size_t i = 0; i < shards.size(); ++i)
  if (catalog->shards[i].path != shards[i]) malformed("retained shard path mismatch");
 // The calling controller advances bounded artifact cursors; worker count never
 // creates extra source threads/readers. Only a projected batch enters a lane.
 struct Reader {
  std::size_t shard = 0, group = 0, replay_bytes = 0;
  std::uint64_t row = 0, segments = 0, projected = 0;
  bool initialized = false, image_pass = true, annotations = true, finished = false;
  CompileAllowance allowance, reader_descriptor;
  std::shared_ptr<CoconutParquetPool> pool;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::unique_ptr<arrow::RecordBatchReader> batches;
  std::vector<CompileExecution::Completion> consumers;
  // Keep the bounded reader descriptor reservation while parked. Repeatedly
  // taking and returning it on a failed byte draw would notify our own retry
  // loop forever without any external resource change.
  void close() {
   batches.reset();
   reader.reset();
   if (pool) pool->close();
   pool.reset();
   allowance = {};
  }
  ~Reader() { close(); }
 };
 std::vector<Reader> readers(catalog->shards.size());
 for (std::size_t i = 0; i < readers.size(); ++i) readers[i].shard = i;
 Reader* replay = nullptr;
 std::size_t remaining = readers.size();
 const auto retire_projections = [&] {
  for (auto& state : readers) state.close();
 };
 while (remaining) {
  bool progressed = false;
  if (execution && execution->oversized_workspace_pending()) retire_projections();
  const auto generation = execution ? execution->admission_generation() : 0;
  for (auto& state : readers) {
   if (state.finished || (replay && replay != &state)) continue;
   // Admitted tasks already staged their output; settlement retires their
   // remaining input custody. Reap before waiting for another source window.
   if (!state.consumers.empty() && settle_consumers) settle_consumers();
   std::erase_if(state.consumers, [](const auto& completion) {
    if (!completion.ready()) return false;
    completion.wait();
    return true;
   });
   // One artifact can have several bounded batches in flight. An earlier
   // borrower never fences later groups when another source window fits.
   if (state.consumers.size() >= 64) continue;
   const auto& shard = catalog->shards[state.shard];
   if (state.group == static_cast<std::size_t>(shard.footer->num_row_groups())) {
    state.close();
    if (replay == &state) {
     replay = nullptr;
     state.replay_bytes = 0;
    }
    state.reader_descriptor = {};
    if (state.consumers.empty()) {
     state.finished = true;
     --remaining;
     progressed = true;
    }
    continue;
   }
   const auto group_index = shard.first_group + state.group;
   auto& group = retained->groups[group_index];
   const auto rows = retained->rows() | std::views::drop(group.first_row) | std::views::take(group.rows);
   try {
    if (!state.initialized) {
     std::uint64_t inspected = 0;
     const bool ready = std::ranges::all_of(rows, [&](const auto& row) {
      if (trace.accepts("benchmark.coconut.metadata_readiness")) ++inspected;
      return row.metadata_ready();
     });
     trace_benchmark_event(
      trace, "benchmark.coconut.metadata_readiness", [&] { return nlohmann::json{{"artifact", shard.path.string()}, {"group", state.group}, {"inspected_rows", inspected}, {"ready", ready}}; });
     state.row = group.first_row + (metadata_only && ready ? group.rows : 0);
     state.segments = 0;
     state.projected = 0;
     state.image_pass = metadata_only || !ready;
     state.initialized = true;
    }
    const auto publish_membership = [&] {
     for (const auto& row : rows) consumer(group_index, row.record(), CoconutAnnotationInput{.membership = true});
     state.image_pass = false;
     state.row = group.first_row;
     state.projected = 0;
    };
    if (state.image_pass && state.row == group.first_row + group.rows) {
     if (state.replay_bytes) {
      state.close();
      state.replay_bytes = 0;
      replay = nullptr;
     }
     publish_membership();
     if (metadata_only) {
      if (settle_consumers) settle_consumers();
      ++state.group;
      state.initialized = false;
     }
     progressed = true;
     continue;
    }
    if (!state.image_pass && state.projected == 0) {
     state.annotations = std::ranges::any_of(rows, [](const auto& row) { return !row.complete(); });
     if (!state.annotations && reusable_native && std::ranges::all_of(rows, [&](const auto& row) { return reusable_native(row.record()); })) {
      if (state.replay_bytes) {
       state.close();
       state.replay_bytes = 0;
       replay = nullptr;
      }
      for (const auto& row : rows) {
       if (row.record().first_segment_ordinal != state.segments) malformed("retained local segment offset");
       state.segments = common::math::checked_add(state.segments, std::uint64_t{row.record().segments.size()}, "COCONut Parquet segment count overflow");
       consumer(group_index, row.record(), {});
      }
      group.segments = state.segments;
      if (settle_consumers) settle_consumers();
      ++state.group;
      state.initialized = false;
      progressed = true;
      continue;
     }
    }
    if (!state.allowance) {
     if (execution) {
      if (!state.reader && !state.reader_descriptor) {
       auto descriptor = execution->try_reserve(CompileResources::handles(1), parent);
       if (!descriptor) {
        state.close();
        continue;
       }
       state.reader_descriptor = std::move(*descriptor);
      }
      // Replay owns no reader pages or semantic borrowers now. Reserve the
      // measured envelope; admission drains idle backing while waiting, and
      // only shapes exceeding node capacity require an exclusive handoff.
      auto next = state.replay_bytes ? std::optional<CompileAllowance>{execution->reserve(parquet_window(state.replay_bytes), parent)} : execution->try_reserve(parquet_window(), parent);
      if (!next) {
       // An idle projection can release its own retained pages without losing
       // its opened generation or parsed row/batch position.
       state.close();
       trace_benchmark_event(trace, "benchmark.coconut.projection_parked",
        [&] { return nlohmann::json{{"artifact", shard.path.string()}, {"group", state.group}, {"rows_projected", state.projected}, {"window_bytes", kParquetPoolBytes}}; });
       continue;
      }
      state.allowance = std::move(*next);
     }
     // The retained footer carries Arrow's allocator into lazy column
     // statistics. Reopen that same pool's allocation window so metadata and
     // projected batches share the current admitted source custody.
     if (!state.pool) state.pool = shard.footer_pool;
     state.pool->resume(state.allowance, state.replay_bytes ? state.replay_bytes : kParquetPoolBytes);
     if (!state.reader) cpu(*state.pool, state.allowance, [&] { state.reader = open_reader("/proc/self/fd/" + std::to_string(shard.file.get()), *state.pool, shard.footer); });
    }
    if (!state.batches) {
     const auto& columns = state.image_pass ? shard.projection.images : state.annotations ? shard.projection.masks : shard.projection.png;
     trace_benchmark_event(trace, "benchmark.coconut.projection_opened",
      [&] { return nlohmann::json{{"artifact", shard.path.string()}, {"group", state.group}, {"image_pass", state.image_pass}, {"replayed_rows", state.projected}}; });
     cpu(*state.pool, state.allowance, [&] {
      state.batches = take(state.reader->GetRecordBatchReader({static_cast<int>(state.group)}, columns));
      // Suspension retains the last complete batch boundary. Replaying its
      // physical prefix never re-admits rows or changes canonical ordinals.
      std::uint64_t skipped = 0;
      while (skipped < state.projected) {
       std::shared_ptr<arrow::RecordBatch> prior;
       check(state.batches->ReadNext(&prior));
       if (!prior) malformed("retained projection cursor");
       skipped += static_cast<std::uint64_t>(prior->num_rows());
      }
      if (skipped != state.projected) malformed("retained projection batch boundary");
     });
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    cpu(*state.pool, state.allowance, [&] { check(state.batches->ReadNext(&batch)); });
    if (!batch) {
     state.batches.reset();
     if (state.replay_bytes) {
      state.close();
      state.replay_bytes = 0;
      replay = nullptr;
     }
     if (state.row != group.first_row + group.rows) malformed("row group extent mismatch");
     if (state.image_pass) {
      publish_membership();
     } else {
      group.segments = state.segments;
      ++state.group;
      state.initialized = false;
     }
     if (metadata_only) {
      ++state.group;
      state.initialized = false;
     }
     if ((!state.initialized || !state.image_pass) && settle_consumers) settle_consumers();
     progressed = true;
     continue;
    }
    struct Ready {
     const CoconutRecord* record;
     std::span<const std::uint8_t> png;
     CoconutStaging::Range<std::uint8_t> staged;
    };
    std::vector<Ready> ready;
    std::exception_ptr parse_failure;
    try {
     cpu(*state.pool, state.allowance, [&] {
      read_batch(*batch, limits, cancellation, [&](const CoconutRecord& record, std::span<const std::uint8_t> png) { ready.push_back({&record, png, {}}); }, state.row, state.segments,
       state.image_pass, *retained, state.annotations);
     });
    } catch (const ParquetFormatError&) { parse_failure = std::current_exception(); }
    if (!state.image_pass)
     for (const auto& row : ready) retained->stage_segments(row.record->source_ordinal, execution, files, parent);
    state.projected += static_cast<std::uint64_t>(batch->num_rows());
    std::shared_ptr<ParquetBatch> chunk;
    // Borrowers share quiesced Arrow backing when protected completion storage
    // can finish them. Otherwise transfer this bounded batch before a descendant
    // can wait on an ordinary or exceptional arena occupied by its own input.
    const bool staged = state.replay_bytes || (!state.image_pass && stage_input && std::ranges::any_of(ready, [&](const auto& row) { return stage_input(*row.record, row.png); }));
    if (staged) {
     for (auto& row : ready) row.staged = retained->staging->copy<std::uint8_t>(row.png, execution, files, parent);
     // Arrow buffers must be released while their memory pool is still alive.
     batch.reset();
     if (state.replay_bytes) {
      state.close();
      state.replay_bytes = 0;
      replay = nullptr;
     }
    } else if (state.image_pass) {
     // Metadata has been copied into canonical rows and has no Arrow borrower.
     // Keep the reader and its allocation window at their forward position.
     batch.reset();
    } else {
     state.pool->quiesce();
     state.allowance = {};
     chunk = std::make_shared<ParquetBatch>(state.pool, std::move(batch));
    }
    trace_benchmark_event(trace, "benchmark.coconut.projected_batch", [&] {
     const auto snapshot = execution ? execution->snapshot() : detail::CompileExecutionSnapshot{};
     return nlohmann::json{
      {"artifact", shard.path.string()}, {"group", state.group}, {"rows_projected", state.projected}, {"rows", ready.size()}, {"batch_bytes", staged ? 0 : state.pool->bytes_allocated()},
      {"retained_bank_capacity", state.pool ? state.pool->retained_capacity() : 0}, {"future_window_bytes", state.allowance.bytes()}, {"staged_transfer", staged},
      {"normal_locked", snapshot.normal_locked}, {"leased", snapshot.leased}
     };
    });
    if (!state.image_pass) {
     group.segments = state.segments;
     for (auto& row : ready)
      consumer(group_index, *row.record,
       CoconutAnnotationInput{
        .png = staged ? row.staged.values : row.png,
        .backing = staged ? row.staged.owner : chunk,
        .live_bytes = staged ? row.staged.values.size() : static_cast<std::uint64_t>(state.pool->bytes_allocated()),
        // A normal staged batch leaves its cursor open. Its controller releases
        // idle readers only if a descendant actually cannot acquire scratch.
        .retire_projection = staged ? std::function<void()>(retire_projections) : std::function<void()>{},
        .admitted = [&](CompileExecution::Completion completion) { state.consumers.push_back(std::move(completion)); }
       });
    }
    if (parse_failure) std::rethrow_exception(parse_failure);
    // The footer already fixes the exact group extent. Its final live batch
    // must not require another allocation window merely to read an EOF that
    // is already known, especially when the caller retains that batch.
    if (state.row == group.first_row + group.rows) {
     state.batches.reset();
     if (state.image_pass) {
      publish_membership();
      if (metadata_only) {
       ++state.group;
       state.initialized = false;
      }
     } else {
      ++state.group;
      state.initialized = false;
     }
     if (settle_consumers) settle_consumers();
    }
    progressed = true;
   } catch (const ParquetContiguousFit& fit) {
    // Close every idle reader before waiting: each may retain its own Arrow
    // pages. Batch borrowers drain at complete semantic boundaries, with
    // settlement releasing their input custody before the larger admission.
    retire_projections();
    for (auto& other : readers) {
     for (const auto& completion : other.consumers) {
      completion.wait();
      if (settle_consumers) settle_consumers();
     }
     other.consumers.clear();
    }
    if (settle_consumers) settle_consumers();
    state.replay_bytes = replay_envelope(fit.envelope);
    replay = &state;
    trace_benchmark_event(trace, "benchmark.coconut.projection_replay", [&] {
     const auto snapshot = execution ? execution->snapshot() : detail::CompileExecutionSnapshot{};
     return nlohmann::json{
      {"artifact", shard.path.string()}, {"group", state.group}, {"rows_projected", state.projected}, {"window_bytes", state.replay_bytes}, {"normal_locked", snapshot.normal_locked},
      {"leased", snapshot.leased}, {"exceptional", snapshot.exceptional}
     };
    });
    progressed = true;
    break;
   } catch (const ParquetFormatError& error) { throw CoconutInputError("COCONut Parquet " + shard.path.string() + ": " + error.what()); }
  }
  throw_if_benchmark_cancelled(cancellation);
  if (!progressed && execution) execution->wait_for_admission_change(generation);
 }
 if (!metadata_only) retained->rebase_parquet_segments();
}
}  // namespace mmltk::backend::data::benchmark_internal
