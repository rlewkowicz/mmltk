#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/imaging/resample/image_resize.h"
// CLEANUP-IGNORE: This module implementation has an independent global-fragment and import preamble.
#include <immintrin.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
#include <unordered_map>
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
    std::array<int16_t, 4> bbox{};
    std::vector<RLEPair> rle_pairs;
};
struct ParsedRle {
    std::vector<RLEPair> pairs;
    size_t foreground = 0;
};
struct ParsedLabels {
    std::vector<ParsedInstance> instances;
    std::uint64_t dropped_instances = 0;
};
struct WorkerResult {
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
    uint32_t original_width = 0;
    uint32_t original_height = 0;
};
struct ImageDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
};
struct MaskResizeLookup {
    ImageDimensions source_dims{};
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    std::vector<uint32_t> source_x;
    std::vector<uint32_t> source_y;
    void prepare(const ImageDimensions& source, const uint32_t target_w, const uint32_t target_h) {
        if (target_w == 0 || target_h == 0) { throw std::runtime_error("mask resize target dimensions must be positive"); }
        if (source_dims.width == source.width && source_dims.height == source.height && target_width == target_w && target_height == target_h) { return; }
        source_dims = source;
        target_width = target_w;
        target_height = target_h;
        source_x.resize(target_w);
        source_y.resize(target_h);
        mmltk::backend::data::dataset::fill_center_scale_lookup(source_x, target_w, source.width, "scaled mask x overflow");
        mmltk::backend::data::dataset::fill_center_scale_lookup(source_y, target_h, source.height, "scaled mask y overflow");
    }
};
struct MaskBounds {
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x = 0;
    uint32_t max_y = 0;
    bool has_foreground = false;
    explicit MaskBounds(const ImageDimensions& dimensions) : min_x(dimensions.width), min_y(dimensions.height) {}
    void include_row_major_run(size_t start, size_t end, uint32_t width) {
        has_foreground = true;
        const auto start_y = checked_cast<uint32_t>(start / width, "mask run y overflow");
        const auto start_x = checked_cast<uint32_t>(start % width, "mask run x overflow");
        const auto end_position = end - 1;
        const auto end_y = checked_cast<uint32_t>(end_position / width, "mask run y overflow");
        const auto end_x = checked_cast<uint32_t>(end_position % width, "mask run x overflow");
        min_y = std::min(min_y, start_y);
        max_y = std::max(max_y, end_y + 1);
        if (start_y == end_y) {
            min_x = std::min(min_x, start_x);
            max_x = std::max(max_x, end_x + 1);
        } else {
            min_x = 0;
            max_x = width;
        }
    }
    [[nodiscard]] std::array<int16_t, 4> bbox() const {
        return {
            checked_cast<int16_t>(min_x, "mask bbox x1 overflow"),
            checked_cast<int16_t>(min_y, "mask bbox y1 overflow"),
            checked_cast<int16_t>(max_x, "mask bbox x2 overflow"),
            checked_cast<int16_t>(max_y, "mask bbox y2 overflow"),
        };
    }
    [[nodiscard]] std::array<int64_t, 4> diagnostic_bbox() const noexcept {
        return {
            static_cast<int64_t>(min_x),
            static_cast<int64_t>(min_y),
            static_cast<int64_t>(max_x),
            static_cast<int64_t>(max_y),
        };
    }
};
struct ResizeObservation {
    bool needs_resize = false;
    bool needs_downscale = false;
    ImageDimensions source_dimensions{};
};
struct ResizedMask {
    std::vector<RLEPair> rle_pairs;
    bool has_foreground = false;
    std::array<int16_t, 4> bbox{};
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
        previous_end = static_cast<size_t>(start) + length;
        parsed.foreground += length;
        parsed.pairs.push_back({start, length});
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
bool read_declared_bbox(const json& record, std::array<int64_t, 4>* bbox) {
    if (bbox == nullptr) { return false; }
    const auto it = record.find("bbox_xyxy");
    if (it == record.end() || !it->is_array() || it->size() != bbox->size()) { return false; }
    for (size_t index = 0; index < bbox->size(); ++index) {
        const json& value = (*it)[index];
        if (!value.is_number_integer()) { return false; }
        (*bbox)[index] = value.get<std::int64_t>();
    }
    return true;
}
void append_diagnostic(std::vector<CompileDiagnostic>* diagnostics, CompileDiagnostic diagnostic) noexcept {
    if (diagnostics == nullptr) { return; }
    try {
        diagnostics->push_back(std::move(diagnostic));
    } catch (...) { return; }
}
// Builds the diagnostic fields shared by every label-resize diagnostic record.
[[nodiscard]] CompileDiagnostic make_resize_diagnostic(const CompileDiagnosticKind kind, const std::filesystem::path& annotation_file,
                                                       const std::string& class_name, const size_t line_number, const ImageDimensions& source_dims,
                                                       const uint32_t target_width, const uint32_t target_height, const size_t source_foreground) {
    CompileDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.annotation_path = annotation_file.string();
    diagnostic.class_name = class_name;
    diagnostic.line = line_number;
    diagnostic.source_width = source_dims.width;
    diagnostic.source_height = source_dims.height;
    diagnostic.target_width = target_width;
    diagnostic.target_height = target_height;
    diagnostic.source_foreground = source_foreground;
    return diagnostic;
}
void append_source_bbox_mismatch(std::vector<CompileDiagnostic>* diagnostics, const json& record, const std::filesystem::path& annotation_file,
                                 const std::string& class_name, const size_t line_number, const ImageDimensions& source_dims, const uint32_t target_width,
                                 const uint32_t target_height, const MaskBounds& source_bounds, const size_t source_foreground) {
    if (diagnostics == nullptr) { return; }
    std::array<int64_t, 4> declared_bbox{};
    if (!read_declared_bbox(record, &declared_bbox)) { return; }
    const std::array<int64_t, 4> mask_bbox = source_bounds.diagnostic_bbox();
    if (declared_bbox == mask_bbox) { return; }
    CompileDiagnostic diagnostic = make_resize_diagnostic(CompileDiagnosticKind::kSourceBoundingBoxMismatch, annotation_file, class_name, line_number,
                                                          source_dims, target_width, target_height, source_foreground);
    diagnostic.declared_bbox = declared_bbox;
    diagnostic.mask_bbox = mask_bbox;
    append_diagnostic(diagnostics, std::move(diagnostic));
}
size_t checked_run_end(const RLEPair& pair, size_t mask_pixels) {
    const size_t start = pair.start;
    const size_t length = pair.length;
    if (start > mask_pixels || length > mask_pixels - start) { throw std::runtime_error("mask_rle run exceeds image bounds"); }
    return start + length;
}
void materialize_mask_row_major(const std::vector<RLEPair>& input_pairs, const ImageDimensions& source_dims, std::vector<uint8_t>& source_mask_scratch,
                                MaskBounds* source_bounds = nullptr) {
    const size_t source_pixels = static_cast<size_t>(source_dims.width) * source_dims.height;
    if (source_mask_scratch.size() != source_pixels) { source_mask_scratch.resize(source_pixels); }
    std::ranges::fill(source_mask_scratch, uint8_t{0});
    for (const RLEPair& pair : input_pairs) {
        const size_t start = pair.start;
        const size_t end = checked_run_end(pair, source_pixels);
        std::fill(source_mask_scratch.data() + start, source_mask_scratch.data() + end, uint8_t{1});
        if (source_bounds != nullptr) { source_bounds->include_row_major_run(start, end, source_dims.width); }
    }
}
ResizedMask encode_dense_mask_row_major(const uint8_t* dense_mask, uint32_t target_width, uint32_t target_height) {
    ResizedMask resized;
    const size_t target_pixels = static_cast<size_t>(target_width) * target_height;
    if (target_pixels == 0 || dense_mask == nullptr) { return resized; }
    const __m256i zero = _mm256_setzero_si256();
    MaskBounds bounds({target_width, target_height});
    size_t cursor = 0;
    while (cursor < target_pixels) {
        while (cursor + 32 <= target_pixels) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dense_mask + cursor));
            const auto nonzero_mask = static_cast<uint32_t>(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero)));
            if (nonzero_mask != 0) {
                cursor += static_cast<uint32_t>(__builtin_ctz(nonzero_mask));
                goto found_start;
            }
            cursor += 32;
        }
        while (cursor < target_pixels && dense_mask[cursor] == 0) { ++cursor; }
        if (cursor >= target_pixels) { break; }
    found_start:;
        const size_t run_start = cursor;
        while (cursor + 32 <= target_pixels) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dense_mask + cursor));
            const auto zero_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero)));
            if (zero_mask != 0) {
                cursor += static_cast<uint32_t>(__builtin_ctz(zero_mask));
                goto found_end;
            }
            cursor += 32;
        }
        while (cursor < target_pixels && dense_mask[cursor] != 0) { ++cursor; }
    found_end:;
        resized.rle_pairs.push_back({
            checked_cast<uint32_t>(run_start, "resized mask run start overflow"),
            checked_cast<uint32_t>(cursor - run_start, "resized mask run length overflow"),
        });
        bounds.include_row_major_run(run_start, cursor, target_width);
    }
    resized.has_foreground = bounds.has_foreground;
    if (bounds.has_foreground) { resized.bbox = bounds.bbox(); }
    return resized;
}
void clear_mask_letterbox_padding(std::vector<uint8_t>& target_mask, const uint32_t target_width, const uint32_t target_height,
                                  const mmltk::backend::imaging::resample::RgbLetterbox& letterbox) {
    const size_t top_pixels = static_cast<size_t>(letterbox.offset_y) * target_width;
    if (top_pixels != 0U) { std::fill_n(target_mask.data(), top_pixels, uint8_t{0}); }
    const uint32_t right = target_width - letterbox.offset_x - letterbox.resized_width;
    if (letterbox.offset_x != 0U || right != 0U) {
        for (uint32_t row = 0U; row < letterbox.resized_height; ++row) {
            uint8_t* target_row = target_mask.data() + static_cast<size_t>(letterbox.offset_y + row) * target_width;
            std::fill_n(target_row, letterbox.offset_x, uint8_t{0});
            std::fill_n(target_row + letterbox.offset_x + letterbox.resized_width, right, uint8_t{0});
        }
    }
    const uint32_t content_end_y = letterbox.offset_y + letterbox.resized_height;
    const size_t bottom_pixels = static_cast<size_t>(target_height - content_end_y) * target_width;
    if (bottom_pixels != 0U) { std::fill_n(target_mask.data() + static_cast<size_t>(content_end_y) * target_width, bottom_pixels, uint8_t{0}); }
}
ResizedMask resize_mask_row_major(const std::vector<RLEPair>& input_pairs, const ImageDimensions& source_dims, uint32_t target_width, uint32_t target_height,
                                  const mmltk::backend::imaging::resample::RgbLetterbox& letterbox, std::vector<uint8_t>& source_mask_scratch,
                                  std::vector<uint8_t>& target_mask_scratch, MaskResizeLookup& resize_lookup, MaskBounds* source_bounds) {
    ResizedMask resized;
    if (input_pairs.empty()) { return resized; }
    const size_t target_pixels = static_cast<size_t>(target_width) * target_height;
    materialize_mask_row_major(input_pairs, source_dims, source_mask_scratch, source_bounds);
    if (target_mask_scratch.size() != target_pixels) { target_mask_scratch.resize(target_pixels); }
    clear_mask_letterbox_padding(target_mask_scratch, target_width, target_height, letterbox);
    resize_lookup.prepare(source_dims, letterbox.resized_width, letterbox.resized_height);
    for (uint32_t y = 0; y < letterbox.resized_height; ++y) {
        const uint32_t src_y = resize_lookup.source_y[y];
        const uint8_t* src_row = source_mask_scratch.data() + static_cast<size_t>(src_y) * source_dims.width;
        uint8_t* dst_row = target_mask_scratch.data() + static_cast<size_t>(letterbox.offset_y + y) * target_width + letterbox.offset_x;
        for (uint32_t x = 0; x < letterbox.resized_width; ++x) { dst_row[x] = src_row[resize_lookup.source_x[x]] != 0 ? 1u : 0u; }
    }
    return encode_dense_mask_row_major(target_mask_scratch.data(), target_width, target_height);
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ParsedLabels parse_jsonl(const std::filesystem::path& annotation_file, const std::filesystem::path& image_file,
                         const std::unordered_map<std::string, uint8_t>& class_map, const CompilerConfig& config, ResizeObservation* resize_observation,
                         std::vector<uint8_t>& source_mask_scratch, std::vector<uint8_t>& target_mask_scratch, MaskResizeLookup& resize_lookup,
                         std::vector<CompileDiagnostic>* diagnostics) {
    mmltk::common::logging::ScopedProfile profile{"compiler.labels.parse_jsonl"};
    ParsedLabels parsed_labels;
    std::ifstream file(annotation_file);
    if (!file.is_open()) { throw std::runtime_error("failed to open annotation file: " + annotation_file.string()); }
    const ImageDimensions source_dims = load_image_dimensions(image_file);
    const uint32_t target_width = config.target_width;
    const uint32_t target_height = config.target_height;
    const bool exact_target = source_dims.width == target_width && source_dims.height == target_height;
    const mmltk::backend::imaging::resample::RgbLetterbox letterbox =
        exact_target ? mmltk::backend::imaging::resample::RgbLetterbox{target_width, target_height, 0U, 0U}
                     : mmltk::backend::imaging::resample::compute_rgb_letterbox(source_dims.width, source_dims.height, target_width, target_height);
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
        const std::string class_name = record["class"].get<std::string>();
        auto class_it = class_map.find(class_name);
        if (class_it == class_map.end()) {
            throw std::runtime_error("annotation class '" + class_name + "' is not declared in categories.json: " + annotation_file.string() + " at line " +
                                     std::to_string(line_number));
        }
        ParsedInstance instance{};
        instance.class_id = class_it->second;
        try {
            if (!record.contains("mask_rle_encoding") || !record["mask_rle_encoding"].is_string() ||
                record["mask_rle_encoding"].get_ref<const std::string&>() != "row_major_start_length") {
                throw std::runtime_error("mask_rle_encoding must be 'row_major_start_length'");
            }
            if (!record.contains("mask_rle") || !record["mask_rle"].is_string()) { throw std::runtime_error("mask_rle must be a string"); }
            ParsedRle parsed_rle = parse_rle(record["mask_rle"].get_ref<const std::string&>());
            if (parsed_rle.pairs.empty()) { throw std::runtime_error("mask_rle has no foreground"); }
            const size_t source_foreground = parsed_rle.foreground;
            instance.rle_pairs = std::move(parsed_rle.pairs);
            MaskBounds source_bounds(source_dims);
            MaskBounds* const source_bounds_out = diagnostics != nullptr ? &source_bounds : nullptr;
            if (needs_resize) {
                ResizedMask resized = resize_mask_row_major(instance.rle_pairs, source_dims, target_width, target_height, letterbox, source_mask_scratch,
                                                            target_mask_scratch, resize_lookup, source_bounds_out);
                if (source_bounds_out != nullptr) {
                    append_source_bbox_mismatch(diagnostics, record, annotation_file, class_name, line_number, source_dims, target_width, target_height,
                                                source_bounds, source_foreground);
                }
                if (!resized.has_foreground) {
                    if (diagnostics != nullptr) {
                        append_diagnostic(diagnostics, make_resize_diagnostic(CompileDiagnosticKind::kDroppedInstanceAfterResize, annotation_file, class_name,
                                                                              line_number, source_dims, target_width, target_height, source_foreground));
                    }
                    ++parsed_labels.dropped_instances;
                    mmltk::common::logging::profile_add_value("compiler.labels.dropped_instances", 1);
                    continue;
                }
                instance.rle_pairs = std::move(resized.rle_pairs);
                instance.bbox = resized.bbox;
            } else {
                materialize_mask_row_major(instance.rle_pairs, source_dims, source_mask_scratch, source_bounds_out);
                ResizedMask decoded = encode_dense_mask_row_major(source_mask_scratch.data(), source_dims.width, source_dims.height);
                if (!decoded.has_foreground) { throw std::runtime_error("mask_rle has no foreground"); }
                if (source_bounds_out != nullptr) {
                    append_source_bbox_mismatch(diagnostics, record, annotation_file, class_name, line_number, source_dims, target_width, target_height,
                                                source_bounds, source_foreground);
                }
                instance.rle_pairs = std::move(decoded.rle_pairs);
                instance.bbox = decoded.bbox;
            }
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
    PackedInstance packed{};
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
        parsed.raw_id = category["id"].get<int>();
        parsed_categories.push_back(std::move(parsed));
        min_raw_id = std::min(min_raw_id, parsed_categories.back().raw_id);
    }
    if (parsed_categories.empty()) { throw std::runtime_error("no classes found in categories.json"); }
    const int class_id_base = min_raw_id == 0 ? 0 : min_raw_id == 1 ? 1 : throw std::runtime_error("class ids must be dense and start at 0 or 1");
    std::array<bool, MAX_CLASSES> seen_ids{};
    for (const auto& category : parsed_categories) {
        check_cancelled();
        const int normalized_id = category.raw_id - class_id_base;
        if (normalized_id < 0 || normalized_id >= static_cast<int>(MAX_CLASSES)) {
            throw std::runtime_error("class id out of supported range in categories.json");
        }
        const auto class_id = static_cast<uint8_t>(normalized_id);
        if (seen_ids[class_id]) { throw std::runtime_error("duplicate class id in categories.json"); }
        seen_ids[class_id] = true;
        auto inserted = scan.class_map.emplace(category.name, class_id);
        if (!inserted.second) { throw std::runtime_error("duplicate class name in categories.json"); }
    }
    for (size_t i = 0; i < scan.class_map.size(); ++i) {
        if (!seen_ids[i]) { throw std::runtime_error("class ids must be dense and start at 0 or 1"); }
    }
    std::vector<std::string> ordered_names(scan.class_map.size());
    for (const auto& [name, index] : scan.class_map) ordered_names[index] = name;
    const catalog::ClassCatalog validated_catalog(std::move(ordered_names), 31U);
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
                               const std::unordered_map<std::string, uint8_t>& class_map, int num_workers, const std::span<const int> worker_cpus,
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
            std::vector<uint8_t> source_mask_scratch;
            std::vector<uint8_t> target_mask_scratch;
            MaskResizeLookup resize_lookup;
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
                    ParsedLabels parsed_labels = parse_jsonl(annotation_path(split_dir, image_index), image_path(split_dir, image_index), class_map, config,
                                                             &resize_observation, source_mask_scratch, target_mask_scratch, resize_lookup, diagnostics);
                    if (resize_observation.needs_resize) { any_image_resize.store(true, std::memory_order_relaxed); }
                    if (resize_observation.needs_downscale) { any_image_downscale.store(true, std::memory_order_relaxed); }
                    WorkerResult& result = worker_results[image_index];
                    result.original_width = resize_observation.source_dimensions.width;
                    result.original_height = resize_observation.source_dimensions.height;
                    std::vector<ParsedInstance>& instances = parsed_labels.instances;
                    result.labels.reserve(instances.size());
                    const size_t instance_rle_pairs = parsed_rle_pair_count(instances);
                    result.rle_pairs.reserve(instance_rle_pairs);
                    for (const ParsedInstance& instance : instances) {
                        result.labels.push_back(pack_instance(instance));
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
