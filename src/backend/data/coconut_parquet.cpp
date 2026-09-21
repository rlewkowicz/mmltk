#include "detail/coconut_annotations.h"
#include <arrow/api.h>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
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
std::shared_ptr<const arrow::StructArray> structure(std::shared_ptr<arrow::Array> array, std::string_view name) {
    return as<arrow::StructArray>(std::move(array), arrow::Type::STRUCT, name);
}
std::uint64_t integer(const arrow::StructArray& parent, std::string_view name, std::int64_t row) {
    const auto array = as<arrow::Int64Array>(parent.GetFieldByName(std::string(name)), arrow::Type::INT64, name);
    present(*array, row, name);
    const auto value = array->Value(row);
    if (value < 0) malformed(name);
    return static_cast<std::uint64_t>(value);
}
std::string_view text(const arrow::StructArray& parent, std::string_view name, std::int64_t row) {
    const auto array = as<arrow::StringArray>(parent.GetFieldByName(std::string(name)), arrow::Type::STRING, name);
    present(*array, row, name);
    const auto value = array->GetView(row);
    if (value.size() > 4096U) malformed(name);
    return value;
}
bool boolean(const arrow::StructArray& parent, std::string_view name, std::int64_t row, bool optional = false) {
    if (optional && !parent.GetFieldByName(std::string(name))) return false;
    const auto value = integer(parent, name, row);
    if (value > 1) malformed(name);
    return value != 0;
}
std::shared_ptr<const arrow::StructType> struct_type(const std::shared_ptr<arrow::Field>& field, std::string_view name) {
    if (!field || field->type()->id() != arrow::Type::STRUCT) malformed(name);
    return std::static_pointer_cast<const arrow::StructType>(field->type());
}
void field_type(const arrow::StructType& parent, std::string_view name, arrow::Type::type type, bool optional = false) {
    const auto field = parent.GetFieldByName(std::string(name));
    if (optional && !field) return;
    if (!field || field->type()->id() != type) malformed(name);
}
void validate_schema(const arrow::Schema& schema) {
    const auto mask = struct_type(schema.GetFieldByName("mask"), "mask");
    field_type(*mask, "bytes", arrow::Type::BINARY);
    field_type(*mask, "path", arrow::Type::STRING);
    const auto images = struct_type(schema.GetFieldByName("image_info"), "image_info");
    for (const auto name : {"file_name", "coco_url", "date_captured"}) field_type(*images, name, arrow::Type::STRING);
    for (const auto name : {"id", "height", "width", "license"}) field_type(*images, name, arrow::Type::INT64);
    const auto annotations = struct_type(schema.GetFieldByName("segments_info"), "segments_info");
    field_type(*annotations, "file_name", arrow::Type::STRING);
    field_type(*annotations, "image_id", arrow::Type::INT64);
    field_type(*annotations, "segments_info", arrow::Type::LIST);
    const auto& list = static_cast<const arrow::ListType&>(*annotations->GetFieldByName("segments_info")->type());
    const auto segment = struct_type(list.value_field(), "segment");
    for (const auto name : {"id", "category_id", "isthing"}) field_type(*segment, name, arrow::Type::INT64);
    field_type(*segment, "iscrowd", arrow::Type::INT64, true);
    field_type(*segment, "ignore", arrow::Type::INT64, true);
    const auto area = segment->GetFieldByName("area");
    if (!area || (area->type()->id() != arrow::Type::INT64 && area->type()->id() != arrow::Type::DOUBLE)) malformed("area");
}
void read_batch(const arrow::RecordBatch& batch, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation,
                const CoconutRecordConsumer& consumer, std::uint64_t& row_ordinal, std::uint64_t& segment_ordinal) {
    check(batch.ValidateFull());  // Includes nested offsets and value-buffer bounds.
    const auto masks = structure(batch.GetColumnByName("mask"), "mask");
    const auto annotations = structure(batch.GetColumnByName("segments_info"), "segments_info");
    const auto images = structure(batch.GetColumnByName("image_info"), "image_info");
    const auto bytes = as<arrow::BinaryArray>(masks->GetFieldByName("bytes"), arrow::Type::BINARY, "mask.bytes");
    (void)as<arrow::StringArray>(masks->GetFieldByName("path"), arrow::Type::STRING, "mask.path");  // HF path is nullable; embedded bytes are authoritative.
    const auto lists = as<arrow::ListArray>(annotations->GetFieldByName("segments_info"), arrow::Type::LIST, "segments_info.segments_info");
    const auto segments = structure(lists->values(), "segment");
    const auto areas = segments->GetFieldByName("area");
    if (!areas || (areas->type_id() != arrow::Type::DOUBLE && areas->type_id() != arrow::Type::INT64)) malformed("area");
    CoconutRecord record;
    for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
        throw_if_benchmark_cancelled(cancellation);
        present(*masks, row, "mask");
        present(*bytes, row, "mask.bytes");
        present(*annotations, row, "segments_info");
        present(*images, row, "image_info");
        present(*lists, row, "segments list");
        const auto png = bytes->GetView(row);
        const auto begin = lists->value_offset(row), length = lists->value_length(row);
        if (png.empty() || png.size() > limits.max_png_bytes || length < 0 || static_cast<std::uint64_t>(length) > limits.max_segments)
            malformed("record exceeds PNG/segment admission");
        record.image_id = integer(*images, "id", row);
        if (integer(*annotations, "image_id", row) != record.image_id) malformed("image_id join");
        record.file_name = text(*images, "file_name", row);
        const auto annotation_file = text(*annotations, "file_name", row);
        if (std::filesystem::path(record.file_name).stem() != std::filesystem::path(annotation_file).stem() ||
            std::filesystem::path(annotation_file).extension() != ".png")
            malformed("mask/image filename join");
        // These pinned fields are schema facts, not physical subset authority.
        (void)text(*images, "coco_url", row);
        (void)text(*images, "date_captured", row);
        (void)integer(*images, "license", row);
        const auto width = integer(*images, "width", row), height = integer(*images, "height", row);
        if (width == 0 || height == 0 || width > limits.max_dimension || height > limits.max_dimension || width > limits.max_pixels / height ||
            width * height > UINT32_MAX)
            malformed("dimensions exceed admission");
        record.width = static_cast<std::uint32_t>(width);
        record.height = static_cast<std::uint32_t>(height);
        record.source_ordinal = row_ordinal;
        record.first_segment_ordinal = segment_ordinal;
        record.segments.clear();
        record.segments.reserve(static_cast<std::size_t>(length));
        for (std::int64_t offset = 0; offset < length; ++offset) {
            const auto index = begin + offset;
            present(*segments, index, "segment");
            CoconutSegment segment;
            const auto id = integer(*segments, "id", index);
            if (id == 0 || id > 0xffffffU) malformed("RGB segment id");
            segment.id = static_cast<std::uint32_t>(id);
            segment.category_id = integer(*segments, "category_id", index);
            segment.isthing = boolean(*segments, "isthing", index);
            segment.crowd = boolean(*segments, "iscrowd", index, true);
            segment.ignore = boolean(*segments, "ignore", index, true);
            if (!areas->IsNull(index)) {
                segment.area = areas->type_id() == arrow::Type::DOUBLE ? static_cast<const arrow::DoubleArray&>(*areas).Value(index)
                                                                       : static_cast<double>(static_cast<const arrow::Int64Array&>(*areas).Value(index));
                if (!std::isfinite(*segment.area) || *segment.area < 0) malformed("area");
            }
            record.segments.push_back(segment);
        }
        if (row_ordinal == UINT64_MAX || record.segments.size() > UINT64_MAX - segment_ordinal) malformed("ordinal overflow");
        throw_if_benchmark_cancelled(cancellation);
        consumer(record, {reinterpret_cast<const std::uint8_t*>(png.data()), png.size()});
        ++row_ordinal;
        segment_ordinal += record.segments.size();
    }
}
}  // namespace
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits,
                          mmltk::common::concurrency::CancellationObservation cancellation, const CoconutRecordConsumer& consumer) {
    std::uint64_t row_ordinal = 0, segment_ordinal = 0;
    for (const auto& path : shards) {
        throw_if_benchmark_cancelled(cancellation);
        try {
            // Private, single-threaded capped pool covers page/dictionary/batch allocations.
            // Fixed buffered input avoids whole column-region reads despite pre_buffer(false).
            arrow::ProxyMemoryPool tracked(arrow::system_memory_pool());
            arrow::CappedMemoryPool pool(&tracked, 256LL * 1024LL * 1024LL);
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
            check(builder.OpenFile(path.string(), false, input));
            builder.memory_pool(&pool);
            builder.properties(properties);
            auto reader = take(builder.Build());
            std::shared_ptr<arrow::Schema> schema;
            check(reader->GetSchema(&schema));
            validate_schema(*schema);
            const auto metadata = reader->parquet_reader()->metadata();
            std::vector<int> columns;
            for (int i = 0; i < metadata->num_columns(); ++i) {
                const auto field = metadata->schema()->Column(i)->path()->ToDotString();
                if (field.starts_with("mask.") || field.starts_with("segments_info.") || field.starts_with("image_info.")) columns.push_back(i);
            }
            if (columns.empty()) malformed("required nested columns");
            std::vector<int> groups(reader->num_row_groups());
            std::iota(groups.begin(), groups.end(), 0);
            auto batches = take(reader->GetRecordBatchReader(groups, columns));
            while (true) {
                throw_if_benchmark_cancelled(cancellation);
                std::shared_ptr<arrow::RecordBatch> batch;
                check(batches->ReadNext(&batch));
                if (!batch) break;
                read_batch(*batch, limits, cancellation, consumer, row_ordinal, segment_ordinal);
            }
        } catch (const ParquetFormatError& error) {
            // Consumer failures retain the types used by scoped cache recovery.
            throw std::runtime_error("COCONut Parquet " + path.string() + ": " + error.what());
        }
    }
}
}  // namespace mmltk::backend::data::benchmark_internal
