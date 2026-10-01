#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/compiled/compiled_format.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class CoconutStaging;

class CoconutInputError final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
struct CoconutSegment {
 std::uint32_t id = 0;
 std::uint64_t category_id = 0;
 bool isthing = false;
 bool crowd = false;
 bool ignore = false;
 std::optional<double> area = std::nullopt;
 // External COCO bbox convention: x, y, width, height.
 std::optional<std::array<double, 4>> bbox = std::nullopt;
 bool valid = true;
};
// Parsed segment metadata becomes immutable mapped input before its row leaves
// the bounded parser batch. Copies share custody; mutation is parser-local.
class CoconutSegments final {
public:
 CoconutSegments() = default;
 CoconutSegments(std::initializer_list<CoconutSegment> values) : values_(values) {}
 CoconutSegments(std::vector<CoconutSegment> values) : values_(std::move(values)) {}
 [[nodiscard]] std::span<const CoconutSegment> view() const noexcept { return mapped_.data() ? mapped_ : std::span<const CoconutSegment>(values_); }
 [[nodiscard]] std::size_t size() const noexcept { return view().size(); }
 [[nodiscard]] bool empty() const noexcept { return view().empty(); }
 [[nodiscard]] const CoconutSegment* data() const noexcept { return view().data(); }
 [[nodiscard]] auto begin() const noexcept { return view().begin(); }
 [[nodiscard]] auto end() const noexcept { return view().end(); }
 [[nodiscard]] const CoconutSegment& operator[](std::size_t i) const { return view()[i]; }
 CoconutSegment& operator[](std::size_t i) {
  writable();
  return values_[i];
 }
 void reserve(std::size_t n) {
  writable();
  values_.reserve(n);
 }
 void push_back(CoconutSegment value) {
  writable();
  values_.push_back(std::move(value));
 }
 void stage(CoconutStaging&);

private:
 void writable();
 std::shared_ptr<const void> backing_;
 std::span<const CoconutSegment> mapped_;
 std::vector<CoconutSegment> values_;
};
struct CoconutRecord {
 std::uint64_t image_id = 0;
 std::string file_name;
 std::string physical_stem;
 std::optional<CoconutImageNamespace> namespace_hint;
 std::uint32_t width = 0, height = 0;
 std::uint64_t source_ordinal = 0;
 std::uint64_t first_segment_ordinal = 0;
 CoconutSegments segments;
};
struct CoconutImportLimits {
 std::uint64_t max_png_bytes = 64U * 1024U * 1024U;
 std::uint64_t max_pixels = 64U * 1024U * 1024U;
 std::uint32_t max_segments = 65535U;
 std::uint32_t max_dimension = MAX_IMAGE_EXTENT;
};
struct CoconutPhysicalName {
 CoconutImageNamespace source;
 std::uint64_t id;
 std::string stem;
};
[[nodiscard]] CoconutPhysicalName parse_coconut_objects_member(std::string_view);
[[nodiscard]] std::uint64_t parse_coconut_coco_member(std::string_view);
}  // namespace mmltk::backend::data::benchmark_internal
