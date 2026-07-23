#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mmltk {

static constexpr uint64_t MAGIC = 0x46415354'4C445232ULL;
static constexpr uint32_t FORMAT_VERSION = 4;
static constexpr uint32_t MAX_CLASSES = 256;
static constexpr size_t PAGE_SIZE = 4096;
static constexpr size_t HUGE_PAGE_SIZE = size_t{2} * 1024 * 1024;

inline size_t align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

struct __attribute__((packed)) PackedInstance {
    uint8_t class_id;
    uint8_t _pad;
    int16_t bbox_x1, bbox_y1, bbox_x2, bbox_y2;
    uint32_t mask_rle_offset;
    uint16_t mask_rle_pairs;
};
static_assert(sizeof(PackedInstance) == 16, "PackedInstance must be 16 bytes");

struct __attribute__((packed)) ImageEntry {
    uint64_t pixel_offset;
    uint32_t label_offset;
    uint16_t num_instances;
    uint16_t _pad;
    uint32_t label_bytes;
    uint32_t _reserved;
};
static_assert(sizeof(ImageEntry) == 24, "ImageEntry must be 24 bytes");

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
    std::array<uint8_t, 56> _reserved{};
};
static_assert(sizeof(FileHeader) == 8328, "FileHeader must preserve the on-disk layout");

struct __attribute__((packed)) RLEPair {
    uint32_t start;
    uint32_t length;
};
static_assert(sizeof(RLEPair) == 8, "RLEPair must be 8 bytes");

}  
