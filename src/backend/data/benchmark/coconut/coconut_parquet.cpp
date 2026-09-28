#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/common/types/utf8.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/common/math/checked_arithmetic.h"
#include <arrow/api.h>
#include "src/common/concurrency/parallel_range.h"
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <tuple>
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
namespace {
class ParquetFormatError final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
void check(const arrow::Status& status) {
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
template<class T> struct ArrowColumn { using type = arrow::StructArray; static constexpr auto id = arrow::Type::STRUCT; };
template<> struct ArrowColumn<std::int64_t> { using type = arrow::Int64Array; static constexpr auto id = arrow::Type::INT64; };
template<> struct ArrowColumn<std::string> { using type = arrow::StringArray; static constexpr auto id = arrow::Type::STRING; };
template<> struct ArrowColumn<std::vector<std::uint8_t>> { using type = arrow::BinaryArray; static constexpr auto id = arrow::Type::BINARY; };
template<> struct ArrowColumn<std::vector<CoconutParquetSegment>> { using type = arrow::ListArray; static constexpr auto id = arrow::Type::LIST; };
template<> struct ArrowColumn<std::variant<std::int64_t, double>> { using type = arrow::Array; };
// Resolved once while the existing reader owns its schema manifest. These
// indices are immutable shard facts, not a second external-field inventory.
struct ParquetProjection {
 std::vector<int> images, masks;
 int segment_id = -1;
};
void append_leaf_columns(const parquet::arrow::SchemaField& field, std::vector<int>& columns) {
 if (field.is_leaf()) columns.push_back(field.column_index);
 else for (const auto& child : field.children) append_leaf_columns(child, columns);
}
template<class Shape>
void resolve_fields(std::span<const parquet::arrow::SchemaField>, ParquetProjection&, bool);
template<class T>
void resolve_value(const parquet::arrow::SchemaField& field, ParquetProjection& projection, bool image) {
 using Value = reflection::OptionalValueT<T>;
 if (!field.field) malformed("schema field");
 const auto& type = field.field->type();
 const auto& name = field.field->name();
 if constexpr (std::same_as<Value, std::variant<std::int64_t, double>>) {
  if (!type || (type->id() != arrow::Type::INT64 && type->id() != arrow::Type::DOUBLE)) malformed(name);
 } else if (!type || type->id() != ArrowColumn<Value>::id) malformed(name);
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
template<class Shape>
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
   if constexpr (std::same_as<Shape, CoconutParquetRow>)
    member_image = Index == reflection::member_index<&CoconutParquetRow::image_info>(reflection::field_declarations<Shape>());
   resolve_value<typename Declaration::member_type>(field, projection, member_image);
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
template<auto Member> auto top_level_column(const arrow::RecordBatch& batch) {
 using Value = std::remove_cvref_t<decltype(std::declval<CoconutParquetRow>().*Member)>;
 constexpr auto name = reflection::materialized_member_name<Member>();
 return as<typename ArrowColumn<Value>::type>(batch.GetColumnByName(std::string(name)), ArrowColumn<Value>::id, name);
}
template<class Fields> struct BoundColumnTuple;
template<class Bases, class... Declaration>
struct BoundColumnTuple<reflection::MaterializedFieldPolicyProduct<Bases, Declaration...>> {
 using type = std::tuple<std::shared_ptr<const typename ArrowColumn<reflection::OptionalValueT<typename Declaration::member_type>>::type>...>;
};
template<class Shape> class BoundColumns {
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 typename BoundColumnTuple<Fields>::type arrays_;
public:
 explicit BoundColumns(const arrow::StructArray& value) {
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   constexpr const auto name = reflection::field_declarations<Shape>()[Index].member_name;
   auto array = value.GetFieldByName(std::string(name));
   using Value = reflection::OptionalValueT<typename Declaration::member_type>;
   if (!array) { if constexpr (!reflection::OptionalValue<typename Declaration::member_type>::value) malformed(name); }
   else {
    if constexpr (std::same_as<Value, std::variant<std::int64_t, double>>) {
     if (array->type_id() != arrow::Type::INT64 && array->type_id() != arrow::Type::DOUBLE) malformed(name);
    } else if (array->type_id() != ArrowColumn<Value>::id) malformed(name);
   }
   std::get<Index>(arrays_) = std::static_pointer_cast<const typename ArrowColumn<Value>::type>(std::move(array));
  });
 }
 template<auto Member> const auto& get() const {
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
template<class Array> std::string_view binary_value(const Array& array, std::int64_t row, std::string_view name) {
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
      std::pair{std::string_view{"/train2017/"}, CoconutImageNamespace::CocoTrain},
      std::pair{std::string_view{"/unlabeled2017/"}, CoconutImageNamespace::CocoUnlabeled},
      std::pair{std::string_view{"/val2017/"}, CoconutImageNamespace::CocoValidation}})
  if (url.find(component) != std::string_view::npos) { record.namespace_hint = source; break; }
 (void)text(images.get<&CoconutParquetImage::date_captured>(), row, "date_captured");
 (void)integer(images.get<&CoconutParquetImage::license>(), row, "license");
 const auto width = integer(images.get<&CoconutParquetImage::width>(), row, "width"), height = integer(images.get<&CoconutParquetImage::height>(), row, "height");
 if (width == 0 || height == 0 || width > limits.max_dimension || height > limits.max_dimension || width > limits.max_pixels / height || width * height > UINT32_MAX)
  malformed("dimensions exceed admission");
 record.width = static_cast<std::uint32_t>(width);
 record.height = static_cast<std::uint32_t>(height);
 record.source_ordinal = ordinal;
}
void read_batch(const arrow::RecordBatch& batch, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation, const std::function<void(CoconutRecord&&, std::span<const std::uint8_t>)>& consumer,
 std::uint64_t& row_ordinal, std::uint64_t& segment_ordinal, bool metadata_only, std::span<CoconutRecord> retained) {
 check(batch.Validate());  // Includes nested offsets and value-buffer bounds.
 if (metadata_only) {
  const auto images = top_level_column<&CoconutParquetRow::image_info>(batch);
  const BoundColumns<CoconutParquetImage> image_fields(*images);
  for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
   throw_if_benchmark_cancelled(cancellation);
   CoconutRecord record;
   present(*images, row, "image_info");
   read_image_record(image_fields, row, limits, row_ordinal, record);
   if (row_ordinal == UINT64_MAX) malformed("ordinal overflow");
   consumer(std::move(record), {});
   ++row_ordinal;
  }
  return;
 }
 const auto masks = top_level_column<&CoconutParquetRow::mask>(batch);
 const auto annotations = top_level_column<&CoconutParquetRow::segments_info>(batch);
 const BoundColumns<CoconutParquetMask> mask_fields(*masks);
 const BoundColumns<CoconutParquetAnnotation> annotation_fields(*annotations);
 const auto bytes = mask_fields.get<&CoconutParquetMask::bytes>();
 const auto mask_paths = mask_fields.get<&CoconutParquetMask::path>();  // HF path is nullable; embedded bytes are authoritative.
 const auto lists = annotation_fields.get<&CoconutParquetAnnotation::segments_info>();
 const auto segments = structure(lists->values(), "segment");
 const BoundColumns<CoconutParquetSegment> segment_fields(*segments);
 const auto areas = segment_fields.get<&CoconutParquetSegment::area>();
 const auto integer_areas = areas->type_id() == arrow::Type::INT64 ? static_cast<const arrow::Int64Array*>(areas.get()) : nullptr;
 const auto floating_areas = integer_areas ? nullptr : static_cast<const arrow::DoubleArray*>(areas.get());
 CoconutRecord record;
 for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
  throw_if_benchmark_cancelled(cancellation);
  present(*masks, row, "mask");
  present(*bytes, row, "mask.bytes");
  present(*annotations, row, "segments_info");
  present(*lists, row, "segments list");
  const auto png = binary_value(*bytes, row, "mask.bytes");
  if (!mask_paths->IsNull(row) && !mmltk::common::types::valid_utf8(binary_value(*mask_paths, row, "mask.path"))) malformed("mask.path");
  const auto begin = lists->value_offset(row), end = lists->value_offset(row + 1);
  if (begin < 0 || end < begin || end > segments->length()) malformed("segment list offsets");
  const auto length = end - begin;
  if (png.empty() || png.size() > limits.max_png_bytes || length < 0 || static_cast<std::uint64_t>(length) > limits.max_segments) malformed("record exceeds PNG/segment admission");
  if (row_ordinal >= retained.size()) malformed("retained image row join");
  record = std::move(retained[static_cast<std::size_t>(row_ordinal)]);
  if (integer(annotation_fields.get<&CoconutParquetAnnotation::image_id>(), row, "image_id") != record.image_id) malformed("image_id join");
  const auto annotation_file = text(annotation_fields.get<&CoconutParquetAnnotation::file_name>(), row, "file_name");
  if (std::filesystem::path(record.file_name).stem() != std::filesystem::path(annotation_file).stem() || std::filesystem::path(annotation_file).extension() != ".png")
   malformed("mask/image filename join");
  record.first_segment_ordinal = segment_ordinal;
  record.segments.clear();
  record.segments.reserve(static_cast<std::size_t>(length));
  for (std::int64_t offset = 0; offset < length; ++offset) {
   const auto index = begin + offset;
   present(*segments, index, "segment");
   CoconutSegment segment;
   const auto id = integer(segment_fields.get<&CoconutParquetSegment::id>(), index, "id");
   if (id == 0 || id > 0xffffffU) malformed("RGB segment id");
   segment.id = static_cast<std::uint32_t>(id);
   segment.category_id = integer(segment_fields.get<&CoconutParquetSegment::category_id>(), index, "category_id");
   segment.isthing = boolean(segment_fields.get<&CoconutParquetSegment::isthing>(), index, "isthing");
   segment.crowd = boolean(segment_fields.get<&CoconutParquetSegment::iscrowd>(), index, "iscrowd");
   segment.ignore = boolean(segment_fields.get<&CoconutParquetSegment::ignore>(), index, "ignore");
   if (!areas->IsNull(index)) {
    segment.area = integer_areas ? static_cast<double>(integer_areas->Value(index)) : floating_areas->Value(index);
    if (!std::isfinite(*segment.area) || *segment.area < 0) malformed("area");
   }
   record.segments.push_back(segment);
  }
  if (row_ordinal == UINT64_MAX || record.segments.size() > UINT64_MAX - segment_ordinal) malformed("ordinal overflow");
  throw_if_benchmark_cancelled(cancellation);
  segment_ordinal += record.segments.size();
  consumer(std::move(record), {reinterpret_cast<const std::uint8_t*>(png.data()), png.size()});
  ++row_ordinal;
 }
}
constexpr std::uint64_t kParquetPoolBytes = 256ULL << 20;
// Fixed for the entire call, including cold discovery and every later group.
// Physical controls are acquired by their owner from this continuation; their
// retained-handle role never becomes the role of decoder or Arrow backing.
class ParquetCallEnvelope final {
 enum class Mode { ColdMetadata, Full, WarmMetadata };
 const Mode mode_;
 const bool metadata_ready_;
 const CoconutPhysicalInputRequirement physical_;
public:
 ParquetCallEnvelope(bool metadata_only, bool metadata_ready, CoconutPhysicalInputRequirement physical)
  : mode_(metadata_only ? (metadata_ready ? Mode::WarmMetadata : Mode::ColdMetadata) : Mode::Full), metadata_ready_(metadata_ready), physical_(physical) {}
 [[nodiscard]] bool full() const noexcept { return mode_ == Mode::Full; }
 [[nodiscard]] bool needs_metadata() const noexcept { return !metadata_ready_; }
 [[nodiscard]] bool reads_projection() const noexcept { return mode_ != Mode::WarmMetadata; }
 [[nodiscard]] BenchmarkResources discovery_demand() const { return {kParquetPoolBytes, 1}; }
 [[nodiscard]] BenchmarkResources demand(std::uint64_t workspace) const {
  using mmltk::common::math::checked_add;
  return {checked_add(checked_add(physical_.workspace_bytes(), reads_projection() ? kParquetPoolBytes : 0, "COCONut Parquet input overflow"),
   workspace, "COCONut Parquet allowance overflow"), reads_projection() ? 1U : 0U, false, 0, false, physical_.continuation_descriptors()};
 }
 [[nodiscard]] std::uint64_t live_bytes(std::uint64_t pool_bytes) const {
  // The future pool cap and its currently allocated bytes overlap. Every
  // published batch shares this entire pool, including older live batches.
  return mmltk::common::math::checked_add(physical_.workspace_bytes(), pool_bytes, "COCONut live input overflow");
 }
};
// Allocation and cross-thread batch destruction share this heap owner. The
// underlying cap remains 256 MiB; serializing its check makes that cap exact.
class ParquetPool final : public arrow::MemoryPool {
 mutable std::mutex mutex_;
 arrow::ProxyMemoryPool tracked_{arrow::system_memory_pool()};
 arrow::CappedMemoryPool capped_{&tracked_, kParquetPoolBytes};
public:
 BenchmarkAllowance allowance;
 explicit ParquetPool(BenchmarkAllowance value) : allowance(std::move(value)) {}
 arrow::Status Allocate(std::int64_t size, std::int64_t alignment, std::uint8_t** out) override { const std::lock_guard lock(mutex_); return capped_.Allocate(size, alignment, out); }
 arrow::Status Reallocate(std::int64_t old_size, std::int64_t size, std::int64_t alignment, std::uint8_t** out) override { const std::lock_guard lock(mutex_); return capped_.Reallocate(old_size, size, alignment, out); }
 void Free(std::uint8_t* buffer, std::int64_t size, std::int64_t alignment) override { const std::lock_guard lock(mutex_); capped_.Free(buffer, size, alignment); }
 std::int64_t bytes_allocated() const override { return tracked_.bytes_allocated(); }
 std::int64_t max_memory() const override { return tracked_.max_memory(); }
 std::int64_t total_bytes_allocated() const override { return tracked_.total_bytes_allocated(); }
 std::int64_t num_allocations() const override { return tracked_.num_allocations(); }
 std::string backend_name() const override { return tracked_.backend_name(); }
};
std::unique_ptr<parquet::arrow::FileReader> open_reader(const std::filesystem::path& path, ParquetPool& pool, std::shared_ptr<parquet::FileMetaData> metadata = {}) {
 parquet::ReaderProperties input(&pool);
 input.enable_buffered_stream(); input.set_buffer_size(128U * 1024U);
 input.set_thrift_string_size_limit(16U * 1024U * 1024U); input.set_thrift_container_size_limit(1000000);
 parquet::ArrowReaderProperties properties;
 properties.set_pre_buffer(false); properties.set_use_threads(false); properties.set_batch_size(8);
 parquet::arrow::FileReaderBuilder builder;
 check(builder.OpenFile(path.string(), false, input, std::move(metadata)));
 builder.memory_pool(&pool); builder.properties(properties);
 return take(builder.Build());
}
struct ParquetBatch {
 std::shared_ptr<ParquetPool> pool;
 std::shared_ptr<arrow::RecordBatch> batch;
};
struct ParquetReaderDescriptors {
 const BenchmarkAllowance& allowance;
 ~ParquetReaderDescriptors() { allowance.retire_descriptors(); }
};
}  // namespace
class CoconutParquetMetadata final {
public:
 struct Shard { std::filesystem::path path; std::shared_ptr<ParquetPool> pool; std::shared_ptr<parquet::FileMetaData> footer; ParquetProjection projection; };
 struct Group { std::size_t shard; int group; };
 bool metadata_ready = false;
 std::vector<Shard> shards;
 std::vector<Group> groups;
};
void CoconutAnnotationRecords::rebase_parquet_segments() {
 std::uint64_t prefix = 0;
 for (auto& group : groups) {
  if (group.first_row > records.size() || group.rows > records.size() - group.first_row) malformed("retained group row extent");
  group.first_segment = prefix;
  for (auto& record : std::span(records).subspan(static_cast<std::size_t>(group.first_row), static_cast<std::size_t>(group.rows)))
   record.first_segment_ordinal = mmltk::common::math::checked_add(record.first_segment_ordinal, prefix, "COCONut Parquet record ordinal overflow");
  prefix = mmltk::common::math::checked_add(prefix, group.segments, "COCONut Parquet segment prefix overflow");
 }
}
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation,
 const CoconutRecordConsumer& consumer, bool metadata_only, BenchmarkCompilePipeline* execution, const std::function<void(std::size_t)>& retire_consumer_scratch, const BenchmarkAllowance& parent, CoconutPhysicalInputRequirement physical_input, CoconutAnnotationRecords* retained,
 const std::function<std::uint64_t(const CoconutRecord&)>& consumer_workspace, const std::function<void(const BenchmarkAllowance&)>& retire_consumer_input) {
 using mmltk::common::math::checked_add;
 using mmltk::common::math::checked_multiply;
 CoconutAnnotationRecords local;
 if (!retained) retained = &local;
 const ParquetCallEnvelope envelope(metadata_only, retained->parquet && retained->parquet->metadata_ready, physical_input);
 if (!retained->parquet) {
  auto catalog = std::make_shared<CoconutParquetMetadata>();
  std::uint64_t rows = 0;
  for (const auto& path : shards) {
   throw_if_benchmark_cancelled(cancellation);
   auto allowance = execution ? execution->reserve(envelope.discovery_demand(), parent) : BenchmarkAllowance{};
   auto pool = std::make_shared<ParquetPool>(allowance);
   // Declared before reader/batches: all library readers close before this
   // guard returns descriptors, including cancellation and consumer throws.
   ParquetReaderDescriptors retire_reader{allowance};
   std::unique_ptr<parquet::arrow::FileReader> reader;
   std::shared_ptr<parquet::FileMetaData> footer;
   ParquetProjection projection;
   const auto discover = [&](std::size_t) {
    reader = open_reader(path, *pool);
    projection = resolve_projection(reader->manifest());
    footer = reader->parquet_reader()->metadata();
    for (int group = 0; group < reader->num_row_groups(); ++group) {
     const auto count = footer->RowGroup(group)->num_rows();
     if (count < 0) malformed("negative row group count");
     catalog->groups.push_back({catalog->shards.size(), group});
     retained->groups.push_back({rows, static_cast<std::uint64_t>(count), 0, 0});
     rows = checked_add(rows, static_cast<std::uint64_t>(count), "COCONut Parquet row prefix overflow");
    }
   };
   if (execution) execution->run(BenchmarkStage::Metadata, {}, discover, allowance); else discover(0);
   reader.reset();
   // Footer facts are canonical metadata. Any Arrow-backed footer storage
   // keeps its pool and its exact charged remainder after the reader closes.
   if (allowance) {
    const auto held = static_cast<std::uint64_t>(pool->bytes_allocated());
    auto backing = held ? allowance.split_storage(held) : BenchmarkAllowance{};
    allowance.retire_descriptors(); allowance.retire_workspace(); pool->allowance = std::move(backing);
   }
   catalog->shards.push_back({path, std::move(pool), std::move(footer), std::move(projection)});
  }
  retained->records.resize(mmltk::common::math::checked_cast<std::size_t>(rows, "COCONut Parquet row count overflow"));
  retained->parquet = std::move(catalog);
  // The first pass projects only image columns and publishes each ready row.
  // A full import then joins these same owned records from mask-only batches.
 }
 auto catalog = retained->parquet;
 if (catalog->shards.size() != shards.size()) malformed("retained shard generation mismatch");
 for (std::size_t i = 0; i < shards.size(); ++i) if (catalog->shards[i].path != shards[i]) malformed("retained shard path mismatch");
 // Each existing parallel range owns one forward Arrow/physical continuation.
 // Groups still publish independently; only their current batch is transient.
 struct Sequence {
  BenchmarkCompilePipeline* execution;
  const BenchmarkAllowance& parent;
  const std::function<void(const BenchmarkAllowance&)>& retire_input;
  const ParquetCallEnvelope& envelope;
  bool active = false;
  BenchmarkAllowance allowance;
  std::shared_ptr<ParquetPool> pool;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::size_t shard = SIZE_MAX;
  void close() {
   reader.reset();
   if (allowance && retire_input) retire_input(allowance);
   if (allowance) {
    const auto held = pool ? static_cast<std::uint64_t>(pool->bytes_allocated()) : 0;
    auto backing = held ? allowance.split_storage(held) : BenchmarkAllowance{};
    allowance.retire_descriptors(); allowance.retire_workspace();
    if (pool) pool->allowance = std::move(backing);
   }
   pool.reset(); allowance = {}; shard = SIZE_MAX; active = false;
  }
  ~Sequence() {
   try { close(); }
   catch (...) { allowance.retire_descriptors(); } // Unwinding keeps any unsplit pool promise until its last batch.
  }
  std::uint64_t live_bytes() const {
   return envelope.live_bytes(pool ? static_cast<std::uint64_t>(pool->bytes_allocated()) : 0);
  }
  void ensure(std::uint64_t workspace) {
   const auto demand = envelope.demand(workspace);
   // Every transition keeps the same future cap (hence every live pool/batch
   // allocation) and complete descriptor promise. Only excess group work varies.
   if (active && (!execution || allowance.try_resize_workspace(demand.bytes, !execution->resource_pressure()))) return;
   // No blocking upgrade with retained input: two readers can both fail growth
   // and release their actual dependents before complete reacquisition. The
   // typed row and group cursor survive; no Parquet prefix is skipped/reparsed.
   close();
   allowance = execution ? execution->reserve(demand, parent) : BenchmarkAllowance{};
   if (envelope.reads_projection()) pool = std::make_shared<ParquetPool>(allowance);
   active = true;
  }
 };
 const auto workers = execution ? execution->workers() : std::size_t{1};
 mmltk::common::concurrency::parallel_for_range_indexed(std::size_t{0}, catalog->groups.size(), mmltk::common::math::checked_cast<int>(workers, "COCONut Parquet worker count overflow"),
  [&](int, std::size_t begin, std::size_t end) {
   Sequence sequence{execution, parent, retire_consumer_input, envelope};
   const auto group_work = [&](std::size_t group_index, bool image_pass) {
    auto& group = retained->groups[group_index];
    const auto position = catalog->groups[group_index];
    const auto& shard = catalog->shards[position.shard];
    throw_if_benchmark_cancelled(cancellation);
    try {
     if (image_pass && !envelope.needs_metadata()) {
      sequence.ensure(0);
      const auto& allowance = sequence.allowance;
      for (auto row = group.first_row; row < group.first_row + group.rows; ++row) consumer(group_index, retained->records[static_cast<std::size_t>(row)], CoconutAnnotationInput{{}, allowance, {}, sequence.live_bytes(), true});
      if (retire_consumer_scratch) retire_consumer_scratch(group_index);
      return;
     }
     std::uint64_t bytes = 0;
     const auto size_group = [&](std::size_t) {
      std::uint64_t scratch = 0;
      if (!image_pass && consumer_workspace) for (auto row = group.first_row; row < group.first_row + group.rows; ++row)
       scratch = std::max(scratch, consumer_workspace(retained->records[static_cast<std::size_t>(row)]));
      // The producer promise covers every possible allocation of the capped
      // reader and the largest admitted image in this group, not import limits.
      std::uint64_t batch_segments = 0;
      if (!image_pass) {
       const auto count = shard.footer->RowGroup(position.group)->ColumnChunk(shard.projection.segment_id)->num_values();
       if (count < 0) malformed("negative segment value count");
       batch_segments = std::min(static_cast<std::uint64_t>(count), checked_multiply(std::min<std::uint64_t>(8, group.rows), std::uint64_t{limits.max_segments}, "COCONut Parquet segment bound overflow"));
      }
      const auto typed = checked_add(checked_multiply(batch_segments, std::uint64_t{sizeof(CoconutSegment)}, "COCONut Parquet typed batch overflow"), 128ULL << 10, "COCONut Parquet row metadata overflow");
      bytes = checked_add(scratch, typed, "COCONut Parquet batch workspace overflow");
     };
     if (execution) execution->run(BenchmarkStage::Metadata, {65536, 0}, size_group, sequence.allowance); else size_group(0);
     sequence.ensure(bytes);
     const auto& allowance = sequence.allowance;
     const auto& pool = sequence.pool;
     struct RetireConsumer {
      const std::function<void(std::size_t)>& callback; std::size_t group; bool pending = true;
      void finish() { if (std::exchange(pending, false) && callback) callback(group); }
      ~RetireConsumer() { finish(); }
     } retire{retire_consumer_scratch, group_index};
     const auto cpu = [&](const std::function<void()>& work) { if (execution) execution->run(image_pass ? BenchmarkStage::Metadata : BenchmarkStage::Normalize, {}, [&](std::size_t) { work(); }, allowance); else work(); };
     if (sequence.shard != position.shard) {
      sequence.reader.reset();
      cpu([&] { sequence.reader = open_reader(shard.path, *pool, shard.footer); });
      sequence.shard = position.shard;
     }
     auto& reader = sequence.reader;
     const auto& columns = image_pass ? shard.projection.images : shard.projection.masks;
     std::unique_ptr<arrow::RecordBatchReader> batches;
     cpu([&] { batches = take(reader->GetRecordBatchReader({position.group}, columns)); });
     auto row_ordinal = group.first_row;
     std::uint64_t segment_ordinal = 0;
     for (;;) {
      throw_if_benchmark_cancelled(cancellation);
      auto chunk = std::make_shared<ParquetBatch>(); chunk->pool = pool;
      cpu([&] { check(batches->ReadNext(&chunk->batch)); });
      if (!chunk->batch) break;
      std::vector<std::pair<CoconutRecord, std::span<const std::uint8_t>>> records;
      records.reserve(static_cast<std::size_t>(chunk->batch->num_rows()));
      cpu([&] { read_batch(*chunk->batch, limits, cancellation, [&](CoconutRecord&& record, std::span<const std::uint8_t> png) { records.emplace_back(std::move(record), png); }, row_ordinal, segment_ordinal, image_pass, retained->records); });
      std::uint64_t live = checked_add(sequence.live_bytes(), records.capacity() * sizeof(records.front()), "COCONut live batch overflow");
      for (const auto& [record, png] : records) live = checked_add(live, record.segments.capacity() * sizeof(CoconutSegment) + record.file_name.capacity() + record.physical_stem.capacity(), "COCONut retained batch overflow");
      // ReadNext is stopped here. Consumer source waits remain outside CPU lanes;
      // its finite normalization may lend only the actually unused promise.
      for (auto& [record, png] : records) {
       consumer(group_index, record, CoconutAnnotationInput{png, allowance, chunk, live, image_pass});
       retained->records[static_cast<std::size_t>(record.source_ordinal)] = std::move(record);
      }
     }
     if (row_ordinal != group.first_row + group.rows) malformed("row group extent mismatch");
     if (!image_pass) group.segments = segment_ordinal;
     batches.reset();
     retire.finish();
    } catch (const ParquetFormatError& error) { throw std::runtime_error("COCONut Parquet " + shard.path.string() + ": " + error.what()); }
   };
   for (auto group = begin; group < end; ++group) {
    // Cold imports publish metadata group by group, then grow using that
    // group's actual dimensions. Successful growth preserves the same decoder;
    // pressure may reopen it and replay a physical prefix, never a row prefix.
    if (envelope.full() && envelope.needs_metadata()) group_work(group, true);
    group_work(group, !envelope.full());
   }
   sequence.close();
  });
 catalog->metadata_ready = true;
 if (envelope.full()) {
  const auto rebase = [&](std::size_t) { retained->rebase_parquet_segments(); };
  if (execution) execution->run(BenchmarkStage::Metadata, {}, rebase); else rebase(0);
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
