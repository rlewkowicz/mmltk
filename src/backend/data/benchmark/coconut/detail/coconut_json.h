#pragma once
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
// JSON has its own numeric/null contract, distinct from the Parquet columns.
// Missing fields, invalid shapes, and reusable parsing storage belong to the
// ordinary parser; only external values and their member names are reflected.
using CoconutJsonScalar = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string>;
struct CoconutJsonSegment {
 CoconutJsonScalar id, category_id, isthing, iscrowd, ignore, area;
 std::vector<CoconutJsonScalar> bbox;
};
MMLTK_REFLECT_FIELDS(CoconutJsonSegment)
struct CoconutJsonRow {
 CoconutJsonScalar id, image_id, file_name, object365_file_name, object365_name, name, width, height;
 std::vector<CoconutJsonSegment> segments_info;
};
MMLTK_REFLECT_FIELDS(CoconutJsonRow)
}  // namespace mmltk::backend::data::benchmark_internal
