#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include <system_error>
#include <sys/mman.h>
#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include <simdjson.h>
#include "src/pch_json.h"
#include "src/pch_std.h"
#include "src/pch_linux.h"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/concurrency/concurrency.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/detail/mask_rle_utils.h"
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileExecution;
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
using mmltk::common::concurrency::parallel_for_range_indexed;
using mmltk::common::io::errno_error;
using mmltk::common::io::FileHandle;
using mmltk::common::io::publish_staged_path_atomically;
using mmltk::common::io::sync_parent_directory;
using mmltk::common::math::checked_cast;
nlohmann::json reject_json(const AnnotationRejectCounts& rejected) {
 nlohmann::json result = nlohmann::json::object();
 mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto& field) { result[field.member_name] = rejected.*Declaration::pointer; });
 return result;
}
class NormalizedAnnotationBacking final {
public:
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::shared_ptr<const void> storage;
 std::span<const NormalizedImage> images;
 std::span<const NormalizedBox> boxes;
 std::span<const RLEPair> runs;
 std::once_flag image_admission, full_admission;
 std::atomic<std::size_t> admitted_rows{0};
 std::shared_ptr<BenchmarkMappedArtifact> staged;
 std::mutex publication_mutex;
 NormalizedImage single_image{};
};
namespace {
constexpr std::uint64_t kNormalizedIndexMagic = 0x4D4D4C544B4E4931ULL;
struct __attribute__((packed)) NormalizedIndexHeader {
 std::uint64_t magic = kNormalizedIndexMagic;
 std::uint32_t version = kNormalizedAnnotationIndexVersion;
 std::uint8_t source = 0U;
 std::uint8_t reserved0[3]{};
 std::uint32_t image_count = 0U;
 std::uint64_t box_count = 0U;
 std::uint64_t mask_rle_pair_count = 0U;
 std::uint64_t image_offset = 0U;
 std::uint64_t box_offset = 0U;
 std::uint64_t mask_rle_offset = 0U;
 std::uint64_t total_file_size = 0U;
 std::array<std::uint8_t, 32> annotation_sha256{};
 std::array<char, 48> split{};
 std::array<char, 48> mapping_revision{};
 std::array<std::uint64_t, 6> rejected{};
 std::array<std::uint8_t, 12> reserved{};
};
static_assert(kNormalizedAnnotationIndexVersion == 3U);
static_assert(sizeof(NormalizedIndexHeader) == 256U);
// One v3 physical layout serves mapped capacity, persisted admission and publication.
// Callers decide whether an overflow is corrupt cache data or a capacity failure.
struct NormalizedIndexLayout {
 std::uint64_t image_offset, box_offset, mask_rle_offset, total_file_size;
};
[[nodiscard]] NormalizedIndexLayout normalized_index_layout(std::uint64_t images, std::uint64_t boxes, std::uint64_t runs) {
 const auto block_end = [](std::uint64_t begin, std::uint64_t count, std::uint64_t record_size, const char* context) {
  return common::math::checked_add(begin, common::math::checked_multiply(count, record_size, context), context);
 };
 NormalizedIndexLayout layout{};
 layout.image_offset = sizeof(NormalizedIndexHeader);
 layout.box_offset = block_end(layout.image_offset, images, sizeof(NormalizedImage), "normalized image block size overflow");
 layout.mask_rle_offset = block_end(layout.box_offset, boxes, sizeof(NormalizedBox), "normalized box block size overflow");
 layout.total_file_size = block_end(layout.mask_rle_offset, runs, sizeof(RLEPair), "normalized mask block size overflow");
 return layout;
}
using RejectFields = std::remove_cvref_t<decltype(mmltk::frameworks::reflection::field_declarations<AnnotationRejectCounts>())>;
[[nodiscard]] AnnotationRejectCounts decode_rejected(const std::array<std::uint64_t, 6>& rejected) noexcept {
 AnnotationRejectCounts result;
 RejectFields::Visit([&]<class Declaration, std::size_t Index>() { result.*Declaration::pointer = rejected[Index]; });
 return result;
}
[[nodiscard]] std::array<std::uint64_t, 6> encode_rejected(const AnnotationRejectCounts& rejected) noexcept {
 std::array<std::uint64_t, 6> result{};
 RejectFields::Visit([&]<class Declaration, std::size_t Index>() { result[Index] = rejected.*Declaration::pointer; });
 return result;
}
void annotation_ranges(CompileExecution* execution, std::size_t count, int workers, const std::function<void(int, std::size_t, std::size_t)>& consume, std::uint64_t workspace = 8U << 20,
 const std::function<void(std::size_t)>& retire = {}) {
 if (execution)
  execution->for_each(CompileTaskKind::Metadata, count, {workspace, 0}, [&](std::size_t index) { consume(static_cast<int>(execution->current_lane()), index, index + 1); }, retire);
 else
  parallel_for_range_indexed<std::size_t>(0U, count, workers, consume);
}
[[nodiscard]] std::filesystem::path normalized_manifest_path(const std::filesystem::path& path) { return path.string() + ".complete.json"; }
[[nodiscard]] int effective_worker_count(const int requested, const std::size_t work_items) {
 const int available = requested > 0 ? requested : static_cast<int>(std::thread::hardware_concurrency());
 const int bounded_work = work_items > static_cast<std::size_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(std::max<std::size_t>(work_items, 1U));
 return std::max(1, std::min(std::max(available, 1), bounded_work));
}
void parallel_object_array(const PaddedMappedFile& file, std::span<const ByteRange> rows, int requested_workers, std::vector<simdjson::ondemand::parser>& parsers,
 CompileExecution::Workspace* parser_workspace, mmltk::common::concurrency::CancellationObservation cancellation, const std::function<void(int, simdjson::ondemand::object, std::uint64_t)>& callback,
 CompileExecution* execution = nullptr, const std::function<void(int)>& begin_chunk = {}) {
 const auto workers = execution ? execution->workers() : static_cast<std::size_t>(effective_worker_count(requested_workers, rows.size()));
 std::vector<ByteRange> chunks;
 for (std::size_t begin = 0; begin < rows.size();) {
  auto end = begin + 1;
  while (end < rows.size() && rows[end].end - rows[begin].begin <= (256U << 10)) ++end;
  chunks.push_back({begin, end});
  begin = end;
 }
 const auto consume = [&](std::size_t lane, std::size_t chunk) {
  if (begin_chunk) begin_chunk(static_cast<int>(lane));
  for (auto row = chunks[chunk].begin; row < chunks[chunk].end; ++row) {
   throw_if_benchmark_cancelled(cancellation);
   const auto range = rows[row];
   simdjson::ondemand::document document;
   simdjson::ondemand::object object;
   try {
    document = parsers[lane].iterate(simdjson::padded_string_view(file.data() + range.begin, range.end - range.begin, file.capacity_from(range.begin)));
    object = document.get_object();
   } catch (const simdjson::simdjson_error& error) { reject_json_document(error); }
   callback(static_cast<int>(lane), object, range.begin);
   if (execution && (row - chunks[chunk].begin + 1) % 32 == 0) execution->cooperate();
  }
 };
 if (execution)
  execution->for_each(CompileTaskKind::Metadata, chunks.size(), [&](std::size_t chunk) {
   const auto range = chunks[chunk];
   const auto bytes = std::uint64_t{rows[range.end - 1].end - rows[range.begin].begin};
   return CompileResources{
    mmltk::common::math::checked_add(std::uint64_t{65536}, mmltk::common::math::checked_multiply(bytes, 64U, "annotation workspace overflow"), "annotation workspace overflow"), 0
   };
  }, [&](std::size_t chunk) { consume(execution->current_lane(), chunk); }, *parser_workspace);
 else
  parallel_for_range_indexed<std::size_t>(0, chunks.size(), static_cast<int>(workers), [&](int lane, std::size_t begin, std::size_t end) {
   for (auto chunk = begin; chunk < end; ++chunk) consume(static_cast<std::size_t>(lane), chunk);
  });
}
[[nodiscard]] std::uint16_t parse_objects365_shard(const std::string_view file_name) {
 const std::size_t patch = file_name.find("patch");
 if (patch == std::string_view::npos) { throw AnnotationDocumentRejected("Objects365 image path does not identify its patch"); }
 const std::size_t begin = patch + 5U;
 std::uint32_t value = 0U;
 const auto result = std::from_chars(file_name.data() + begin, file_name.data() + file_name.size(), value);
 const bool valid_delimiter =
  result.ptr != file_name.data() + begin && (result.ptr == file_name.data() + file_name.size() || *result.ptr == '/' || *result.ptr == '\\' || *result.ptr == '_' || *result.ptr == '.');
 if (result.ec != std::errc{} || !valid_delimiter || value > 50U) { throw AnnotationDocumentRejected("Objects365 image path has an invalid patch number"); }
 return static_cast<std::uint16_t>(value);
}
struct ParsedImage {
 std::uint64_t id = 0U;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::uint16_t shard = 0U;
};
[[nodiscard]] ParsedImage parse_image_object(simdjson::ondemand::object object, const BenchmarkDatasetSource source) try {
 ParsedImage parsed;
 bool have_id = false;
 bool have_width = false;
 bool have_height = false;
 bool have_filename = source != BenchmarkDatasetSource::kObjects365V2;
 for (auto field : object) {
  simdjson::ondemand::raw_json_string key = field.key();
  if (key == "id") {
   parsed.id = field.value().get_uint64().value();
   have_id = true;
  } else if (key == "width") {
   parsed.width = checked_cast<std::uint32_t>(field.value().get_uint64().value(), "image width overflow");
   have_width = true;
  } else if (key == "height") {
   parsed.height = checked_cast<std::uint32_t>(field.value().get_uint64().value(), "image height overflow");
   have_height = true;
  } else if (key == "file_name" && source == BenchmarkDatasetSource::kObjects365V2) {
   parsed.shard = parse_objects365_shard(field.value().get_string().value());
   have_filename = true;
  }
 }
 if (!have_id || !have_width || !have_height || !have_filename || parsed.width == 0U || parsed.height == 0U) { throw AnnotationDocumentRejected("benchmark image annotation is incomplete"); }
 return parsed;
} catch (const simdjson::simdjson_error& error) { reject_json_document(error); } catch (const std::overflow_error& error) {
 throw AnnotationDocumentRejected(error.what());
}
struct ParsedAnnotation {
 struct Segmentation {
  std::vector<std::vector<double>> polygons;
  std::vector<std::uint32_t> counts;
  std::string compressed_counts;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
 };
 std::uint64_t image_id = 0U;
 std::uint32_t category_id = 0U;
 std::array<double, 4> bbox{};
 // raw_json points into the mapped input, not the ephemeral ondemand value.
 // It is decoded synchronously after scalar admission and never escapes the callback.
 std::string_view segmentation_json;
 bool complete = false;
 bool has_bbox = false;
 bool has_mask = false;
 bool has_area = false;
 double area = 0.0;
 std::uint64_t id = 0U;
 std::uint8_t flags = kAnnotationCategory;
};
void parse_segmentation_object(simdjson::ondemand::object object, ParsedAnnotation::Segmentation* segmentation) {
 bool have_width = false;
 bool have_height = false;
 for (auto field : object) {
  const simdjson::ondemand::raw_json_string key = field.key();
  if (key == "size") {
   std::size_t coordinate = 0U;
   for (const std::uint64_t value : field.value().get_array()) {
    if (coordinate == 0U) {
     segmentation->height = checked_cast<std::uint32_t>(value, "segmentation height overflow");
     have_height = true;
    } else if (coordinate == 1U) {
     segmentation->width = checked_cast<std::uint32_t>(value, "segmentation width overflow");
     have_width = true;
    } else {
     throw std::runtime_error("benchmark segmentation size has too many values");
    }
    ++coordinate;
   }
   if (coordinate != 2U) { throw std::runtime_error("benchmark segmentation size is incomplete"); }
  } else if (key == "counts") {
   simdjson::ondemand::value counts = field.value();
   const simdjson::ondemand::json_type type = counts.type().value();
   if (type == simdjson::ondemand::json_type::string) {
    segmentation->compressed_counts = std::string(counts.get_string().value());
   } else if (type == simdjson::ondemand::json_type::array) {
    for (const std::uint64_t value : counts.get_array()) { segmentation->counts.push_back(checked_cast<std::uint32_t>(value, "segmentation run length overflow")); }
   } else {
    throw std::runtime_error("benchmark segmentation counts have an invalid type");
   }
  }
 }
 if (!have_width || !have_height || segmentation->width == 0U || segmentation->height == 0U || (segmentation->counts.empty() && segmentation->compressed_counts.empty())) {
  throw std::runtime_error("benchmark RLE segmentation is incomplete");
 }
}
void parse_segmentation(simdjson::ondemand::value value, ParsedAnnotation::Segmentation* segmentation) {
 const simdjson::ondemand::json_type type = value.type().value();
 if (type == simdjson::ondemand::json_type::array) {
  for (simdjson::ondemand::value polygon_value : value.get_array()) {
   std::vector<double> polygon;
   for (const double coordinate : polygon_value.get_array()) {
    if (!std::isfinite(coordinate)) { throw std::runtime_error("benchmark segmentation polygon has a non-finite coordinate"); }
    polygon.push_back(coordinate);
   }
   if (!polygon.empty()) {
    if (polygon.size() < 6U || (polygon.size() & 1U) != 0U) { throw std::runtime_error("benchmark segmentation polygon is malformed"); }
    segmentation->polygons.push_back(std::move(polygon));
   }
  }
 } else if (type == simdjson::ondemand::json_type::object) {
  parse_segmentation_object(value.get_object(), segmentation);
 } else if (type != simdjson::ondemand::json_type::null) {
  throw std::runtime_error("benchmark segmentation has an invalid type");
 }
}
[[nodiscard]] ParsedAnnotation parse_annotation_object(simdjson::ondemand::object object) {
 ParsedAnnotation parsed;
 bool have_image = false;
 bool have_category = false;
 bool have_bbox = false;
 for (auto field : object) {
  simdjson::ondemand::raw_json_string key = field.key();
  if (key == "image_id") {
   parsed.image_id = field.value().get_uint64().value();
   have_image = true;
  } else if (key == "category_id") {
   parsed.category_id = checked_cast<std::uint32_t>(field.value().get_uint64().value(), "category ID overflow");
   have_category = true;
  } else if (key == "bbox") {
   std::size_t coordinate = 0U;
   simdjson::ondemand::array coordinates = field.value().get_array();
   for (const double value : coordinates) {
    if (coordinate >= parsed.bbox.size()) { throw std::runtime_error("benchmark annotation bbox has too many coordinates"); }
    parsed.bbox[coordinate++] = value;
   }
   if (coordinate != parsed.bbox.size()) throw std::runtime_error("benchmark annotation bbox must have four coordinates");
   have_bbox = true;
  } else if (key == "id") {
   parsed.id = field.value().get_uint64().value();
   parsed.flags |= kAnnotationId;
  } else if (key == "area") {
   parsed.area = field.value().get_double().value();
   if (!std::isfinite(parsed.area) || parsed.area < 0.0) throw std::runtime_error("invalid annotation area");
   parsed.has_area = true;
  } else if (key == "iscrowd" || key == "ignore") {
   auto flag_value = field.value();
   const auto flag = flag_value.type().value() == simdjson::ondemand::json_type::boolean ? static_cast<std::uint64_t>(flag_value.get_bool().value()) : flag_value.get_uint64().value();
   if (flag > 1U) throw std::runtime_error("invalid annotation flag");
   if (flag) parsed.flags |= key == "iscrowd" ? kAnnotationCrowd : kAnnotationIgnore;
  } else if (key == "segmentation") {
   auto value = field.value();
   parsed.has_mask = value.type().value() != simdjson::ondemand::json_type::null;
   parsed.segmentation_json = value.raw_json().value();
  }
 }
 parsed.has_bbox = have_bbox;
 parsed.complete = have_image && have_category;
 return parsed;
}
void validate_numeric_categories(const PaddedMappedFile& file, std::span<const ByteRange> categories, std::vector<simdjson::ondemand::parser>& parsers, CompileExecution::Workspace* parser_workspace,
 NumericCategoryAdmission& admission, bool complete, mmltk::common::concurrency::CancellationObservation cancel_requested, CompileExecution* execution) try {
 std::mutex admission_mutex;
 parallel_object_array(file, categories, 1, parsers, parser_workspace, cancel_requested, [&](int, simdjson::ondemand::object object, std::uint64_t) {
  std::uint32_t id = 0U;
  std::string_view name;
  bool have_id = false;
  bool have_name = false;
  for (auto field : object) {
   simdjson::ondemand::raw_json_string key = field.key();
   if (key == "id") {
    id = checked_cast<std::uint32_t>(field.value().get_uint64().value(), "category ID overflow");
    have_id = true;
   } else if (key == "name") {
    name = field.value().get_string().value();
    have_name = true;
   }
  }
  // This call validates source category values; allocations still propagate.
  try {
   const std::lock_guard lock(admission_mutex);
   admission.observe(have_id ? std::optional(id) : std::nullopt, have_name ? std::optional(name) : std::nullopt);
  } catch (const std::runtime_error& error) { throw AnnotationDocumentRejected(error.what()); }
  throw_if_benchmark_cancelled(cancel_requested);
 }, execution);
 try {
  if (complete) admission.complete();
 } catch (const std::runtime_error& error) { throw AnnotationDocumentRejected(error.what()); }
} catch (const simdjson::simdjson_error& error) { reject_json_document(error); } catch (const std::overflow_error& error) {
 throw AnnotationDocumentRejected(error.what());
}
struct BoxCandidate {
 std::uint32_t image_index = 0U;
 NormalizedBox box;
 std::span<const RLEPair> mask_rle;
};
void decode_compressed_coco_counts(const std::string_view encoded, std::vector<std::uint32_t>& counts) {
 counts.clear();
 counts.reserve(encoded.size() / 2U);
 std::size_t cursor = 0U;
 while (cursor < encoded.size()) {
  std::int64_t value = 0;
  std::uint32_t shift = 0U;
  std::uint8_t chunk = 0U;
  bool more = false;
  do {
   if (cursor == encoded.size() || shift >= 60U) { throw std::runtime_error("compressed COCO mask run is truncated or overflowing"); }
   const int decoded = static_cast<unsigned char>(encoded[cursor++]) - 48;
   if (decoded < 0 || decoded > 0x3f) { throw std::runtime_error("compressed COCO mask run contains an invalid byte"); }
   chunk = static_cast<std::uint8_t>(decoded);
   value |= static_cast<std::int64_t>(chunk & 0x1fU) << shift;
   more = (chunk & 0x20U) != 0U;
   shift += 5U;
  } while (more);
  if ((chunk & 0x10U) != 0U) { value -= std::int64_t{1} << shift; }
  if (counts.size() > 2U) { value += counts[counts.size() - 2U]; }
  if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) { throw std::runtime_error("compressed COCO mask run is out of range"); }
  counts.push_back(static_cast<std::uint32_t>(value));
 }
}
// The half-open pixel span a continuous [low, high) interval covers, clamped to
// the raster extent. Rasterization samples pixel centres, so a coordinate
// rounds to the first centre at or past it; rows and columns are decided the
// same way and therefore decided here.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> covered_pixel_span(const double low, const double high, const std::uint32_t extent) noexcept {
 const auto boundary = [extent](const double coordinate) {
  if (coordinate <= 0.5) return std::uint32_t{0U};
  if (coordinate >= static_cast<double>(extent) + 0.5) return extent;
  return static_cast<std::uint32_t>(std::ceil(coordinate - 0.5));
 };
 return {boundary(low), boundary(high)};
}
struct MaskRowEvent {
 std::uint32_t y, x1, x2;
 int delta;
};
struct PolygonEdge {
 double x1, y1, x2, y2;
 std::uint32_t begin, end;
 std::size_t polygon;
};
struct SegmentationScratch {
 std::vector<RLEPair>* output = nullptr;
 std::size_t output_begin = 0;
 std::vector<double> intersections;
 std::vector<std::uint32_t> counts;
 std::vector<MaskRowEvent> events;
 std::vector<PolygonEdge> edges;
 std::vector<std::size_t> active;
 std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
 [[nodiscard]] std::size_t retained_bytes() const noexcept {
  return intersections.capacity() * sizeof(double) + counts.capacity() * sizeof(std::uint32_t) + events.capacity() * sizeof(MaskRowEvent) + edges.capacity() * sizeof(PolygonEdge) +
         active.capacity() * sizeof(std::size_t) + intervals.capacity() * sizeof(intervals.front());
 }
 void reset() noexcept {
  output = nullptr;
  intersections.clear();
  counts.clear();
  events.clear();
  edges.clear();
  active.clear();
  intervals.clear();
 }
};
void merge_mask_intervals(std::vector<std::pair<std::uint32_t, std::uint32_t>>& intervals) {
 std::ranges::sort(intervals);
 std::size_t count = 0;
 for (const auto interval : intervals) {
  if (interval.first == interval.second) continue;
  if (count && interval.first <= intervals[count - 1].second)
   intervals[count - 1].second = std::max(interval.second, intervals[count - 1].second);
  else
   intervals[count++] = interval;
 }
 intervals.resize(count);
}
std::span<const RLEPair> convert_coco_counts(std::span<const std::uint32_t> counts, dataset::MaskDimensions dimensions, SegmentationScratch& scratch) {
 const auto pixels = std::uint64_t{dimensions.width} * dimensions.height;
 auto& events = scratch.events;
 events.clear();
 const auto rectangle = [&](std::uint32_t x1, std::uint32_t x2, std::uint32_t y1, std::uint32_t y2) {
  if (x1 == x2 || y1 == y2) return;
  events.push_back({y1, x1, x2, 1});
  events.push_back({y2, x1, x2, -1});
 };
 std::uint64_t position = 0;
 bool foreground = false;
 for (auto count : counts) {
  if (count > pixels - position) throw std::runtime_error("COCO mask run exceeds its declared dimensions");
  const auto end = position + count;
  if (foreground && count) {
   auto x = static_cast<std::uint32_t>(position / dimensions.height);
   auto y = static_cast<std::uint32_t>(position % dimensions.height);
   auto remaining = std::uint64_t{count};
   if (y) {
    const auto length = std::min(remaining, std::uint64_t{dimensions.height - y});
    rectangle(x, x + 1, y, static_cast<std::uint32_t>(y + length));
    remaining -= length;
    ++x;
   }
   const auto columns = static_cast<std::uint32_t>(remaining / dimensions.height);
   rectangle(x, x + columns, 0, dimensions.height);
   x += columns;
   remaining %= dimensions.height;
   rectangle(x, x + 1, 0, static_cast<std::uint32_t>(remaining));
  }
  position = end;
  foreground = !foreground;
 }
 if (position != pixels) throw std::runtime_error("COCO mask runs do not cover their declared dimensions");
 std::ranges::sort(events, {}, &MaskRowEvent::y);
 // Endpoint counts describe the union during a row slab. Each input run adds
 // at most three rectangles; no foreground pixel is expanded.
 std::map<std::uint32_t, std::int64_t> endpoints;
 auto& output = *scratch.output;
 dataset::MaskRunEmitter emitter(output, scratch.output_begin, dimensions.width, nullptr, "COCO mask run overflow", "COCO mask run overflow");
 for (std::size_t next = 0; next < events.size();) {
  const auto y = events[next].y;
  do {
   const auto event = events[next++];
   const auto add = [&](auto x, auto delta) {
    auto& value = endpoints[x];
    value += delta;
    if (!value) endpoints.erase(x);
   };
   add(event.x1, event.delta);
   add(event.x2, -event.delta);
  } while (next < events.size() && events[next].y == y);
  if (next == events.size()) break;
  scratch.intervals.clear();
  std::int64_t coverage = 0;
  std::uint32_t start = 0;
  for (const auto [x, delta] : endpoints) {
   const auto previous = coverage;
   coverage += delta;
   if (!previous && coverage) start = x;
   if (previous && !coverage) scratch.intervals.emplace_back(start, x);
  }
  emitter.slab(y, events[next].y, scratch.intervals);
 }
 return std::span(output).subspan(scratch.output_begin);
}
std::span<const RLEPair> rasterize_coco_polygons(const std::vector<std::vector<double>>& polygons, dataset::MaskDimensions dimensions, SegmentationScratch& scratch) {
 auto& edges = scratch.edges;
 edges.clear();
 for (std::size_t polygon = 0; polygon < polygons.size(); ++polygon) {
  const auto& coordinates = polygons[polygon];
  for (std::size_t point = 0; point < coordinates.size(); point += 2) {
   const auto next = (point + 2) % coordinates.size();
   const auto y1 = coordinates[point + 1], y2 = coordinates[next + 1];
   const auto [begin, end] = covered_pixel_span(std::min(y1, y2), std::max(y1, y2), dimensions.height);
   if (begin != end) edges.push_back({coordinates[point], y1, coordinates[next], y2, begin, end, polygon});
  }
 }
 std::ranges::sort(edges, {}, &PolygonEdge::begin);
 scratch.active.clear();
 auto& output = *scratch.output;
 dataset::MaskRunEmitter emitter(output, scratch.output_begin, dimensions.width, nullptr, "COCO mask run overflow", "COCO mask run overflow");
 std::size_t next = 0;
 auto y = edges.empty() ? dimensions.height : edges.front().begin;
 while (next < edges.size() || !scratch.active.empty()) {
  bool changed = std::erase_if(scratch.active, [&](auto i) { return edges[i].end <= y; }) != 0;
  while (next < edges.size() && edges[next].begin <= y) {
   scratch.active.push_back(next++);
   changed = true;
  }
  if (scratch.active.empty()) {
   if (next == edges.size()) break;
   y = edges[next].begin;
   continue;
  }
  // Polygon membership changes only at edge events, independently of crossing
  // order inside each polygon. Vertical crossings are constant to that event.
  if (changed) std::ranges::sort(scratch.active, [&](auto a, auto b) { return edges[a].polygon < edges[b].polygon; });
  auto slab_end = next < edges.size() ? edges[next].begin : dimensions.height;
  for (const auto i : scratch.active) {
   slab_end = std::min(slab_end, edges[i].end);
   if (edges[i].x1 != edges[i].x2) slab_end = y + 1;
  }
  scratch.intervals.clear();
  for (std::size_t first = 0; first < scratch.active.size();) {
   const auto polygon = edges[scratch.active[first]].polygon;
   scratch.intersections.clear();
   do {
    const auto& edge = edges[scratch.active[first++]];
    const double scan_y = static_cast<double>(y) + 0.5;
    const double denominator = edge.y2 - edge.y1;
    const double numerator = (scan_y - edge.y1) * (edge.x2 - edge.x1);
    double intersection = edge.x1 + numerator / denominator;
    if (!std::isfinite(denominator) || !std::isfinite(numerator) || !std::isfinite(intersection)) {
     const double fraction = (scan_y * 0.5 - edge.y1 * 0.5) / (edge.y2 * 0.5 - edge.y1 * 0.5);
     intersection = std::lerp(edge.x1, edge.x2, fraction);
    }
    if (!std::isfinite(intersection)) throw std::runtime_error("COCO polygon intersection is not representable");
    scratch.intersections.push_back(intersection);
   } while (first < scratch.active.size() && edges[scratch.active[first]].polygon == polygon);
   std::ranges::sort(scratch.intersections);
   for (std::size_t pair = 0; pair + 1 < scratch.intersections.size(); pair += 2)
    scratch.intervals.push_back(covered_pixel_span(scratch.intersections[pair], scratch.intersections[pair + 1], dimensions.width));
  }
  merge_mask_intervals(scratch.intervals);
  emitter.slab(y, slab_end, scratch.intervals);
  y = slab_end;
 }
 return std::span(output).subspan(scratch.output_begin);
}
[[nodiscard]] std::span<const RLEPair> encode_segmentation(const ParsedAnnotation::Segmentation& segmentation, const ParsedImage& image, SegmentationScratch& scratch) {
 if (segmentation.polygons.empty() && segmentation.counts.empty() && segmentation.compressed_counts.empty()) return {};
 const dataset::MaskDimensions dimensions{image.width, image.height};
 if (!segmentation.polygons.empty()) return rasterize_coco_polygons(segmentation.polygons, dimensions, scratch);
 if (segmentation.width != image.width || segmentation.height != image.height) throw std::runtime_error("COCO mask dimensions disagree with image metadata");
 std::span<const std::uint32_t> counts = segmentation.counts;
 if (counts.empty()) {
  decode_compressed_coco_counts(segmentation.compressed_counts, scratch.counts);
  counts = scratch.counts;
 }
 return convert_coco_counts(counts, dimensions, scratch);
}
[[nodiscard]] std::span<const RLEPair> materialize_segmentation(
 const ParsedAnnotation& annotation, const ParsedImage& image, const PaddedMappedFile& file, simdjson::ondemand::parser& parser, SegmentationScratch& scratch) {
 if (!annotation.has_mask) return {};
 scratch.output_begin = scratch.output->size();
 const auto raw = annotation.segmentation_json;
 const auto offset = static_cast<std::size_t>(raw.data() - file.data());
 simdjson::ondemand::document document = parser.iterate(simdjson::padded_string_view(raw.data(), raw.size(), file.capacity_from(offset)));
 ParsedAnnotation::Segmentation segmentation;
 parse_segmentation(document.get_value(), &segmentation);
 return encode_segmentation(segmentation, image, scratch);
}
[[nodiscard]] bool store_normalized_coordinates(NormalizedBox& box, const std::array<double, 4>& coordinates) noexcept {
 constexpr double limit = std::numeric_limits<float>::max();
 for (const double value : coordinates) {
  if (!std::isfinite(value) || value < -limit || value > limit) return false;
 }
 box.x1 = static_cast<float>(coordinates[0]);
 box.y1 = static_cast<float>(coordinates[1]);
 box.x2 = static_cast<float>(coordinates[2]);
 box.y2 = static_cast<float>(coordinates[3]);
 return box.x2 > box.x1 && box.y2 > box.y1;
}
[[nodiscard]] std::optional<BoxCandidate> normalize_coco_box(const ParsedAnnotation& annotation, const std::vector<ParsedImage>& images,
 const std::unordered_map<std::uint64_t, std::uint32_t>& image_lookup, const CategoryLookup& categories, AnnotationRejectCounts* rejected, const PaddedMappedFile& file,
 simdjson::ondemand::parser& segmentation_parser, SegmentationScratch& scratch) {
 if (!annotation.complete) {
  ++rejected->malformed_records;
  return std::nullopt;
 }
 if (annotation.category_id >= categories.target_by_id.size() || categories.target_by_id[annotation.category_id] < 0) {
  ++rejected->unmapped_categories;
  return std::nullopt;
 }
 const auto image = image_lookup.find(annotation.image_id);
 if (image == image_lookup.end()) {
  ++rejected->unknown_images;
  return std::nullopt;
 }
 const ParsedImage& metadata = images[image->second];
 std::span<const RLEPair> mask;
 std::uint64_t foreground = 0;
 double x1 = annotation.bbox[0], y1 = annotation.bbox[1];
 double x2 = x1 + annotation.bbox[2], y2 = y1 + annotation.bbox[3];
 if (!annotation.has_bbox) {
  mask = materialize_segmentation(annotation, metadata, file, segmentation_parser, scratch);
  if (mask.empty()) {
   ++rejected->malformed_records;
   return std::nullopt;
  }
  const auto bounds = dataset::row_major_mask_bounds(mask, {metadata.width, metadata.height}, annotation.has_area ? nullptr : &foreground);
  x1 = bounds.min_x;
  y1 = bounds.min_y;
  x2 = bounds.max_x;
  y2 = bounds.max_y;
 }
 if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) {
  ++rejected->malformed_records;
  return std::nullopt;
 }
 if (x2 <= x1 || y2 <= y1) {
  ++rejected->degenerate_boxes;
  return std::nullopt;
 }
 BoxCandidate result;
 result.image_index = image->second;
 if (!store_normalized_coordinates(result.box, {x1 / metadata.width, y1 / metadata.height, x2 / metadata.width, y2 / metadata.height})) {
  ++rejected->malformed_records;
  return std::nullopt;
 }
 if (annotation.has_bbox) mask = materialize_segmentation(annotation, metadata, file, segmentation_parser, scratch);
 result.box.class_id = static_cast<std::uint8_t>(categories.target_by_id[annotation.category_id]);
 result.box.flags = annotation.flags | (annotation.has_mask ? kAnnotationMask : 0U);
 result.box.annotation_id = annotation.id;
 result.box.source_category_id = annotation.category_id;
 if (!annotation.has_area && annotation.has_bbox && annotation.has_mask)
  for (const auto& run : mask) foreground += run.length;
 result.box.original_area = annotation.has_area ? annotation.area : annotation.has_mask ? static_cast<double>(foreground) : (x2 - x1) * (y2 - y1);
 if (!std::isfinite(result.box.original_area)) {
  ++rejected->malformed_records;
  return std::nullopt;
 }
 result.mask_rle = mask;
 return result;
}
// Seeds a normalized index result with its provenance and the accumulated per-worker reject counts.
[[nodiscard]] NormalizedAnnotationBuilder begin_normalized_index_result(
 const BenchmarkDatasetSource source, const std::string& split, std::string annotation_sha256, const std::span<const AnnotationRejectCounts> worker_rejected) {
 NormalizedAnnotationBuilder result;
 result.source = source;
 result.split = split;
 result.annotation_sha256 = std::move(annotation_sha256);
 for (const AnnotationRejectCounts& local : worker_rejected) {
  mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
   result.rejected.*Declaration::pointer = mmltk::common::math::checked_add(result.rejected.*Declaration::pointer, local.*Declaration::pointer, "annotation rejection count overflow");
  });
 }
 return result;
}
// Shared payload of the `benchmark.annotations.indexed` trace event: provenance, the record counts every parser
// produces, and the reject tally. Parser-specific fields are added by the caller to the returned object, which is
// only built from inside the trace sink's lambda so untraced runs still pay nothing.
template <class Index>
[[nodiscard]] nlohmann::json normalized_index_trace_json(const Index& index) {
 return nlohmann::json{
  {"source", benchmark_source_name(index.source)}, {"split", index.split}, {"images", index.images.size()}, {"boxes", index.boxes.size()}, {"raw_records", index.rejected.raw_records},
  {"unmapped", index.rejected.unmapped_categories}, {"malformed", index.rejected.malformed_records}, {"degenerate", index.rejected.degenerate_boxes}, {"duplicates", index.rejected.duplicate_boxes}
 };
}
struct AnnotationChunk {
 struct Entry {
  std::uint32_t image;
  NormalizedBox box;
 };
 std::vector<Entry> boxes;
 std::vector<RLEPair> runs;
 void append(BoxCandidate candidate, std::uint64_t ordinal) {
  candidate.box.source_ordinal = ordinal;
  candidate.box.mask_rle_offset = candidate.mask_rle.empty() ? runs.size() : static_cast<std::size_t>(candidate.mask_rle.data() - runs.data());
  candidate.box.mask_rle_pairs = checked_cast<std::uint32_t>(candidate.mask_rle.size(), "normalized mask run count overflow");
  boxes.push_back({candidate.image_index, candidate.box});
 }
};
NormalizedAnnotationIndex assemble_annotation_chunks(NormalizedAnnotationBuilder metadata, std::span<const ParsedImage> images, std::vector<AnnotationChunk>& chunks, bool keep_empty,
 mmltk::common::concurrency::CancellationObservation cancellation, CompileExecution* execution, const BenchmarkTraceSink& trace) {
 struct Reference {
  std::size_t chunk, box;
 };
 std::size_t total = 0, runs = 0;
 for (const auto& chunk : chunks) {
  total = mmltk::common::math::checked_add(total, chunk.boxes.size(), "normalized box count overflow");
  runs = mmltk::common::math::checked_add(runs, chunk.runs.size(), "normalized run count overflow");
 }
 NormalizedAnnotationIndex output;
 const auto assemble = [&](std::size_t) {
  std::vector<Reference> ordered;
  ordered.reserve(total);
  for (std::size_t chunk = 0; chunk < chunks.size(); ++chunk)
   for (std::size_t box = 0; box < chunks[chunk].boxes.size(); ++box) ordered.push_back({chunk, box});
  const auto entry = [&](Reference ref) -> const AnnotationChunk::Entry& { return chunks[ref.chunk].boxes[ref.box]; };
  std::ranges::sort(ordered, [&](Reference a, Reference b) { return std::pair{entry(a).image, entry(a).box.source_ordinal} < std::pair{entry(b).image, entry(b).box.source_ordinal}; });
  NormalizedAnnotationAssembler result(std::move(metadata), images.size(), total, runs, cancellation);
  std::size_t cursor = 0;
  for (std::size_t image = 0; image < images.size(); ++image) {
   throw_if_benchmark_cancelled(cancellation);
   const bool populated = cursor < ordered.size() && entry(ordered[cursor]).image == image;
   if (keep_empty || populated) result.begin_image({images[image].id, 0, 0, images[image].width, images[image].height, images[image].shard, 0});
   while (cursor < ordered.size() && entry(ordered[cursor]).image == image) {
    const auto ref = ordered[cursor++];
    auto box = entry(ref).box;
    const auto mask = std::span(chunks[ref.chunk].runs).subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs);
    result.append_box(box, mask);
   }
  }
  output = result.finish(trace);
 };
 const auto workspace = mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(std::uint64_t{total}, std::uint64_t{sizeof(Reference)}, "normalized ordering workspace overflow"),
  std::uint64_t{65536}, "normalized ordering workspace overflow");
 if (execution)
  execution->run(CompileTaskKind::Metadata, {workspace, 0}, assemble);
 else
  assemble(0);
 return output;
}
class CsvFields final {
 struct Field {
  std::size_t begin, length;
  bool decoded;
 };
 std::vector<Field> positions_;
 std::vector<std::string_view> fields_;
 std::string decoded_;

public:
 std::span<const std::string_view> parse(std::string_view line) {
  positions_.clear();
  fields_.clear();
  decoded_.clear();
  std::size_t begin = 0;
  while (begin <= line.size()) {
   if (begin < line.size() && line[begin] == '"') {
    const auto offset = decoded_.size();
    auto cursor = begin + 1;
    bool closed = false;
    while (cursor < line.size()) {
     if (line[cursor] != '"') {
      decoded_.push_back(line[cursor++]);
      continue;
     }
     if (cursor + 1 < line.size() && line[cursor + 1] == '"') {
      decoded_.push_back('"');
      cursor += 2;
     } else {
      ++cursor;
      closed = true;
      break;
     }
    }
    if (!closed || (cursor < line.size() && line[cursor] != ',')) throw std::runtime_error("malformed quoted CSV field in benchmark annotations");
    positions_.push_back({offset, decoded_.size() - offset, true});
    if (cursor == line.size()) break;
    begin = cursor + 1;
   } else {
    const auto comma = line.find(',', begin);
    const auto end = comma == std::string_view::npos ? line.size() : comma;
    positions_.push_back({begin, end - begin, false});
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
   }
  }
  // Resolve views after the last possible decoded-buffer relocation.
  fields_.reserve(positions_.size());
  for (const auto& field : positions_) fields_.emplace_back((field.decoded ? decoded_.data() : line.data()) + field.begin, field.length);
  return fields_;
 }
};
[[nodiscard]] std::uint64_t parse_hex_image_id(const std::string_view value) {
 std::uint64_t image_id = 0U;
 const auto parsed = std::from_chars(value.data(), value.data() + value.size(), image_id, 16);
 if (value.size() != 16U || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) { throw std::runtime_error("Open Images annotation has an invalid image ID"); }
 return image_id;
}
[[nodiscard]] double parse_csv_double(const std::string_view value) {
 double parsed_value = 0.0;
 const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
 if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(parsed_value)) { throw std::runtime_error("Open Images annotation has an invalid coordinate"); }
 return parsed_value;
}
struct OpenImagesCandidate {
 std::uint64_t image_id = 0U;
 NormalizedBox box;
};
[[nodiscard]] std::optional<OpenImagesCandidate> parse_open_images_line(
 const std::string_view line, const std::unordered_map<std::string_view, std::uint8_t>& mappings, AnnotationRejectCounts* rejected) {
 ++rejected->raw_records;
 try {
  std::array<std::string_view, 8> fields{};
  std::size_t begin = 0U;
  for (std::size_t field = 0U; field < fields.size(); ++field) {
   const std::size_t comma = line.find(',', begin);
   const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
   if (begin == end || line[begin] == '"' || (field + 1U < fields.size() && comma == std::string_view::npos)) { throw std::runtime_error("Open Images bounding-box row has invalid fixed columns"); }
   fields[field] = line.substr(begin, end - begin);
   begin = comma == std::string_view::npos ? line.size() : comma + 1U;
  }
  const auto category = mappings.find(fields[2]);
  if (category == mappings.end()) {
   ++rejected->unmapped_categories;
   return std::nullopt;
  }
  const double x1 = parse_csv_double(fields[4]);
  const double x2 = parse_csv_double(fields[5]);
  const double y1 = parse_csv_double(fields[6]);
  const double y2 = parse_csv_double(fields[7]);
  if (x2 <= x1 || y2 <= y1) {
   ++rejected->degenerate_boxes;
   return std::nullopt;
  }
  OpenImagesCandidate candidate;
  candidate.image_id = parse_hex_image_id(fields[0]);
  if (!store_normalized_coordinates(candidate.box, {x1, y1, x2, y2})) {
   ++rejected->malformed_records;
   return std::nullopt;
  }
  candidate.box.class_id = category->second;
  candidate.box.flags = kAnnotationCategory;
  // Standard Open Images extras: IsOccluded, IsTruncated, IsGroupOf.
  for (std::size_t column = 8U; column <= 10U && begin < line.size(); ++column) {
   const auto comma = line.find(',', begin);
   const auto end = comma == std::string_view::npos ? line.size() : comma;
   if (column == 10U) {
    const auto group = parse_csv_double(line.substr(begin, end - begin));
    if (group != 0.0 && group != 1.0) throw std::runtime_error("invalid Open Images IsGroupOf flag");
    if (group == 1.0) candidate.box.flags |= kAnnotationCrowd;
   }
   begin = comma == std::string_view::npos ? line.size() : comma + 1U;
  }
  candidate.box.source_category_id = encode_open_images_category(fields[2]);
  // Open Images dimensions arrive with the cached JPEG. Resolve bbox area then.
  candidate.box.original_area = (x2 - x1) * (y2 - y1);
  if (!std::isfinite(candidate.box.original_area)) {
   ++rejected->malformed_records;
   return std::nullopt;
  }
  return candidate;
 } catch (const std::exception&) {
  ++rejected->malformed_records;
  return std::nullopt;
 }
}
[[nodiscard]] std::vector<ByteRange> newline_ranges(const PaddedMappedFile& file, int requested_workers, bool bounded = false, bool header = true) {
 std::size_t begin = 0;
 if (header) {
  const auto* newline = static_cast<const char*>(std::memchr(file.data(), '\n', file.size()));
  if (!newline) throw std::runtime_error("benchmark CSV annotation file has no header line");
  begin = static_cast<std::size_t>(newline - file.data()) + 1;
 }
 const auto bytes = file.size() - begin;
 const auto workers = static_cast<std::size_t>(effective_worker_count(requested_workers, bytes));
 const auto step = bounded ? std::size_t{262144} : std::max<std::size_t>(1, bytes / workers + (bytes % workers != 0));
 std::vector<ByteRange> ranges;
 do {
  auto end = begin + std::min(step, file.size() - begin);
  while (end < file.size() && file.data()[end - 1] != '\n') ++end;
  ranges.push_back({begin, end});
  begin = end;
 } while (begin < file.size());
 return ranges;
}
void for_each_csv_line(
 const PaddedMappedFile& file, const ByteRange range, mmltk::common::concurrency::CancellationObservation cancel_requested, const std::function<void(std::string_view)>& callback) {
 std::size_t begin = range.begin;
 std::uint64_t line_count = 0U;
 while (begin < range.end) {
  std::size_t end = begin;
  while (end < range.end && file.data()[end] != '\n') { ++end; }
  std::size_t content_end = end;
  if (content_end > begin && file.data()[content_end - 1U] == '\r') { --content_end; }
  if (content_end > begin) { callback(std::string_view(file.data() + begin, content_end - begin)); }
  begin = end < range.end ? end + 1U : end;
  if ((++line_count & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
 }
}
void validate_open_images_classes(const std::filesystem::path& path, const std::span<const StringCategoryMapping> mappings, const AnnotationParseOptions& options) {
 PaddedMappedFile classes(path);
 std::unordered_map<std::string_view, std::string_view> expected;
 expected.reserve(mappings.size());
 for (const StringCategoryMapping& mapping : mappings) {
  if (!expected.emplace(mapping.source_id, mapping.expected_name).second || mapping.target_id >= coco80_class_names().size()) {
   throw std::runtime_error("Open Images fixed category mapping contains a duplicate");
  }
 }
 std::unordered_set<std::string_view> matched;
 for (const auto range : newline_ranges(classes, 1, true, false)) {
  const auto consume = [&](std::size_t) {
   CsvFields scratch;
   std::size_t rows = 0;
   for_each_csv_line(classes, range, options.cancel_requested, [&](const std::string_view line) {
    const auto fields = scratch.parse(line);
    if (fields.size() < 2U) throw std::runtime_error("Open Images class description row is malformed");
    const auto found = expected.find(fields[0]);
    if (found != expected.end() && (fields[1] != found->second || !matched.emplace(found->first).second)) throw std::runtime_error("Open Images class metadata disagrees with its fixed mapping");
    if (options.execution && ++rows % 64 == 0) options.execution->cooperate();
   });
  };
  if (options.execution)
   options.execution->run(CompileTaskKind::Metadata, {mmltk::common::math::checked_multiply(std::uint64_t{range.end - range.begin}, 64U, "Open Images class workspace overflow"), 0}, consume);
  else
   consume(0);
 }
 if (matched.size() != expected.size()) { throw std::runtime_error("Open Images class metadata is missing a required mapped class"); }
}
void validate_index_layout(const NormalizedIndexHeader& header, const std::size_t file_size) {
 const auto layout = [&] {
  try {
   return normalized_index_layout(header.image_count, header.box_count, header.mask_rle_pair_count);
  } catch (const std::overflow_error& error) {
   // Only arithmetic read from the persisted header is a cache miss. Capacity,
   // mapping I/O and callback failures still propagate from the loading owner.
   throw std::runtime_error(error.what());
  }
 }();
 if (header.magic != kNormalizedIndexMagic || header.version != kNormalizedAnnotationIndexVersion || header.source > static_cast<std::uint8_t>(BenchmarkDatasetSource::kOpenImagesV7) ||
     header.image_count == 0U || header.image_offset != layout.image_offset || header.box_offset != layout.box_offset || header.mask_rle_offset != layout.mask_rle_offset ||
     header.total_file_size != layout.total_file_size || header.total_file_size != file_size || !std::ranges::all_of(header.reserved0, [](const std::uint8_t value) { return value == 0U; }) ||
     !std::ranges::all_of(header.reserved, [](const std::uint8_t value) { return value == 0U; })) {
  throw std::runtime_error("normalized benchmark annotation index header is invalid");
 }
}
void validate_box_value(const NormalizedBox& box, BenchmarkDatasetSource source) {
 if (box.class_id >= coco80_class_names().size() || !std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1 ||
     !std::isfinite(box.original_area) || box.original_area < 0.0 || (box.flags & ~kAnnotationFlags) != 0U || (box.flags & kAnnotationCategory) == 0U ||
     ((box.flags & kAnnotationMask) == 0U && box.mask_rle_pairs != 0U) || ((box.flags & kAnnotationId) == 0U && box.annotation_id != 0U) ||
     ((box.flags & kAnnotationCategory) == 0U && box.source_category_id != 0U) || !std::ranges::all_of(box.reserved, [](std::uint8_t value) { return value == 0; }))
  throw std::runtime_error("normalized benchmark box record is invalid");
 if (source == BenchmarkDatasetSource::kOpenImagesV7 && !valid_open_images_category(box.source_category_id)) throw std::runtime_error("invalid normalized Open Images source category");
}
std::uint64_t validate_run_value(RLEPair pair, std::uint64_t pixels, std::uint64_t previous_end) {
 const auto end = std::uint64_t{pair.start} + pair.length;
 if (!pair.length || pair.start < previous_end || end > pixels) throw std::runtime_error("normalized benchmark mask record is invalid");
 return end;
}
template <class Index>
void validate_normalized_records(const Index& index, mmltk::common::concurrency::CancellationObservation cancel_requested, bool metadata_only = false, bool images_admitted = false,
 const std::function<void(std::size_t)>& ready = {}) {
 std::uint64_t expected_first = 0U;
 std::uint64_t expected_mask_offset = 0U;
 std::uint64_t previous_id = 0U;
 bool first = true;
 for (std::size_t image_index = 0U; image_index < index.images.size(); ++image_index) {
  if (ready || (image_index & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
  const NormalizedImage& image = index.images[image_index];
  if (!images_admitted &&
      ((!first && image.source_image_id <= previous_id) || image.first_box != expected_first || image.first_box > index.boxes.size() || image.box_count > index.boxes.size() - image.first_box ||
       (index.source != BenchmarkDatasetSource::kOpenImagesV7 && (image.width == 0U || image.height == 0U)) || image.reserved != 0U)) {
   throw std::runtime_error("normalized benchmark image record is invalid at index " + std::to_string(image_index) + " (source_image_id=" + std::to_string(image.source_image_id) +
                            ", previous_id=" + std::to_string(previous_id) + ", first_box=" + std::to_string(image.first_box) + ", expected_first_box=" + std::to_string(expected_first) +
                            ", box_count=" + std::to_string(image.box_count) + ", width=" + std::to_string(image.width) + ", height=" + std::to_string(image.height) + ")");
  }
  first = false;
  previous_id = image.source_image_id;
  expected_first += image.box_count;
  if (metadata_only) {
   if (ready) ready(image_index);
   continue;
  }
  for (std::uint64_t box_index = image.first_box; box_index < image.first_box + image.box_count; ++box_index) {
   const NormalizedBox& box = index.boxes[checked_cast<std::size_t>(box_index, "box index overflow")];
   validate_box_value(box, index.source);
   if (box.mask_rle_offset != expected_mask_offset || expected_mask_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - expected_mask_offset)
    throw std::runtime_error("normalized benchmark box record is invalid");
   const std::uint64_t mask_pixels = static_cast<std::uint64_t>(image.width) * image.height;
   std::uint64_t previous_end = 0U;
   for (std::uint64_t pair_index = box.mask_rle_offset; pair_index < box.mask_rle_offset + box.mask_rle_pairs; ++pair_index) {
    const RLEPair pair = index.mask_rle_pairs[checked_cast<std::size_t>(pair_index, "normalized mask offset overflow")];
    previous_end = validate_run_value(pair, mask_pixels, previous_end);
   }
   expected_mask_offset += box.mask_rle_pairs;
  }
  if (ready) ready(image_index);
 }
 throw_if_benchmark_cancelled(cancel_requested);
 if (!images_admitted && expected_first != index.boxes.size()) { throw std::runtime_error("normalized benchmark box records are not fully referenced"); }
 if (!metadata_only && expected_mask_offset != index.mask_rle_pairs.size()) { throw std::runtime_error("normalized benchmark mask records are not fully referenced"); }
}
// Slice admission deliberately does not require persisted sorted image IDs.
template <class Index>
std::span<const NormalizedBox> image_boxes(const Index& index, std::size_t position) {
 if (position >= index.images.size()) throw std::out_of_range("normalized image position is invalid");
 const auto& image = index.images[position];
 if (image.first_box > index.boxes.size() || image.box_count > index.boxes.size() - image.first_box) throw std::runtime_error("normalized image slice box range is invalid");
 return std::span(index.boxes).subspan(static_cast<std::size_t>(image.first_box), image.box_count);
}
template <class Index>
std::size_t slice_run_count(const Index& index, std::span<const NormalizedBox> boxes, mmltk::common::concurrency::CancellationObservation cancellation) {
 std::size_t count = 0;
 for (std::size_t i = 0; i < boxes.size(); ++i) {
  if ((i & 1023U) == 0) throw_if_benchmark_cancelled(cancellation);
  const auto& box = boxes[i];
  if (box.mask_rle_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - box.mask_rle_offset)
   throw std::runtime_error("normalized image slice mask range is invalid");
  if (box.mask_rle_pairs > std::numeric_limits<std::size_t>::max() / sizeof(RLEPair) - count) throw std::overflow_error("normalized image slice mask count overflow");
  count += box.mask_rle_pairs;
 }
 return count;
}
// Append and forward compaction share the complete admitted slice transfer.
// memmove handles overlapping runs, and each box is captured before overwriting it.
template <class Index>
void transfer_image_slice(NormalizedAnnotationBuilder& destination, const Index& source, std::size_t position, std::size_t image_destination, std::size_t box_destination, std::size_t& run_destination,
 mmltk::common::concurrency::CancellationObservation cancellation) {
 auto image = source.images[position];
 const auto first = image.first_box;
 image.first_box = box_destination;
 for (std::size_t i = 0; i < image.box_count; ++i) {
  if ((i & 1023U) == 0) throw_if_benchmark_cancelled(cancellation);
  auto box = source.boxes[first + i];
  for (std::size_t offset = 0; offset < box.mask_rle_pairs;) {
   throw_if_benchmark_cancelled(cancellation);
   const auto count = std::min<std::size_t>(65536, box.mask_rle_pairs - offset);
   const auto* input = source.mask_rle_pairs.data() + box.mask_rle_offset + offset;
   if (static_cast<const void*>(&destination) != static_cast<const void*>(&source)) {
    destination.mask_rle_pairs.insert(destination.mask_rle_pairs.end(), input, input + count);
   } else {
    auto* target = destination.mask_rle_pairs.data() + run_destination + offset;
    if (target != input) std::memmove(target, input, count * sizeof(RLEPair));
   }
   offset += count;
  }
  box.mask_rle_offset = run_destination;
  run_destination += box.mask_rle_pairs;
  if (static_cast<const void*>(&destination) != static_cast<const void*>(&source))
   destination.boxes.push_back(box);
  else
   destination.boxes[box_destination + i] = box;
 }
 if (static_cast<const void*>(&destination) != static_cast<const void*>(&source))
  destination.images.push_back(image);
 else
  destination.images[image_destination] = image;
}
}  // namespace
NormalizedAnnotationIndex seal_normalized_annotation_metadata(NormalizedAnnotationBuilder&& builder) {
 auto storage = std::make_shared<const NormalizedAnnotationBuilder>(std::move(builder));
 auto owner = std::make_shared<NormalizedAnnotationBacking>();
 owner->source = storage->source;
 owner->storage = storage;
 owner->images = storage->images;
 owner->boxes = storage->boxes;
 owner->runs = storage->mask_rle_pairs;
 NormalizedAnnotationIndex result;
 static_cast<NormalizedAnnotationMetadata&>(result) = *storage;
 result.images = owner->images;
 result.boxes = owner->boxes;
 result.mask_rle_pairs = owner->runs;
 result.backing = std::move(owner);
 return result;
}
struct NormalizedAnnotationAssembler::Mapped {
 BenchmarkMappedArtifact artifact;
 std::span<NormalizedImage> images;
 std::span<NormalizedBox> boxes;
 std::span<RLEPair> runs;
 std::size_t image_count = 0, box_count = 0, run_count = 0;
 Mapped(detail::StorageReservationPool& storage, const std::filesystem::path& path, std::size_t bytes) : artifact(storage, path, bytes, "normalized annotation assembly") {}
};
NormalizedAnnotationAssembler::NormalizedAnnotationAssembler(
 NormalizedAnnotationMetadata metadata, mmltk::common::concurrency::CancellationObservation cancellation, const std::filesystem::path& staging_directory, detail::StorageReservationPool* storage)
    : cancellation_(cancellation), staging_directory_(staging_directory), storage_(storage) {
 throw_if_benchmark_cancelled(cancellation_);
 static_cast<NormalizedAnnotationMetadata&>(builder_) = std::move(metadata);
}
NormalizedAnnotationAssembler::NormalizedAnnotationAssembler(NormalizedAnnotationMetadata metadata, std::size_t images, std::size_t boxes, std::size_t runs,
 mmltk::common::concurrency::CancellationObservation cancellation, const std::filesystem::path& staging_directory, detail::StorageReservationPool* storage)
    : NormalizedAnnotationAssembler(std::move(metadata), cancellation, staging_directory, storage) {
 prepare(images, boxes, runs);
}
void NormalizedAnnotationAssembler::prepare(std::size_t images, std::size_t boxes, std::size_t runs) {
 throw_if_benchmark_cancelled(cancellation_);
 if (!staging_directory_.empty()) {
  const auto layout = normalized_index_layout(images, boxes, runs);
  const auto bytes = checked_cast<std::size_t>(layout.total_file_size, "normalized staging overflow");
  std::filesystem::create_directories(staging_directory_);
  detail::StorageReservationPool reservations(staging_directory_, {}, storage_);
  auto owner = std::make_shared<Mapped>(reservations, staging_directory_ / ".normalized-assembly", bytes);
  auto* data = static_cast<std::byte*>(owner->artifact.map_writable());
  if (bytes) {
   owner->images = {reinterpret_cast<NormalizedImage*>(data + layout.image_offset), images};
   owner->boxes = {reinterpret_cast<NormalizedBox*>(data + layout.box_offset), boxes};
   owner->runs = {reinterpret_cast<RLEPair*>(data + layout.mask_rle_offset), runs};
  }
  mapped_ = std::move(owner);
 } else {
  builder_.images.reserve(images);
  builder_.boxes.reserve(boxes);
  builder_.mask_rle_pairs.reserve(runs);
 }
 prepared_ = true;
}
namespace {
void validate_assembled_image(const NormalizedImage& image, BenchmarkDatasetSource source, std::optional<std::uint64_t> previous) {
 if ((previous && image.source_image_id <= *previous) || image.reserved || (source != BenchmarkDatasetSource::kOpenImagesV7 && (!image.width || !image.height)))
  throw std::runtime_error("normalized benchmark image record is invalid");
}
void validate_assembled_box(NormalizedBox& box, std::size_t runs, BenchmarkDatasetSource source) {
 box.mask_rle_pairs = checked_cast<std::uint32_t>(runs, "normalized mask run count overflow");
 validate_box_value(box, source);
}
}  // namespace
void NormalizedAnnotationAssembler::gather(std::span<const Slice> slices, CompileExecution* execution, detail::FileWorkQueue* files, const CompileAllowance& allowance, const BenchmarkTraceSink& trace) {
 if (finished_ || incomplete_gather_ || (mapped_ ? mapped_->image_count : builder_.images.size())) throw std::logic_error("normalized gather requires an empty assembler");
 if (execution && execution->on_lane()) throw std::logic_error("normalized gather requires a controller outside CPU work");
 trace_benchmark_event(trace, "benchmark.annotations.gather_started", [&] { return nlohmann::json{{"images", slices.size()}}; });
 // Every planning/preparation/copy failure leaves the assembler unsealable.
 incomplete_gather_ = true;
 struct Cursor {
  std::size_t first_box, first_run, box = 0, run = 0, output_run = 0;
  std::uint64_t previous = 0;
  bool complete = false, admitted = false;
 };
 std::vector<Cursor> cursors;
 std::size_t boxes = 0, runs = 0;
 auto producer = execution ? execution->open_class(detail::CompileWorkClass::Gather) : CompileExecution::Producer{};
 const auto plan = [&](std::size_t) {
  throw_if_benchmark_cancelled(cancellation_);
  cursors.reserve(slices.size());
  std::optional<std::uint64_t> previous;
  for (const auto& slice : slices) {
   throw_if_benchmark_cancelled(cancellation_);
   validate_assembled_image(slice.image, builder_.source, previous);
   previous = slice.image.source_image_id;
   if (slice.image.box_count != slice.boxes.size()) throw std::runtime_error("normalized gather box extent mismatch");
   cursors.push_back({.first_box = boxes, .first_run = runs, .admitted = slice.proven(builder_.source)});
   boxes = common::math::checked_add(boxes, slice.boxes.size(), "normalized gather box overflow");
   for (const auto& box : slice.boxes) {
    if (box.mask_rle_offset > slice.runs.size() || box.mask_rle_pairs > slice.runs.size() - box.mask_rle_offset) throw std::runtime_error("normalized gather run extent mismatch");
    runs = common::math::checked_add(runs, std::size_t{box.mask_rle_pairs}, "normalized gather run overflow");
   }
  }
  if (mapped_) {
   if (slices.size() != mapped_->images.size() || boxes != mapped_->boxes.size() || runs != mapped_->runs.size()) throw std::runtime_error("normalized gather planned extent mismatch");
  } else if (staging_directory_.empty()) {
   builder_.images.resize(slices.size());
   builder_.boxes.resize(boxes);
   builder_.mask_rle_pairs.resize(runs);
   prepared_ = true;
  }
 };
 if (execution)
  execution->run(CompileTaskKind::Gather, {}, plan, allowance);
 else
  plan(0);
 if (!prepared_) {
  const auto prepare_destination = [&, allowance] { prepare(slices.size(), boxes, runs); };
  if (files)
   files->submit({.work = prepare_destination}).wait();
  else
   prepare_destination();
 }
 auto images = mapped_ ? mapped_->images : std::span(builder_.images);
 auto output_boxes = mapped_ ? mapped_->boxes : std::span(builder_.boxes);
 auto output_runs = mapped_ ? mapped_->runs : std::span(builder_.mask_rle_pairs);
 const auto chunk = [&](std::size_t i) {
  auto& cursor = cursors[i];
  const auto& slice = slices[i];
  if (cursor.complete) return;
  std::size_t copied = 0, completed_boxes = 0;
  while (cursor.box < slice.boxes.size() && copied < 65536 && completed_boxes < 256) {
   throw_if_benchmark_cancelled(cancellation_);
   auto box = slice.boxes[cursor.box];
   const auto input = slice.runs.subspan(box.mask_rle_offset, box.mask_rle_pairs);
   if (!cursor.run) {
    box.source_ordinal = common::math::checked_add(box.source_ordinal, slice.ordinal_base, "normalized gather source ordinal overflow");
    if (slice.incomplete_ordinals) box.flags |= kAnnotationOrdinalIncomplete;
    box.mask_rle_offset = cursor.first_run + cursor.output_run;
    validate_assembled_box(box, input.size(), builder_.source);
    output_boxes[cursor.first_box + cursor.box] = box;
   }
   const auto count = std::min<std::size_t>(input.size() - cursor.run, 65536 - copied);
   if (!cursor.admitted)
    for (std::size_t r = 0; r < count; ++r) cursor.previous = validate_run_value(input[cursor.run + r], std::uint64_t{slice.image.width} * slice.image.height, cursor.previous);
   if (count) std::memcpy(output_runs.data() + cursor.first_run + cursor.output_run, input.data() + cursor.run, count * sizeof(RLEPair));
   cursor.run += count;
   cursor.output_run += count;
   copied += count;
   if (cursor.run == input.size()) {
    ++cursor.box;
    ++completed_boxes;
    cursor.run = 0;
    cursor.previous = 0;
   }
  }
  if (cursor.box == slice.boxes.size()) {
   auto image = slice.image;
   image.first_box = cursor.first_box;
   images[i] = image;
   cursor.complete = true;
  }
 };
 // Each disjoint range advances as soon as its own preceding chunk settles.
 // A held peer never creates a round barrier for ready successors.
 if (execution) {
  struct Active {
   std::size_t image;
   CompileExecution::Completion completion;
  };
  std::vector<Active> active;
  active.reserve(std::min(slices.size(), execution->workers() * 2));
  std::size_t next = 0;
  std::exception_ptr failure;
  try {
   while (next < slices.size() || !active.empty()) {
    const auto observed = execution->admission_generation();
    for (auto i = active.begin(); i != active.end();) {
     if (!i->completion.ready()) {
      ++i;
      continue;
     }
     i->completion.wait();
     if (cursors[i->image].complete)
      i = active.erase(i);
     else {
      i->completion = {};
      ++i;
     }
    }
    while (next < slices.size() && active.size() < active.capacity()) active.push_back({next++, {}});
    for (auto& item : active)
     if (!item.completion) {
      auto admitted = execution->try_submit({.kind = CompileTaskKind::Gather, .work = [&, image = item.image](std::size_t, const CompileAllowance&) { chunk(image); }, .allowance = allowance});
      if (admitted) item.completion = std::move(*admitted);
     }
    if (next < slices.size() || !active.empty()) execution->wait_for_admission_change(observed);
   }
  } catch (...) {
   failure = std::current_exception();
   for (auto& item : active) item.completion.cancel();
  }
  for (auto& item : active) try {
    item.completion.wait();
   } catch (...) {
    if (!failure) failure = std::current_exception();
   }
  if (failure) std::rethrow_exception(failure);
 } else
  for (std::size_t i = 0; i < slices.size(); ++i)
   while (!cursors[i].complete) chunk(i);
 trace_benchmark_event(trace, "benchmark.annotations.gathered", [&] {
  std::uint64_t validated = 0;
  for (std::size_t i = 0; i < slices.size(); ++i)
   if (!cursors[i].admitted) validated += cursors[i].output_run;
  return nlohmann::json{{"images", slices.size()}, {"boxes", boxes}, {"runs", runs}, {"validated_runs", validated}, {"copied_bytes", boxes * sizeof(NormalizedBox) + runs * sizeof(RLEPair)}};
 });
 if (mapped_) {
  mapped_->image_count = slices.size();
  mapped_->box_count = boxes;
  mapped_->run_count = runs;
 }
 incomplete_gather_ = false;
}
std::span<NormalizedImage> NormalizedAnnotationAssembler::writable_images() {
 if (!prepared_ || finished_ || incomplete_gather_) throw std::logic_error("normalized assembler is unprepared, sealed or has incomplete gather work");
 throw_if_benchmark_cancelled(cancellation_);
 return mapped_ ? mapped_->images.first(mapped_->image_count) : std::span(builder_.images);
}
void NormalizedAnnotationAssembler::begin_image(NormalizedImage image) {
 const auto images = writable_images();
 validate_assembled_image(image, builder_.source, images.empty() ? std::nullopt : std::optional{std::uint64_t{images.back().source_image_id}});
 image.first_box = mapped_ ? mapped_->box_count : builder_.boxes.size();
 image.box_count = 0;
 if (mapped_) {
  if (mapped_->image_count == mapped_->images.size()) throw std::runtime_error("normalized benchmark image record is invalid");
  mapped_->images[mapped_->image_count++] = image;
 } else
  builder_.images.push_back(image);
}
void NormalizedAnnotationAssembler::append_box(NormalizedBox box, std::span<const RLEPair> runs) {
 const auto images = writable_images();
 if (images.empty()) throw std::logic_error("normalized box requires an owning image");
 auto& image = images.back();
 if (image.box_count == UINT32_MAX) throw std::overflow_error("normalized per-image box count overflow");
 if (mapped_ && (mapped_->box_count == mapped_->boxes.size() || runs.size() > mapped_->runs.size() - mapped_->run_count)) throw std::overflow_error("normalized assembly extent exceeded");
 const auto first_run = mapped_ ? mapped_->run_count : builder_.mask_rle_pairs.size();
 box.mask_rle_offset = first_run;
 validate_assembled_box(box, runs.size(), builder_.source);
 std::uint64_t previous = 0;
 for (std::size_t i = 0; i < runs.size(); ++i) {
  if ((i & 4095U) == 0) throw_if_benchmark_cancelled(cancellation_);
  previous = validate_run_value(runs[i], std::uint64_t{image.width} * image.height, previous);
 }
 if (mapped_) {
  if (!runs.empty()) std::memcpy(mapped_->runs.data() + mapped_->run_count, runs.data(), runs.size_bytes());
  mapped_->run_count += runs.size();
  mapped_->boxes[mapped_->box_count++] = box;
 } else {
  try {
   builder_.mask_rle_pairs.insert(builder_.mask_rle_pairs.end(), runs.begin(), runs.end());
   builder_.boxes.push_back(box);
  } catch (...) {
   builder_.mask_rle_pairs.resize(first_run);
   throw;
  }
 }
 ++image.box_count;
}
NormalizedAnnotationIndex NormalizedAnnotationAssembler::finish(const BenchmarkTraceSink& trace) {
 const auto images = writable_images();
 trace_benchmark_event(trace, "benchmark.annotations.assembled", [&] {
  return nlohmann::json{
   {"source", benchmark_source_name(builder_.source)}, {"split", builder_.split}, {"images", images.size()}, {"boxes", mapped_ ? mapped_->box_count : builder_.boxes.size()},
   {"mask_rle_pairs", mapped_ ? mapped_->run_count : builder_.mask_rle_pairs.size()}, {"image_capacity", mapped_ ? mapped_->images.size() : builder_.images.capacity()},
   {"box_capacity", mapped_ ? mapped_->boxes.size() : builder_.boxes.capacity()}, {"mask_rle_capacity", mapped_ ? mapped_->runs.size() : builder_.mask_rle_pairs.capacity()}
  };
 });
 NormalizedAnnotationIndex result;
 if (mapped_) {
  auto owner = std::make_shared<NormalizedAnnotationBacking>();
  owner->source = builder_.source;
  owner->storage = mapped_;
  // An exact-capacity assembly is already in the canonical persisted layout.
  // Partial-capacity standalone builders remain ordinary immutable inputs.
  if (mapped_->image_count == mapped_->images.size() && mapped_->box_count == mapped_->boxes.size() && mapped_->run_count == mapped_->runs.size())
   owner->staged = std::shared_ptr<BenchmarkMappedArtifact>(mapped_, &mapped_->artifact);
  owner->images = images;
  owner->boxes = mapped_->boxes.first(mapped_->box_count);
  owner->runs = mapped_->runs.first(mapped_->run_count);
  static_cast<NormalizedAnnotationMetadata&>(result) = std::move(static_cast<NormalizedAnnotationMetadata&>(builder_));
  result.images = owner->images;
  result.boxes = owner->boxes;
  result.mask_rle_pairs = owner->runs;
  result.backing = std::move(owner);
  mapped_.reset();
 } else
  result = seal_normalized_annotation_metadata(std::move(builder_));
 finished_ = true;
 std::call_once(result.backing->image_admission, [] {});
 std::call_once(result.backing->full_admission, [] {});
 return result;
}
namespace {
void check_normalized_backing(const NormalizedAnnotationIndex& index) {
 if (!index.backing || index.source != index.backing->source || index.images.data() != index.backing->images.data() || index.images.size() != index.backing->images.size() ||
     index.boxes.data() != index.backing->boxes.data() || index.boxes.size() != index.backing->boxes.size() || index.mask_rle_pairs.data() != index.backing->runs.data() ||
     index.mask_rle_pairs.size() != index.backing->runs.size())
  throw std::runtime_error("normalized annotation view does not belong to its backing");
}
}  // namespace
void admit_normalized_annotation_image(const NormalizedAnnotationIndex& index, std::size_t image) {
 check_normalized_backing(index);
 if (image >= index.images.size()) throw std::out_of_range("normalized image position is invalid");
 if (index.backing->admitted_rows.load(std::memory_order_acquire) <= image) admit_normalized_annotations(index);
}
void admit_normalized_annotations(const NormalizedAnnotationIndex& index, mmltk::common::concurrency::CancellationObservation cancellation) {
 check_normalized_backing(index);
 std::call_once(index.backing->full_admission, [&] {
  bool combined = false;
  std::call_once(index.backing->image_admission, [&] {
   validate_normalized_records(index, cancellation);
   combined = true;
  });
  if (!combined) validate_normalized_records(index, cancellation, false, true);
 });
}
NormalizedAnnotationIndex seal_normalized_annotation_image(BenchmarkDatasetSource source, NormalizedImage image, std::span<const NormalizedBox> boxes, std::span<const RLEPair> runs,
 std::shared_ptr<const void> custody, mmltk::common::concurrency::CancellationObservation cancellation) {
 if ((!boxes.empty() || !runs.empty()) && !custody) throw std::logic_error("normalized row requires immutable payload custody");
 auto backing = std::make_shared<NormalizedAnnotationBacking>();
 backing->source = source;
 backing->storage = std::move(custody);
 backing->single_image = image;
 backing->images = {&backing->single_image, 1};
 backing->boxes = boxes;
 backing->runs = runs;
 NormalizedAnnotationIndex result;
 result.source = source;
 result.images = backing->images;
 result.boxes = boxes;
 result.mask_rle_pairs = runs;
 result.backing = std::move(backing);
 admit_normalized_annotations(result, cancellation);
 return result;
}
NormalizedAnnotationIndex rebind_normalized_annotation_image(const NormalizedAnnotationIndex& source, BenchmarkDatasetSource kind, NormalizedImage image) {
 admit_normalized_annotations(source);
 if (source.images.size() != 1) throw std::logic_error("normalized row rebind requires one image");
 const auto& before = source.images.front();
 if (image.width != before.width || image.height != before.height || image.first_box != before.first_box || image.box_count != before.box_count)
  throw std::logic_error("normalized row rebind changed admitted payload geometry");
 validate_assembled_image(image, kind, {});
 // Source-specific box rules cannot inherit a different domain's admission.
 if (kind != source.source) return seal_normalized_annotation_image(kind, image, source.boxes, source.mask_rle_pairs, source.backing);
 auto backing = std::make_shared<NormalizedAnnotationBacking>();
 backing->source = kind;
 backing->storage = source.backing;
 backing->single_image = image;
 backing->images = {&backing->single_image, 1};
 backing->boxes = source.boxes;
 backing->runs = source.mask_rle_pairs;
 auto result = source;
 result.images = backing->images;
 result.backing = std::move(backing);
 std::call_once(result.backing->image_admission, [] {});
 std::call_once(result.backing->full_admission, [] {});
 return result;
}
NormalizedAnnotationAssembler::Slice NormalizedAnnotationAssembler::Slice::admitted(const NormalizedAnnotationIndex& index, std::size_t position, std::uint64_t base, bool incomplete) {
 admit_normalized_annotation_image(index, position);
 const auto& image = index.images[position];
 Slice result(image, index.boxes.subspan(image.first_box, image.box_count), index.mask_rle_pairs, base, incomplete);
 result.backing_ = index.backing;
 result.position_ = position;
 return result;
}
bool NormalizedAnnotationAssembler::Slice::proven(BenchmarkDatasetSource source) const noexcept {
 if (!backing_ || backing_->source != source || position_ >= backing_->images.size()) return false;
 const auto& admitted = backing_->images[position_];
 return std::memcmp(&image, &admitted, sizeof(image)) == 0 && boxes.data() == backing_->boxes.subspan(admitted.first_box, admitted.box_count).data() && boxes.size() == admitted.box_count &&
        runs.data() == backing_->runs.data() && runs.size() == backing_->runs.size();
}
NormalizedAnnotationIndex seal_normalized_annotations(NormalizedAnnotationBuilder&& builder, mmltk::common::concurrency::CancellationObservation cancellation) {
 auto result = seal_normalized_annotation_metadata(std::move(builder));
 admit_normalized_annotations(result, cancellation);
 return result;
}
void append_normalized_image_slice(
 NormalizedAnnotationBuilder& destination, const NormalizedAnnotationIndex& source, std::size_t position, mmltk::common::concurrency::CancellationObservation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 if (static_cast<const void*>(&destination) == static_cast<const void*>(&source)) throw std::invalid_argument("normalized slice append requires distinct storage");
 const auto boxes = image_boxes(source, position);
 const auto runs = slice_run_count(source, boxes, cancellation);
 if (destination.images.size() == destination.images.max_size() || boxes.size() > destination.boxes.max_size() - destination.boxes.size() ||
     runs > destination.mask_rle_pairs.max_size() - destination.mask_rle_pairs.size())
  throw std::overflow_error("normalized slice destination size overflow");
 const auto image_start = destination.images.size(), box_start = destination.boxes.size(), run_start = destination.mask_rle_pairs.size();
 try {
  auto run_destination = run_start;
  transfer_image_slice(destination, source, position, image_start, box_start, run_destination, cancellation);
  throw_if_benchmark_cancelled(cancellation);
 } catch (...) {
  destination.images.resize(image_start);
  destination.boxes.resize(box_start);
  destination.mask_rle_pairs.resize(run_start);
  throw;
 }
}
void retain_normalized_image_slices(NormalizedAnnotationBuilder& index, std::span<const std::size_t> order, mmltk::common::concurrency::CancellationObservation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 bool identity = order.size() == index.images.size(), increasing = true;
 for (std::size_t i = 0; i < order.size(); ++i) {
  if ((i & 1023U) == 0) throw_if_benchmark_cancelled(cancellation);
  (void)image_boxes(index, order[i]);
  identity = identity && order[i] == i;
  increasing = increasing && (i == 0 || order[i - 1] < order[i]);
 }
 if (identity) return;
 std::size_t box_count = 0, run_count = 0;
 std::uint64_t previous_box_end = 0, previous_run_end = 0;
 for (const auto position : order) {
  throw_if_benchmark_cancelled(cancellation);
  const auto boxes = image_boxes(index, position);
  const auto runs = slice_run_count(index, boxes, cancellation);
  if (boxes.size() > index.boxes.max_size() - box_count || runs > index.mask_rle_pairs.max_size() - run_count) throw std::overflow_error("normalized retained slice size overflow");
  if (increasing) {
   const auto& image = index.images[position];
   if (image.first_box < previous_box_end) throw std::runtime_error("normalized retained box slices overlap");
   previous_box_end = image.first_box + image.box_count;
   for (std::size_t i = 0; i < boxes.size(); ++i) {
    if ((i & 1023U) == 0) throw_if_benchmark_cancelled(cancellation);
    const auto& box = boxes[i];
    if (box.mask_rle_offset < previous_run_end) throw std::runtime_error("normalized retained mask slices overlap");
    previous_run_end = box.mask_rle_offset + box.mask_rle_pairs;
   }
  }
  box_count += boxes.size();
  run_count += runs;
 }
 if (increasing) {
  std::size_t box_destination = 0, run_destination = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto count = index.images[order[i]].box_count;
   transfer_image_slice(index, index, order[i], i, box_destination, run_destination, cancellation);
   box_destination += count;
  }
  throw_if_benchmark_cancelled(cancellation);
  index.images.resize(order.size());
  index.boxes.resize(box_count);
  index.mask_rle_pairs.resize(run_count);
 } else {
  NormalizedAnnotationBuilder next;
  next.completion = index.completion;
  next.source = index.source;
  next.split = index.split;
  next.annotation_sha256 = index.annotation_sha256;
  next.rejected = index.rejected;
  next.images.reserve(order.size());
  next.boxes.reserve(box_count);
  next.mask_rle_pairs.reserve(run_count);
  for (const auto position : order) {
   auto run = next.mask_rle_pairs.size();
   transfer_image_slice(next, index, position, next.images.size(), next.boxes.size(), run, cancellation);
  }
  throw_if_benchmark_cancelled(cancellation);
  index = std::move(next);
 }
}
NormalizedAnnotationIndex parse_coco_style_annotations(
 const std::filesystem::path& json_path, std::string annotation_sha256, const std::span<const NumericCategoryMapping> mappings, const AnnotationParseOptions& options) {
 auto producer = options.execution ? options.execution->open_class(detail::CompileWorkClass::Annotations) : CompileExecution::Producer{};
 auto handles = options.input_allowance ? options.input_allowance : options.execution ? options.execution->reserve(CompileResources::handles(1, false)) : CompileAllowance{};
 PaddedMappedFile file(json_path);
 constexpr std::array<std::string_view, 3> fields{"images", "annotations", "categories"};
 const CategoryLookup category_lookup = make_numeric_lookup(mappings);
 NumericCategoryAdmission category_admission(category_lookup);
 const int workers = options.execution ? static_cast<int>(options.execution->workers()) : effective_worker_count(options.num_workers, file.size());
 std::vector<std::vector<ParsedImage>> worker_images(static_cast<std::size_t>(workers));
 std::vector<ParsedImage> images;
 std::unordered_map<std::uint64_t, std::uint32_t> image_lookup;
 const auto join_images = [&](std::size_t) {
  std::size_t image_count = 0;
  for (const auto& local : worker_images) image_count = mmltk::common::math::checked_add(image_count, local.size(), "benchmark image count overflow");
  images.reserve(image_count);
  for (auto& local : worker_images) {
   images.insert(images.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
   std::vector<ParsedImage>().swap(local);
  }
  std::ranges::sort(images, {}, &ParsedImage::id);
  if (images.empty() || std::ranges::adjacent_find(images, {}, &ParsedImage::id) != images.end() || (options.expected_image_count != 0U && images.size() != options.expected_image_count))
   throw AnnotationDocumentRejected("benchmark source image metadata count or IDs are invalid");
  image_lookup.reserve(images.size());
  for (std::size_t index = 0; index < images.size(); ++index) image_lookup.emplace(images[index].id, checked_cast<std::uint32_t>(index, "benchmark image index overflow"));
 };
 std::vector<AnnotationChunk> chunks(static_cast<std::size_t>(workers));
 std::vector<AnnotationRejectCounts> worker_rejected(static_cast<std::size_t>(workers));
 // Independent parsers cannot invalidate a borrowed outer document. Capacity
 // survives adjacent chunks with its charged lane, until pressure or pass retirement.
 std::vector<simdjson::ondemand::parser> parsers(static_cast<std::size_t>(workers));
 std::vector<simdjson::ondemand::parser> segmentation_parsers(static_cast<std::size_t>(workers));
 std::vector<SegmentationScratch> segmentation_scratch(static_cast<std::size_t>(workers));
 std::optional<CompileExecution::Workspace> parser_workspace;
 if (options.execution)
  parser_workspace.emplace(*options.execution, [&](std::size_t lane) noexcept {
   segmentation_scratch[lane] = {};
   segmentation_parsers[lane] = simdjson::ondemand::parser{};
   parsers[lane] = simdjson::ondemand::parser{};
  });
 auto* workspace = parser_workspace ? &*parser_workspace : nullptr;
 const auto parse_annotations = [&](std::span<const ByteRange> rows) {
  parallel_object_array(file, rows, workers, parsers, workspace, options.cancel_requested, [&](const int worker, simdjson::ondemand::object object, std::uint64_t ordinal) {
   auto& scratch = segmentation_scratch[static_cast<std::size_t>(worker)];
   scratch.reset();
   auto& rejected = worker_rejected[static_cast<std::size_t>(worker)];
   ++rejected.raw_records;
   auto& chunk = chunks[static_cast<std::size_t>(worker)];
   const auto first_run = chunk.runs.size();
   scratch.output = &chunk.runs;
   std::optional<BoxCandidate> candidate;
   try {
    const auto annotation = parse_annotation_object(object);
    candidate = normalize_coco_box(annotation, images, image_lookup, category_lookup, &rejected, file, segmentation_parsers[static_cast<std::size_t>(worker)], scratch);
   } catch (const simdjson::simdjson_error&) {
    chunk.runs.resize(first_run);
    ++rejected.malformed_records;
    return;
   } catch (const std::runtime_error&) {
    chunk.runs.resize(first_run);
    ++rejected.malformed_records;
    return;
   }
   if (candidate)
    chunk.append(std::move(*candidate), ordinal);
   else
    chunk.runs.resize(first_run);
  }, options.execution, [&](int worker) {
   trace_benchmark_event(
    options.trace, "benchmark.annotations.workspace", [&] { return nlohmann::json{{"retained_sparse_bytes", segmentation_scratch[static_cast<std::size_t>(worker)].retained_bytes()}}; });
  });
 };
 bool images_complete = false, categories_complete = false;
 std::vector<ByteRange> pending_annotations;
 discover_json_arrays(file, fields, false, [&](std::size_t field, std::span<const ByteRange> rows, bool complete) {
  if (field == 0) {
   parallel_object_array(file, rows, workers, parsers, workspace, options.cancel_requested,
    [&](int worker, simdjson::ondemand::object object, std::uint64_t) { worker_images[static_cast<std::size_t>(worker)].push_back(parse_image_object(object, options.source)); }, options.execution);
   if (complete) {
    if (options.execution)
     options.execution->run(CompileTaskKind::Metadata, {65536, 0}, join_images);
    else
     join_images(0);
    images_complete = true;
   }
  } else if (field == 2) {
   validate_numeric_categories(file, rows, parsers, workspace, category_admission, complete, options.cancel_requested, options.execution);
   categories_complete = complete;
  } else if (images_complete && categories_complete)
   parse_annotations(rows);
  else
   pending_annotations.insert(pending_annotations.end(), rows.begin(), rows.end());
  if (images_complete && categories_complete && !pending_annotations.empty()) {
   parse_annotations(pending_annotations);
   std::vector<ByteRange>().swap(pending_annotations);
  }
 }, options.cancel_requested, options.execution);
 auto metadata = begin_normalized_index_result(options.source, options.split, std::move(annotation_sha256), worker_rejected);
 auto result = assemble_annotation_chunks(std::move(metadata), images, chunks, options.keep_images_without_mapped_boxes, options.cancel_requested, options.execution, options.trace);
 trace_benchmark_event(options.trace, "benchmark.annotations.indexed", [&] {
  nlohmann::json event = normalized_index_trace_json(result);
  event["mask_rle_pairs"] = result.mask_rle_pairs.size();
  return event;
 });
 return result;
}
NormalizedAnnotationIndex parse_open_images_annotations(const std::filesystem::path& boxes_csv_path, const std::filesystem::path& classes_csv_path, std::string annotation_sha256,
 const std::span<const StringCategoryMapping> mappings, const AnnotationParseOptions& options) {
 auto producer = options.execution ? options.execution->open_class(detail::CompileWorkClass::Annotations) : CompileExecution::Producer{};
 auto handles = options.input_allowance ? options.input_allowance : options.execution ? options.execution->reserve(CompileResources::handles(1, false)) : CompileAllowance{};
 validate_open_images_classes(classes_csv_path, mappings, options);
 std::unordered_map<std::string_view, std::uint8_t> category_lookup;
 category_lookup.reserve(mappings.size());
 for (const StringCategoryMapping& mapping : mappings) {
  if (!category_lookup.emplace(mapping.source_id, mapping.target_id).second) { throw std::runtime_error("Open Images category mapping contains a duplicate MID"); }
 }
 PaddedMappedFile file(boxes_csv_path);
 const std::vector<ByteRange> ranges = newline_ranges(file, options.num_workers, options.execution != nullptr);
 const int workers = effective_worker_count(options.num_workers, ranges.size());
 struct ImageRun {
  std::uint64_t image_id = 0U;
  std::uint32_t count = 0U;
 };
 std::vector<std::vector<ImageRun>> worker_runs(ranges.size());
 std::vector<std::vector<OpenImagesCandidate>> candidates(ranges.size());
 std::vector<AnnotationRejectCounts> worker_rejected(ranges.size());
 annotation_ranges(options.execution, ranges.size(), workers, [&](const int, const std::size_t begin, const std::size_t end) {
  for (std::size_t range_index = begin; range_index < end; ++range_index) {
   auto& runs = worker_runs[range_index];
   auto& rejected = worker_rejected[range_index];
   for_each_csv_line(file, ranges[range_index], options.cancel_requested, [&](const std::string_view line) {
    auto candidate = parse_open_images_line(line, category_lookup, &rejected);
    if (!candidate) { return; }
    candidate->box.source_ordinal = static_cast<std::uint64_t>(line.data() - file.data());
    candidates[range_index].push_back(*candidate);
    if (runs.empty() || runs.back().image_id != candidate->image_id) {
     if (!runs.empty() && candidate->image_id < runs.back().image_id) { throw std::runtime_error("Open Images bounding boxes are not sorted by image ID"); }
     runs.push_back(ImageRun{candidate->image_id, 1U});
    } else {
     if (runs.back().count == std::numeric_limits<std::uint32_t>::max()) { throw std::overflow_error("Open Images per-image annotation count overflow"); }
     ++runs.back().count;
    }
   });
  }
 });
 std::uint64_t run_count = 0;
 for (const auto& runs : worker_runs) run_count = mmltk::common::math::checked_add(run_count, std::uint64_t{runs.size()}, "Open Images boundary workspace overflow");
 NormalizedAnnotationIndex result;
 const auto assemble = [&](std::size_t) {
  std::vector<ImageRun> merged;
  merged.reserve(checked_cast<std::size_t>(run_count, "Open Images boundary run count overflow"));
  for (auto& runs : worker_runs) {
   for (const ImageRun run : runs) {
    if (!merged.empty() && merged.back().image_id == run.image_id) {
     if (run.count > std::numeric_limits<std::uint32_t>::max() - merged.back().count) { throw std::overflow_error("Open Images merged per-image annotation count overflow"); }
     merged.back().count += run.count;
    } else {
     if (!merged.empty() && run.image_id < merged.back().image_id) { throw std::runtime_error("Open Images partition merge is not ordered"); }
     merged.push_back(run);
    }
   }
  }
  if (merged.empty()) { throw std::runtime_error("Open Images has no mapped bounding-box images"); }
  std::vector<std::uint64_t> offsets(merged.size() + 1U, 0U);
  for (std::size_t index = 0; index < merged.size(); ++index)
   offsets[index + 1] = mmltk::common::math::checked_add(offsets[index], std::uint64_t{merged[index].count}, "Open Images annotation offset overflow");
  auto metadata = begin_normalized_index_result(BenchmarkDatasetSource::kOpenImagesV7, options.split, std::move(annotation_sha256), worker_rejected);
  NormalizedAnnotationAssembler assembly(std::move(metadata), merged.size(), checked_cast<std::size_t>(offsets.back(), "Open Images box count overflow"), 0, options.cancel_requested);
  std::optional<std::uint64_t> previous;
  for (auto& chunk : candidates)
   for (auto& candidate : chunk) {
    throw_if_benchmark_cancelled(options.cancel_requested);
    if (!previous || *previous != candidate.image_id) {
     assembly.begin_image({candidate.image_id, 0, 0, 0, 0, 0, 0});
     previous = candidate.image_id;
    }
    assembly.append_box(candidate.box, {});
   }
  result = assembly.finish(options.trace);
 };
 const auto workspace =
  mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(run_count, std::uint64_t{sizeof(ImageRun) + sizeof(std::uint64_t)}, "Open Images boundary workspace overflow"),
   std::uint64_t{65536}, "Open Images boundary workspace overflow");
 if (options.execution)
  options.execution->run(CompileTaskKind::Metadata, {workspace, 0}, assemble);
 else
  assemble(0);
 trace_benchmark_event(options.trace, "benchmark.annotations.indexed", [&] { return normalized_index_trace_json(result); });
 return result;
}
std::optional<NormalizedAnnotationIndex> load_normalized_annotation_index(const std::filesystem::path& path, const BenchmarkDatasetSource expected_source, const std::string_view expected_split,
 const std::string_view expected_annotation_sha256, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace, const nlohmann::json* supplied_completion,
 bool metadata_only, std::uint64_t completion_bytes, const NormalizedAnnotationRowReady& ready) {
 bool callback_failed = false, mapping_io = false;
 try {
  throw_if_benchmark_cancelled(cancel_requested);
  if (!std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(normalized_manifest_path(path))) { return std::nullopt; }
  nlohmann::json opened_completion;
  if (!supplied_completion) opened_completion = read_json_file(normalized_manifest_path(path), &completion_bytes);
  const auto& manifest = supplied_completion ? *supplied_completion : opened_completion;
  if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || manifest.value("index_version", 0U) != kNormalizedAnnotationIndexVersion || !manifest.value("complete", false) ||
      manifest.value("mapping_revision", std::string{}) != kBenchmarkMappingRevision || manifest.value("source", std::string{}) != benchmark_source_name(expected_source) ||
      manifest.value("split", std::string{}) != expected_split || manifest.value("annotation_sha256", std::string{}) != expected_annotation_sha256) {
   return std::nullopt;
  }
  mapping_io = true;
  auto file = FileHandle::open_readonly(path.string());
  const auto mapped_bytes = file.size();
  if (mapped_bytes < sizeof(NormalizedIndexHeader)) return std::nullopt;
  auto mapping = std::make_shared<mmltk::common::io::MappedByteRegion>();
  void* address = ::mmap(nullptr, mapped_bytes, PROT_READ, MAP_PRIVATE, file.get(), 0);
  if (address == MAP_FAILED) throw errno_error("cannot map normalized annotation index", path.string());
  mapping->adopt(address, mapped_bytes);
  // Linux mapping custody retains the inode without retaining an open file
  // slot for every full index and every selected read view.
  file = {};
  mapping_io = false;
  const auto* data = static_cast<const std::uint8_t*>(mapping->address());
  NormalizedIndexHeader header{};
  std::memcpy(&header, data, sizeof(header));
  validate_index_layout(header, mapped_bytes);
  if (header.source != static_cast<std::uint8_t>(expected_source) || std::string_view(header.split.data(), ::strnlen(header.split.data(), header.split.size())) != expected_split ||
      std::string_view(header.mapping_revision.data(), ::strnlen(header.mapping_revision.data(), header.mapping_revision.size())) != kBenchmarkMappingRevision ||
      header.annotation_sha256 != mmltk::common::io::parse_sha256_hex(std::string(expected_annotation_sha256)) || manifest.value("size", 0ULL) != mapped_bytes ||
      manifest.value("identity", std::string{}).empty()) {
   return std::nullopt;
  }
  NormalizedAnnotationIndex index;
  index.source = expected_source;
  index.split = std::string(expected_split);
  index.annotation_sha256 = std::string(expected_annotation_sha256);
  index.rejected = decode_rejected(header.rejected);
  if (header.image_offset % alignof(NormalizedImage) || header.box_offset % alignof(NormalizedBox) || header.mask_rle_offset % alignof(RLEPair)) return std::nullopt;
  index.images = {reinterpret_cast<const NormalizedImage*>(data + header.image_offset), header.image_count};
  index.boxes = {reinterpret_cast<const NormalizedBox*>(data + header.box_offset), checked_cast<std::size_t>(header.box_count, "normalized box count overflow")};
  index.mask_rle_pairs = {reinterpret_cast<const RLEPair*>(data + header.mask_rle_offset), checked_cast<std::size_t>(header.mask_rle_pair_count, "normalized run count overflow")};
  index.backing = std::make_shared<NormalizedAnnotationBacking>();
  index.backing->source = index.source;
  index.backing->storage = mapping;
  index.backing->images = index.images;
  index.backing->boxes = index.boxes;
  index.backing->runs = index.mask_rle_pairs;
  auto completion = std::make_shared<NormalizedAnnotationCompletion>();
  mmltk::frameworks::reflection::visit_materialized_members<NormalizedAnnotationCompletionFacts>([&]<class Declaration>(const auto& field) {
   // Required identity fields were admitted above. Informational fields have
   // historically not controlled admission; retain them when representable.
   if (manifest.contains(field.member_name)) try {
     manifest.at(field.member_name).get_to(completion.get()->*Declaration::pointer);
    } catch (const nlohmann::json::exception&) {}
  });
  completion->images = header.image_count;
  completion->boxes = header.box_count;
  completion->mask_rle_pairs = header.mask_rle_pair_count;
  completion->proof_bytes = completion_bytes;
  index.completion = std::move(completion);
  if (ready) {
   validate_normalized_records(index, cancel_requested, metadata_only, false, [&](std::size_t image) {
    if (!metadata_only) index.backing->admitted_rows.store(image + 1, std::memory_order_release);
    try {
     ready(index, image);
    } catch (...) {
     callback_failed = true;
     throw;
    }
   });
   std::call_once(index.backing->image_admission, [] {});
   if (!metadata_only) std::call_once(index.backing->full_admission, [] {});
  } else if (metadata_only)
   std::call_once(index.backing->image_admission, [&] { validate_normalized_records(index, cancel_requested, true); });
  else
   admit_normalized_annotations(index, cancel_requested);
  trace_benchmark_event(trace, "benchmark.annotations.cache_hit", [&] {
   return nlohmann::json{
    {"source", benchmark_source_name(index.source)}, {"split", index.split}, {"images", index.images.size()}, {"boxes", index.boxes.size()}, {"mask_rle_pairs", index.mask_rle_pairs.size()}
   };
  });
  return index;
 } catch (const std::exception& error) {
  if (callback_failed || mapping_io || is_benchmark_capacity_failure(error) || dynamic_cast<const std::system_error*>(&error)) throw;
  if (cancel_requested.requested()) { throw; }
  trace_benchmark_event(trace, "benchmark.annotations.cache_invalid", [&] { return nlohmann::json{{"path", path.string()}, {"error", error.what()}}; });
  return std::nullopt;
 }
}
void remove_normalized_annotation_index(const std::filesystem::path& path) {
 // Retire admission before removing the bytes it authorizes.
 remove_cache_path(normalized_manifest_path(path));
 remove_cache_path(path);
}
std::shared_ptr<const NormalizedAnnotationCompletion> store_normalized_annotation_index(const std::filesystem::path& path, const NormalizedAnnotationIndex& index,
 mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace, detail::StorageReservationPool* storage, const nlohmann::json& extension) {
 throw_if_benchmark_cancelled(cancel_requested);
 admit_normalized_annotations(index, cancel_requested);
 if (index.split.empty() || index.annotation_sha256.empty() || index.split.size() >= NormalizedIndexHeader{}.split.size() ||
     kBenchmarkMappingRevision.size() >= NormalizedIndexHeader{}.mapping_revision.size()) {
  throw std::runtime_error("normalized benchmark annotation index identity is invalid");
 }
 const auto layout = normalized_index_layout(index.images.size(), index.boxes.size(), index.mask_rle_pairs.size());
 const auto total_size = layout.total_file_size;
 detail::StorageReservationPool destination(path, storage_observer(trace), storage);
 NormalizedIndexHeader header;
 header.source = static_cast<std::uint8_t>(index.source);
 header.image_count = checked_cast<std::uint32_t>(index.images.size(), "normalized image count overflow");
 header.box_count = index.boxes.size();
 header.mask_rle_pair_count = index.mask_rle_pairs.size();
 header.image_offset = layout.image_offset;
 header.box_offset = layout.box_offset;
 header.mask_rle_offset = layout.mask_rle_offset;
 header.total_file_size = total_size;
 header.annotation_sha256 = mmltk::common::io::parse_sha256_hex(index.annotation_sha256);
 std::memcpy(header.split.data(), index.split.data(), index.split.size());
 std::memcpy(header.mapping_revision.data(), kBenchmarkMappingRevision.data(), kBenchmarkMappingRevision.size());
 header.rejected = encode_rejected(index.rejected);
 (void)mmltk::common::io::ensure_parent_directory(path);
 std::unique_lock publication_lock(index.backing->publication_mutex);
 auto backing = index.backing->staged;
 const bool transfer = backing && backing->can_publish(path);
 detail::StagedArtifact staging;
 trace_benchmark_event(trace, "benchmark.annotations.cache_prepare", [&] {
  return nlohmann::json{{"path", path.string()}, {"bytes", total_size}, {"payload_copy_bytes", transfer ? 0 : total_size - sizeof(header)}, {"same_backing", transfer}};
 });
 if (transfer) {
  backing->seal_prefix(std::as_bytes(std::span(&header, 1)));
 } else {
  staging = detail::StagedArtifact::create(destination, path, total_size, "normalized annotation index staging");
  staging.preallocate(checked_cast<std::size_t>(total_size, "normalized index size overflow"));
  staging.file().pwrite_all(&header, sizeof(header), 0U);
  staging.file().pwrite_all(index.images.data(), index.images.size_bytes(), checked_cast<std::size_t>(layout.image_offset, "normalized image offset overflow"));
  staging.file().pwrite_all(index.boxes.data(), index.boxes.size_bytes(), checked_cast<std::size_t>(layout.box_offset, "normalized box offset overflow"));
  staging.file().pwrite_all(index.mask_rle_pairs.data(), index.mask_rle_pairs.size_bytes(), checked_cast<std::size_t>(layout.mask_rle_offset, "normalized mask offset overflow"));
  staging.file().sync_data();
  staging.close();
 }
 trace_benchmark_event(trace, "benchmark.annotations.cache_synced", [&] { return nlohmann::json{{"path", path.string()}, {"bytes", total_size}, {"same_backing", transfer}}; });
 std::string identity_material = index.annotation_sha256 + "\n" + std::string(kBenchmarkMappingRevision) + "\n" + index.split + "\n" + std::to_string(total_size) + "\n" +
                                 std::to_string(index.images.size()) + "\n" + std::to_string(index.boxes.size());
 identity_material += "\n" + std::to_string(index.mask_rle_pairs.size());
 const std::string identity = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
 const std::filesystem::path completion = normalized_manifest_path(path);
 std::error_code error;
 const bool removed = std::filesystem::remove(completion, error);
 if (error) { throw std::filesystem::filesystem_error("cannot invalidate normalized annotation completion manifest", completion, error); }
 if (removed) { sync_parent_directory(completion); }
 throw_if_benchmark_cancelled(cancel_requested);
 if (transfer) backing->publish(path, cancel_requested);
 else staging.publish(path, cancel_requested);
 auto facts = std::make_shared<NormalizedAnnotationCompletion>();
 facts->schema_version = kBenchmarkCacheSchemaVersion;
 facts->index_version = kNormalizedAnnotationIndexVersion;
 facts->complete = true;
 facts->source = benchmark_source_name(index.source);
 facts->split = index.split;
 facts->mapping_revision = kBenchmarkMappingRevision;
 facts->annotation_sha256 = index.annotation_sha256;
 facts->size = total_size;
 facts->identity = identity;
 facts->integrity_mode = "atomic_layout_identity";
 facts->images = index.images.size();
 facts->mask_rle_pairs = index.mask_rle_pairs.size();
 facts->boxes = index.boxes.size();
 auto manifest = extension.is_object() ? extension : nlohmann::json::object();
 mmltk::frameworks::reflection::visit_materialized_members<NormalizedAnnotationCompletionFacts>(
  [&]<class Declaration>(const auto& field) { manifest[field.member_name] = facts.get()->*Declaration::pointer; });
 facts->proof_bytes = write_json_atomically(completion, manifest, cancel_requested, &destination);
 trace_benchmark_event(trace, "benchmark.annotations.cache_store",
  [&] { return nlohmann::json{{"path", path.string()}, {"source", benchmark_source_name(index.source)}, {"split", index.split}, {"images", index.images.size()}, {"boxes", index.boxes.size()}, {"bytes", total_size}}; });
 return facts;
}
NormalizedAnnotationReadView::NormalizedAnnotationReadView(NormalizedAnnotationIndex storage)
    : NormalizedAnnotationMetadata(storage), storage_(std::move(storage)), counts_{storage_.boxes.size(), storage_.mask_rle_pairs.size()} {}
NormalizedAnnotationReadView NormalizedAnnotationReadView::select_images(
 std::vector<std::size_t> positions, std::optional<Counts> known, mmltk::common::concurrency::CancellationObservation cancellation) const {
 NormalizedAnnotationReadView result = *this;
 Counts counts;
 bool identity = positions.size() == image_count();
 std::size_t previous = 0;
 for (std::size_t i = 0; i < positions.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  const auto position = positions[i];
  if (position >= image_count() || (i && position <= previous)) throw std::runtime_error("normalized selection is not a canonical image subset");
  previous = position;
  identity = identity && position == i;
  if (!known) {
   const auto& selected_image = image(position);
   counts.boxes = mmltk::common::math::checked_add(counts.boxes, std::size_t{selected_image.box_count}, "normalized selected box count overflow");
   if (selected_image.box_count) {
    if (selected_image.first_box > storage_.boxes.size() || selected_image.box_count > storage_.boxes.size() - selected_image.first_box)
     throw std::runtime_error("invalid normalized selected image extent");
    const auto boxes = storage_.boxes.subspan(static_cast<std::size_t>(selected_image.first_box), selected_image.box_count);
    const auto end = mmltk::common::math::checked_add(boxes.back().mask_rle_offset, std::uint64_t{boxes.back().mask_rle_pairs}, "normalized selected mask extent overflow");
    if (end < boxes.front().mask_rle_offset || end > storage_.mask_rle_pairs.size()) throw std::runtime_error("invalid normalized selected mask extent");
    counts.runs = mmltk::common::math::checked_add(
     counts.runs, checked_cast<std::size_t>(end - boxes.front().mask_rle_offset, "normalized selected mask count overflow"), "normalized selected mask count overflow");
   }
  }
  if (positions_) positions[i] = (*positions_)[position];
 }
 if (identity) return result;
 result.counts_ = known.value_or(counts);
 if (result.counts_.boxes > counts_.boxes || result.counts_.runs > counts_.runs) throw std::runtime_error("invalid normalized selection counts");
 result.positions_ = std::make_shared<const std::vector<std::size_t>>(std::move(positions));
 return result;
}
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationReadView& index) {
 std::vector<std::uint64_t> ids;
 ids.reserve(index.image_count());
 for (const NormalizedImage& image : index.images()) { ids.push_back(image.source_image_id); }
 return ids;
}
}  // namespace mmltk::backend::data::benchmark_internal
