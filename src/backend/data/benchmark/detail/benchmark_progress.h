#pragma once
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include <array>
#include <utility>
#include <chrono>
#include <atomic>
#include "src/backend/data/detail/compile_progress_delivery.h"
#include <exception>
#include <functional>
#include <cstddef>
#include <span>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <vector>
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
namespace mmltk::backend::data::benchmark_internal {
class ProgressReporter;
class IndexingProgressTotals;
// One compile-owned ledger, keyed by admitted artifact identity. Its mutations
// and reporter snapshots share the reporter lock; no source lock spans enqueue.
class ArtifactProgressTotals final {
public:
 void update(const DownloadProgress& update, ProgressReporter& reporter);
 void images(BenchmarkDatasetSource source, const std::string& artifact, std::uint64_t completed, std::uint64_t source_total, ProgressReporter& reporter, std::string activity = {});

private:
 struct Observation {
  std::uint64_t completed = 0U;
  std::uint64_t total = 0U;
  std::uint64_t retries = 0;
  bool resumed = false, cached = false;
 };
 struct SourceTotals {
  std::unordered_map<std::string, Observation> artifacts;
  std::unordered_map<std::string, std::uint64_t> images;
  std::uint64_t completed_images = 0;
  std::uint64_t completed = 0U;
  std::uint64_t known_total = 0U;
  std::uint64_t unknown_count = 0U;
  std::uint64_t retries = 0, resumed = 0, cached = 0;
 };
 std::map<BenchmarkDatasetSource, SourceTotals> sources_;
};
class ProgressReporter {
public:
 ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace, std::span<const BenchmarkDatasetSource> sources = {}, std::function<void(std::exception_ptr)> failed = {});
 ~ProgressReporter();
 // Join accepted callbacks before publication or releasing callback captures.
 void flush();
 void warning(std::string activity) noexcept;
 [[nodiscard]] IndexingProgressTotals* indexing() noexcept { return indexing_.get(); }
 void invalidate_indexing(std::uint64_t count);
 [[nodiscard]] IndexingProgressTotals* indexing(std::span<const std::uint64_t> totals);
 void phase(const DatasetCompilePhase phase, const std::uint64_t completed = 0U, const std::uint64_t total = 0U);
 void compacting(std::size_t split, std::size_t completed, std::size_t total);
 void label_plans(std::size_t total);
 void label_plan_started(std::size_t slot);
 void label_plan_completed(std::size_t slot);
 void discard_label_plans();
 void activity(std::string activity);
 void pixels(std::uint64_t completed, std::uint64_t total, bool total_known = true);
 void invalidate_pixels(std::uint64_t count);
 [[nodiscard]] ArtifactProgressTotals& transfers() noexcept { return transfers_; }
 void pixel_completed();
 void acquisition_complete();
 void source_activity(const BenchmarkDatasetSource source, std::string activity, bool foreground = true, bool preserve = false);
 [[nodiscard]] bool transfer_observer_enabled() const noexcept;
 [[nodiscard]] bool normalization_observer_enabled() const noexcept;
 [[nodiscard]] bool pixel_observer_enabled() const noexcept;
 void projected(const std::uint64_t bytes);
 void rejected(const std::uint64_t dropped, const std::uint64_t quarantined);
 void source_transfer(const DownloadProgress& update, const BenchmarkSourceProgress& aggregate);
 void source_images(BenchmarkDatasetSource source, std::uint64_t completed, std::uint64_t total, std::string activity = {});
 void source_complete(const BenchmarkDatasetSource source, const bool cache_hit);

private:
 friend class ArtifactProgressTotals;
 friend class IndexingProgressTotals;
 void phase_unlocked(std::unique_lock<std::mutex>&, DatasetCompilePhase, std::uint64_t, std::uint64_t, bool indexing_facts = false);
 void invalidate_indexing_unlocked(std::unique_lock<std::mutex>&, std::uint64_t);
 void source_transfer_unlocked(std::unique_lock<std::mutex>&, const DownloadProgress&, const BenchmarkSourceProgress&);
 void source_images_unlocked(std::unique_lock<std::mutex>&, BenchmarkDatasetSource, std::uint64_t, std::uint64_t, std::string);
 [[nodiscard]] static std::string default_phase_activity(const DatasetCompilePhase phase);
 void set_activity_unlocked(std::string activity);
 static void add_progress(std::uint64_t& target, const std::uint64_t value, const char* context);
 BenchmarkSourceProgress& source_progress(const BenchmarkDatasetSource source);
 void update_acquisition();
 void update_labels();
 void set_source_activity_unlocked(BenchmarkDatasetSource source, std::string activity, bool foreground = true);
 void select_acquisition_source(BenchmarkDatasetSource source);
 void trace_activity(DatasetCompilePhase phase, std::optional<BenchmarkDatasetSource> source, std::string_view activity);
 void flush_pixels_unlocked();
 struct Snapshot {
  BenchmarkCompileProgress progress;
  double pixel_elapsed_seconds = 0;
  bool activity_changed = false;
 };
 using DeliveryMode = detail::CompileProgressDeliveryMode;
 void emit(std::unique_lock<std::mutex>&, DeliveryMode mode = DeliveryMode::Dirty);
 [[nodiscard]] Snapshot materialize();
 void select_work_unlocked();
 BenchmarkProgressCallback callback_;
 std::function<void(std::exception_ptr)> failed_;
 BenchmarkTraceSink trace_;
 BenchmarkCompileProgress state_;
 std::array<std::pair<std::size_t, std::size_t>, 2> compaction_{};
 std::chrono::steady_clock::time_point activity_started_{};
 std::chrono::steady_clock::time_point pixel_started_{};
 bool activity_trace_pending_ = false;
 std::atomic<bool> pixel_total_known_{true};
 std::uint64_t indexed_completed_ = 0, indexed_total_ = 0;
 ArtifactProgressTotals transfers_;
 std::unique_ptr<IndexingProgressTotals> indexing_;
 enum class LabelPlan { Waiting, Active, Complete };
 std::vector<LabelPlan> label_plans_;
 std::uint64_t label_completed_ = 0, label_active_ = 0;
 bool labels_admitted_ = false, indexing_started_ = false, indexing_known_ = false, indexing_settled_ = false;
 // Producers accumulate in cache-line-sized thread stripes. The bounded
 // set is compile-owned, so withdrawal/total changes can flush every live lane.
 struct alignas(64) PixelDelta {
  std::atomic<std::uint64_t> completed{0};
 };
 std::array<PixelDelta, 64> pixel_deltas_{};
 std::atomic<std::uint64_t> pixel_events_{0}, remaining_pixels_{0};
 std::uint64_t observed_pixel_events_ = 0;
 std::mutex mutex_;
 std::unique_ptr<detail::CompileProgressDelivery<Snapshot>> delivery_;
};
// One compile-owned instance. Each release contributes its full-import row
// count once across retained products and repairs; metadata never counts twice.
class IndexingProgressTotals final {
public:
 explicit IndexingProgressTotals(std::span<const std::uint64_t> totals);
 void update(std::size_t release, std::uint64_t completed, ProgressReporter& reporter);
 void settle(std::size_t release, std::uint64_t completed, ProgressReporter& reporter);
 // The replaced importer must have returned or joined before withdrawal.
 void invalidate(std::size_t release, ProgressReporter& reporter, std::uint64_t rows = UINT64_MAX);

private:
 struct Observation {
  std::uint64_t completed = 0, total = 0, declared_total = 0;
  bool settled = false;
 };
 void publish(std::unique_lock<std::mutex>&, ProgressReporter&);
 std::vector<Observation> releases_;
 std::uint64_t completed_ = 0, total_ = 0;
 std::size_t unknown_ = 0, unsettled_ = 0;
};
}  // namespace mmltk::backend::data::benchmark_internal
