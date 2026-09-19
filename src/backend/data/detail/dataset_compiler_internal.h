#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/compiled_file_layout.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data::compiler_internal {
struct DatasetScan {
    catalog::ClassCatalog class_catalog;
    std::uint8_t source_category_base = 0;
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
    mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
};
int resolve_num_workers(int configured_workers, std::span<const int> worker_cpus);
std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
DatasetScan scan_dataset(const CompilerConfig& config, const std::vector<std::string>& splits,
                         mmltk::common::concurrency::CancellationObservation cancellation = {});
LabelBlocks build_label_blocks(const std::filesystem::path& split_dir, uint32_t num_images, const CompilerConfig& config,
                               const catalog::ClassCatalog& class_catalog, std::uint8_t source_category_base, int num_workers, std::span<const int> worker_cpus,
                               ProgressCounter* completed_images = nullptr, std::atomic<bool>* failure_requested = nullptr,
                               mmltk::common::concurrency::CancellationObservation cancellation = {});
void assign_pixel_offsets(std::vector<ImageEntry>& index, size_t pixel_offset, size_t image_stride);
void write_metadata_blocks(const mmltk::common::io::FileHandle& fd, const FileLayout& layout, const FileHeader& header, const LabelBlocks& label_blocks,
                           mmltk::common::concurrency::CancellationObservation cancel_requested = {});
void write_pixel_blob(const mmltk::common::io::FileHandle& fd, const PixelBlobWriteRequest& request);
}  // namespace mmltk::backend::data::compiler_internal
