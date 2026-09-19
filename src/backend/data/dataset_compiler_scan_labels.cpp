#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/imaging/resample/image_resize.h"
// CLEANUP-IGNORE: This module implementation has an independent global-fragment and import preamble.
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/math/checked_arithmetic.h"
#include "stb_image.h"
#if MMLTK_ENABLE_PROFILING
#include <chrono>
#endif
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "detail/dataset_compiler_internal.h"
#include "detail/mask_rle_utils.h"
using json = nlohmann::json;
namespace mmltk::backend::data::compiler_internal {
using mmltk::common::concurrency::parallel_for_range_indexed;
using mmltk::common::math::checked_cast;
namespace {
struct ParsedInstance {
    uint8_t class_id;
    std::array<float, 4> bbox{};
    PackedInstance metadata{};
    std::vector<RLEPair> rle_pairs;
};
struct ParsedRle {
    std::vector<RLEPair> pairs;
    size_t foreground = 0;
};
struct ParsedLabels {
    std::vector<ParsedInstance> instances;
    std::uint64_t dropped_instances = 0;
    bool has_image_id = false;
    std::uint64_t image_id = 0;
};
struct WorkerResult {
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
    bool has_image_id = false;
    std::uint64_t image_id = 0;
    uint32_t original_width = 0;
    uint32_t original_height = 0;
};
using ImageDimensions = dataset::MaskDimensions;
struct ResizeObservation {
    bool needs_resize = false;
    bool needs_downscale = false;
    ImageDimensions source_dimensions{};
};
struct LabelWorkerStats {
#if MMLTK_ENABLE_PROFILING
    std::uint64_t active_ns = 0;
    std::uint64_t images = 0;
    std::uint64_t instances = 0;
    std::uint64_t rle_pairs = 0;
#endif
    std::uint64_t dropped_instances = 0;
};
#if MMLTK_ENABLE_PROFILING
std::uint64_t compute_skew_x1000(std::uint64_t total, std::uint64_t peak, std::uint64_t count) {
    if (count == 0 || total == 0) { return 0; }
    return (peak * 1000ULL * count) / total;
}
#endif
std::filesystem::path numbered_path(const std::filesystem::path& dir, uint32_t one_based_index, const char* extension) {
    return dir / std::format("{:06}{}", one_based_index, extension);
}
uint32_t count_sequential_images(const std::filesystem::path& split_dir, mmltk::common::concurrency::CancellationObservation cancel_requested) {
    mmltk::common::logging::ScopedProfile profile{"compiler.scan.count_sequential_images"};
    if (!std::filesystem::exists(split_dir) || !std::filesystem::is_directory(split_dir)) {
        throw std::runtime_error("split directory not found: " + split_dir.string());
    }
    uint32_t png_count = 0U;
    uint32_t jsonl_count = 0U;
    uint32_t max_png_index = 0U;
    uint32_t max_jsonl_index = 0U;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(split_dir)) {
        if (cancel_requested.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
        const std::string extension = entry.path().extension().string();
        if (extension != ".png" && extension != ".jsonl") { continue; }
        const std::string stem = entry.path().stem().string();
        uint32_t one_based_index = 0U;
        const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), one_based_index);
        if (stem.size() != 6U || parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size() || one_based_index == 0U) {
            throw std::runtime_error("dataset split contains a non-sequential annotation asset: " + entry.path().string());
        }
        if (extension == ".png") {
            ++png_count;
            max_png_index = std::max(max_png_index, one_based_index);
        } else {
            ++jsonl_count;
            max_jsonl_index = std::max(max_jsonl_index, one_based_index);
        }
    }
    if (png_count == 0U) { throw std::runtime_error("no sequential PNG images found under: " + split_dir.string()); }
    if (png_count != max_png_index || jsonl_count != max_jsonl_index || png_count != jsonl_count) {
        throw std::runtime_error("dataset split PNG and JSONL assets must form matching contiguous sequences: " + split_dir.string());
    }
    mmltk::common::logging::profile_set_value("compiler.scan.num_images", png_count);
    return png_count;
}
ParsedRle parse_rle(const std::string_view rle) {
    ParsedRle parsed;
    const char* cursor = rle.data();
    const char* const end = cursor + rle.size();
    size_t previous_end = 0;
    while (cursor != end) {
        while (cursor != end && std::isspace(static_cast<unsigned char>(*cursor))) { ++cursor; }
        if (cursor == end) { break; }
        uint32_t start = 0;
        uint32_t length = 0;
        const auto start_result = std::from_chars(cursor, end, start);
        if (start_result.ec != std::errc{} || start_result.ptr == cursor) { throw std::runtime_error("mask_rle contains an invalid or overflowing run start"); }
        cursor = start_result.ptr;
        if (cursor == end || *cursor != ':') { throw std::runtime_error("mask_rle run is missing ':' separator"); }
        ++cursor;
        const auto length_result = std::from_chars(cursor, end, length);
        if (length_result.ec != std::errc{} || length_result.ptr == cursor || length == 0U) {
            throw std::runtime_error("mask_rle contains an invalid, zero, or overflowing run length");
        }
        cursor = length_result.ptr;
        if (cursor != end && !std::isspace(static_cast<unsigned char>(*cursor))) { throw std::runtime_error("mask_rle runs must be separated by whitespace"); }
        if (!parsed.pairs.empty() && static_cast<size_t>(start) < previous_end) {
            throw std::runtime_error("mask_rle runs must be sorted and non-overlapping");
        }
        if (length > std::numeric_limits<size_t>::max() - parsed.foreground) { throw std::runtime_error("mask_rle foreground count overflow"); }
        const bool adjacent = !parsed.pairs.empty() && static_cast<size_t>(start) == previous_end;
        previous_end = static_cast<size_t>(start) + length;
        parsed.foreground += length;
        if (adjacent) parsed.pairs.back().length = checked_cast<uint32_t>(static_cast<std::uint64_t>(parsed.pairs.back().length) + length, "mask run length overflow");
        else parsed.pairs.push_back({start, length});
    }
    return parsed;
}
ImageDimensions load_image_dimensions(const std::filesystem::path& image_file) {
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(image_file.c_str(), &width, &height, &channels) == 0) {
        throw std::runtime_error("failed to read image dimensions for " + image_file.string());
    }
    if (width <= 0 || height <= 0) { throw std::runtime_error("invalid image dimensions for " + image_file.string()); }
    return {
        checked_cast<uint32_t>(width, "image width overflow"),
        checked_cast<uint32_t>(height, "image height overflow"),
    };
}
void validate_record_image_dimensions(const json& record, const std::filesystem::path& annotation_file, const ImageDimensions& source_dims) {
    if (!record.contains("image_size_wh")) { return; }
    if (!record["image_size_wh"].is_array() || record["image_size_wh"].size() != 2) {
        throw std::runtime_error("invalid image_size_wh in " + annotation_file.string());
    }
    const int record_width = record["image_size_wh"][0].get<int>();
    const int record_height = record["image_size_wh"][1].get<int>();
    if (record_width != static_cast<int>(source_dims.width) || record_height != static_cast<int>(source_dims.height)) {
        throw std::runtime_error("annotation image_size_wh does not match PNG dimensions in " + annotation_file.string());
    }
}
void append_diagnostic(std::vector<CompileDiagnostic>* diagnostics, CompileDiagnostic diagnostic) noexcept {
    if (diagnostics == nullptr) return;
    try { diagnostics->push_back(std::move(diagnostic)); } catch (...) {}
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ParsedLabels parse_jsonl(const std::filesystem::path& annotation_file, const std::filesystem::path& image_file,
                         const catalog::ClassCatalog& class_catalog, const CompilerConfig& config, ResizeObservation* resize_observation,
                         dataset::MaskResizeScratch& mask_scratch,
                         std::vector<CompileDiagnostic>* diagnostics) {
    mmltk::common::logging::ScopedProfile profile{"compiler.labels.parse_jsonl"};
    ParsedLabels parsed_labels;
    std::ifstream file(annotation_file);
    if (!file.is_open()) { throw std::runtime_error("failed to open annotation file: " + annotation_file.string()); }
    const ImageDimensions source_dims = load_image_dimensions(image_file);
    const uint32_t target_width = config.target_width;
    const uint32_t target_height = config.target_height;
    const bool exact_target = source_dims.width == target_width && source_dims.height == target_height;
    const mmltk::backend::imaging::resample::ImageResizeGeometry letterbox =
        exact_target ? mmltk::backend::imaging::resample::ImageResizeGeometry{target_width, target_height, 0U, 0U}
                     : mmltk::backend::imaging::resample::compute_image_resize_geometry(source_dims.width, source_dims.height, target_width, target_height, config.resize_mode);
    const bool needs_resize = !exact_target;
    const bool needs_downscale = source_dims.width > letterbox.resized_width || source_dims.height > letterbox.resized_height;
    if (resize_observation != nullptr) {
        resize_observation->needs_resize = needs_resize;
        resize_observation->needs_downscale = needs_downscale;
        resize_observation->source_dimensions = source_dims;
    }
    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) { continue; }
        json record = json::parse(line, nullptr, false);
        if (record.is_discarded()) {
            throw std::runtime_error("invalid JSON annotation record in " + annotation_file.string() + " at line " + std::to_string(line_number));
        }
        validate_record_image_dimensions(record, annotation_file, source_dims);
        if (record.contains("image_id")) {
            const auto& value = record["image_id"];
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
                throw std::runtime_error("image_id must be a nonnegative integer");
            const auto id = value.get<std::uint64_t>();
            if (parsed_labels.has_image_id && parsed_labels.image_id != id) throw std::runtime_error("inconsistent source image identities");
            parsed_labels.image_id = id;
            parsed_labels.has_image_id = true;
        }
        const std::string class_name = record["class"].get<std::string>();
        const auto class_id = class_catalog.resolve(class_name);
        if (!class_id) {
            throw std::runtime_error("annotation class '" + class_name + "' is not declared in categories.json: " + annotation_file.string() + " at line " +
                                     std::to_string(line_number));
        }
        ParsedInstance instance{};
        instance.class_id = checked_cast<std::uint8_t>(*class_id, "compiled class index overflow");
        try {
            auto& metadata = instance.metadata;
            metadata.source_ordinal = line_number;
            const auto read_flag = [&](const char* name, const std::uint8_t bit) {
                const auto field = record.find(name);
                if (field == record.end()) return;
                if (!field->is_boolean() && !field->is_number_integer()) throw std::runtime_error("annotation flag must be boolean or 0/1");
                bool enabled = false;
                if (field->is_boolean()) enabled = field->get<bool>();
                else if (field->is_number_unsigned()) {
                    const auto value = field->get<std::uint64_t>();
                    if (value > 1U) throw std::runtime_error("annotation flag must be 0/1");
                    enabled = value == 1U;
                } else {
                    const auto value = field->get<std::int64_t>();
                    if (value != 0 && value != 1) throw std::runtime_error("annotation flag must be 0/1");
                    enabled = value == 1;
                }
                if (enabled) metadata.flags |= bit;
            };
            read_flag("iscrowd", kAnnotationCrowd);
            read_flag("ignore", kAnnotationIgnore);
            const auto read_identity = [&](const char* name, const std::uint8_t bit) -> std::uint64_t {
                const auto field = record.find(name);
                if (field == record.end()) return 0U;
                if (!field->is_number_integer() || (field->is_number_integer() && !field->is_number_unsigned() && field->get<std::int64_t>() < 0))
                    throw std::runtime_error("annotation identity must be a nonnegative integer");
                metadata.flags |= bit;
                return field->get<std::uint64_t>();
            };
            metadata.annotation_id = read_identity("id", kAnnotationId);
            metadata.source_category_id = read_identity("category_id", kAnnotationCategory);
            const bool has_mask = record.contains("mask_rle");
            ParsedRle parsed_rle;
            if (has_mask) {
                if (record.value("mask_rle_encoding", std::string{}) != "row_major_start_length" || !record["mask_rle"].is_string())
                    throw std::runtime_error("mask_rle requires row_major_start_length string encoding");
                metadata.flags |= kAnnotationMask;
                parsed_rle = parse_rle(record["mask_rle"].get_ref<const std::string&>());
            }
            const auto source_bounds = dataset::row_major_mask_bounds(parsed_rle.pairs, source_dims);
            std::array<double, 4> source_box{};
            if (record.contains("bbox_xyxy")) {
                const auto& box = record["bbox_xyxy"];
                if (!box.is_array() || box.size() != 4U) throw std::runtime_error("bbox_xyxy must have four numeric coordinates");
                for (size_t coordinate = 0; coordinate < 4U; ++coordinate) {
                    if (!box[coordinate].is_number()) throw std::runtime_error("bbox_xyxy coordinates must be numeric");
                    source_box[coordinate] = box[coordinate].get<double>();
                }
            } else {
                if (!source_bounds.has_foreground) throw std::runtime_error("annotation requires a bbox or a nonempty source mask");
                source_box = {static_cast<double>(source_bounds.min_x), static_cast<double>(source_bounds.min_y),
                              static_cast<double>(source_bounds.max_x), static_cast<double>(source_bounds.max_y)};
            }
            if (!std::ranges::all_of(source_box, [](double value) { return std::isfinite(value); }) ||
                source_box[2] <= source_box[0] || source_box[3] <= source_box[1]) throw std::runtime_error("invalid source bbox");
            if (diagnostics != nullptr && has_mask && source_bounds.has_foreground && record.contains("bbox_xyxy")) {
                const std::array<double, 4> mask_box{static_cast<double>(source_bounds.min_x), static_cast<double>(source_bounds.min_y),
                                                     static_cast<double>(source_bounds.max_x), static_cast<double>(source_bounds.max_y)};
                if (source_box != mask_box) {
                    CompileDiagnostic diagnostic;
                    diagnostic.kind = CompileDiagnosticKind::kSourceBoundingBoxMismatch;
                    diagnostic.annotation_path = annotation_file.string(); diagnostic.class_name = class_name; diagnostic.line = line_number;
                    diagnostic.source_width = source_dims.width; diagnostic.source_height = source_dims.height;
                    diagnostic.target_width = target_width; diagnostic.target_height = target_height;
                    diagnostic.source_foreground = parsed_rle.foreground;
                    diagnostic.declared_bbox = source_box; diagnostic.mask_bbox = mask_box;
                    append_diagnostic(diagnostics, std::move(diagnostic));
                }
            }
            metadata.original_area = record.contains("area") ? record["area"].get<double>() :
                has_mask ? static_cast<double>(parsed_rle.foreground) : (source_box[2] - source_box[0]) * (source_box[3] - source_box[1]);
            if (!std::isfinite(metadata.original_area) || metadata.original_area < 0.0) throw std::runtime_error("invalid annotation area");
            for (size_t coordinate = 0; coordinate < 4U; ++coordinate) {
                const bool x = (coordinate & 1U) == 0U;
                const double scaled = source_box[coordinate] * (x ? letterbox.resized_width : letterbox.resized_height) /
                                      (x ? source_dims.width : source_dims.height) + (x ? letterbox.offset_x : letterbox.offset_y);
                constexpr double limit = std::numeric_limits<float>::max();
                if (!std::isfinite(scaled) || scaled < -limit || scaled > limit) throw std::runtime_error("transformed bbox overflow");
                instance.bbox[coordinate] = static_cast<float>(scaled);
            }
            if (instance.bbox[2] <= instance.bbox[0] || instance.bbox[3] <= instance.bbox[1])
                throw std::runtime_error("transformed bbox loses strict corner ordering");
            instance.rle_pairs = needs_resize ? dataset::resize_row_major_mask(parsed_rle.pairs, source_dims, {target_width, target_height}, letterbox, &mask_scratch).pairs : std::move(parsed_rle.pairs);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + " in " + annotation_file.string() + " at line " + std::to_string(line_number) + " (source " +
                                     std::to_string(source_dims.width) + "x" + std::to_string(source_dims.height) + ", target " + std::to_string(target_width) +
                                     "x" + std::to_string(target_height) + ")");
        }
        mmltk::common::logging::profile_add_value("compiler.labels.instances", 1);
        mmltk::common::logging::profile_add_value("compiler.labels.rle_pairs", instance.rle_pairs.size());
        parsed_labels.instances.push_back(std::move(instance));
    }
    return parsed_labels;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
