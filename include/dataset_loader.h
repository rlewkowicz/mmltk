#pragma once
#include "compiled_format.h"
#include "cuda_stream_manager.h"
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk {

struct LabelIndexEntry {
    uint32_t label_begin;     // first PackedInstance index for this image
    uint16_t num_instances;   // 0 = background
    uint16_t _pad;
};
static_assert(sizeof(LabelIndexEntry) == 8, "LabelIndexEntry must be 8 bytes");

// A batch: pixel pointers into mmap, label pointers into RAM, and device memory.
struct Batch {
    size_t num_images;
    const float* host_images;           // float32 NCHW view: direct mmap span or pinned gather buffer
    const float* device_images;         // float32 NCHW on GPU
    const LabelIndexEntry* label_index; // per-image label spans
    const PackedInstance* labels;       // flat in-memory label block
    const RLEPair* rle_pairs;           // flat in-memory RLE block
    const uint32_t* image_indices;      // which dataset indices are in this batch
    size_t slot_index;                  // reusable device/gather slot backing this batch
    uint64_t lease_id;                  // validates handoff/release against slot reuse
};

class DatasetLoader {
public:
    struct Config {
        std::string compiled_path;
        size_t batch_size = 32;
        bool shuffle = true;
        uint64_t seed = 42;
        int device_id = 0;
        int prefetch_factor = 6;
        // Gather workers only. The loader always keeps one dedicated H2D transfer thread alive.
        int gather_workers = 0;
        // Optional Linux CPU list/range string, for example "0-7" or "0,2,4,6".
        std::string cpu_affinity;
        uint32_t batch_shard_rank = 0;
        uint32_t batch_shard_count = 1;
        bool drop_last = false;
    };

    explicit DatasetLoader(const Config& config);
    ~DatasetLoader();

    DatasetLoader(const DatasetLoader&) = delete;
    DatasetLoader& operator=(const DatasetLoader&) = delete;

    // Epoch lifecycle
    void begin_epoch();
    bool next_batch(Batch& out);
    void wait_batch(const Batch& batch);
    void handoff_batch(const Batch& batch, void* consumer_stream);
    void release_batch(const Batch& batch);
    void release_batch(const Batch& batch, void* consumer_stream);

    // Metadata
    [[nodiscard]] size_t num_images() const;
    [[nodiscard]] size_t num_batches() const;
    [[nodiscard]] uint32_t image_width() const;
    [[nodiscard]] uint32_t image_height() const;
    [[nodiscard]] uint32_t num_classes() const;
    [[nodiscard]] const char* class_name(uint32_t id) const;
    [[nodiscard]] size_t image_stride() const; // bytes per image
    [[nodiscard]] size_t num_label_instances() const;
    [[nodiscard]] size_t num_rle_pairs() const;

    // Direct runtime views: pixels are mmap-backed, labels live in process memory.
    [[nodiscard]] const float* pixel_blob() const;          // entire float32 NCHW blob
    [[nodiscard]] const LabelIndexEntry* label_index() const;
    [[nodiscard]] const PackedInstance* label_data() const;
    [[nodiscard]] const RLEPair* rle_data() const;

    [[nodiscard]] CudaStreamManager& cuda() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmltk
