#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/backend/data/catalog/class_catalog.h"
#include "detail/writable_pixel_range.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/image_resize.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
// CLEANUP-IGNORE: This implementation imports the concrete data owners named by its independent compilation unit.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "benchmark_images.h"
#include "benchmark_jpeg.h"
#include "detail/benchmark_writer.h"
#include "staging_file_cleanup.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_concurrency = mmltk::common::concurrency;
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::size_t checked_size_add(const std::size_t left, const std::size_t right, const char* context) {
    return common_math::checked_add(left, right, context);
}
[[nodiscard]] std::size_t checked_size_multiply(const std::size_t left, const std::size_t right, const char* context) {
    return common_math::checked_multiply(left, right, context);
}
[[nodiscard]] std::size_t checked_align_up(const std::size_t value, const std::size_t alignment, const char* context) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) { throw std::invalid_argument("benchmark alignment must be a nonzero power of two"); }
    return checked_size_add(value, alignment - 1U, context) & ~(alignment - 1U);
}
using detail::WritablePixelRange;
class ReadOnlyMappedRange {
   public:
    ReadOnlyMappedRange() = default;
    ReadOnlyMappedRange(const common_io::FileHandle& file, const std::size_t offset, const std::size_t bytes) {
        if (bytes == 0U) { return; }
        const std::size_t mapping_offset = offset & ~(PAGE_SIZE - 1U);
        const std::size_t delta = offset - mapping_offset;
        const std::size_t mapping_bytes = checked_size_add(delta, bytes, "benchmark validation mmap size overflow");
        void* mapping = ::mmap(nullptr, mapping_bytes, PROT_READ, MAP_SHARED, file.get(),
                               common_math::checked_cast<off_t>(mapping_offset, "benchmark validation mmap offset overflow"));
        if (mapping == MAP_FAILED) { throw common_io::errno_error("benchmark validation mmap failed"); }
        region_.adopt(mapping, mapping_bytes);
        data_ = static_cast<const std::uint8_t*>(mapping) + delta;
    }
    ReadOnlyMappedRange(const ReadOnlyMappedRange&) = delete;
    ReadOnlyMappedRange& operator=(const ReadOnlyMappedRange&) = delete;
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

