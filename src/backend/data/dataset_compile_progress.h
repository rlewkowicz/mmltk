#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include "src/frameworks/reflection/field_policy.h"
#include <string>
#include <string_view>
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::data {
inline constexpr std::size_t kDatasetCompileProgressTextCapacity = 1024U;
enum class BenchmarkDatasetSource : std::uint8_t {
 kCoco2017,
 kObjects365V2,
 kOpenImagesV7,
 kCoconut,
 kObjects365V1,
};
[[nodiscard]] inline constexpr std::string_view benchmark_source_label(BenchmarkDatasetSource source) noexcept {
 switch (source) {
  case BenchmarkDatasetSource::kCoco2017: return "COCO 2017";
  case BenchmarkDatasetSource::kObjects365V2: return "Objects365 v2";
  case BenchmarkDatasetSource::kOpenImagesV7: return "Open Images v7";
  case BenchmarkDatasetSource::kCoconut: return "COCONut";
  case BenchmarkDatasetSource::kObjects365V1: return "Objects365 v1";
 }
 return "Unknown source";
}
// The latest artifact is independent of cumulative source acquisition totals.
struct BenchmarkTransferProgress final {
 // Retained prefix plus accepted artifact writes; discarded HTTP bodies never count.
 std::uint64_t completed_bytes = 0;
 // Zero remains unknown until an admitted response or successful settlement establishes size.
 std::uint64_t total_bytes = 0;
 // Durable bytes retained before the active attempt; excludes in-flight observations.
 std::uint64_t retained_bytes = 0;
 std::uint32_t attempt = 0;
 bool cache_hit = false;
 bool resumed = false;
 [[nodiscard]] bool valid() const noexcept { return retained_bytes <= completed_bytes && (total_bytes == 0 || completed_bytes <= total_bytes); }
 bool operator==(const BenchmarkTransferProgress&) const = default;
};
MMLTK_REFLECT_FIELDS(BenchmarkTransferProgress)
struct BenchmarkSourceProgress {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 [[= mmltk::frameworks::reflection::MaxBytes{kDatasetCompileProgressTextCapacity}]] std::string activity;
 std::optional<BenchmarkTransferProgress> transfer{};
 std::uint64_t completed_bytes = 0;
 std::uint64_t total_bytes = 0;
 std::uint64_t completed_images = 0;
 std::uint64_t total_images = 0;
 std::uint64_t invalidated_images = 0;
 std::uint64_t retry_count = 0;
 bool cache_hit = false;
 bool resumed = false;
 bool complete = false;
 // Unobserved sources are neutral; an observed unknown-size contribution clears this.
 bool byte_total_known = true;
 bool operator==(const BenchmarkSourceProgress&) const = default;
};
MMLTK_REFLECT_ENUM(BenchmarkDatasetSource)
MMLTK_REFLECT_FIELDS(BenchmarkSourceProgress)
// Fixed-size observed facts: safe to snapshot in local compiler noexcept paths.
enum class DatasetCompileActivity : std::uint8_t { Waiting, Unnecessary, Acquiring, Normalizing, Preparing, Compiling, Complete };
struct DatasetCompileTrack final {
 std::uint64_t completed = 0;
 std::uint64_t total = 0;
 bool total_known = false;
 bool active = false;
 bool complete = false;
 DatasetCompileActivity activity = DatasetCompileActivity::Waiting;
 // Successful contributions explicitly removed by repair, never attempts.
 std::uint64_t invalidated = 0;
 [[nodiscard]] bool valid() const noexcept {
  return static_cast<std::uint8_t>(activity) <= static_cast<std::uint8_t>(DatasetCompileActivity::Complete) && (!total_known || completed <= total) && !(active && complete);
 }
 bool operator==(const DatasetCompileTrack&) const = default;
};
struct DatasetCompileTracks final {
 DatasetCompileTrack acquisition{};
 DatasetCompileTrack labels{};
 DatasetCompileTrack pixels{};
 [[nodiscard]] bool valid() const noexcept { return acquisition.valid() && labels.valid() && pixels.valid(); }
 bool operator==(const DatasetCompileTracks&) const = default;
};
[[nodiscard]] inline std::string format_dataset_compile_tracks(const DatasetCompileTracks& tracks) {
 const auto format = [](std::string_view name, const DatasetCompileTrack& track) {
  return std::string(name) + " " + std::to_string(track.completed) + "/" + (track.total_known ? std::to_string(track.total) : "?") +
         (track.activity == DatasetCompileActivity::Unnecessary ? " unnecessary"
          : track.complete                                      ? " complete"
           : track.active                                       ? " active"
                                                                : " waiting") +
         (track.invalidated ? " (" + std::to_string(track.invalidated) + " invalidated)" : "");
 };
 return format("Acquisition", tracks.acquisition) + " | " + format("Labels/masks", tracks.labels) + " | " + format("Pixels", tracks.pixels);
}
MMLTK_REFLECT_ENUM(DatasetCompileActivity)
MMLTK_REFLECT_FIELDS(DatasetCompileTrack)
MMLTK_REFLECT_FIELDS(DatasetCompileTracks)
}  // namespace mmltk::backend::data
