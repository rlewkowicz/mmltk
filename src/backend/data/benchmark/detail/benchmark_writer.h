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
#include <vector>
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
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
 void finish(const BenchmarkWriteRequest&);

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
void write_benchmark_split(const BenchmarkWriteRequest& request);
}  // namespace mmltk::backend::data::benchmark_internal
