#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/data/compiled_format_limits.h"
namespace mmltk::backend::data {
inline constexpr uint64_t MAGIC = 0x46415354'4C445232ULL;
inline constexpr uint32_t FORMAT_VERSION = 9;
inline constexpr size_t PAGE_SIZE = 4096;
inline constexpr size_t HUGE_PAGE_SIZE = size_t{2} * 1024 * 1024;
using PackedCoordinate = float;
inline constexpr std::uint32_t MAX_IMAGE_EXTENT = 32767U;
inline size_t align_up(size_t val, size_t alignment) { return (val + alignment - 1) & ~(alignment - 1); }
inline constexpr std::uint8_t kAnnotationMask = 1U, kAnnotationCrowd = 2U, kAnnotationIgnore = 4U, kAnnotationId = 8U, kAnnotationCategory = 16U;
inline constexpr std::uint8_t kAnnotationFlags = kAnnotationMask | kAnnotationCrowd | kAnnotationIgnore | kAnnotationId | kAnnotationCategory;
struct __attribute__((packed)) PackedInstance {
    uint8_t class_id;
    uint8_t flags;
    PackedCoordinate bbox_x1, bbox_y1, bbox_x2, bbox_y2;
    uint64_t mask_rle_offset;
    uint16_t mask_rle_pairs;
    double original_area = 0.0;
    std::uint64_t annotation_id = 0;
    std::uint64_t source_category_id = 0;
    std::uint64_t source_ordinal = 0;
    [[nodiscard]] bool has_annotation_id() const noexcept { return (flags & kAnnotationId) != 0U; }
    [[nodiscard]] bool has_source_category() const noexcept { return (flags & kAnnotationCategory) != 0U; }
    [[nodiscard]] bool has_mask() const noexcept { return (flags & kAnnotationMask) != 0U; }
    [[nodiscard]] bool is_crowd() const noexcept { return (flags & kAnnotationCrowd) != 0U; }
    [[nodiscard]] bool raw_ignore() const noexcept { return (flags & kAnnotationIgnore) != 0U; }
};
enum class AnnotationSource : std::uint8_t {
    Generic = 0,
    Coco = 1,
    Objects365 = 2,
    OpenImages = 3,
    CoconutCoco = 4,
    CoconutObjects365V1 = 5,
    CoconutObjects365V2 = 6
};
// Open Images MIDs are /m/ followed by at most eight ASCII identifier bytes.
// Store those bytes little-endian, with zero padding; this is not a hash.
inline std::uint64_t encode_open_images_category(std::string_view mid) {
    if (!mid.starts_with("/m/") || mid.size() <= 3U || mid.size() > 11U) throw std::runtime_error("Open Images category MID is not representable");
    std::uint64_t encoded = 0U;
    for (std::size_t index = 3U; index < mid.size(); ++index) {
        const char value = mid[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') || value == '_')) throw std::runtime_error("invalid Open Images category MID");
        encoded |= static_cast<std::uint64_t>(static_cast<unsigned char>(value)) << ((index - 3U) * 8U);
    }
    return encoded;
}
[[nodiscard]] inline bool valid_open_images_category(std::uint64_t encoded) noexcept {
    if (encoded == 0U) return false;
    while (encoded != 0U) {
        const auto value = static_cast<unsigned char>(encoded & 255U);
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') || value == '_')) return false;
        encoded >>= 8U;
    }
    return true;
}
inline std::string decode_open_images_category(std::uint64_t encoded) {
    if (!valid_open_images_category(encoded)) throw std::runtime_error("invalid encoded Open Images category MID");
    std::string mid = "/m/";
    while (encoded != 0U) {
        mid.push_back(static_cast<char>(encoded & 255U));
        encoded >>= 8U;
    }
    return mid;
}
static_assert(sizeof(PackedInstance) == 60);
struct __attribute__((packed)) ImageEntry {
    uint64_t pixel_offset;
    uint32_t label_offset;
    uint16_t num_instances;
    uint16_t _pad;
    uint32_t label_bytes;
    uint32_t original_width;
    uint32_t original_height;
    std::uint8_t has_source_image_id = 0;
    AnnotationSource source = AnnotationSource::Generic;
    std::uint16_t _reserved = 0;
    std::uint64_t source_image_id = 0;
};
static_assert(sizeof(ImageEntry) == 40, "ImageEntry must be 40 bytes");
struct SourceCategoryIdentity {
    enum class Kind : std::uint8_t { Missing, Numeric, OpenImagesMid };
    Kind kind = Kind::Missing;
    std::uint64_t value = 0;
};
[[nodiscard]] inline SourceCategoryIdentity source_category_identity(const ImageEntry& image, const PackedInstance& annotation) noexcept {
    if (!annotation.has_source_category()) return {};
    return {image.source == AnnotationSource::OpenImages ? SourceCategoryIdentity::Kind::OpenImagesMid : SourceCategoryIdentity::Kind::Numeric,
            annotation.source_category_id};
}
struct __attribute__((packed)) FileHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t num_images;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t channels;
    uint32_t num_classes;
    uint64_t index_offset;
    uint64_t label_offset;
    uint64_t pixel_offset;
    uint64_t mask_rle_offset;
    uint64_t total_file_size;
    uint64_t image_stride;
    std::array<std::array<char, 32>, MAX_CLASSES> class_names{};
    uint32_t max_instances_per_image;
    mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
    std::array<uint8_t, 51> _reserved{};
};
inline constexpr std::size_t COMPILED_CLASS_NAME_CAPACITY = FileHeader{}.class_names[0].size() - 1U;
static_assert(sizeof(FileHeader) == 8328, "FileHeader must preserve the on-disk layout");
struct __attribute__((packed)) RLEPair {
    uint32_t start;
    uint32_t length;
};
static_assert(sizeof(RLEPair) == 8, "RLEPair must be 8 bytes");
}  // namespace mmltk::backend::data
