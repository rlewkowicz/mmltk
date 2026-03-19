#include "dataset_compiler_internal.h"
#include "debug_utils.h"
#include "profile_utils.h"

#include <nlohmann/json.hpp>
#include "stb_image.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace fastloader::compiler_internal {

namespace {

struct ParsedInstance {
    uint8_t class_id;
    int16_t bbox[4];
    std::vector<RLEPair> rle_pairs;
};

struct WorkerResult {
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
};

struct ImageDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ResizedMask {
    std::vector<RLEPair> rle_pairs;
    bool has_foreground = false;
    int16_t bbox[4] = {0, 0, 0, 0};
};

struct LabelWorkerStats {
    std::uint64_t active_ns = 0;
    std::uint64_t images = 0;
    std::uint64_t instances = 0;
    std::uint64_t rle_pairs = 0;
};

std::uint64_t compute_skew_x1000(std::uint64_t total, std::uint64_t peak, std::uint64_t count) {
    if (count == 0 || total == 0) {
        return 0;
    }
    return (peak * 1000ULL * count) / total;
}

std::filesystem::path numbered_path(const std::filesystem::path& dir,
                                    uint32_t one_based_index,
                                    const char* extension) {
    std::array<char, 32> fname{};
    const int written = std::snprintf(fname.data(), fname.size(), "%06u%s", one_based_index, extension);
    if (written <= 0 || static_cast<size_t>(written) >= fname.size()) {
        throw std::runtime_error("failed to format sequential filename");
    }
    return dir / std::string(fname.data(), static_cast<size_t>(written));
}

uint32_t count_sequential_images(const std::filesystem::path& split_dir) {
    FASTLOADER_PROFILE_SCOPE("compiler.scan.count_sequential_images");
    if (!std::filesystem::exists(split_dir) || !std::filesystem::is_directory(split_dir)) {
        throw std::runtime_error("split directory not found: " + split_dir.string());
    }

    uint32_t count = 0;
    while (std::filesystem::exists(image_path(split_dir, count))) {
        const std::filesystem::path jsonl_path = annotation_path(split_dir, count);
        if (!std::filesystem::exists(jsonl_path)) {
            throw std::runtime_error("missing annotation file: " + jsonl_path.string());
        }
        ++count;
    }
    if (count == 0) {
        throw std::runtime_error("no sequential PNG images found under: " + split_dir.string());
    }

    FASTLOADER_PROFILE_SET("compiler.scan.num_images", count);
    return count;
}

std::vector<RLEPair> parse_rle(const std::string& rle_str) {
    std::vector<RLEPair> pairs;
    const char* cursor = rle_str.c_str();
    while (*cursor) {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }

        uint32_t start = 0;
        uint32_t length = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            start = start * 10 + static_cast<uint32_t>(*cursor - '0');
            ++cursor;
        }
        if (*cursor == ':') {
            ++cursor;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            length = length * 10 + static_cast<uint32_t>(*cursor - '0');
            ++cursor;
        }
        pairs.push_back({start, length});
    }
    return pairs;
}

ImageDimensions load_image_dimensions(const std::filesystem::path& image_file) {
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(image_file.c_str(), &width, &height, &channels) == 0) {
        throw std::runtime_error("failed to read image dimensions for " + image_file.string());
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("invalid image dimensions for " + image_file.string());
    }
    return {
        checked_cast<uint32_t>(width, "image width overflow"),
        checked_cast<uint32_t>(height, "image height overflow"),
    };
}

void validate_record_image_dimensions(const json& record,
                                      const std::filesystem::path& annotation_file,
                                      const ImageDimensions& source_dims) {
    if (!record.contains("image_size_wh")) {
        return;
    }
    if (!record["image_size_wh"].is_array() || record["image_size_wh"].size() != 2) {
        throw std::runtime_error("invalid image_size_wh in " + annotation_file.string());
    }
    const int record_width = record["image_size_wh"][0].get<int>();
    const int record_height = record["image_size_wh"][1].get<int>();
    if (record_width != static_cast<int>(source_dims.width) ||
        record_height != static_cast<int>(source_dims.height)) {
        throw std::runtime_error("annotation image_size_wh does not match PNG dimensions in " +
                                 annotation_file.string());
    }
}