   private:
    common_io::MappedByteRegion region_;
    const std::uint8_t* data_ = nullptr;
};
[[nodiscard]] std::size_t checked_rgb_bytes(const std::uint32_t width, const std::uint32_t height) {
    if (width == 0U || height == 0U) { throw std::runtime_error("benchmark image dimensions must be positive"); }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    return common_math::checked_cast<std::size_t>(pixels * 3U, "benchmark RGB buffer size overflow");
}
[[nodiscard]] std::vector<ImageEntry> build_index(const PreparedBenchmarkSplit& split, const std::uint64_t pixel_offset, const std::uint64_t image_stride) {
    std::vector<ImageEntry> index(split.images.size());
    std::size_t expected_first_label = 0U;
    for (std::size_t image_index = 0U; image_index < split.images.size(); ++image_index) {
        const EncodedImageRecord& source = split.images[image_index];
        if (source.first_label != expected_first_label || expected_first_label > split.labels.size() ||
            static_cast<std::size_t>(source.label_count) > split.labels.size() - expected_first_label) {
            throw std::runtime_error("benchmark split label ranges are not contiguous");
        }
        ImageEntry& destination = index[image_index];
        const std::size_t image_offset = checked_size_multiply(image_index, image_stride, "benchmark pixel index offset overflow");
        if (image_offset > std::numeric_limits<std::uint64_t>::max() - pixel_offset) { throw std::overflow_error("benchmark pixel index offset overflow"); }
        destination.pixel_offset = pixel_offset + image_offset;
        const std::size_t label_offset = checked_size_multiply(expected_first_label, sizeof(PackedInstance), "benchmark label offset overflow");
        destination.label_offset = common_math::checked_cast<std::uint32_t>(label_offset, "benchmark label offset overflow");
        destination.num_instances = source.label_count;
        destination.label_bytes =
            common_math::checked_cast<std::uint32_t>(static_cast<std::size_t>(source.label_count) * sizeof(PackedInstance), "benchmark label size overflow");
        destination.original_width = source.source_width;
        destination.original_height = source.source_height;
        expected_first_label += source.label_count;
    }
    if (expected_first_label != split.labels.size()) { throw std::runtime_error("benchmark labels contain unreferenced records"); }
    return index;
}
[[nodiscard]] FileHeader make_header(const PreparedBenchmarkSplit& split, const std::uint32_t resolution, const std::size_t image_stride,
                                     const std::size_t index_offset, const std::size_t pixel_offset, const std::size_t label_offset,
                                     const std::size_t rle_offset, const std::size_t total_size) {
    FileHeader header{};
    header.magic = MAGIC;
    header.version = FORMAT_VERSION;
    header.num_images = common_math::checked_cast<std::uint32_t>(split.images.size(), "benchmark image count overflow");
    header.image_width = resolution;
    header.image_height = resolution;
    header.channels = 3U;
    header.num_classes = common_math::checked_cast<std::uint32_t>(split.class_names.size(), "benchmark class count overflow");
    header.index_offset = index_offset;
    header.pixel_offset = pixel_offset;
    header.label_offset = label_offset;
    header.mask_rle_offset = rle_offset;
    header.total_file_size = total_size;
    header.image_stride = image_stride;
    for (std::size_t class_index = 0U; class_index < split.class_names.size(); ++class_index) {
        const std::string& name = split.class_names[class_index];
        if (name.empty() || name.size() >= header.class_names[class_index].size()) { throw std::runtime_error("benchmark class name is empty or too long"); }
        std::memcpy(header.class_names[class_index].data(), name.data(), name.size());
    }
    for (const EncodedImageRecord& image : split.images) {
        header.max_instances_per_image = std::max<std::uint32_t>(header.max_instances_per_image, image.label_count);
    }
    return header;
}
void decode_images(const BenchmarkWriteRequest& request, const common_io::FileHandle& output, const std::size_t pixel_offset, const std::size_t image_stride,
                   const std::size_t pixel_bytes) {
    const PreparedBenchmarkSplit& split = request.split;
    WritablePixelRange output_pixels(output.get(), pixel_offset, pixel_bytes);
    const int worker_count = std::max(1, std::min(request.num_workers, common_math::checked_cast<int>(split.images.size(), "worker count overflow")));
    std::vector<common_io::FileHandle> source_directories;
    source_directories.reserve(split.sources.size());
    for (const CachedImageSource& source : split.sources) {
        const int descriptor = ::open(source.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) { throw common_io::errno_error("cannot open cached benchmark image directory", source.root.string()); }
        source_directories.emplace_back(descriptor);
    }
    const std::uint32_t image_count = common_math::checked_cast<std::uint32_t>(split.images.size(), "benchmark image count overflow");
    std::atomic<std::uint32_t> next_image{0U};
    std::atomic<bool> worker_failed{false};
    const auto decode_worker = [&](int, const int, const int) {
        BenchmarkJpegDecoder jpeg;
        RgbImageResizer resizer(1, request.perceptual_downscale);
        std::vector<std::uint8_t> encoded;
        std::vector<std::uint8_t> decoded;
        std::vector<std::uint8_t> cmyk;
        std::vector<std::uint8_t> resized;
        const std::size_t target_bytes = checked_rgb_bytes(request.resolution, request.resolution);
        resized.reserve(target_bytes);
        constexpr std::uint32_t kSchedulingBatch = 8U;
        while (!worker_failed.load(std::memory_order_relaxed)) {
            const std::uint32_t begin = next_image.fetch_add(kSchedulingBatch, std::memory_order_relaxed);
            if (begin >= image_count) { return; }
            const std::uint32_t end = std::min(image_count, begin + kSchedulingBatch);
            try {
                for (std::uint32_t image_index = begin; image_index < end; ++image_index) {
                    if (request.cancel_requested.requested()) { throw std::runtime_error("benchmark dataset compilation cancelled"); }
                    const EncodedImageRecord& image = split.images[image_index];
                    if (image.source_width == 0U || image.source_height == 0U || image.source_index >= source_directories.size()) {
                        throw std::runtime_error("benchmark image metadata is incomplete");
                    }
                    std::array<char, 24> relative_path{};
                    (void)format_cached_image_relative_path(image.source_image_id, relative_path);
                    int jpeg_width = 0;
                    int jpeg_height = 0;
                    try {
                        const int image_descriptor =
                            ::openat(source_directories[image.source_index].get(), relative_path.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                        if (image_descriptor < 0) { throw common_io::errno_error("cannot open cached benchmark image", relative_path.data()); }
                        common_io::FileHandle image_file(image_descriptor);
                        const std::size_t encoded_size = image_file.size();
                        if (encoded_size == 0U || encoded_size > std::numeric_limits<std::uint32_t>::max()) {
                            throw std::runtime_error("cached benchmark JPEG has an invalid size");
                        }
                        encoded.resize(encoded_size);
                        image_file.pread_all(encoded.data(), encoded.size(), 0U);
                        const BenchmarkJpegHeader jpeg_header = jpeg.read_header(encoded, image.source_width, image.source_height);
                        jpeg_width = common_math::checked_cast<int>(jpeg_header.width, "benchmark JPEG width overflow");
                        jpeg_height = common_math::checked_cast<int>(jpeg_header.height, "benchmark JPEG height overflow");
                        jpeg.decode_rgb(encoded, jpeg_header, &decoded, &cmyk);
                    } catch (const std::bad_alloc&) { throw; } catch (const BenchmarkImageReadError&) {
                        throw;
                    } catch (const std::exception& error) { throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what()); }
                    const RgbLetterbox letterbox = compute_rgb_letterbox(image.source_width, image.source_height, request.resolution, request.resolution);
                    const std::uint8_t* source_pixels = decoded.data();
                    if (image.source_width != letterbox.resized_width || image.source_height != letterbox.resized_height) {
                        resized.resize(checked_rgb_bytes(letterbox.resized_width, letterbox.resized_height));
                        resizer.resize(decoded.data(), jpeg_width, jpeg_height, resized.data(),
                                       common_math::checked_cast<int>(letterbox.resized_width, "benchmark letterbox width overflow"),
                                       common_math::checked_cast<int>(letterbox.resized_height, "benchmark letterbox height overflow"));
                        source_pixels = resized.data();
                    }
                    float* destination = output_pixels.image(image_index, image_stride);
                    if (letterbox.resized_width == request.resolution && letterbox.resized_height == request.resolution && letterbox.offset_x == 0U &&
                        letterbox.offset_y == 0U) {
                        rgb_hwc_u8_to_nchw_f32(source_pixels, destination, request.resolution, request.resolution);
                    } else {
                        letterboxed_rgb_hwc_u8_to_nchw_f32(source_pixels, destination, letterbox.resized_width, letterbox.resized_height, request.resolution,
                                                           request.resolution, letterbox.offset_x, letterbox.offset_y);
                    }
                    if (request.progress) { request.progress(); }
                }
            } catch (...) {
                worker_failed.store(true, std::memory_order_relaxed);
                throw;
            }
        }
    };
    if (request.worker_cpus.empty()) {
        common_concurrency::parallel_for_range_indexed<int>(0, worker_count, worker_count, decode_worker);
    } else {
        common_concurrency::parallel_for_range_indexed<int>(0, worker_count, worker_count, request.worker_cpus, decode_worker);
    }
}
}  // namespace
BenchmarkImageReadError::BenchmarkImageReadError(const std::uint16_t source_index, const std::uint64_t source_image_id, std::string detail)
    : std::runtime_error("benchmark cached image " + std::to_string(source_image_id) + " cannot be read: " + std::move(detail)),
      source_index_(source_index),
      source_image_id_(source_image_id) {}
