#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <memory>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
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
class BenchmarkImageReadError final : public std::runtime_error {
public:
 BenchmarkImageReadError(std::uint16_t source_index, std::uint64_t source_image_id, std::string detail);
 [[nodiscard]] std::uint16_t source_index() const noexcept;
 [[nodiscard]] std::uint64_t source_image_id() const noexcept;

private:
 std::uint16_t source_index_ = 0U;
 std::uint64_t source_image_id_ = 0U;
};
[[nodiscard]] PackedInstance benchmark_canvas_box(std::uint8_t class_id, float x1, float y1, float x2, float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox);
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
};
// Membership and physical pixel storage settle before annotations. Calls on different
// slots may overlap; each lane has exclusive reusable decoder/resizer scratch.
// The owner must drain pixel calls before finalization, repair, or destruction.
class BenchmarkSplitWriter final {
public:
 explicit BenchmarkSplitWriter(const BenchmarkWriteRequest&, bool use_actual_dimensions = false);
 ~BenchmarkSplitWriter();
 BenchmarkSplitWriter(const BenchmarkSplitWriter&) = delete;
 BenchmarkSplitWriter& operator=(const BenchmarkSplitWriter&) = delete;
 void write_pixel(std::size_t slot, std::size_t lane);
 void write_remaining(const BenchmarkWriteRequest&);
 [[nodiscard]] bool matches_membership(const PreparedBenchmarkSplit&) const;
 void invalidate_source(const std::filesystem::path&);
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
