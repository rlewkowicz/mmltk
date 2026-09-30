#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include <span>
#include <string>
#include <vector>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/common/io/file_memory.h"
#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/detail/file_work_queue.h"
namespace mmltk::backend::data::compiler_internal {
struct DatasetScan {
 catalog::ClassCatalog class_catalog;
 std::uint8_t source_category_base = 0;
 std::vector<DatasetCompileSplitPlan> splits;
};
struct LabelBlocks {
 std::vector<ImageEntry> index;
 std::uint32_t max_instances_per_image = 0;
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
// A context owns all scratch across bounded label continuations. No borrowed
// lane-local parser, input, mask scratch or output survives a return.
class DirectoryLabels final {
 struct Parser;
 struct Impl;
 std::unique_ptr<Parser> parser_;
 std::unique_ptr<Impl> impl_;
public:
 explicit DirectoryLabels(detail::CompileExecution&);
 ~DirectoryLabels();
 DirectoryLabels(const DirectoryLabels&) = delete;
 DirectoryLabels& operator=(const DirectoryLabels&) = delete;
 [[nodiscard]] static detail::CompileResources resources(std::uint32_t width, std::size_t maximum_line);
 void begin(std::uint32_t image_index, detail::CompileAllowance, std::span<const std::uint8_t> input, std::size_t maximum_line,
  const detail::CompileImageHeader&, const CompilerConfig&, const catalog::ClassCatalog&, std::uint8_t category_base,
  std::filesystem::path annotation, std::filesystem::path image);
 [[nodiscard]] bool step(mmltk::common::concurrency::CancellationObservation);
 [[nodiscard]] const ImageEntry& entry() const;
 [[nodiscard]] detail::CompiledLabelWorkspace::OutputLease release_output();
 void take_diagnostics(std::vector<CompileDiagnostic>&);
 void reset() noexcept;
};
inline constexpr std::size_t directory_encoded_window = 32U << 20;
inline constexpr std::size_t directory_line_window = 64U << 10;
LabelBlocks compile_directory_images(const std::filesystem::path&, std::uint32_t images, const CompilerConfig&, const catalog::ClassCatalog&, std::uint8_t category_base,
 detail::CompiledSlotStorage&, detail::CompileExecution&, detail::FileWorkQueue&, ProgressCounter* labels, ProgressCounter* pixels,
 mmltk::common::concurrency::CancellationObservation);
int resolve_num_workers(int configured_workers, std::span<const int> worker_cpus);
std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
DatasetScan scan_dataset(const CompilerConfig& config, const std::vector<std::string>& splits, mmltk::common::concurrency::CancellationObservation cancellation = {});
void write_metadata_blocks(const mmltk::common::io::FileHandle& fd, const FileLayout& layout, const FileHeader& header, const LabelBlocks& label_blocks,
 mmltk::common::concurrency::CancellationObservation cancel_requested = {});
}  // namespace mmltk::backend::data::compiler_internal