size_t parsed_rle_pair_count(const std::vector<ParsedInstance>& instances) {
    size_t total_rle_pairs = 0;
    for (const ParsedInstance& instance : instances) { total_rle_pairs += instance.rle_pairs.size(); }
    return total_rle_pairs;
}
PackedInstance pack_instance(const ParsedInstance& instance) {
    PackedInstance packed = instance.metadata;
    packed.class_id = instance.class_id;
    packed.bbox_x1 = instance.bbox[0];
    packed.bbox_y1 = instance.bbox[1];
    packed.bbox_x2 = instance.bbox[2];
    packed.bbox_y2 = instance.bbox[3];
    packed.mask_rle_pairs = checked_cast<uint16_t>(instance.rle_pairs.size(), "too many RLE pairs for one instance");
    return packed;
}
std::pair<size_t, size_t> aggregate_worker_sizes(const std::vector<WorkerResult>& worker_results) {
    size_t total_labels = 0;
    size_t total_rle_pairs = 0;
    for (const WorkerResult& result : worker_results) {
        total_labels += result.labels.size();
        total_rle_pairs += result.rle_pairs.size();
    }
    return {total_labels, total_rle_pairs};
}
}  // namespace
std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) {
    return numbered_path(split_dir, zero_based_index + 1, ".png");
}
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) {
    return numbered_path(split_dir, zero_based_index + 1, ".jsonl");
}
DatasetScan scan_dataset(const CompilerConfig& config, const std::vector<std::string>& splits,
                         const mmltk::common::concurrency::CancellationObservation cancellation) {
    const auto check_cancelled = [&] {
        if (cancellation.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
    };
    check_cancelled();
    const std::filesystem::path categories_path = std::filesystem::path(config.source_dir) / "categories.json";
    std::ifstream file(categories_path);
    if (!file.is_open()) { throw std::runtime_error("cannot open categories file: " + categories_path.string()); }
    json categories = json::parse(file);
    DatasetScan scan;
    struct ParsedCategory {
        std::string name;
        int raw_id = -1;
    };
    std::vector<ParsedCategory> parsed_categories;
    parsed_categories.reserve(categories["classes"].size());
    int min_raw_id = std::numeric_limits<int>::max();
    for (const auto& category : categories["classes"]) {
        check_cancelled();
        ParsedCategory parsed;
        parsed.name = category["name"].get<std::string>();
        const auto& id = category["id"];
        if (!id.is_number_integer() || (!id.is_number_unsigned() && id.get<std::int64_t>() < 0) ||
            id.get<std::uint64_t>() > MAX_CLASSES)
            throw std::runtime_error("class id must be an exact nonnegative integer in the supported range");
        parsed.raw_id = static_cast<int>(id.get<std::uint64_t>());
        parsed_categories.push_back(std::move(parsed));
        min_raw_id = std::min(min_raw_id, parsed_categories.back().raw_id);
    }
    if (parsed_categories.empty()) { throw std::runtime_error("no classes found in categories.json"); }
    const int class_id_base = min_raw_id == 0 ? 0 : min_raw_id == 1 ? 1 : throw std::runtime_error("class ids must be dense and start at 0 or 1");
    if (parsed_categories.size() > MAX_CLASSES) throw std::runtime_error("too many classes in categories.json");
    scan.source_category_base = static_cast<std::uint8_t>(class_id_base);
    std::vector<std::string> ordered_names(parsed_categories.size());
    std::array<bool, MAX_CLASSES> seen_ids{};
    for (const auto& category : parsed_categories) {
        check_cancelled();
        const int normalized_id = category.raw_id - class_id_base;
        if (normalized_id < 0 || normalized_id >= static_cast<int>(ordered_names.size())) {
            throw std::runtime_error("class id out of supported range in categories.json");
        }
        const auto class_id = static_cast<uint8_t>(normalized_id);
        if (seen_ids[class_id]) { throw std::runtime_error("duplicate class id in categories.json"); }
        seen_ids[class_id] = true;
        ordered_names[class_id] = category.name;
    }
    scan.class_catalog = catalog::ClassCatalog(std::move(ordered_names), COMPILED_CLASS_NAME_CAPACITY);
    scan.splits.reserve(splits.size());
    for (const std::string& split : splits) {
        check_cancelled();
        if (split.empty()) { throw std::runtime_error("dataset split name must not be empty"); }
        uint32_t num_images = 0U;
        if (categories.contains("splits") && categories["splits"].contains(split)) {
            num_images = categories["splits"][split]["total"].get<uint32_t>();
            if (num_images == 0U) { throw std::runtime_error("categories.json declares an empty dataset split: " + split); }
            mmltk::common::logging::debug(
                [&](spdlog::logger& log) { log.debug("[compile] split '{}' count from categories.json: {} images", split, num_images); });
        } else {
            num_images = count_sequential_images(std::filesystem::path(config.source_dir) / split, cancellation);
        }
        scan.splits.push_back(DatasetCompileSplitPlan{split, num_images});
    }
    check_cancelled();
    return scan;
}
LabelBlocks build_label_blocks(const std::filesystem::path& split_dir, uint32_t num_images, const CompilerConfig& config,
                               const catalog::ClassCatalog& class_catalog, std::uint8_t source_category_base, int num_workers, const std::span<const int> worker_cpus,
                               ProgressCounter* completed_images, std::atomic<bool>* failure_requested,
                               const mmltk::common::concurrency::CancellationObservation cancellation) {
    mmltk::common::logging::ScopedProfile profile{"compiler.labels.build_blocks"};
    auto record_worker_stats = [&](const std::vector<LabelWorkerStats>& worker_stats) {
#if MMLTK_ENABLE_PROFILING
        std::uint64_t active_workers = 0;
        std::uint64_t total_active_ns = 0;
        std::uint64_t max_active_ns = 0;
        std::uint64_t total_instances = 0;
        std::uint64_t max_instances = 0;
#endif
        std::uint64_t total_dropped_instances = 0;
        for (const LabelWorkerStats& stats : worker_stats) {
#if MMLTK_ENABLE_PROFILING
            if (stats.images == 0) { continue; }
            ++active_workers;
            mmltk::common::logging::profile_record_duration_ns("compiler.labels.worker.active", stats.active_ns);
            mmltk::common::logging::profile_add_value("compiler.labels.worker.images", stats.images);
            mmltk::common::logging::profile_add_value("compiler.labels.worker.instances", stats.instances);
            mmltk::common::logging::profile_add_value("compiler.labels.worker.rle_pairs", stats.rle_pairs);
            mmltk::common::logging::profile_add_value("compiler.labels.worker.dropped_instances", stats.dropped_instances);
            total_active_ns += stats.active_ns;
            max_active_ns = std::max(max_active_ns, stats.active_ns);
            total_instances += stats.instances;
            max_instances = std::max(max_instances, stats.instances);
#endif
            total_dropped_instances += stats.dropped_instances;
        }
#if MMLTK_ENABLE_PROFILING
        mmltk::common::logging::profile_set_value("compiler.labels.worker.count", active_workers);
        mmltk::common::logging::profile_set_value("compiler.labels.worker.active_skew_x1000",
                                                  compute_skew_x1000(total_active_ns, max_active_ns, active_workers));
        mmltk::common::logging::profile_set_value("compiler.labels.worker.instances_skew_x1000",
                                                  compute_skew_x1000(total_instances, max_instances, active_workers));
#endif
        return total_dropped_instances;
    };
    std::atomic<bool> any_image_resize{false};
    std::atomic<bool> any_image_downscale{false};
    std::atomic<bool> worker_cancelled{false};
    std::vector<WorkerResult> worker_results(num_images);
    const int worker_count = std::max(1, std::min(num_workers, checked_cast<int>(num_images, "label worker count overflow")));
    std::vector<LabelWorkerStats> worker_stats(static_cast<size_t>(worker_count));
    std::vector<std::vector<CompileDiagnostic>> worker_diagnostics;
    if (config.diagnostics != nullptr) { worker_diagnostics.resize(static_cast<size_t>(worker_count)); }
    parallel_for_range_indexed<uint32_t>(
        0, num_images, num_workers, worker_cpus, [&](int worker_id, uint32_t start, uint32_t end) {  // NOLINT(bugprone-easily-swappable-parameters)
            LabelWorkerStats& stats = worker_stats[static_cast<size_t>(worker_id)];
#if MMLTK_ENABLE_PROFILING
            const auto worker_start = std::chrono::steady_clock::now();
#endif
            dataset::MaskResizeScratch mask_scratch;
            ProgressBatch progress(completed_images);
            std::vector<CompileDiagnostic>* diagnostics = worker_diagnostics.empty() ? nullptr : &worker_diagnostics[static_cast<size_t>(worker_id)];
            for (uint32_t image_index = start; image_index < end; ++image_index) {
                if (cancellation.requested()) {
                    worker_cancelled.store(true, std::memory_order_relaxed);
                    throw std::runtime_error("dataset compilation cancelled");
                }
                if (worker_cancelled.load(std::memory_order_relaxed) || (failure_requested != nullptr && failure_requested->load(std::memory_order_relaxed))) {
                    break;
                }
                try {
                    ResizeObservation resize_observation;
                    ParsedLabels parsed_labels = parse_jsonl(annotation_path(split_dir, image_index), image_path(split_dir, image_index), class_catalog, config,
                                                             &resize_observation, mask_scratch, diagnostics);
                    if (resize_observation.needs_resize) { any_image_resize.store(true, std::memory_order_relaxed); }
                    if (resize_observation.needs_downscale) { any_image_downscale.store(true, std::memory_order_relaxed); }
                    WorkerResult& result = worker_results[image_index];
                    result.has_image_id = parsed_labels.has_image_id;
                    result.image_id = parsed_labels.image_id;
                    result.original_width = resize_observation.source_dimensions.width;
                    result.original_height = resize_observation.source_dimensions.height;
                    std::vector<ParsedInstance>& instances = parsed_labels.instances;
                    result.labels.reserve(instances.size());
                    const size_t instance_rle_pairs = parsed_rle_pair_count(instances);
                    result.rle_pairs.reserve(instance_rle_pairs);
                    for (const ParsedInstance& instance : instances) {
                        auto packed = pack_instance(instance);
                        if ((packed.flags & kAnnotationCategory) == 0U) {
                            packed.source_category_id = static_cast<std::uint64_t>(packed.class_id) + source_category_base;
                            packed.flags |= kAnnotationCategory;
                        }
                        result.labels.push_back(packed);
                        result.rle_pairs.insert(result.rle_pairs.end(), instance.rle_pairs.begin(), instance.rle_pairs.end());
                    }
#if MMLTK_ENABLE_PROFILING
                    ++stats.images;
                    stats.instances += instances.size();
                    stats.rle_pairs += instance_rle_pairs;
#endif
                    stats.dropped_instances += parsed_labels.dropped_instances;
                    progress.increment();
                } catch (...) {
                    worker_cancelled.store(true, std::memory_order_relaxed);
                    if (failure_requested != nullptr) { failure_requested->store(true, std::memory_order_relaxed); }
                    throw;
                }
            }
#if MMLTK_ENABLE_PROFILING
            stats.active_ns = checked_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - worker_start).count(), "worker active time overflow");
#endif
        });
    if (config.diagnostics != nullptr) {
        for (std::vector<CompileDiagnostic>& diagnostics : worker_diagnostics) {
            for (CompileDiagnostic& diagnostic : diagnostics) { append_diagnostic(config.diagnostics, std::move(diagnostic)); }
        }
    }
    const std::uint64_t total_dropped_instances = record_worker_stats(worker_stats);
    const auto [total_labels, total_rle_pairs] = aggregate_worker_sizes(worker_results);
    LabelBlocks blocks;
    blocks.index.resize(num_images);
    blocks.labels.resize(total_labels);
    blocks.rle_pairs.resize(total_rle_pairs);
    blocks.any_image_resize = any_image_resize.load(std::memory_order_relaxed);
    blocks.any_image_downscale = any_image_downscale.load(std::memory_order_relaxed);
    blocks.dropped_instances = total_dropped_instances;
    mmltk::common::logging::profile_set_value("compiler.labels.total_labels", total_labels);
    mmltk::common::logging::profile_set_value("compiler.labels.total_rle_pairs", total_rle_pairs);
    size_t label_cursor = 0;
    size_t rle_cursor = 0;
    std::uint16_t max_instances_per_image = 0;
    {
        mmltk::common::logging::ScopedProfile merge_profile{"compiler.labels.merge_blocks"};
        for (uint32_t image_index = 0; image_index < num_images; ++image_index) {
            WorkerResult& result = worker_results[image_index];
            ImageEntry& entry = blocks.index[image_index];
            entry.has_source_image_id = result.has_image_id;
            entry.source_image_id = result.image_id;
            entry.original_width = result.original_width;
            entry.original_height = result.original_height;
            entry.num_instances = checked_cast<uint16_t>(result.labels.size(), "too many instances for one image");
            max_instances_per_image = std::max(max_instances_per_image, entry.num_instances);
            entry.label_offset = checked_cast<uint32_t>(label_cursor * sizeof(PackedInstance), "label offset overflow");
            entry.label_bytes = checked_cast<uint32_t>(result.labels.size() * sizeof(PackedInstance), "label bytes overflow");
            const size_t image_rle_start = rle_cursor;
            size_t image_rle_cursor = image_rle_start;
            for (PackedInstance& packed : result.labels) {
                packed.mask_rle_offset = checked_cast<uint32_t>(image_rle_cursor * sizeof(RLEPair), "mask RLE offset overflow");
                image_rle_cursor += packed.mask_rle_pairs;
            }
            if (image_rle_cursor - image_rle_start != result.rle_pairs.size()) {
                throw std::runtime_error("label and RLE counts diverged while assembling label blocks");
            }
            if (!result.labels.empty()) { std::ranges::copy(result.labels, blocks.labels.data() + label_cursor); }
            if (!result.rle_pairs.empty()) { std::ranges::copy(result.rle_pairs, blocks.rle_pairs.data() + image_rle_start); }
            label_cursor += result.labels.size();
            rle_cursor = image_rle_cursor;
        }
    }
    blocks.max_instances_per_image = max_instances_per_image;
    if (mmltk::common::logging::profile_enabled()) {
        mmltk::common::logging::profile_set_value("compiler.labels.max_instances_per_image", max_instances_per_image);
    }
    return blocks;
}
}  // namespace mmltk::backend::data::compiler_internal
