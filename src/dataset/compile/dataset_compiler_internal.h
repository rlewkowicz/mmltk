#pragma once

#include "compiled_format.h"
#include "common_utils.h"
#include "dataset_compiler.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastloader::compiler_internal {

struct DatasetScan {
    std::unordered_map<std::string, uint8_t> class_map;
    uint32_t num_images = 0;
};

struct LabelBlocks {
    std::vector<ImageEntry> index;
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
    bool any_image_resize = false;
    bool any_image_downscale = false;
    size_t dropped_annotations = 0;
    std::vector<std::string> dropped_annotation_examples;
};

struct ProgressCounter {
    std::atomic<size_t> done{0};
    std::atomic<size_t> active{0};
    std::condition_variable* wake_cv = nullptr;

    void increment() noexcept {
        done.fetch_add(1, std::memory_order_release);
        wake();
    }

    void begin_work() noexcept {
        active.fetch_add(1, std::memory_order_release);
        wake();
    }

    void end_work() noexcept {
        active.fetch_sub(1, std::memory_order_release);
        wake();
    }

    void finish_work() noexcept {
        done.fetch_add(1, std::memory_order_release);
        active.fetch_sub(1, std::memory_order_release);
        wake();
    }

    size_t load() const noexcept {
        return done.load(std::memory_order_acquire);
    }

    size_t active_load() const noexcept {
        return active.load(std::memory_order_acquire);
    }

private:
    void wake() noexcept {
        if (wake_cv != nullptr) {
            wake_cv->notify_one();
        }
    }
};

struct DroppedAnnotationTracker {
    std::atomic<size_t> count{0};
    std::condition_variable* wake_cv = nullptr;
    mutable std::mutex samples_mutex;
    std::vector<std::string> samples;

    void increment(std::string sample) {
        count.fetch_add(1, std::memory_order_release);
        if (!sample.empty()) {
            std::lock_guard<std::mutex> lock(samples_mutex);
            if (samples.size() < 8) {
                samples.push_back(std::move(sample));
            }
        }
        wake();
    }

    size_t load() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    std::vector<std::string> sample_snapshot() const {
        std::lock_guard<std::mutex> lock(samples_mutex);
        return samples;
    }

private:
    void wake() noexcept {
        if (wake_cv != nullptr) {
            wake_cv->notify_one();
        }
    }
};

struct FileLayout {
    size_t index_size = 0;
    size_t label_block_size = 0;
    size_t rle_block_size = 0;
    size_t index_offset = sizeof(FileHeader);
    size_t label_offset = 0;
    size_t rle_offset = 0;
    size_t pixel_offset = 0;
    size_t pixel_blob_size = 0;
    size_t total_size = 0;
};

int resolve_num_workers(int configured_workers);

std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);

DatasetScan scan_dataset(const CompilerConfig& config);
LabelBlocks build_label_blocks(const std::filesystem::path& split_dir,
                               uint32_t num_images,
                               const CompilerConfig& config,
                               const std::unordered_map<std::string, uint8_t>& class_map,
                               int num_workers,
                               ProgressCounter* completed_images = nullptr,
                               DroppedAnnotationTracker* dropped_annotations = nullptr);

FileLayout compute_pixel_layout(uint32_t num_images, size_t image_stride);
void finalize_layout(FileLayout& layout, size_t label_count, size_t rle_count);
void assign_pixel_offsets(std::vector<ImageEntry>& index,
                          size_t pixel_offset,
                          size_t image_stride);
FileHeader make_file_header(uint32_t num_images,
                            uint32_t width,
                            uint32_t height,
                            uint32_t channels,
                            size_t image_stride,
                            const std::unordered_map<std::string, uint8_t>& class_map,
                            const FileLayout& layout);
void write_metadata_blocks(const FileHandle& fd,
                           const FileLayout& layout,
                           const FileHeader& header,
                           const LabelBlocks& label_blocks);

void write_pixel_blob(const FileHandle& fd,
                      const std::filesystem::path& split_dir,
                      uint32_t num_images,
                      uint32_t width,
                      uint32_t height,
                      size_t image_stride,
                      int num_workers,
                      bool any_resize,
                      bool any_downscale,
                      ProgressCounter* completed_images,
                      size_t pixel_offset);

} // namespace fastloader::compiler_internal