std::uint16_t BenchmarkImageReadError::source_index() const noexcept { return source_index_; }
std::uint64_t BenchmarkImageReadError::source_image_id() const noexcept { return source_image_id_; }
PackedInstance benchmark_letterbox_box(const std::uint8_t class_id, const float x1, const float y1, const float x2, const float y2,
                                       const RgbLetterbox& letterbox) {
    if (letterbox.resized_width == 0U || letterbox.resized_height == 0U) { throw std::runtime_error("benchmark box requires a valid letterbox"); }
    const auto scaled_coordinate = [](const float value, const std::uint32_t extent, const std::uint32_t offset, const bool round_up,
                                      const char* overflow_message) {
        const double scaled = std::clamp(static_cast<double>(value), 0.0, 1.0) * extent;
        const double rounded = round_up ? std::ceil(scaled) : std::floor(scaled);
        return common_math::checked_cast<std::int16_t>(
            static_cast<std::int32_t>(rounded) + common_math::checked_cast<std::int32_t>(offset, "benchmark box offset overflow"), overflow_message);
    };
    const auto minimum = [&](const float value, const std::uint32_t extent, const std::uint32_t offset) {
        return scaled_coordinate(value, extent, offset, false, "benchmark minimum box coordinate overflow");
    };
    const auto maximum = [&](const float value, const std::uint32_t extent, const std::uint32_t offset) {
        return scaled_coordinate(value, extent, offset, true, "benchmark maximum box coordinate overflow");
    };
    return PackedInstance{
        class_id,
        0U,
        minimum(x1, letterbox.resized_width, letterbox.offset_x),
        minimum(y1, letterbox.resized_height, letterbox.offset_y),
        maximum(x2, letterbox.resized_width, letterbox.offset_x),
        maximum(y2, letterbox.resized_height, letterbox.offset_y),
        0U,
        0U,
    };
}
void write_benchmark_split(const BenchmarkWriteRequest& request) {
    mmltk::common::logging::ScopedProfile profile{"benchmark.writer.total"};
    if (request.resolution == 0U || request.resolution > MAX_IMAGE_EXTENT) {
        throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format");
    }
    if (request.split.images.empty() || request.split.class_names.empty()) { throw std::runtime_error("benchmark split must contain images and classes"); }
    const catalog::ClassCatalog class_catalog(request.split.class_names, 31U);
    if (request.split.class_names.size() > MAX_CLASSES) { throw std::runtime_error("benchmark split exceeds the compiled class limit"); }
    if (request.split.sources.empty()) { throw std::runtime_error("benchmark split has no cached image sources"); }
    const std::uint64_t image_stride_u64 = static_cast<std::uint64_t>(request.resolution) * request.resolution * 3U * sizeof(float);
    const std::size_t image_stride = common_math::checked_cast<std::size_t>(image_stride_u64, "benchmark image stride overflow");
    const std::size_t index_offset = sizeof(FileHeader);
    const std::size_t index_bytes = checked_size_multiply(request.split.images.size(), sizeof(ImageEntry), "benchmark index size overflow");
    const std::size_t pixel_offset =
        checked_align_up(checked_size_add(index_offset, index_bytes, "benchmark index end overflow"), HUGE_PAGE_SIZE, "benchmark pixel alignment overflow");
    const std::size_t pixel_bytes = checked_size_multiply(request.split.images.size(), image_stride, "benchmark pixel blob overflow");
    const std::size_t label_offset = checked_size_add(pixel_offset, pixel_bytes, "benchmark label offset overflow");
    const std::size_t label_bytes = checked_size_multiply(request.split.labels.size(), sizeof(PackedInstance), "benchmark label block overflow");
    const std::size_t rle_offset = checked_size_add(label_offset, label_bytes, "benchmark RLE offset overflow");
    const std::size_t rle_bytes = checked_size_multiply(request.split.rle_pairs.size(), sizeof(RLEPair), "benchmark RLE block overflow");
    const std::size_t total_size = checked_size_add(rle_offset, rle_bytes, "benchmark file size overflow");
    std::vector<ImageEntry> index = build_index(request.split, pixel_offset, image_stride);
    const FileHeader header = make_header(request.split, request.resolution, image_stride, index_offset, pixel_offset, label_offset, rle_offset, total_size);
    validate_compiled_index_entries(index, header, request.split.labels.size(), request.cancel_requested);
    validate_compiled_original_image_dimensions(index, request.cancel_requested);
    const std::size_t used_rle = validate_compiled_label_entries(request.split.labels, header, rle_bytes, request.cancel_requested);
    if (used_rle != rle_bytes) { throw std::runtime_error("benchmark labels do not reference the complete mask block"); }
    validate_compiled_rle_pairs(request.split.labels, request.split.rle_pairs, static_cast<std::size_t>(request.resolution) * request.resolution,
                                request.cancel_requested);
    const std::filesystem::path output_parent = request.output_path.parent_path().empty() ? std::filesystem::path{"."} : request.output_path.parent_path();
    std::filesystem::create_directories(output_parent);
    std::string staging_path_text = request.output_path.string() + ".tmp.XXXXXX";
    common_io::FileHandle output = common_io::FileHandle::create_unique_output(staging_path_text, total_size);
    const std::filesystem::path staging_path(staging_path_text);
    StagingFileCleanup cleanup(staging_path);
    decode_images(request, output, pixel_offset, image_stride, pixel_bytes);
    output.pwrite_all(&header, sizeof(header), 0U);
    output.pwrite_all(index.data(), index_bytes, index_offset);
    output.pwrite_all(request.split.labels.data(), label_bytes, label_offset);
    output.pwrite_all(request.split.rle_pairs.data(), rle_bytes, rle_offset);
    output.sync_data();
    output = common_io::FileHandle{};
    const common_io::FileHandle staged = common_io::FileHandle::open_readonly(staging_path.string());
    const FileHeader staged_header = read_compiled_header(staged);
    const CompiledFileSections sections = validate_compiled_file_sections(staged_header, staged.size());
    if (sections.label_count != request.split.labels.size() || sections.rle_region_bytes != rle_bytes) {
        throw std::runtime_error("staged benchmark split metadata does not match its compile plan");
    }
    const ReadOnlyMappedRange persisted_index(staged, sections.index_offset, sections.expected_index_bytes);
    const auto persisted_index_span =
        std::span(reinterpret_cast<const ImageEntry*>(persisted_index.data()), static_cast<std::size_t>(staged_header.num_images));
    validate_compiled_index_entries(persisted_index_span, staged_header, sections.label_count, request.cancel_requested);
    validate_compiled_original_image_dimensions(persisted_index_span, request.cancel_requested);
    const ReadOnlyMappedRange persisted_labels(staged, sections.label_offset, sections.label_bytes);
    const auto persisted_label_span = std::span(reinterpret_cast<const PackedInstance*>(persisted_labels.data()), sections.label_count);
    const std::size_t persisted_rle_bytes =
        validate_compiled_label_entries(persisted_label_span, staged_header, sections.rle_region_bytes, request.cancel_requested);
    if (persisted_rle_bytes != rle_bytes) { throw std::runtime_error("persisted benchmark labels do not reference the complete mask block"); }
    const ReadOnlyMappedRange persisted_rle(staged, sections.rle_offset, sections.rle_region_bytes);
    const auto persisted_rle_span = std::span(reinterpret_cast<const RLEPair*>(persisted_rle.data()), request.split.rle_pairs.size());
    validate_compiled_rle_pairs(persisted_label_span, persisted_rle_span, static_cast<std::size_t>(request.resolution) * request.resolution,
                                request.cancel_requested);
    throw_if_benchmark_cancelled(request.cancel_requested);
    common_io::publish_staged_path_atomically(staging_path, request.output_path, request.overwrite);
    cleanup.published();
}
}  // namespace mmltk::backend::data::benchmark_internal
