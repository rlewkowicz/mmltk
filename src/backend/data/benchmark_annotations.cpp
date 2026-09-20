#include <simdjson.h>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <sys/mman.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_format.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "detail/benchmark_annotations.h"
#include "detail/mask_rle_utils.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::concurrency::parallel_for_range_indexed;
using mmltk::common::io::errno_error;
using mmltk::common::io::FileHandle;
using mmltk::common::io::publish_staged_path_atomically;
using mmltk::common::io::sync_parent_directory;
using mmltk::common::math::checked_cast;
nlohmann::json reject_json(const AnnotationRejectCounts& rejected) {
    nlohmann::json result = nlohmann::json::object();
    mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>(
        [&]<class Declaration>(const auto& field) { result[field.member_name] = rejected.*Declaration::pointer; });
    return result;
}
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
struct ByteRange {
    std::size_t begin = 0U;
    std::size_t end = 0U;
};
[[nodiscard]] bool consume_json_string_token(const char token, bool& in_string, bool& escaped) noexcept {
    if (!in_string) {
        if (token == '"') {
            in_string = true;
            return true;
        }
        return false;
    }
    if (escaped) {
        escaped = false;
    } else if (token == '\\') {
        escaped = true;
    } else if (token == '"') {
        in_string = false;
    }
    return true;
}
class PaddedMappedFile {
   public:
    explicit PaddedMappedFile(const std::filesystem::path& path) : file_(FileHandle::open_readonly(path.string())) {
        size_ = file_.size();
        if (size_ == 0U) { throw std::runtime_error("benchmark annotation file is empty: " + path.string()); }
        if (size_ > std::numeric_limits<std::size_t>::max() - simdjson::SIMDJSON_PADDING) {
            throw std::overflow_error("benchmark annotation mapping size overflow");
        }
        capacity_ = size_ + simdjson::SIMDJSON_PADDING;
        void* reservation = ::mmap(nullptr, capacity_, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) { throw errno_error("cannot reserve padded benchmark annotation mapping", path.string()); }
        data_ = static_cast<const char*>(reservation);
        void* mapped = ::mmap(reservation, size_, PROT_READ, MAP_PRIVATE | MAP_FIXED, file_.get(), 0);
        if (mapped == MAP_FAILED) {
            const int saved_errno = errno;
            (void)::munmap(reservation, capacity_);
            data_ = nullptr;
            errno = saved_errno;
            throw errno_error("cannot map benchmark annotation file", path.string());
        }
    }
    PaddedMappedFile(const PaddedMappedFile&) = delete;
    PaddedMappedFile& operator=(const PaddedMappedFile&) = delete;
    ~PaddedMappedFile() {
        if (data_ != nullptr) { (void)::munmap(const_cast<char*>(data_), capacity_); }
    }
    [[nodiscard]] const char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity_from(const std::size_t offset) const {
        if (offset > capacity_) { throw std::runtime_error("benchmark annotation mapping offset is out of bounds"); }
        return capacity_ - offset;
    }

