#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <memory>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
class BenchmarkCompilePipeline;
struct BenchmarkLabelChunk;
struct CachedImageSource {
 std::filesystem::path root;
};
struct EncodedImageRecord {
 std::uint64_t source_image_id = 0;
 std::uint32_t source_width = 0;
 std::uint32_t source_height = 0;
 std::uint16_t label_count = 0;
 std::uint16_t source_index = 0;
 AnnotationSource annotation_source = AnnotationSource::Generic;
};
struct PreparedBenchmarkSplit {
 std::string name;
 std::vector<std::string> class_names;
 std::vector<CachedImageSource> sources;
 std::vector<EncodedImageRecord> images;
 std::size_t labels = 0, runs = 0;
};
struct BenchmarkSealedSplit final {
 detail::StagedArtifact artifact;
 CompiledDatasetInfo info;
};
struct BenchmarkWriteProgressEvent final {
 void* context = nullptr;
 void (*image_completed)(void*) = nullptr;
 void (*images_invalidated)(void*, std::uint64_t) = nullptr;
 void operator()() const {
  if (image_completed != nullptr) { image_completed(context); }
 }
 [[nodiscard]] explicit operator bool() const noexcept { return image_completed != nullptr; }
};
using BenchmarkImageReadObserver = std::function<void(const std::filesystem::path&, std::uint64_t)>;
struct BenchmarkWriteRequest {
 const PreparedBenchmarkSplit& split;
 std::filesystem::path output_path;
 std::uint32_t resolution = 0;
 int num_workers = 0;
 std::span<const int> worker_cpus{};
 bool overwrite = false;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 BenchmarkWriteProgressEvent progress{};
 bool perceptual_downscale = false;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 // Private effect-only boundary after opening a cached image, outside locks.
 BenchmarkImageReadObserver image_opened{};
 // Effect-only preparation observation, called once per checked plan outside locks.
 BenchmarkImageReadObserver pixel_preparing{};
 BenchmarkCompilePipeline* execution = nullptr;
 CompiledCompactionProgress compaction{};
 std::span<std::byte> compaction_scratch{};
 // Effect-only physical preparation observation, outside source/facts locks.
 BenchmarkImageReadObserver extent_preparing{};
 BenchmarkTraceSink trace{};
};
// Physical slots are stable across sparse growth. Calls on different
// slots may overlap; execution lends an exclusive pixel context and arena.
// The owner must drain pixel calls before finalization, repair, or destruction.
struct BenchmarkPixelInput;
class BenchmarkSplitWriter final {
public:
 explicit BenchmarkSplitWriter(const BenchmarkWriteRequest&, bool use_actual_dimensions = false);
 ~BenchmarkSplitWriter();
 BenchmarkSplitWriter(const BenchmarkSplitWriter&) = delete;
 BenchmarkSplitWriter& operator=(const BenchmarkSplitWriter&) = delete;
 // Slot allocation and label publication are independent of pixel readiness.
 [[nodiscard]] std::pair<std::filesystem::path, std::uint64_t> identity(std::size_t) const;
 [[nodiscard]] std::uint32_t allocate_slot(EncodedImageRecord, const CachedImageSource&);
 void input_root(std::uint32_t, const std::filesystem::path&);
 void invalidate_labels(std::uint32_t);
 void settle_labels(std::uint32_t, std::uint64_t ordinal_base, bool ordinal_incomplete = false);
 [[nodiscard]] ImageEntry entry(std::uint32_t) const;
 [[nodiscard]] BenchmarkSealedSplit seal_slots(const BenchmarkWriteRequest&, std::span<const std::uint32_t>);
 void prepare_pixels(std::uint32_t);
 void write_label_output(std::uint32_t, ImageEntry, detail::CompiledLabelWorkspace::OutputLease, std::uint64_t ordinal_base = 0);
 void write_labels(std::uint32_t slot, ImageEntry entry, ImageAnnotations, std::uint64_t ordinal_base = 0);
 [[nodiscard]] std::shared_ptr<BenchmarkPixelInput> prepare_pixel(std::size_t slot, std::size_t lane, BenchmarkSourcePublication publication = {}, CompileAllowance allowance = {},
  std::shared_ptr<const BenchmarkEncodedImage> payload = {}, std::shared_ptr<const detail::CompilePixelWorkspacePlan> plan = {});
 [[nodiscard]] std::shared_ptr<const detail::CompilePixelWorkspacePlan> prepare_pixel_plan(std::size_t slot, const BenchmarkImageHeader&) const;
 [[nodiscard]] CompileAllowance pixel_input_allowance(const BenchmarkPixelInput&) const;
 [[nodiscard]] std::size_t pixel_input_bytes(const BenchmarkPixelInput&) const;
 [[nodiscard]] CompileResources pixel_resources(const BenchmarkPixelInput&) const;
 [[nodiscard]] CompileResources preload_resources(const BenchmarkPixelInput&) const;
 // Completes the already admitted input+conversion envelope on a file service.
 // Its encoded slice and remaining conversion slice retain one physical arena.
 void preload_pixel(BenchmarkPixelInput&, CompileAllowance&);
 void write_pixel(std::size_t slot, std::size_t lane, const std::shared_ptr<BenchmarkPixelInput>&, CompileAllowance, detail::CompilePixelWorkspace&);
 void write_pixel(std::size_t slot, std::size_t lane);
 void write_remaining(const BenchmarkWriteRequest&);
 void invalidate_source(const std::filesystem::path&);
 void invalidate_image(std::size_t slot);
 [[nodiscard]] std::uint64_t allocated_bytes() const;
 [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> header_dimensions(std::size_t slot) const;
 [[nodiscard]] bool image_complete(std::size_t slot) const;
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> dimensions(std::size_t slot) const;
 [[nodiscard]] std::size_t completed() const noexcept;
 // Final membership preserves slot IDs while compacting the staged partitions.
 [[nodiscard]] BenchmarkSealedSplit seal(const BenchmarkWriteRequest&);
 void finish(const BenchmarkWriteRequest&);

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
void write_benchmark_split(const BenchmarkWriteRequest& request);
// Joins both physical seals before returning either result or propagating an
// error. Each callback receives admitted pinned compaction scratch.
[[nodiscard]] std::array<BenchmarkSealedSplit, 2> seal_benchmark_splits(BenchmarkCompilePipeline&, const std::function<BenchmarkSealedSplit(std::size_t, std::span<std::byte>)>&);
}  // namespace mmltk::backend::data::benchmark_internal