uint32_t clamp_coord_to_extent(std::int64_t value, uint32_t extent) {
    if (value < 0) {
        return 0;
    }
    const auto clamped = std::min<std::int64_t>(value, static_cast<std::int64_t>(extent));
    return checked_cast<uint32_t>(clamped, "scaled coordinate overflow");
}

int16_t scale_box_min(int16_t coord, uint32_t source_extent, uint32_t target_extent) {
    if (coord < 0) {
        throw std::runtime_error("negative bbox coordinate is not supported");
    }
    const double scaled =
        std::floor((static_cast<double>(coord) * static_cast<double>(target_extent)) /
                   static_cast<double>(source_extent));
    const uint32_t clamped =
        clamp_coord_to_extent(static_cast<std::int64_t>(scaled), target_extent);
    return checked_cast<int16_t>(clamped, "scaled bbox minimum overflow");
}

int16_t scale_box_max(int16_t coord, uint32_t source_extent, uint32_t target_extent) {
    if (coord < 0) {
        throw std::runtime_error("negative bbox coordinate is not supported");
    }
    const double scaled =
        std::ceil((static_cast<double>(coord) * static_cast<double>(target_extent)) /
                  static_cast<double>(source_extent));
    const uint32_t clamped =
        clamp_coord_to_extent(static_cast<std::int64_t>(scaled), target_extent);
    return checked_cast<int16_t>(clamped, "scaled bbox maximum overflow");
}

void scale_bbox_in_place(ParsedInstance& instance,
                         const ImageDimensions& source_dims,
                         uint32_t target_width,
                         uint32_t target_height) {
    instance.bbox[0] = scale_box_min(instance.bbox[0], source_dims.width, target_width);
    instance.bbox[1] = scale_box_min(instance.bbox[1], source_dims.height, target_height);
    instance.bbox[2] = scale_box_max(instance.bbox[2], source_dims.width, target_width);
    instance.bbox[3] = scale_box_max(instance.bbox[3], source_dims.height, target_height);
}

ResizedMask resize_mask_row_major(const std::vector<RLEPair>& input_pairs,
                                  const ImageDimensions& source_dims,
                                  uint32_t target_width,
                                  uint32_t target_height,
                                  std::vector<uint8_t>& source_mask_scratch,
                                  std::vector<uint8_t>& target_mask_scratch) {
    ResizedMask resized;
    if (input_pairs.empty()) {
        return resized;
    }

    const size_t source_pixels = static_cast<size_t>(source_dims.width) * source_dims.height;
    const size_t target_pixels = static_cast<size_t>(target_width) * target_height;
    if (source_mask_scratch.size() != source_pixels) {
        source_mask_scratch.resize(source_pixels);
    }
    std::fill(source_mask_scratch.begin(), source_mask_scratch.end(), uint8_t{0});
    if (target_mask_scratch.size() != target_pixels) {
        target_mask_scratch.resize(target_pixels);
    }

    for (const RLEPair& pair : input_pairs) {
        const size_t start = pair.start;
        const size_t length = pair.length;
        if (start > source_pixels || length > source_pixels - start) {
            throw std::runtime_error("mask_rle run exceeds image bounds");
        }
        std::fill_n(source_mask_scratch.data() + start, length, uint8_t{1});
    }

    uint32_t min_x = target_width;
    uint32_t min_y = target_height;
    uint32_t max_x = 0;
    uint32_t max_y = 0;

    for (uint32_t y = 0; y < target_height; ++y) {
        const uint32_t src_y = std::min<uint32_t>(
            source_dims.height - 1,
            checked_cast<uint32_t>(((static_cast<std::uint64_t>(2) * y + 1) * source_dims.height) /
                                       (static_cast<std::uint64_t>(2) * target_height),
                                   "scaled mask y overflow"));
        for (uint32_t x = 0; x < target_width; ++x) {
            const uint32_t src_x = std::min<uint32_t>(
                source_dims.width - 1,
                checked_cast<uint32_t>(((static_cast<std::uint64_t>(2) * x + 1) * source_dims.width) /
                                           (static_cast<std::uint64_t>(2) * target_width),
                                       "scaled mask x overflow"));
            const bool value =
                source_mask_scratch[static_cast<size_t>(src_y) * source_dims.width + src_x] != 0;
            target_mask_scratch[static_cast<size_t>(y) * target_width + x] = value ? 1u : 0u;
            if (!value) {
                continue;
            }
            resized.has_foreground = true;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x + 1);
            max_y = std::max(max_y, y + 1);
        }
    }

    size_t cursor = 0;
    while (cursor < target_pixels) {
        while (cursor < target_pixels && target_mask_scratch[cursor] == 0) {
            ++cursor;
        }
        if (cursor == target_pixels) {
            break;
        }
        const size_t run_start = cursor;
        while (cursor < target_pixels && target_mask_scratch[cursor] != 0) {
            ++cursor;
        }
        resized.rle_pairs.push_back({
            checked_cast<uint32_t>(run_start, "resized mask run start overflow"),
            checked_cast<uint32_t>(cursor - run_start, "resized mask run length overflow"),
        });
    }

    if (resized.has_foreground) {
        resized.bbox[0] = checked_cast<int16_t>(min_x, "resized bbox x1 overflow");
        resized.bbox[1] = checked_cast<int16_t>(min_y, "resized bbox y1 overflow");
        resized.bbox[2] = checked_cast<int16_t>(max_x, "resized bbox x2 overflow");
        resized.bbox[3] = checked_cast<int16_t>(max_y, "resized bbox y2 overflow");
    }
    return resized;
}

