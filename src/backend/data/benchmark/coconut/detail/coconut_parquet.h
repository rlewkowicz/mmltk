#pragma once
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
// External Parquet vocabulary. Structural binding derives from these fields;
// Arrow allocation, null admission, and execution remain ordinary owners.
struct CoconutParquetImage {
 std::string file_name, coco_url, date_captured;
 std::int64_t id, height, width, license;
};
MMLTK_REFLECT_FIELDS(CoconutParquetImage)
struct CoconutParquetMask {
 std::vector<std::uint8_t> bytes;
 std::string path;
};
MMLTK_REFLECT_FIELDS(CoconutParquetMask)
struct CoconutParquetSegment {
 std::int64_t id, category_id, isthing;
 std::optional<std::int64_t> iscrowd, ignore;
 std::variant<std::int64_t, double> area;
};
MMLTK_REFLECT_FIELDS(CoconutParquetSegment)
struct CoconutParquetAnnotation {
 std::string file_name;
 std::int64_t image_id;
 std::vector<CoconutParquetSegment> segments_info;
};
MMLTK_REFLECT_FIELDS(CoconutParquetAnnotation)
struct CoconutParquetRow {
 CoconutParquetMask mask;
 CoconutParquetImage image_info;
 CoconutParquetAnnotation segments_info;
};
MMLTK_REFLECT_FIELDS(CoconutParquetRow)
}  // namespace mmltk::backend::data::benchmark_internal
