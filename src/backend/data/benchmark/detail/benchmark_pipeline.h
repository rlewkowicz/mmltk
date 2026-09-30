#pragma once
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <source_location>
#include <stdexcept>
#include <utility>
#include <string_view>
#include "src/backend/imaging/resample/image_resize.h"
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCurl;
class BenchmarkSplitWriter;
struct PreparedBenchmarkSplit;
struct BenchmarkLabelChunk;
class BenchmarkLabelInput;
class BenchmarkLabelWorkspace;

class BenchmarkCompilePipeline final {
 friend class BenchmarkSourcePublication;
public:
 explicit BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus = {}, detail::CompileExecutionLimits = {}, mmltk::common::concurrency::CancellationObservation = {}, BenchmarkTraceSink = {});
 ~BenchmarkCompilePipeline();
 BenchmarkCompilePipeline(const BenchmarkCompilePipeline&) = delete;
 BenchmarkCompilePipeline& operator=(const BenchmarkCompilePipeline&) = delete;
 [[nodiscard]] detail::CompileExecution& execution() noexcept;
 [[nodiscard]] BenchmarkCurl& curl();
 [[nodiscard]] detail::FileWorkQueue& files() noexcept;
 [[nodiscard]] detail::StorageReservationPool& storage() noexcept;
 void write_remaining(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&, std::span<const std::size_t>);
 [[nodiscard]] detail::CompileExecution::Completion image_completion(const std::filesystem::path&, std::uint64_t) const;
 [[nodiscard]] detail::CompileExecution::Completion label_completion(const std::filesystem::path&, std::uint64_t, std::string_view) const;
 [[nodiscard]] bool subscribe_labels(const BenchmarkSourcePublication&, std::uint64_t, std::string_view, std::uint64_t original_generation, std::function<void(std::exception_ptr)>) const;
 void membership_ready();
 void bind_slot(const std::filesystem::path&, std::uint64_t, BenchmarkSplitWriter&, std::uint32_t);
 // Bind a source family before acquisition. Its first image or label allocates
 // one stable physical slot through the sole compile-local identity join.
 void bind_source(const std::filesystem::path&, BenchmarkSplitWriter&, AnnotationSource);
 [[nodiscard]] std::optional<std::uint32_t> image_slot(const std::filesystem::path&, std::uint64_t) const;
 void wait_image(const std::filesystem::path&, std::uint64_t);
 // Capture once while the producer holds its physical mutation lease. A repair
 // captures only its replacement image; all other events keep the source epoch.
 [[nodiscard]] BenchmarkSourcePublication source_publication(
  const std::filesystem::path&, std::shared_ptr<const ArtifactLease>, std::optional<std::uint64_t> repaired_image = {}, bool defer_pixels = false);
 [[nodiscard]] BenchmarkSourceGeneration source_generation(const std::filesystem::path&);
 [[nodiscard]] BenchmarkSourceGeneration image_generation(const std::filesystem::path&, std::uint64_t);
 void retire_image(const std::filesystem::path&, std::uint64_t);
 void geometry_ready(const BenchmarkImageGeometry&);
 [[nodiscard]] std::shared_ptr<const BenchmarkEncodedImage> image_input(const std::filesystem::path&, std::uint64_t) const;
 [[nodiscard]] std::optional<BenchmarkImageGeometry> geometry(const std::filesystem::path&, std::uint64_t) const;
 // Publication retains native/normalized custody and never waits for geometry,
 // resources or CPU completion. ImageState owns the join with physical facts.
 // False means the publication generation withdrew before admission.
 bool labels_ready(const BenchmarkSourcePublication&, std::uint64_t, BenchmarkLabelInput, std::string_view dependency, std::uint64_t original_generation = 0, unsigned priority = 0);
 [[nodiscard]] std::shared_ptr<const BenchmarkLabelChunk> image_labels(const std::filesystem::path&, std::uint64_t, std::string_view dependency) const;
 // Pending claims remain reusable. Once settled, an incremental claim must
 // occupy its slot; labels_ready can replay a displaced claim from mapped input.
 [[nodiscard]] bool has_image_labels(const std::filesystem::path&, std::uint64_t, std::string_view dependency, std::uint64_t original_generation = 0) const;
 // Required final assembly joins the same admission outside every CPU lane.
 [[nodiscard]] std::shared_ptr<const BenchmarkLabelChunk> wait_image_labels(const std::filesystem::path&, std::uint64_t, std::string_view dependency, std::uint64_t original_generation = 0);
 void retire_labels(const std::filesystem::path&, std::uint64_t, std::string_view dependency);
 void original_generation(const std::filesystem::path&, std::uint64_t, bool withdrawn);
 // Effect-only observation after admission and before projection, outside locks.
 void label_configuration(std::uint32_t, mmltk::backend::imaging::resample::ImageResizeMode, std::function<void(const std::filesystem::path&, std::uint64_t, const BenchmarkLabelWorkspace&)> = {},
  std::function<void(const std::filesystem::path&, std::uint64_t)> = {});
 // Withdraw admission first, join only this source, then clear affected facts.
 // Unrelated source tasks and completed products continue to be usable.
 void retire_source(const std::filesystem::path&);
 void drain();
 // Explicit borrower boundary for construction before recipe/writer owners.
 // Discards queued work and joins active calls before registered owners die.
 void retire_attempt() noexcept;
 class Attempt final {
 public:
  explicit Attempt(BenchmarkCompilePipeline& owner) : owner_(owner) {}
  ~Attempt() { owner_.retire_attempt(); }
  Attempt(const Attempt&) = delete;
  Attempt& operator=(const Attempt&) = delete;

 private:
  BenchmarkCompilePipeline& owner_;
 };

private:
 struct Impl;
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::data::benchmark_internal