std::vector<ParsedInstance> parse_jsonl(const std::filesystem::path& annotation_file,
                                        const std::filesystem::path& image_file,
                                        const std::unordered_map<std::string, uint8_t>& class_map,
                                        uint32_t target_width,
                                        uint32_t target_height,
                                        std::vector<uint8_t>& source_mask_scratch,
                                        std::vector<uint8_t>& target_mask_scratch) {
    FASTLOADER_PROFILE_SCOPE("compiler.labels.parse_jsonl");
    std::vector<ParsedInstance> instances;
    std::ifstream file(annotation_file);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open annotation file: " + annotation_file.string());
    }

    const ImageDimensions source_dims = load_image_dimensions(image_file);
    const bool needs_resize = source_dims.width != target_width || source_dims.height != target_height;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        json record = json::parse(line, nullptr, false);
        if (record.is_discarded()) {
            continue;
        }
        validate_record_image_dimensions(record, annotation_file, source_dims);

        auto class_it = class_map.find(record["class"].get<std::string>());
        if (class_it == class_map.end()) {
            continue;
        }

        ParsedInstance instance{};
        instance.class_id = class_it->second;
        auto& bbox = record["bbox_xyxy"];
        instance.bbox[0] = bbox[0].get<int16_t>();
        instance.bbox[1] = bbox[1].get<int16_t>();
        instance.bbox[2] = bbox[2].get<int16_t>();
        instance.bbox[3] = bbox[3].get<int16_t>();
        instance.rle_pairs = parse_rle(record["mask_rle"].get<std::string>());
        if (needs_resize) {
            if (!instance.rle_pairs.empty()) {
                ResizedMask resized = resize_mask_row_major(instance.rle_pairs,
                                                            source_dims,
                                                            target_width,
                                                            target_height,
                                                            source_mask_scratch,
                                                            target_mask_scratch);
                instance.rle_pairs = std::move(resized.rle_pairs);
                if (resized.has_foreground) {
                    std::copy(std::begin(resized.bbox), std::end(resized.bbox), std::begin(instance.bbox));
                } else {
                    scale_bbox_in_place(instance, source_dims, target_width, target_height);
                }
            } else {
                scale_bbox_in_place(instance, source_dims, target_width, target_height);
            }
        }
        FASTLOADER_PROFILE_ADD("compiler.labels.instances", 1);
        FASTLOADER_PROFILE_ADD("compiler.labels.rle_pairs", instance.rle_pairs.size());
        instances.push_back(std::move(instance));
    }

    return instances;
}

size_t parsed_rle_pair_count(const std::vector<ParsedInstance>& instances) {
    size_t total_rle_pairs = 0;
    for (const ParsedInstance& instance : instances) {
        total_rle_pairs += instance.rle_pairs.size();
    }
    return total_rle_pairs;
}

PackedInstance pack_instance(const ParsedInstance& instance) {
    PackedInstance packed{};
    packed.class_id = instance.class_id;
    packed.bbox_x1 = instance.bbox[0];
    packed.bbox_y1 = instance.bbox[1];
    packed.bbox_x2 = instance.bbox[2];
    packed.bbox_y2 = instance.bbox[3];
    packed.mask_rle_pairs =
        checked_cast<uint16_t>(instance.rle_pairs.size(), "too many RLE pairs for one instance");
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

} // namespace

