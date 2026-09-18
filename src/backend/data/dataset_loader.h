#pragma once
#include "src/backend/data/data_loading_options.h"
#include <cstdint>
#include <cuda_runtime_api.h>
#include <memory>
#include <string>
#include <optional>
#include <stop_token>
#include "src/frameworks/gpu/device_execution.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/imaging/resample/image_resize.h"
namespace mmltk::frameworks::gpu {
class TerminalCudaRetirementOwner;
}
namespace mmltk::backend::data {
struct Batch {
    size_t num_images = 0;
    const float* device_images = nullptr;
    const LabelIndexEntry* label_index = nullptr;
    const PackedInstance* labels = nullptr;
    const RLEPair* rle_pairs = nullptr;
    const uint32_t* image_indices = nullptr;
    size_t slot_index = 0;
    uint64_t lease_id = 0;
    const void* owner = nullptr;
    std::weak_ptr<const void> image_custody{};
    std::size_t image_capacity_bytes = 0;
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
        int gather_workers = 0;
        std::string cpu_affinity{};
        uint32_t batch_shard_rank = 0;
        uint32_t batch_shard_count = 1;
        bool drop_last = false;
        DataLoadingOptions loading{};
        std::optional<mmltk::frameworks::gpu::DeviceExecution> execution{};
    };
    explicit DatasetLoader(const Config& config, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {},
                           decltype(&cudaEventRecord) record_consumer = &cudaEventRecord);
    ~DatasetLoader();
    DatasetLoader(const DatasetLoader&) = delete;
    DatasetLoader& operator=(const DatasetLoader&) = delete;
    void begin_epoch();
    bool next_batch(Batch& out);
    // Cancellation only wakes acquisition; joining and checked-out custody remain with the owner.
    bool next_batch(Batch& out, std::stop_token);
    // Optional CPU pixels for this checked-out lease; valid until release_batch.
    // Contiguous images alias the source. Scattered GDR images gather once here.
    [[nodiscard]] std::span<const float> host_images(const Batch& batch);
    void wait_batch(const Batch& batch);
    void handoff_batch(const Batch& batch, void* consumer_stream);
    void release_batch(const Batch& batch);
    void release_batch(const Batch& batch, void* consumer_stream);
    void synchronize();
    // Stop/join CPU workers without releasing checked-out GPU storage. Idempotent.
    void stop_workers();
    [[nodiscard]] size_t num_images() const;
    [[nodiscard]] size_t num_batches() const;
    [[nodiscard]] uint32_t image_width() const;
    [[nodiscard]] uint32_t image_height() const;
    [[nodiscard]] uint32_t num_classes() const;
    [[nodiscard]] uint32_t max_instances_per_image() const;
    [[nodiscard]] const std::shared_ptr<const catalog::ClassCatalog>& class_catalog() const noexcept;
    [[nodiscard]] const char* class_name(uint32_t id) const;
    [[nodiscard]] size_t image_stride() const;
    [[nodiscard]] size_t num_label_instances() const;
    [[nodiscard]] size_t num_rle_pairs() const;
    [[nodiscard]] bool masks_available() const noexcept;
    [[nodiscard]] const float* pixel_blob() const;
    [[nodiscard]] const LabelIndexEntry* label_index() const;
    [[nodiscard]] const PackedInstance* label_data() const;
    [[nodiscard]] const RLEPair* rle_data() const;
    [[nodiscard]] const ImageEntry& image_entry(std::uint32_t index) const;
    [[nodiscard]] mmltk::backend::imaging::resample::ImageResizeGeometry geometry(std::uint32_t index) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::data
