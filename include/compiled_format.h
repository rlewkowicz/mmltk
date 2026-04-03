#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ============================================================
// Compiled dataset binary format v2 — mmap-friendly, zero-copy
// ============================================================
//
// File layout:
//   [FileHeader]
//   [ImageIndex entries]  — immediately after header
//   [huge_page_pad]       — padding to align pixel blob
//   [PixelBlob]           — float32 NCHW [0,1] contiguous, page-aligned start
//   [LabelBlock]          — flat packed labels
//   [RLE Block]           — mask RLE pairs
//
// The pixel blob IS the tensor. mmap it, pointer-cast, done.
// Python gets numpy views directly into the mmap. No copies.
//
// All offsets are absolute byte offsets from file start.
// All multi-byte values are little-endian (x86 native).
// ============================================================

namespace mmltk {

static constexpr uint64_t MAGIC = 0x46415354'4C445232ULL; // "FASTLDR2"
static constexpr uint32_t FORMAT_VERSION = 4;
static constexpr uint32_t MAX_CLASSES = 256;
static constexpr size_t   PAGE_SIZE = 4096;
static constexpr size_t   HUGE_PAGE_SIZE = size_t{2} * 1024 * 1024; // 2MB THP

inline size_t align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

// Packed instance label — 16 bytes
struct __attribute__((packed)) PackedInstance {
    uint8_t  class_id;       // 0-based class index
    uint8_t  _pad;
    int16_t  bbox_x1, bbox_y1, bbox_x2, bbox_y2; // xyxy absolute pixels
    uint32_t mask_rle_offset; // byte offset into RLE block
    uint16_t mask_rle_pairs;  // number of start:length pairs
};
static_assert(sizeof(PackedInstance) == 16, "PackedInstance must be 16 bytes");

// Per-image index entry — 24 bytes
struct __attribute__((packed)) ImageEntry {
    uint64_t pixel_offset;     // byte offset to float32 NCHW data in pixel blob
    uint32_t label_offset;     // byte offset into label block
    uint16_t num_instances;    // 0 = background
    uint16_t _pad;
    uint32_t label_bytes;      // num_instances * sizeof(PackedInstance)
    uint32_t _reserved;
};
static_assert(sizeof(ImageEntry) == 24, "ImageEntry must be 24 bytes");

struct __attribute__((packed)) FileHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t num_images;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t channels;          // always 3
    uint32_t num_classes;
    uint64_t index_offset;      // byte offset to ImageEntry array
    uint64_t label_offset;      // byte offset to label block
    uint64_t pixel_offset;      // byte offset to float32 NCHW pixel blob (page-aligned)
    uint64_t mask_rle_offset;   // byte offset to RLE block
    uint64_t total_file_size;
    uint64_t image_stride;      // bytes per image: C * H * W * sizeof(float)
    std::array<std::array<char, 32>, MAX_CLASSES> class_names{};
    std::array<uint8_t, 56> _reserved{};
};
static_assert(sizeof(FileHeader) == 8328, "FileHeader must preserve the on-disk layout");

// Mask RLE pair
struct __attribute__((packed)) RLEPair {
    uint32_t start;
    uint32_t length;
};
static_assert(sizeof(RLEPair) == 8, "RLEPair must be 8 bytes");

} // namespace mmltk