std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) {
    return numbered_path(split_dir, zero_based_index + 1, ".png");
}

std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) {
    return numbered_path(split_dir, zero_based_index + 1, ".jsonl");
}

DatasetScan scan_dataset(const CompilerConfig& config) {
    const std::filesystem::path categories_path = std::filesystem::path(config.source_dir) / "categories.json";
    std::ifstream file(categories_path);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open categories file: " + categories_path.string());
    }

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
        ParsedCategory parsed;
        parsed.name = category["name"].get<std::string>();
        parsed.raw_id = category["id"].get<int>();
        parsed_categories.push_back(std::move(parsed));
        min_raw_id = std::min(min_raw_id, parsed_categories.back().raw_id);
    }

    if (parsed_categories.empty()) {
        throw std::runtime_error("no classes found in categories.json");
    }

    // Mirror RF-DETR category normalization: leave dense 0-based ids alone, and
    // shift dense 1-based ids down into the same 0-based internal space.
    const int class_id_base =
        min_raw_id == 0 ? 0 :
        min_raw_id == 1 ? 1 :
        throw std::runtime_error("class ids must be dense and start at 0 or 1");

    bool seen_ids[MAX_CLASSES] = {};
    for (const auto& category : parsed_categories) {
        const int normalized_id = category.raw_id - class_id_base;
        if (normalized_id < 0 || normalized_id >= static_cast<int>(MAX_CLASSES)) {
            throw std::runtime_error("class id out of supported range in categories.json");
        }

        const uint8_t class_id = static_cast<uint8_t>(normalized_id);
        if (seen_ids[class_id]) {
            throw std::runtime_error("duplicate class id in categories.json");
        }
        seen_ids[class_id] = true;

        auto inserted = scan.class_map.emplace(category.name, class_id);
        if (!inserted.second) {
            throw std::runtime_error("duplicate class name in categories.json");
        }
    }

    for (size_t i = 0; i < scan.class_map.size(); ++i) {
        if (!seen_ids[i]) {
            throw std::runtime_error("class ids must be dense and start at 0 or 1");
        }
    }

    // Determine num_images
    if (categories.contains("splits") && categories["splits"].contains(config.split)) {
        scan.num_images = categories["splits"][config.split]["total"].get<uint32_t>();
        FASTLOADER_DEBUG_LOG("[compile] split '%s' count from categories.json: %u images\n",
                             config.split.c_str(), scan.num_images);
    } else {
        scan.num_images = count_sequential_images(std::filesystem::path(config.source_dir) / config.split);
    }

    return scan;
}