   private:
    FileHandle file_;
    const char* data_ = nullptr;
    std::size_t size_ = 0U;
    std::size_t capacity_ = 0U;
};
[[nodiscard]] std::filesystem::path normalized_manifest_path(const std::filesystem::path& path) { return path.string() + ".complete.json"; }
[[nodiscard]] int effective_worker_count(const int requested, const std::size_t work_items) {
    const int available = requested > 0 ? requested : static_cast<int>(std::thread::hardware_concurrency());
    const int bounded_work = work_items > static_cast<std::size_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                                                    : static_cast<int>(std::max<std::size_t>(work_items, 1U));
    return std::max(1, std::min(std::max(available, 1), bounded_work));
}
[[nodiscard]] ByteRange find_named_top_level_array(const PaddedMappedFile& file, const std::string_view wanted_key) {
    const char* data = file.data();
    std::size_t index = 0U;
    while (index < file.size() && static_cast<unsigned char>(data[index]) <= ' ') { ++index; }
    if (index == file.size() || data[index] != '{') { throw std::runtime_error("benchmark COCO-style annotations must contain a top-level object"); }
    int object_depth = 0;
    int array_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; index < file.size(); ++index) {
        const char value = data[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            if (object_depth == 1 && array_depth == 0) {
                const std::size_t key_begin = index + 1U;
                std::size_t key_end = key_begin;
                bool key_escape = false;
                for (; key_end < file.size(); ++key_end) {
                    if (key_escape) {
                        key_escape = false;
                    } else if (data[key_end] == '\\') {
                        key_escape = true;
                    } else if (data[key_end] == '"') {
                        break;
                    }
                }
                if (key_end == file.size()) { throw std::runtime_error("unterminated key in benchmark annotation JSON"); }
                std::size_t cursor = key_end + 1U;
                while (cursor < file.size() && static_cast<unsigned char>(data[cursor]) <= ' ') { ++cursor; }
                const bool is_object_key = cursor < file.size() && data[cursor] == ':';
                if (!key_escape && is_object_key && std::string_view(data + key_begin, key_end - key_begin) == wanted_key) {
                    ++cursor;
                    while (cursor < file.size() && static_cast<unsigned char>(data[cursor]) <= ' ') { ++cursor; }
                    if (cursor == file.size() || data[cursor] != '[') { throw std::runtime_error("benchmark annotation JSON field is not an array"); }
                    const std::size_t content_begin = cursor + 1U;
                    int depth = 1;
                    bool value_string = false;
                    bool value_escape = false;
                    for (++cursor; cursor < file.size(); ++cursor) {
                        const char token = data[cursor];
                        if (value_string) {
                            if (value_escape) {
                                value_escape = false;
                            } else if (token == '\\') {
                                value_escape = true;
                            } else if (token == '"') {
                                value_string = false;
                            }
                        } else if (token == '"') {
                            value_string = true;
                        } else if (token == '[') {
                            ++depth;
                        } else if (token == ']' && --depth == 0) {
                            return ByteRange{content_begin, cursor};
                        }
                    }
                    throw std::runtime_error("unterminated array in benchmark annotation JSON");
                }
                index = key_end;
                continue;
            }
            in_string = true;
        } else if (value == '{') {
            ++object_depth;
        } else if (value == '}') {
            --object_depth;
        } else if (value == '[') {
            ++array_depth;
        } else if (value == ']') {
            --array_depth;
        }
    }
    throw std::runtime_error("benchmark annotation JSON is missing array '" + std::string(wanted_key) + "'");
}
// Converts an ascending boundary list (plus the terminal offset) into contiguous byte ranges.
[[nodiscard]] std::vector<ByteRange> ranges_from_boundaries(std::vector<std::size_t>& boundaries, const std::size_t end) {
    boundaries.push_back(end);
    std::vector<ByteRange> ranges;
    ranges.reserve(boundaries.size() - 1U);
    for (std::size_t index = 1U; index < boundaries.size(); ++index) { ranges.push_back(ByteRange{boundaries[index - 1U], boundaries[index]}); }
    return ranges;
}
[[nodiscard]] std::vector<ByteRange> partition_object_array(const PaddedMappedFile& file, const ByteRange array, const int requested_workers) {
    const int workers = effective_worker_count(requested_workers, array.end - array.begin);
    std::vector<std::size_t> boundaries;
    boundaries.reserve(static_cast<std::size_t>(workers) + 1U);
    boundaries.push_back(array.begin);
    const std::size_t target_step = std::max<std::size_t>((array.end - array.begin) / static_cast<std::size_t>(workers), 1U);
    std::size_t next_target = array.begin + target_step;
    int object_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = array.begin; index < array.end; ++index) {
        const char token = file.data()[index];
        if (consume_json_string_token(token, in_string, escaped)) { continue; }
        if (token == '{') {
            if (object_depth == 0 && index >= next_target && boundaries.size() < static_cast<std::size_t>(workers)) {
                boundaries.push_back(index);
                next_target = array.begin + boundaries.size() * target_step;
            }
            ++object_depth;
        } else if (token == '}') {
            if (object_depth == 0) { throw std::runtime_error("unbalanced object in benchmark annotation array"); }
            --object_depth;
        } else if (object_depth == 0 && token != ',' && static_cast<unsigned char>(token) > ' ') {
            throw std::runtime_error("benchmark annotation array contains a non-object value");
        }
    }
    if (object_depth != 0 || in_string) { throw std::runtime_error("unterminated object in benchmark annotation array"); }
    return ranges_from_boundaries(boundaries, array.end);
}
void for_each_object(const PaddedMappedFile& file, const ByteRange range, simdjson::ondemand::parser& parser,
                     const std::function<void(simdjson::ondemand::object, std::uint64_t)>& callback) {
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t object_begin = 0U;
    for (std::size_t index = range.begin; index < range.end; ++index) {
        const char token = file.data()[index];
        if (consume_json_string_token(token, in_string, escaped)) { continue; }
        if (token == '{') {
            if (depth++ == 0) { object_begin = index; }
        } else if (token == '}') {
            if (depth == 0) { throw std::runtime_error("unbalanced benchmark JSON object"); }
            if (--depth == 0) {
                const std::size_t object_size = index + 1U - object_begin;
                const simdjson::padded_string_view view(file.data() + object_begin, object_size, file.capacity_from(object_begin));
                simdjson::ondemand::document document = parser.iterate(view);
                callback(document.get_object(), object_begin);
            }
        }
    }
    if (depth != 0 || in_string) { throw std::runtime_error("partition ended inside a benchmark JSON object"); }
}
void parallel_object_array(const PaddedMappedFile& file, const ByteRange array, const int requested_workers,
                           mmltk::common::concurrency::CancellationObservation cancel_requested,
                           const std::function<void(int, simdjson::ondemand::object, std::uint64_t)>& callback) {
    const std::vector<ByteRange> ranges = partition_object_array(file, array, requested_workers);
    const int workers = effective_worker_count(requested_workers, ranges.size());
    parallel_for_range_indexed<std::size_t>(0U, ranges.size(), workers, [&](const int worker, const std::size_t begin, const std::size_t end) {
        simdjson::ondemand::parser parser;
        for (std::size_t range_index = begin; range_index < end; ++range_index) {
            throw_if_benchmark_cancelled(cancel_requested);
            for_each_object(file, ranges[range_index], parser,
                            [&](simdjson::ondemand::object object, std::uint64_t ordinal) { callback(worker, object, ordinal); });
        }
    });
}
[[nodiscard]] std::uint16_t parse_objects365_shard(const std::string_view file_name) {
    const std::size_t patch = file_name.find("patch");
    if (patch == std::string_view::npos) { throw std::runtime_error("Objects365 image path does not identify its patch"); }
    const std::size_t begin = patch + 5U;
    std::uint32_t value = 0U;
    const auto result = std::from_chars(file_name.data() + begin, file_name.data() + file_name.size(), value);
    const bool valid_delimiter = result.ptr != file_name.data() + begin && (result.ptr == file_name.data() + file_name.size() || *result.ptr == '/' ||
                                                                            *result.ptr == '\\' || *result.ptr == '_' || *result.ptr == '.');
    if (result.ec != std::errc{} || !valid_delimiter || value > 50U) { throw std::runtime_error("Objects365 image path has an invalid patch number"); }
    return static_cast<std::uint16_t>(value);
}
struct ParsedImage {
    std::uint64_t id = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint16_t shard = 0U;
};
[[nodiscard]] ParsedImage parse_image_object(simdjson::ondemand::object object, const BenchmarkDatasetSource source) {
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
    if (!have_id || !have_width || !have_height || !have_filename || parsed.width == 0U || parsed.height == 0U) {
        throw std::runtime_error("benchmark image annotation is incomplete");
    }
    return parsed;
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
                for (const std::uint64_t value : counts.get_array()) {
                    segmentation->counts.push_back(checked_cast<std::uint32_t>(value, "segmentation run length overflow"));
                }
            } else {
                throw std::runtime_error("benchmark segmentation counts have an invalid type");
            }
        }
    }
    if (!have_width || !have_height || segmentation->width == 0U || segmentation->height == 0U ||
        (segmentation->counts.empty() && segmentation->compressed_counts.empty())) {
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
            const auto flag = flag_value.type().value() == simdjson::ondemand::json_type::boolean ? static_cast<std::uint64_t>(flag_value.get_bool().value())
                                                                                                  : flag_value.get_uint64().value();
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
void validate_numeric_categories(const PaddedMappedFile& file, const ByteRange categories, const CategoryLookup& lookup,
                                 mmltk::common::concurrency::CancellationObservation cancel_requested) {
    NumericCategoryAdmission admission(lookup);
    simdjson::ondemand::parser parser;
    for_each_object(file, categories, parser, [&](simdjson::ondemand::object object, std::uint64_t) {
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
        admission.observe(have_id ? std::optional(id) : std::nullopt, have_name ? std::optional(name) : std::nullopt);
        throw_if_benchmark_cancelled(cancel_requested);
    });
    admission.complete();
}
struct BoxCandidate {
    std::uint32_t image_index = 0U;
    NormalizedBox box;
    std::vector<RLEPair> mask_rle;
};
[[nodiscard]] std::vector<std::uint32_t> decode_compressed_coco_counts(const std::string_view encoded) {
    std::vector<std::uint32_t> counts;
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
    return counts;
}
void materialize_coco_counts(const std::span<const std::uint32_t> counts, const dataset::MaskDimensions dimensions, std::vector<std::uint8_t>* dense) {
    if (dimensions.width > std::numeric_limits<std::size_t>::max() / dimensions.height) { throw std::overflow_error("COCO mask dimensions overflow"); }
    const std::size_t pixels = static_cast<std::size_t>(dimensions.width) * dimensions.height;
    dense->assign(pixels, std::uint8_t{0U});
    std::size_t position = 0U;
    bool foreground = false;
    for (const std::uint32_t count : counts) {
        if (count > pixels - position) { throw std::runtime_error("COCO mask run exceeds its declared dimensions"); }
        std::size_t remaining = count;
        while (foreground && remaining != 0U) {
            const std::size_t x = position / dimensions.height;
            const std::size_t y = position % dimensions.height;
            const std::size_t length = std::min<std::size_t>(remaining, dimensions.height - y);
            for (std::size_t row = y; row < y + length; ++row) { (*dense)[row * dimensions.width + x] = 1U; }
            position += length;
            remaining -= length;
        }
        if (!foreground) { position += remaining; }
        foreground = !foreground;
    }
    if (position != pixels) { throw std::runtime_error("COCO mask runs do not cover their declared dimensions"); }
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
struct SegmentationScratch {
    std::vector<std::uint8_t> dense;
    std::vector<double> intersections;
    void reset() noexcept {
        dense.clear();
        intersections.clear();
    }
};
void rasterize_coco_polygons(const std::vector<std::vector<double>>& polygons, const dataset::MaskDimensions dimensions, SegmentationScratch& scratch) {
    auto* dense = &scratch.dense;
    auto& intersections = scratch.intersections;
    if (dimensions.width > std::numeric_limits<std::size_t>::max() / dimensions.height) { throw std::overflow_error("COCO polygon mask dimensions overflow"); }
    dense->assign(static_cast<std::size_t>(dimensions.width) * dimensions.height, std::uint8_t{0U});
    for (const std::vector<double>& polygon : polygons) {
        intersections.reserve(polygon.size() / 2U);
        double minimum_y = polygon[1];
        double maximum_y = polygon[1];
        for (std::size_t coordinate = 3U; coordinate < polygon.size(); coordinate += 2U) {
            minimum_y = std::min(minimum_y, polygon[coordinate]);
            maximum_y = std::max(maximum_y, polygon[coordinate]);
        }
        const auto [first_row, final_row] = covered_pixel_span(minimum_y, maximum_y, dimensions.height);
        for (std::uint32_t y = first_row; y < final_row; ++y) {
            intersections.clear();
            const double scan_y = static_cast<double>(y) + 0.5;
            const std::size_t points = polygon.size() / 2U;
            for (std::size_t point = 0U; point < points; ++point) {
                const std::size_t next = (point + 1U) % points;
                const double x1 = polygon[point * 2U];
                const double y1 = polygon[point * 2U + 1U];
                const double x2 = polygon[next * 2U];
                const double y2 = polygon[next * 2U + 1U];
                if ((y1 > scan_y) != (y2 > scan_y)) {
                    const double denominator = y2 - y1;
                    const double numerator = (scan_y - y1) * (x2 - x1);
                    double intersection = x1 + numerator / denominator;
                    if (!std::isfinite(denominator) || !std::isfinite(numerator) || !std::isfinite(intersection)) {
                        // Half-scaled differences cannot overflow for finite endpoints.
                        // lerp preserves finite endpoints for an interior scan position.
                        const double fraction = (scan_y * 0.5 - y1 * 0.5) / (y2 * 0.5 - y1 * 0.5);
                        intersection = std::lerp(x1, x2, fraction);
                    }
                    if (!std::isfinite(intersection)) throw std::runtime_error("COCO polygon intersection is not representable");
                    intersections.push_back(intersection);
                }
            }
            std::ranges::sort(intersections);
            for (std::size_t pair = 0U; pair + 1U < intersections.size(); pair += 2U) {
                const auto [x1, x2] = covered_pixel_span(intersections[pair], intersections[pair + 1U], dimensions.width);
                std::fill(dense->data() + static_cast<std::size_t>(y) * dimensions.width + x1,
                          dense->data() + static_cast<std::size_t>(y) * dimensions.width + x2, std::uint8_t{1U});
            }
        }
    }
}
[[nodiscard]] std::vector<RLEPair> encode_segmentation(const ParsedAnnotation::Segmentation& segmentation, const ParsedImage& image,
                                                       SegmentationScratch& scratch) {
    if (segmentation.polygons.empty() && segmentation.counts.empty() && segmentation.compressed_counts.empty()) { return {}; }
    const dataset::MaskDimensions dimensions{image.width, image.height};
    if (!segmentation.polygons.empty()) {
        rasterize_coco_polygons(segmentation.polygons, dimensions, scratch);
    } else {
        if (segmentation.width != image.width || segmentation.height != image.height) {
            throw std::runtime_error("COCO mask dimensions disagree with image metadata");
        }
        std::vector<std::uint32_t> decoded;
        std::span<const std::uint32_t> counts = segmentation.counts;
        if (counts.empty()) {
            decoded = decode_compressed_coco_counts(segmentation.compressed_counts);
            counts = decoded;
        }
        materialize_coco_counts(counts, dimensions, &scratch.dense);
    }
    return dataset::encode_dense_row_major_mask(scratch.dense, dimensions).pairs;
}
[[nodiscard]] std::vector<RLEPair> materialize_segmentation(const ParsedAnnotation& annotation, const ParsedImage& image, const PaddedMappedFile& file,
                                                            simdjson::ondemand::parser& parser, SegmentationScratch& scratch) {
    if (!annotation.has_mask) return {};
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
                                                             const std::unordered_map<std::uint64_t, std::uint32_t>& image_lookup,
                                                             const CategoryLookup& categories, AnnotationRejectCounts* rejected, const PaddedMappedFile& file,
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
    std::vector<RLEPair> mask;
    double x1 = annotation.bbox[0], y1 = annotation.bbox[1];
    double x2 = x1 + annotation.bbox[2], y2 = y1 + annotation.bbox[3];
    if (!annotation.has_bbox) {
        mask = materialize_segmentation(annotation, metadata, file, segmentation_parser, scratch);
        if (mask.empty()) {
            ++rejected->malformed_records;
            return std::nullopt;
        }
        const auto bounds = dataset::row_major_mask_bounds(mask, {metadata.width, metadata.height});
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
    std::uint64_t foreground = 0U;
    for (const auto& run : mask) foreground += run.length;
    result.box.original_area = annotation.has_area ? annotation.area : annotation.has_mask ? static_cast<double>(foreground) : (x2 - x1) * (y2 - y1);
    if (!std::isfinite(result.box.original_area)) {
        ++rejected->malformed_records;
        return std::nullopt;
    }
    result.mask_rle = std::move(mask);
    return result;
}
// Seeds a normalized index result with its provenance and the accumulated per-worker reject counts.
[[nodiscard]] NormalizedAnnotationIndex begin_normalized_index_result(const BenchmarkDatasetSource source, const std::string& split,
                                                                      std::string annotation_sha256,
                                                                      const std::span<const AnnotationRejectCounts> worker_rejected) {
    NormalizedAnnotationIndex result;
    result.source = source;
    result.split = split;
    result.annotation_sha256 = std::move(annotation_sha256);
    for (const AnnotationRejectCounts& local : worker_rejected) {
        result.rejected.raw_records += local.raw_records;
        result.rejected.unmapped_categories += local.unmapped_categories;
        result.rejected.unknown_images += local.unknown_images;
        result.rejected.malformed_records += local.malformed_records;
        result.rejected.degenerate_boxes += local.degenerate_boxes;
    }
    return result;
}
// Shared payload of the `benchmark.annotations.indexed` trace event: provenance, the record counts every parser
// produces, and the reject tally. Parser-specific fields are added by the caller to the returned object, which is
// only built from inside the trace sink's lambda so untraced runs still pay nothing.
[[nodiscard]] nlohmann::json normalized_index_trace_json(const NormalizedAnnotationIndex& index) {
    return nlohmann::json{{"source", benchmark_source_name(index.source)},
                          {"split", index.split},
                          {"images", index.images.size()},
                          {"boxes", index.boxes.size()},
                          {"raw_records", index.rejected.raw_records},
                          {"unmapped", index.rejected.unmapped_categories},
                          {"malformed", index.rejected.malformed_records},
                          {"degenerate", index.rejected.degenerate_boxes},
                          {"duplicates", index.rejected.duplicate_boxes}};
}
void compact_annotations(NormalizedAnnotationIndex* index, const std::vector<ParsedImage>& parsed_images, const std::vector<std::uint64_t>& offsets,
                         const std::span<const std::uint64_t> accepted_ends, std::vector<NormalizedBox> boxes,
                         std::optional<std::vector<std::vector<RLEPair>>> masks, const bool keep_empty,
                         mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (offsets.size() != parsed_images.size() + 1U || accepted_ends.size() != parsed_images.size())
        throw std::runtime_error("accepted annotation image ranges are inconsistent");
    const auto accepted_range = [&](std::size_t image_index) {
        if ((image_index & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
        return std::pair{checked_cast<std::size_t>(offsets[image_index], "box offset overflow"),
                         checked_cast<std::size_t>(accepted_ends[image_index], "box offset overflow")};
    };
    std::size_t accepted_boxes = 0U;
    std::size_t accepted_pairs = 0U;
    std::size_t accepted_images = 0U;
    for (std::size_t image_index = 0U; image_index < parsed_images.size(); ++image_index) {
        const auto [begin, end] = accepted_range(image_index);
        if (end < begin || end > offsets[image_index + 1U] || offsets[image_index + 1U] > boxes.size())
            throw std::runtime_error("accepted annotations exceed image capacity");
        accepted_boxes = mmltk::common::math::checked_add(accepted_boxes, end - begin, "accepted box count overflow");
        if (keep_empty || end != begin) ++accepted_images;
        for (std::size_t box_index = begin; box_index < end; ++box_index) {
            if ((box_index & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
            const std::size_t mask_index = checked_cast<std::size_t>(boxes[box_index].mask_rle_offset, "normalized mask index overflow");
            if (mask_index >= (masks ? masks->size() : boxes.size())) throw std::runtime_error("normalized mask index is out of bounds");
            if (masks) accepted_pairs = mmltk::common::math::checked_add(accepted_pairs, (*masks)[mask_index].size(), "accepted mask run count overflow");
        }
    }
    index->images.clear();
    index->boxes.clear();
    index->mask_rle_pairs.clear();
    index->images.reserve(accepted_images);
    index->boxes.reserve(accepted_boxes);
    index->mask_rle_pairs.reserve(accepted_pairs);
    for (std::size_t image_index = 0U; image_index < parsed_images.size(); ++image_index) {
        const auto [begin, end] = accepted_range(image_index);
        std::span<NormalizedBox> image_boxes = std::span(boxes).subspan(begin, end - begin);
        std::ranges::sort(image_boxes, {}, &NormalizedBox::source_ordinal);
        const std::size_t annotation_count = image_boxes.size();
        if (!keep_empty && annotation_count == 0U) { continue; }
        const ParsedImage& source = parsed_images[image_index];
        index->images.push_back(NormalizedImage{
            source.id,
            index->boxes.size(),
            checked_cast<std::uint32_t>(annotation_count, "per-image box count overflow"),
            source.width,
            source.height,
            source.shard,
            0U,
        });
        for (NormalizedBox box : image_boxes.first(annotation_count)) {
            if ((index->boxes.size() & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
            const std::size_t mask_index = checked_cast<std::size_t>(box.mask_rle_offset, "normalized mask index overflow");
            const std::span<const RLEPair> mask = masks ? std::span<const RLEPair>((*masks)[mask_index]) : std::span<const RLEPair>{};
            box.mask_rle_offset = index->mask_rle_pairs.size();
            box.mask_rle_pairs = checked_cast<std::uint32_t>(mask.size(), "normalized mask run count overflow");
            index->boxes.push_back(box);
            index->mask_rle_pairs.insert(index->mask_rle_pairs.end(), mask.begin(), mask.end());
        }
    }
}
[[nodiscard]] std::vector<std::string_view> csv_fields(const std::string_view line, std::vector<std::string>* unescaped) {
    std::vector<std::string_view> fields;
    fields.reserve(24U);
    unescaped->reserve(4U);
    std::size_t begin = 0U;
    while (begin <= line.size()) {
        if (begin < line.size() && line[begin] == '"') {
            std::string decoded;
            std::size_t cursor = begin + 1U;
            bool closed = false;
            while (cursor < line.size()) {
                if (line[cursor] == '"') {
                    if (cursor + 1U < line.size() && line[cursor + 1U] == '"') {
                        decoded.push_back('"');
                        cursor += 2U;
                    } else {
                        ++cursor;
                        closed = true;
                        break;
                    }
                } else {
                    decoded.push_back(line[cursor++]);
                }
            }
            if (!closed || (cursor < line.size() && line[cursor] != ',')) { throw std::runtime_error("malformed quoted CSV field in benchmark annotations"); }
            unescaped->push_back(std::move(decoded));
            fields.push_back(unescaped->back());
            begin = cursor + (cursor < line.size() ? 1U : 0U);
        } else {
            const std::size_t comma = line.find(',', begin);
            const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
            fields.push_back(line.substr(begin, end - begin));
            if (comma == std::string_view::npos) { break; }
            begin = comma + 1U;
        }
    }
    return fields;
}
[[nodiscard]] std::uint64_t parse_hex_image_id(const std::string_view value) {
    std::uint64_t image_id = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), image_id, 16);
    if (value.size() != 16U || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("Open Images annotation has an invalid image ID");
    }
    return image_id;
}
[[nodiscard]] double parse_csv_double(const std::string_view value) {
    double parsed_value = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(parsed_value)) {
        throw std::runtime_error("Open Images annotation has an invalid coordinate");
    }
    return parsed_value;
}
struct OpenImagesCandidate {
    std::uint64_t image_id = 0U;
    NormalizedBox box;
};
[[nodiscard]] std::optional<OpenImagesCandidate> parse_open_images_line(const std::string_view line,
                                                                        const std::unordered_map<std::string_view, std::uint8_t>& mappings,
                                                                        AnnotationRejectCounts* rejected) {
    ++rejected->raw_records;
    try {
        std::array<std::string_view, 8> fields{};
        std::size_t begin = 0U;
        for (std::size_t field = 0U; field < fields.size(); ++field) {
            const std::size_t comma = line.find(',', begin);
            const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
            if (begin == end || line[begin] == '"' || (field + 1U < fields.size() && comma == std::string_view::npos)) {
                throw std::runtime_error("Open Images bounding-box row has invalid fixed columns");
            }
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
[[nodiscard]] std::vector<ByteRange> newline_ranges(const PaddedMappedFile& file, const int requested_workers) {
    const char* data = file.data();
    const char* first_newline = static_cast<const char*>(std::memchr(data, '\n', file.size()));
    if (first_newline == nullptr) { throw std::runtime_error("benchmark CSV annotation file has no header line"); }
    const std::size_t data_begin = static_cast<std::size_t>(first_newline - data) + 1U;
    const int workers = effective_worker_count(requested_workers, file.size() - data_begin);
    std::vector<std::size_t> boundaries;
    boundaries.reserve(static_cast<std::size_t>(workers) + 1U);
    boundaries.push_back(data_begin);
    for (int worker = 1; worker < workers; ++worker) {
        std::size_t boundary = data_begin + (file.size() - data_begin) * static_cast<std::size_t>(worker) / static_cast<std::size_t>(workers);
        while (boundary < file.size() && data[boundary - 1U] != '\n') { ++boundary; }
        if (boundary < file.size() && boundary > boundaries.back()) { boundaries.push_back(boundary); }
    }
    return ranges_from_boundaries(boundaries, file.size());
}
void for_each_csv_line(const PaddedMappedFile& file, const ByteRange range, mmltk::common::concurrency::CancellationObservation cancel_requested,
                       const std::function<void(std::string_view)>& callback) {
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
void validate_open_images_classes(const std::filesystem::path& path, const std::span<const StringCategoryMapping> mappings) {
    PaddedMappedFile classes(path);
    std::unordered_map<std::string_view, std::string_view> expected;
    expected.reserve(mappings.size());
    for (const StringCategoryMapping& mapping : mappings) {
        if (!expected.emplace(mapping.source_id, mapping.expected_name).second || mapping.target_id >= coco80_class_names().size()) {
            throw std::runtime_error("Open Images fixed category mapping contains a duplicate");
        }
    }
    std::unordered_set<std::string_view> matched;
    for_each_csv_line(classes, ByteRange{0U, classes.size()}, {}, [&](const std::string_view line) {
        std::vector<std::string> unescaped;
        const std::vector<std::string_view> fields = csv_fields(line, &unescaped);
        if (fields.size() < 2U) { throw std::runtime_error("Open Images class description row is malformed"); }
        const auto found = expected.find(fields[0]);
        if (found != expected.end()) {
            if (fields[1] != found->second || !matched.emplace(found->first).second) {
                throw std::runtime_error("Open Images class metadata disagrees with its fixed mapping");
            }
        }
    });
    if (matched.size() != expected.size()) { throw std::runtime_error("Open Images class metadata is missing a required mapped class"); }
}
void validate_index_layout(const NormalizedIndexHeader& header, const std::size_t file_size) {
    const std::uint64_t expected_box_offset = sizeof(NormalizedIndexHeader) + static_cast<std::uint64_t>(header.image_count) * sizeof(NormalizedImage);
    if (header.box_count > (std::numeric_limits<std::uint64_t>::max() - expected_box_offset) / sizeof(NormalizedBox)) {
        throw std::runtime_error("normalized benchmark annotation index size overflows");
    }
    const std::uint64_t expected_mask_offset = expected_box_offset + header.box_count * sizeof(NormalizedBox);
    if (header.mask_rle_pair_count > (std::numeric_limits<std::uint64_t>::max() - expected_mask_offset) / sizeof(RLEPair)) {
        throw std::runtime_error("normalized benchmark mask index size overflows");
    }
    const std::uint64_t expected_size = expected_mask_offset + header.mask_rle_pair_count * sizeof(RLEPair);
    if (header.magic != kNormalizedIndexMagic || header.version != kNormalizedAnnotationIndexVersion ||
        header.source > static_cast<std::uint8_t>(BenchmarkDatasetSource::kOpenImagesV7) || header.image_count == 0U ||
        header.image_offset != sizeof(NormalizedIndexHeader) || header.box_offset != expected_box_offset || header.mask_rle_offset != expected_mask_offset ||
        header.total_file_size != expected_size || header.total_file_size != file_size ||
        !std::ranges::all_of(header.reserved0, [](const std::uint8_t value) { return value == 0U; }) ||
        !std::ranges::all_of(header.reserved, [](const std::uint8_t value) { return value == 0U; })) {
        throw std::runtime_error("normalized benchmark annotation index header is invalid");
    }
}
void validate_normalized_records(const NormalizedAnnotationIndex& index, mmltk::common::concurrency::CancellationObservation cancel_requested) {
    std::uint64_t expected_first = 0U;
    std::uint64_t expected_mask_offset = 0U;
    std::uint64_t previous_id = 0U;
    bool first = true;
    for (std::size_t image_index = 0U; image_index < index.images.size(); ++image_index) {
        if ((image_index & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
        const NormalizedImage& image = index.images[image_index];
        if ((!first && image.source_image_id <= previous_id) || image.first_box != expected_first || image.first_box > index.boxes.size() ||
            image.box_count > index.boxes.size() - image.first_box ||
            (index.source != BenchmarkDatasetSource::kOpenImagesV7 && (image.width == 0U || image.height == 0U)) || image.reserved != 0U) {
            throw std::runtime_error("normalized benchmark image record is invalid at index " + std::to_string(image_index) +
                                     " (source_image_id=" + std::to_string(image.source_image_id) + ", previous_id=" + std::to_string(previous_id) +
                                     ", first_box=" + std::to_string(image.first_box) + ", expected_first_box=" + std::to_string(expected_first) +
                                     ", box_count=" + std::to_string(image.box_count) + ", width=" + std::to_string(image.width) +
                                     ", height=" + std::to_string(image.height) + ")");
        }
        first = false;
        previous_id = image.source_image_id;
        expected_first += image.box_count;
        for (std::uint64_t box_index = image.first_box; box_index < image.first_box + image.box_count; ++box_index) {
            const NormalizedBox& box = index.boxes[checked_cast<std::size_t>(box_index, "box index overflow")];
            if (box.class_id >= coco80_class_names().size() || !std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) ||
                !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1 || !std::isfinite(box.original_area) || box.original_area < 0.0 ||
                (box.flags & ~kAnnotationFlags) != 0U || (box.flags & kAnnotationCategory) == 0U ||
                ((box.flags & kAnnotationMask) == 0U && box.mask_rle_pairs != 0U) || ((box.flags & kAnnotationId) == 0U && box.annotation_id != 0U) ||
                ((box.flags & kAnnotationCategory) == 0U && box.source_category_id != 0U) || box.mask_rle_offset != expected_mask_offset ||
                expected_mask_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - expected_mask_offset ||
                !std::ranges::all_of(box.reserved, [](const std::uint8_t value) { return value == 0U; })) {
                throw std::runtime_error("normalized benchmark box record is invalid");
            }
            if (index.source == BenchmarkDatasetSource::kOpenImagesV7 && (box.flags & kAnnotationCategory) != 0U &&
                !valid_open_images_category(box.source_category_id))
                throw std::runtime_error("invalid normalized Open Images source category");
            const std::uint64_t mask_pixels = static_cast<std::uint64_t>(image.width) * image.height;
            std::uint64_t previous_end = 0U;
            for (std::uint64_t pair_index = box.mask_rle_offset; pair_index < box.mask_rle_offset + box.mask_rle_pairs; ++pair_index) {
                const RLEPair pair = index.mask_rle_pairs[checked_cast<std::size_t>(pair_index, "normalized mask offset overflow")];
                const std::uint64_t end = static_cast<std::uint64_t>(pair.start) + pair.length;
                if (pair.length == 0U || pair.start < previous_end || end > mask_pixels) {
                    throw std::runtime_error("normalized benchmark mask record is invalid");
                }
                previous_end = end;
            }
            expected_mask_offset += box.mask_rle_pairs;
        }
    }
    if (expected_first != index.boxes.size()) { throw std::runtime_error("normalized benchmark box records are not fully referenced"); }
    if (expected_mask_offset != index.mask_rle_pairs.size()) { throw std::runtime_error("normalized benchmark mask records are not fully referenced"); }
}
// Slice admission deliberately does not require persisted sorted image IDs.
std::span<const NormalizedBox> image_boxes(const NormalizedAnnotationIndex& index, std::size_t position) {
    const auto& image = index.images.at(position);
    if (image.first_box > index.boxes.size() || image.box_count > index.boxes.size() - image.first_box)
        throw std::runtime_error("normalized image slice box range is invalid");
    return std::span(index.boxes).subspan(static_cast<std::size_t>(image.first_box), image.box_count);
}
std::size_t slice_run_count(const NormalizedAnnotationIndex& index, std::span<const NormalizedBox> boxes,
                            mmltk::common::concurrency::CancellationObservation cancellation) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if ((i & 1023U) == 0) throw_if_benchmark_cancelled(cancellation);
        const auto& box = boxes[i];
        if (box.mask_rle_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - box.mask_rle_offset)
            throw std::runtime_error("normalized image slice mask range is invalid");
        if (box.mask_rle_pairs > index.mask_rle_pairs.max_size() - count)
            throw std::overflow_error("normalized image slice mask count overflow");
        count += box.mask_rle_pairs;
    }
    return count;
}
// Append and forward compaction share the complete admitted slice transfer.
// memmove handles overlapping runs, and each box is captured before overwriting it.
void transfer_image_slice(NormalizedAnnotationIndex& destination, const NormalizedAnnotationIndex& source,
                          std::size_t position, std::size_t image_destination, std::size_t box_destination,
                          std::size_t& run_destination, mmltk::common::concurrency::CancellationObservation cancellation) {
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
            if (&destination != &source) {
                destination.mask_rle_pairs.insert(destination.mask_rle_pairs.end(), input, input + count);
            } else {
                auto* target = destination.mask_rle_pairs.data() + run_destination + offset;
                if (target != input) std::memmove(target, input, count * sizeof(RLEPair));
            }
            offset += count;
        }
        box.mask_rle_offset = run_destination;
        run_destination += box.mask_rle_pairs;
        if (&destination != &source) destination.boxes.push_back(box);
        else destination.boxes[box_destination + i] = box;
    }
    if (&destination != &source) destination.images.push_back(image);
    else destination.images[image_destination] = image;
}
}  // namespace
void append_normalized_image_slice(NormalizedAnnotationIndex& destination, const NormalizedAnnotationIndex& source,
                                   std::size_t position, mmltk::common::concurrency::CancellationObservation cancellation) {
    throw_if_benchmark_cancelled(cancellation);
    if (&destination == &source) throw std::invalid_argument("normalized slice append requires distinct storage");
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
void retain_normalized_image_slices(NormalizedAnnotationIndex& index, std::span<const std::size_t> order,
                                    mmltk::common::concurrency::CancellationObservation cancellation) {
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
        if (boxes.size() > index.boxes.max_size() - box_count || runs > index.mask_rle_pairs.max_size() - run_count)
            throw std::overflow_error("normalized retained slice size overflow");
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
        NormalizedAnnotationIndex next;
        next.source = index.source;
        next.split = index.split;
        next.annotation_sha256 = index.annotation_sha256;
        next.rejected = index.rejected;
        next.images.reserve(order.size());
        next.boxes.reserve(box_count);
        next.mask_rle_pairs.reserve(run_count);
        for (const auto position : order) append_normalized_image_slice(next, index, position, cancellation);
        throw_if_benchmark_cancelled(cancellation);
        index = std::move(next);
    }
}
NormalizedAnnotationIndex parse_coco_style_annotations(const std::filesystem::path& json_path, std::string annotation_sha256,
                                                       const std::span<const NumericCategoryMapping> mappings, const AnnotationParseOptions& options) {
    PaddedMappedFile file(json_path);
    const ByteRange images_array = find_named_top_level_array(file, "images");
    const ByteRange annotations_array = find_named_top_level_array(file, "annotations");
    const ByteRange categories_array = find_named_top_level_array(file, "categories");
    const CategoryLookup category_lookup = make_numeric_lookup(mappings);
    validate_numeric_categories(file, categories_array, category_lookup, options.cancel_requested);
    const int workers = effective_worker_count(options.num_workers, images_array.end - images_array.begin);
    std::vector<std::vector<ParsedImage>> worker_images(static_cast<std::size_t>(workers));
    parallel_object_array(file, images_array, workers, options.cancel_requested, [&](const int worker, simdjson::ondemand::object object, std::uint64_t) {
        worker_images[static_cast<std::size_t>(worker)].push_back(parse_image_object(object, options.source));
    });
    std::size_t image_count = 0U;
    for (const auto& local : worker_images) { image_count += local.size(); }
    std::vector<ParsedImage> images;
    images.reserve(image_count);
    for (auto& local : worker_images) { images.insert(images.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end())); }
    std::ranges::sort(images, {}, &ParsedImage::id);
    if (images.empty() || std::ranges::adjacent_find(images, {}, &ParsedImage::id) != images.end() ||
        (options.expected_image_count != 0U && images.size() != options.expected_image_count)) {
        throw std::runtime_error("benchmark source image metadata count or IDs are invalid");
    }
    std::unordered_map<std::uint64_t, std::uint32_t> image_lookup;
    image_lookup.reserve(images.size());
    for (std::size_t index = 0U; index < images.size(); ++index) {
        image_lookup.emplace(images[index].id, checked_cast<std::uint32_t>(index, "benchmark image index overflow"));
    }
    std::vector<std::uint32_t> counts(images.size(), 0U);
    // Capacity only: inspect identities without constructing segmentation storage.
    // Semantic admission, rejection counters, and mask materialization have one owner below.
    parallel_object_array(file, annotations_array, workers, options.cancel_requested, [&](const int, simdjson::ondemand::object object, std::uint64_t) {
        std::optional<std::uint32_t> image_index;
        try {
            std::optional<std::uint64_t> image_id;
            std::optional<std::uint32_t> category_id;
            for (auto field : object) {
                const simdjson::ondemand::raw_json_string key = field.key();
                if (key == "image_id")
                    image_id = field.value().get_uint64().value();
                else if (key == "category_id")
                    category_id = checked_cast<std::uint32_t>(field.value().get_uint64().value(), "category ID overflow");
            }
            if (!image_id || !category_id || *category_id >= category_lookup.target_by_id.size() || category_lookup.target_by_id[*category_id] < 0) return;
            const auto image = image_lookup.find(*image_id);
            if (image == image_lookup.end()) return;
            image_index = image->second;
        } catch (const simdjson::simdjson_error&) { return; } catch (const std::runtime_error&) {
            return;
        }
        const auto previous = std::atomic_ref<std::uint32_t>(counts[*image_index]).fetch_add(1U, std::memory_order_relaxed);
        if (previous == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("benchmark per-image annotation capacity overflow");
    });
    std::vector<std::uint64_t> offsets(images.size() + 1U, 0U);
    for (std::size_t index = 0U; index < images.size(); ++index) {
        if (counts[index] > std::numeric_limits<std::uint64_t>::max() - offsets[index]) {
            throw std::overflow_error("benchmark normalized annotation offset overflow");
        }
        offsets[index + 1U] = offsets[index] + counts[index];
    }
    std::vector<NormalizedBox> boxes(checked_cast<std::size_t>(offsets.back(), "benchmark normalized box count overflow"));
    std::vector<std::vector<RLEPair>> masks(boxes.size());
    std::vector<std::uint64_t> cursors(offsets.begin(), offsets.end() - 1);
    std::vector<AnnotationRejectCounts> worker_rejected(static_cast<std::size_t>(workers));
    // Independent worker-local parsers retain their capacity and cannot invalidate
    // the outer document while decoding a borrowed segmentation slice.
    std::vector<simdjson::ondemand::parser> segmentation_parsers(static_cast<std::size_t>(workers));
    std::vector<SegmentationScratch> segmentation_scratch(static_cast<std::size_t>(workers));
    parallel_object_array(
        file, annotations_array, workers, options.cancel_requested, [&](const int worker, simdjson::ondemand::object object, std::uint64_t ordinal) {
            auto& scratch = segmentation_scratch[static_cast<std::size_t>(worker)];
            scratch.reset();
            auto& rejected = worker_rejected[static_cast<std::size_t>(worker)];
            ++rejected.raw_records;
            std::optional<BoxCandidate> candidate;
            try {
                const auto annotation = parse_annotation_object(object);
                candidate = normalize_coco_box(annotation, images, image_lookup, category_lookup, &rejected, file,
                                               segmentation_parsers[static_cast<std::size_t>(worker)], scratch);
            } catch (const simdjson::simdjson_error&) {
                ++rejected.malformed_records;
                return;
            } catch (const std::runtime_error&) {
                ++rejected.malformed_records;
                return;
            }
            if (candidate) {
                const std::uint64_t destination = std::atomic_ref<std::uint64_t>(cursors[candidate->image_index]).fetch_add(1U, std::memory_order_relaxed);
                if (destination >= offsets[candidate->image_index + 1U]) {
                    throw std::runtime_error("benchmark annotation fill exceeds its counted image span");
                }
                const std::size_t target = checked_cast<std::size_t>(destination, "benchmark normalized box offset overflow");
                candidate->box.source_ordinal = ordinal;
                candidate->box.mask_rle_offset = destination;
                candidate->box.mask_rle_pairs = checked_cast<std::uint32_t>(candidate->mask_rle.size(), "normalized mask run count overflow");
                boxes[target] = candidate->box;
                masks[target] = std::move(candidate->mask_rle);
            }
        });
    NormalizedAnnotationIndex result = begin_normalized_index_result(options.source, options.split, std::move(annotation_sha256), worker_rejected);
    compact_annotations(&result, images, offsets, cursors, std::move(boxes), std::move(masks), options.keep_images_without_mapped_boxes,
                        options.cancel_requested);
    validate_normalized_records(result, options.cancel_requested);
    trace_benchmark_event(options.trace, "benchmark.annotations.indexed", [&] {
        nlohmann::json event = normalized_index_trace_json(result);
        event["mask_rle_pairs"] = result.mask_rle_pairs.size();
        return event;
    });
    return result;
}
NormalizedAnnotationIndex parse_open_images_annotations(const std::filesystem::path& boxes_csv_path, const std::filesystem::path& classes_csv_path,
                                                        std::string annotation_sha256, const std::span<const StringCategoryMapping> mappings,
                                                        const AnnotationParseOptions& options) {
    validate_open_images_classes(classes_csv_path, mappings);
    std::unordered_map<std::string_view, std::uint8_t> category_lookup;
    category_lookup.reserve(mappings.size());
    for (const StringCategoryMapping& mapping : mappings) {
        if (!category_lookup.emplace(mapping.source_id, mapping.target_id).second) {
            throw std::runtime_error("Open Images category mapping contains a duplicate MID");
        }
    }
    PaddedMappedFile file(boxes_csv_path);
    const std::vector<ByteRange> ranges = newline_ranges(file, options.num_workers);
    const int workers = effective_worker_count(options.num_workers, ranges.size());
    struct ImageRun {
        std::uint64_t image_id = 0U;
        std::uint32_t count = 0U;
    };
    std::vector<std::vector<ImageRun>> worker_runs(ranges.size());
    std::vector<AnnotationRejectCounts> worker_rejected(ranges.size());
    parallel_for_range_indexed<std::size_t>(0U, ranges.size(), workers, [&](const int, const std::size_t begin, const std::size_t end) {
        for (std::size_t range_index = begin; range_index < end; ++range_index) {
            auto& runs = worker_runs[range_index];
            auto& rejected = worker_rejected[range_index];
            for_each_csv_line(file, ranges[range_index], options.cancel_requested, [&](const std::string_view line) {
                auto candidate = parse_open_images_line(line, category_lookup, &rejected);
                if (!candidate) { return; }
                if (runs.empty() || runs.back().image_id != candidate->image_id) {
                    if (!runs.empty() && candidate->image_id < runs.back().image_id) {
                        throw std::runtime_error("Open Images bounding boxes are not sorted by image ID");
                    }
                    runs.push_back(ImageRun{candidate->image_id, 1U});
                } else {
                    if (runs.back().count == std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error("Open Images per-image annotation count overflow");
                    }
                    ++runs.back().count;
                }
            });
        }
    });
    std::vector<ImageRun> merged;
    for (auto& runs : worker_runs) {
        for (const ImageRun run : runs) {
            if (!merged.empty() && merged.back().image_id == run.image_id) {
                if (run.count > std::numeric_limits<std::uint32_t>::max() - merged.back().count) {
                    throw std::overflow_error("Open Images merged per-image annotation count overflow");
                }
                merged.back().count += run.count;
            } else {
                if (!merged.empty() && run.image_id < merged.back().image_id) { throw std::runtime_error("Open Images partition merge is not ordered"); }
                merged.push_back(run);
            }
        }
    }
    if (merged.empty()) { throw std::runtime_error("Open Images has no mapped bounding-box images"); }
    std::unordered_map<std::uint64_t, std::uint32_t> image_lookup;
    image_lookup.reserve(merged.size());
    std::vector<ParsedImage> images;
    images.reserve(merged.size());
    std::vector<std::uint64_t> offsets(merged.size() + 1U, 0U);
    for (std::size_t index = 0U; index < merged.size(); ++index) {
        images.push_back(ParsedImage{merged[index].image_id, 0U, 0U, 0U});
        image_lookup.emplace(merged[index].image_id, checked_cast<std::uint32_t>(index, "Open Images image index overflow"));
        if (merged[index].count > std::numeric_limits<std::uint64_t>::max() - offsets[index]) {
            throw std::overflow_error("Open Images annotation offset overflow");
        }
        offsets[index + 1U] = offsets[index] + merged[index].count;
    }
    std::vector<NormalizedBox> boxes(checked_cast<std::size_t>(offsets.back(), "Open Images box count overflow"));
    std::vector<std::uint64_t> cursors(offsets.begin(), offsets.end() - 1);
    parallel_for_range_indexed<std::size_t>(0U, ranges.size(), workers, [&](const int, const std::size_t begin, const std::size_t end) {
        for (std::size_t range_index = begin; range_index < end; ++range_index) {
            for_each_csv_line(file, ranges[range_index], options.cancel_requested, [&](const std::string_view line) {
                AnnotationRejectCounts ignored;
                auto candidate = parse_open_images_line(line, category_lookup, &ignored);
                if (!candidate) { return; }
                const auto image = image_lookup.find(candidate->image_id);
                if (image == image_lookup.end()) { throw std::runtime_error("Open Images count and fill image sets disagree"); }
                const std::uint64_t destination = std::atomic_ref<std::uint64_t>(cursors[image->second]).fetch_add(1U, std::memory_order_relaxed);
                if (destination >= offsets[image->second + 1U]) { throw std::runtime_error("Open Images annotation fill exceeds its counted image span"); }
                candidate->box.source_ordinal = static_cast<std::uint64_t>(line.data() - file.data());
                candidate->box.mask_rle_offset = destination;
                boxes[checked_cast<std::size_t>(destination, "Open Images box offset overflow")] = candidate->box;
            });
        }
    });
    for (std::size_t index = 0U; index < images.size(); ++index) {
        if (cursors[index] != offsets[index + 1U]) { throw std::runtime_error("Open Images count and fill passes disagree"); }
    }
    NormalizedAnnotationIndex result =
        begin_normalized_index_result(BenchmarkDatasetSource::kOpenImagesV7, options.split, std::move(annotation_sha256), worker_rejected);
    compact_annotations(&result, images, offsets, cursors, std::move(boxes), std::nullopt, false, options.cancel_requested);
    validate_normalized_records(result, options.cancel_requested);
    trace_benchmark_event(options.trace, "benchmark.annotations.indexed", [&] { return normalized_index_trace_json(result); });
    return result;
}
std::optional<NormalizedAnnotationIndex> load_normalized_annotation_index(const std::filesystem::path& path, const BenchmarkDatasetSource expected_source,
                                                                          const std::string_view expected_split,
                                                                          const std::string_view expected_annotation_sha256,
                                                                          mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                          const BenchmarkTraceSink& trace) {
    try {
        throw_if_benchmark_cancelled(cancel_requested);
        if (!std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(normalized_manifest_path(path))) { return std::nullopt; }
        const nlohmann::json manifest = read_json_file(normalized_manifest_path(path));
        if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || manifest.value("index_version", 0U) != kNormalizedAnnotationIndexVersion ||
            !manifest.value("complete", false) || manifest.value("mapping_revision", std::string{}) != kBenchmarkMappingRevision ||
            manifest.value("source", std::string{}) != benchmark_source_name(expected_source) || manifest.value("split", std::string{}) != expected_split ||
            manifest.value("annotation_sha256", std::string{}) != expected_annotation_sha256) {
            return std::nullopt;
        }
        const FileHandle file = FileHandle::open_readonly(path.string());
        NormalizedIndexHeader header{};
        file.pread_all(&header, sizeof(header), 0U);
        validate_index_layout(header, file.size());
        if (header.source != static_cast<std::uint8_t>(expected_source) ||
            std::string_view(header.split.data(), ::strnlen(header.split.data(), header.split.size())) != expected_split ||
            std::string_view(header.mapping_revision.data(), ::strnlen(header.mapping_revision.data(), header.mapping_revision.size())) !=
                kBenchmarkMappingRevision ||
            header.annotation_sha256 != mmltk::common::io::parse_sha256_hex(std::string(expected_annotation_sha256)) ||
            manifest.value("size", 0ULL) != file.size() || manifest.value("identity", std::string{}).empty()) {
            return std::nullopt;
        }
        NormalizedAnnotationIndex index;
        index.source = expected_source;
        index.split = std::string(expected_split);
        index.annotation_sha256 = std::string(expected_annotation_sha256);
        index.rejected = decode_rejected(header.rejected);
        index.images.resize(header.image_count);
        index.boxes.resize(checked_cast<std::size_t>(header.box_count, "normalized box count overflow"));
        index.mask_rle_pairs.resize(checked_cast<std::size_t>(header.mask_rle_pair_count, "normalized mask run count overflow"));
        file.pread_all(index.images.data(), index.images.size() * sizeof(NormalizedImage), header.image_offset);
        file.pread_all(index.boxes.data(), index.boxes.size() * sizeof(NormalizedBox), header.box_offset);
        file.pread_all(index.mask_rle_pairs.data(), index.mask_rle_pairs.size() * sizeof(RLEPair), header.mask_rle_offset);
        validate_normalized_records(index, cancel_requested);
        trace_benchmark_event(trace, "benchmark.annotations.cache_hit", [&] {
            return nlohmann::json{{"source", benchmark_source_name(index.source)},
                                  {"split", index.split},
                                  {"images", index.images.size()},
                                  {"boxes", index.boxes.size()},
                                  {"mask_rle_pairs", index.mask_rle_pairs.size()}};
        });
        return index;
    } catch (const std::exception& error) {
        if (cancel_requested.requested()) { throw; }
        trace_benchmark_event(trace, "benchmark.annotations.cache_invalid", [&] { return nlohmann::json{{"path", path.string()}, {"error", error.what()}}; });
        return std::nullopt;
    }
}
void store_normalized_annotation_index(const std::filesystem::path& path, const NormalizedAnnotationIndex& index,
                                       mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace) {
    validate_normalized_records(index, cancel_requested);
    if (index.split.empty() || index.annotation_sha256.empty() || index.split.size() >= NormalizedIndexHeader{}.split.size() ||
        kBenchmarkMappingRevision.size() >= NormalizedIndexHeader{}.mapping_revision.size()) {
        throw std::runtime_error("normalized benchmark annotation index identity is invalid");
    }
    const std::uint64_t image_offset = sizeof(NormalizedIndexHeader);
    if (index.images.size() > (std::numeric_limits<std::uint64_t>::max() - image_offset) / sizeof(NormalizedImage)) {
        throw std::overflow_error("normalized image block size overflow");
    }
    const std::uint64_t box_offset = image_offset + static_cast<std::uint64_t>(index.images.size()) * sizeof(NormalizedImage);
    if (index.boxes.size() > (std::numeric_limits<std::uint64_t>::max() - box_offset) / sizeof(NormalizedBox)) {
        throw std::overflow_error("normalized box block size overflow");
    }
    const std::uint64_t mask_rle_offset = box_offset + static_cast<std::uint64_t>(index.boxes.size()) * sizeof(NormalizedBox);
    if (index.mask_rle_pairs.size() > (std::numeric_limits<std::uint64_t>::max() - mask_rle_offset) / sizeof(RLEPair)) {
        throw std::overflow_error("normalized mask block size overflow");
    }
    const std::uint64_t total_size = mask_rle_offset + static_cast<std::uint64_t>(index.mask_rle_pairs.size()) * sizeof(RLEPair);
    NormalizedIndexHeader header;
    header.source = static_cast<std::uint8_t>(index.source);
    header.image_count = checked_cast<std::uint32_t>(index.images.size(), "normalized image count overflow");
    header.box_count = index.boxes.size();
    header.mask_rle_pair_count = index.mask_rle_pairs.size();
    header.image_offset = image_offset;
    header.box_offset = box_offset;
    header.mask_rle_offset = mask_rle_offset;
    header.total_file_size = total_size;
    header.annotation_sha256 = mmltk::common::io::parse_sha256_hex(index.annotation_sha256);
    std::memcpy(header.split.data(), index.split.data(), index.split.size());
    std::memcpy(header.mapping_revision.data(), kBenchmarkMappingRevision.data(), kBenchmarkMappingRevision.size());
    header.rejected = encode_rejected(index.rejected);
    (void)mmltk::common::io::ensure_parent_directory(path);
    std::string staging_text = path.string() + ".tmp.XXXXXX";
    FileHandle staging = FileHandle::create_unique_output(staging_text, checked_cast<std::size_t>(total_size, "normalized index size overflow"));
    const std::filesystem::path staging_path(staging_text);
    try {
        staging.pwrite_all(&header, sizeof(header), 0U);
        staging.pwrite_all(index.images.data(), index.images.size() * sizeof(NormalizedImage),
                           checked_cast<std::size_t>(image_offset, "normalized image offset overflow"));
        staging.pwrite_all(index.boxes.data(), index.boxes.size() * sizeof(NormalizedBox),
                           checked_cast<std::size_t>(box_offset, "normalized box offset overflow"));
        staging.pwrite_all(index.mask_rle_pairs.data(), index.mask_rle_pairs.size() * sizeof(RLEPair),
                           checked_cast<std::size_t>(mask_rle_offset, "normalized mask offset overflow"));
        staging.sync_data();
        staging = FileHandle{};
        std::string identity_material = index.annotation_sha256 + "\n" + std::string(kBenchmarkMappingRevision) + "\n" + index.split + "\n" +
                                        std::to_string(total_size) + "\n" + std::to_string(index.images.size()) + "\n" + std::to_string(index.boxes.size());
        identity_material += "\n" + std::to_string(index.mask_rle_pairs.size());
        const std::string identity = mmltk::common::io::sha256_hex(
            mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
        const std::filesystem::path completion = normalized_manifest_path(path);
        std::error_code error;
        const bool removed = std::filesystem::remove(completion, error);
        if (error) { throw std::filesystem::filesystem_error("cannot invalidate normalized annotation completion manifest", completion, error); }
        if (removed) { sync_parent_directory(completion); }
        throw_if_benchmark_cancelled(cancel_requested);
        publish_staged_path_atomically(staging_path, path, true);
        write_json_atomically(completion,
                              nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                             {"index_version", kNormalizedAnnotationIndexVersion},
                                             {"complete", true},
                                             {"source", benchmark_source_name(index.source)},
                                             {"split", index.split},
                                             {"mapping_revision", kBenchmarkMappingRevision},
                                             {"annotation_sha256", index.annotation_sha256},
                                             {"size", total_size},
                                             {"identity", identity},
                                             {"integrity_mode", "atomic_layout_identity"},
                                             {"images", index.images.size()},
                                             {"mask_rle_pairs", index.mask_rle_pairs.size()},
                                             {"boxes", index.boxes.size()}},
                              cancel_requested);
        trace_benchmark_event(trace, "benchmark.annotations.cache_store", [&] {
            return nlohmann::json{{"source", benchmark_source_name(index.source)},
                                  {"split", index.split},
                                  {"images", index.images.size()},
                                  {"boxes", index.boxes.size()},
                                  {"bytes", total_size}};
        });
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(staging_path, ignored);
        throw;
    }
}
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationIndex& index, const std::optional<std::uint16_t> shard) {
    std::vector<std::uint64_t> ids;
    ids.reserve(index.images.size());
    for (const NormalizedImage& image : index.images) {
        if (!shard || image.source_shard == *shard) { ids.push_back(image.source_image_id); }
    }
    return ids;
}
}  // namespace mmltk::backend::data::benchmark_internal
