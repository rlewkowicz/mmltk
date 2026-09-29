#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include <atomic>
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
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
struct BenchmarkLabelChunk;
struct CachedImageSource {
 std::filesystem::path root;
};
struct EncodedImageRecord {
 std::uint64_t source_image_id = 0;
 std::uint32_t source_width = 0;
 std::uint32_t source_height = 0;
 std::uint32_t first_label = 0;
 std::uint16_t label_count = 0;
 std::uint16_t source_index = 0;
 AnnotationSource annotation_source = AnnotationSource::Generic;
};
struct PreparedBenchmarkSplit {
 std::string name;
 std::vector<std::string> class_names;
 std::vector<CachedImageSource> sources;
 std::vector<EncodedImageRecord> images;
 std::vector<PackedInstance> labels;
 std::vector<RLEPair> rle_pairs;
};
// Owns immutable image chunks through their single final checked placement.
// Callers can inspect the plan, but cannot mutate admitted records or forge a seal.
class BenchmarkSplitAssembly final {
public:
 BenchmarkSplitAssembly() = default;
 BenchmarkSplitAssembly(std::string name, std::span<const std::string_view> classes, std::uint32_t resolution,
  mmltk::backend::imaging::resample::ImageResizeMode);
 BenchmarkSplitAssembly(BenchmarkSplitAssembly&&) = default;
 BenchmarkSplitAssembly& operator=(BenchmarkSplitAssembly&&) = default;
 BenchmarkSplitAssembly(const BenchmarkSplitAssembly&) = delete;
 BenchmarkSplitAssembly& operator=(const BenchmarkSplitAssembly&) = delete;
 [[nodiscard]] const PreparedBenchmarkSplit& data() const noexcept { return split_; }
 operator const PreparedBenchmarkSplit&() const noexcept { return split_; }
 [[nodiscard]] std::size_t label_count() const noexcept { return labels_; }
 [[nodiscard]] std::size_t run_count() const noexcept { return runs_; }
 void add_source(std::filesystem::path);
 void image(EncodedImageRecord, std::shared_ptr<const BenchmarkLabelChunk>, std::uint64_t ordinal_base);
 void append(BenchmarkSplitAssembly&&);
 // The source controller calls this as soon as this split's counts settle,
 // while acquisition/pixels may continue. Joins stay outside the CPU lanes.
 // Return (also on failure) settles all borrowed placement jobs before rebuild.
 void materialize(BenchmarkCompilePipeline&, mmltk::common::concurrency::CancellationObservation = {});
private:
 friend class BenchmarkSplitWriter;
 void mutable_records() const;
 void finish(mmltk::common::concurrency::CancellationObservation);
 PreparedBenchmarkSplit split_;
 struct Placement {
  std::shared_ptr<const BenchmarkLabelChunk> chunk;
  std::uint64_t ordinal_base = 0;
  std::size_t first_run = 0;
 };
 std::vector<Placement> placements_;
 std::vector<ImageEntry> index_;
 std::size_t labels_ = 0, runs_ = 0;
 std::uint32_t maximum_ = 0;
 std::uint32_t resolution_ = 0;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode_{};
 FileLayout layout_{};
 FileHeader header_{};
 std::shared_ptr<const catalog::ClassCatalog> catalog_;
 bool placement_started_ = false, materialized_ = false;
 bool sealed_ = false;
};
struct BenchmarkSealedSplit final {
 BenchmarkStagedArtifact artifact;
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
 std::span<const int> worker_cpus;
 bool overwrite = false;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 BenchmarkWriteProgressEvent progress;
 bool perceptual_downscale = false;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 // Private effect-only boundary after opening a cached image, outside locks.
 BenchmarkImageReadObserver image_opened{};
 BenchmarkCompilePipeline* execution = nullptr;
};
// Membership and physical pixel storage settle before annotations. Calls on different
// slots may overlap; each lane has exclusive reusable decoder/resizer scratch.
// The owner must drain pixel calls before finalization, repair, or destruction.
struct BenchmarkPixelInput;
class BenchmarkSplitWriter final {
public:
 explicit BenchmarkSplitWriter(const BenchmarkWriteRequest&, bool use_actual_dimensions = false);
 ~BenchmarkSplitWriter();
 BenchmarkSplitWriter(const BenchmarkSplitWriter&) = delete;
 BenchmarkSplitWriter& operator=(const BenchmarkSplitWriter&) = delete;
 void prepare_lanes(std::size_t);
 void retire_scratch(std::size_t) noexcept;
 [[nodiscard]] std::shared_ptr<BenchmarkPixelInput> prepare_pixel(std::size_t slot, std::size_t lane,
  BenchmarkSourcePublication publication = {}, BenchmarkAllowance allowance = {}, std::shared_ptr<const BenchmarkEncodedImage> payload = {});
 [[nodiscard]] BenchmarkAllowance pixel_input_allowance(const BenchmarkPixelInput&) const;
 [[nodiscard]] std::uint64_t pixel_workspace_bytes(const BenchmarkPixelInput&) const;
 [[nodiscard]] std::uint64_t pixel_workspace_bytes(const BenchmarkImageHeader&, std::size_t encoded_bytes) const;
 void write_pixel(std::size_t slot, std::size_t lane, const std::shared_ptr<BenchmarkPixelInput>&);
 void write_pixel(std::size_t slot, std::size_t lane);
 void write_remaining(const BenchmarkWriteRequest&);
 [[nodiscard]] bool matches_membership(const PreparedBenchmarkSplit&) const;
 void invalidate_source(const std::filesystem::path&);
 void invalidate_image(std::size_t slot);
 void retain_completed(const BenchmarkSplitWriter& previous);
 [[nodiscard]] std::uint64_t allocated_bytes() const;
 [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> header_dimensions(std::size_t slot) const;
 [[nodiscard]] bool image_complete(std::size_t slot) const;
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> dimensions(std::size_t slot) const;
 [[nodiscard]] std::size_t completed() const noexcept;
 // Final membership may only remove slots, preserving canonical order. This
 // performs one forward bounded compaction only when quarantine shrinks it.
 [[nodiscard]] BenchmarkSealedSplit seal(const BenchmarkWriteRequest&, BenchmarkSplitAssembly* = nullptr);
 void finish(const BenchmarkWriteRequest&);

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
void write_benchmark_split(const BenchmarkWriteRequest& request);
}  // namespace mmltk::backend::data::benchmark_internal