LabelBlocks build_label_blocks(const std::filesystem::path& split_dir,
                               uint32_t num_images,
                               uint32_t target_width,
                               uint32_t target_height,
                               const std::unordered_map<std::string, uint8_t>& class_map,
                               int num_workers,
                               const std::function<void(size_t, size_t)>& progress_cb) {
    FASTLOADER_PROFILE_SCOPE("compiler.labels.build_blocks");
    std::vector<WorkerResult> worker_results(num_images);
    const int worker_count = std::max(
        1,
        std::min(num_workers, checked_cast<int>(num_images, "label worker count overflow")));
    std::vector<LabelWorkerStats> worker_stats(static_cast<size_t>(worker_count));

    std::atomic<size_t> completed_images{0};
    std::mutex progress_mtx;

    parallel_for_range_indexed<uint32_t>(0, num_images, num_workers, [&](int worker_id, uint32_t start, uint32_t end) {
        LabelWorkerStats& stats = worker_stats[static_cast<size_t>(worker_id)];
        const auto worker_start = std::chrono::steady_clock::now();
        std::vector<uint8_t> source_mask_scratch;
        std::vector<uint8_t> target_mask_scratch;
        for (uint32_t image_index = start; image_index < end; ++image_index) {
            std::vector<ParsedInstance> instances =
                parse_jsonl(annotation_path(split_dir, image_index),
                           image_path(split_dir, image_index),
                           class_map,
                           target_width,
                           target_height,
                           source_mask_scratch,
                           target_mask_scratch);
            WorkerResult& result = worker_results[image_index];
            result.labels.reserve(instances.size());
            const size_t instance_rle_pairs = parsed_rle_pair_count(instances);
            result.rle_pairs.reserve(instance_rle_pairs);

            for (const ParsedInstance& instance : instances) {
                result.labels.push_back(pack_instance(instance));
                result.rle_pairs.insert(result.rle_pairs.end(),
                                        instance.rle_pairs.begin(),
                                        instance.rle_pairs.end());
            }

            ++stats.images;
            stats.instances += instances.size();
            stats.rle_pairs += instance_rle_pairs;

            if (progress_cb) {
                const size_t done = completed_images.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done % 100 == 0 || done == num_images) {
                    std::lock_guard<std::mutex> lock(progress_mtx);
                    progress_cb(done, num_images);
                }
            }
        }
        stats.active_ns = checked_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - worker_start).count(),
            "worker active time overflow");
    });

    std::uint64_t active_workers = 0;
    std::uint64_t total_active_ns = 0;
    std::uint64_t max_active_ns = 0;
    std::uint64_t total_instances = 0;
    std::uint64_t max_instances = 0;
    for (const LabelWorkerStats& stats : worker_stats) {
        if (stats.images == 0) {
            continue;
        }
        ++active_workers;
        FASTLOADER_PROFILE_RECORD_DURATION_NS("compiler.labels.worker.active", stats.active_ns);
        FASTLOADER_PROFILE_ADD("compiler.labels.worker.images", stats.images);
        FASTLOADER_PROFILE_ADD("compiler.labels.worker.instances", stats.instances);
        FASTLOADER_PROFILE_ADD("compiler.labels.worker.rle_pairs", stats.rle_pairs);
        total_active_ns += stats.active_ns;
        max_active_ns = std::max(max_active_ns, stats.active_ns);
        total_instances += stats.instances;
        max_instances = std::max(max_instances, stats.instances);
    }
    FASTLOADER_PROFILE_SET("compiler.labels.worker.count", active_workers);
    FASTLOADER_PROFILE_SET("compiler.labels.worker.active_skew_x1000",
                           compute_skew_x1000(total_active_ns, max_active_ns, active_workers));
    FASTLOADER_PROFILE_SET("compiler.labels.worker.instances_skew_x1000",
                           compute_skew_x1000(total_instances, max_instances, active_workers));

    const auto [total_labels, total_rle_pairs] = aggregate_worker_sizes(worker_results);
    LabelBlocks blocks;
    blocks.index.resize(num_images);
    blocks.labels.resize(total_labels);
    blocks.rle_pairs.resize(total_rle_pairs);
    FASTLOADER_PROFILE_SET("compiler.labels.total_labels", total_labels);
    FASTLOADER_PROFILE_SET("compiler.labels.total_rle_pairs", total_rle_pairs);

    size_t label_cursor = 0;
    size_t rle_cursor = 0;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.labels.merge_blocks");
        for (uint32_t image_index = 0; image_index < num_images; ++image_index) {
            WorkerResult& result = worker_results[image_index];
            ImageEntry& entry = blocks.index[image_index];
            entry.num_instances =
                checked_cast<uint16_t>(result.labels.size(), "too many instances for one image");
            entry.label_offset =
                checked_cast<uint32_t>(label_cursor * sizeof(PackedInstance), "label offset overflow");
            entry.label_bytes =
                checked_cast<uint32_t>(result.labels.size() * sizeof(PackedInstance),
                                       "label bytes overflow");

            const size_t image_rle_start = rle_cursor;
            size_t image_rle_cursor = image_rle_start;
            for (PackedInstance& packed : result.labels) {
                packed.mask_rle_offset =
                    checked_cast<uint32_t>(image_rle_cursor * sizeof(RLEPair),
                                           "mask RLE offset overflow");
                image_rle_cursor += packed.mask_rle_pairs;
            }
            if (image_rle_cursor - image_rle_start != result.rle_pairs.size()) {
                throw std::runtime_error(
                    "label and RLE counts diverged while assembling label blocks");
            }

            if (!result.labels.empty()) {
                std::copy(result.labels.begin(), result.labels.end(), blocks.labels.data() + label_cursor);
            }
            if (!result.rle_pairs.empty()) {
                std::copy(result.rle_pairs.begin(),
                          result.rle_pairs.end(),
                          blocks.rle_pairs.data() + image_rle_start);
            }

            label_cursor += result.labels.size();
            rle_cursor = image_rle_cursor;
        }
    }

    return blocks;
}

} // namespace fastloader::compiler_internal
