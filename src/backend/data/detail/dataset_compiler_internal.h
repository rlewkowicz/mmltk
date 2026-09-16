#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/backend/data/dataset_compiler.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data::compiler_internal {
struct DatasetScan {
    std::unordered_map<std::string, uint8_t> class_map;
    std::vector<DatasetCompileSplitPlan> splits;
};
struct LabelBlocks {
    std::vector<ImageEntry> index;
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
    std::uint32_t max_instances_per_image = 0;
    bool any_image_resize = false;
    bool any_image_downscale = false;
    std::uint64_t dropped_instances = 0;
};
struct ProgressCounter {
    enum class Stage : std::uint8_t { kLabels, kPixels };
    CompileTelemetry* telemetry = nullptr;
    Stage stage = Stage::kLabels;
    void begin_worker() noexcept;
    void add_completed(size_t count) noexcept;
    void end_worker() noexcept;
    void update_worker_count(bool starting) noexcept;
};
class ProgressBatch {
   public:
    explicit ProgressBatch(ProgressCounter* counter) noexcept;
    ~ProgressBatch();
    void increment() noexcept;

   private:
    void flush() noexcept;
    static constexpr size_t kPublishBatch = 32U;
    ProgressCounter* counter_ = nullptr;
    size_t pending_ = 0U;
};
struct LayoutFinalizeInputs {
    size_t label_count = 0;
    size_t rle_count = 0;
};
struct FileHeaderInputs {
    uint32_t num_images = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    uint32_t max_instances_per_image = 0;
    size_t image_stride = 0;
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
struct PixelBlobWriteRequest {
    const std::filesystem::path& split_dir;
    uint32_t num_images = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t image_stride = 0;
    int num_workers = 0;
    bool any_resize = false;
    bool any_downscale = false;
    bool perceptual_downscale = false;
    ProgressCounter* completed_images = nullptr;
    std::span<const int> worker_cpus;
    int initial_active_workers = 0;
    const std::atomic<bool>* release_all_workers = nullptr;
    size_t pixel_offset = 0;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    std::atomic<bool>* failure_requested = nullptr;
};
int resolve_num_workers(int configured_workers, std::span<const int> worker_cpus);
std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
DatasetScan scan_dataset(const CompilerConfig& config, const std::vector<std::string>& splits,
                         mmltk::common::concurrency::CancellationObservation cancellation = {});
LabelBlocks build_label_blocks(const std::filesystem::path& split_dir, uint32_t num_images, const CompilerConfig& config,
                               const std::unordered_map<std::string, uint8_t>& class_map, int num_workers, std::span<const int> worker_cpus,
                               ProgressCounter* completed_images = nullptr, std::atomic<bool>* failure_requested = nullptr,
                               mmltk::common::concurrency::CancellationObservation cancellation = {});
FileLayout compute_pixel_layout(uint32_t num_images, size_t image_stride);
void finalize_layout(FileLayout& layout, const LayoutFinalizeInputs& inputs);
void assign_pixel_offsets(std::vector<ImageEntry>& index, size_t pixel_offset, size_t image_stride);
FileHeader make_file_header(const FileHeaderInputs& inputs, const std::unordered_map<std::string, uint8_t>& class_map, const FileLayout& layout);
void write_metadata_blocks(const mmltk::common::io::FileHandle& fd, const FileLayout& layout, const FileHeader& header, const LabelBlocks& label_blocks,
                           mmltk::common::concurrency::CancellationObservation cancel_requested = {});
void write_pixel_blob(const mmltk::common::io::FileHandle& fd, const PixelBlobWriteRequest& request);
}  // namespace mmltk::backend::data::compiler_internal
