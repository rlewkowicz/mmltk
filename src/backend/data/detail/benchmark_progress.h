#pragma once
#include <chrono>
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
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "benchmark_cache.h"
#include "benchmark_download.h"
namespace mmltk::backend::data::benchmark_internal {
class ProgressReporter;
class IndexingProgressTotals;
// One compile-owned ledger, keyed by admitted artifact identity.
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
 std::mutex mutex_;
};
class ProgressReporter {
public:
 ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace, std::span<const BenchmarkDatasetSource> sources = {});
 ~ProgressReporter();
 [[nodiscard]] IndexingProgressTotals* indexing() noexcept { return indexing_.get(); }
 void invalidate_indexing(std::uint64_t count);
 [[nodiscard]] IndexingProgressTotals* indexing(std::span<const std::uint64_t> totals);
 void phase(const DatasetCompilePhase phase, const std::uint64_t completed = 0U, const std::uint64_t total = 0U);
 void label_plans(std::size_t total);
 void label_plan_started(std::size_t slot);
 void label_plan_completed(std::size_t slot);
 void discard_label_plans();
 void activity(std::string activity);
 void pixels(std::uint64_t completed, std::uint64_t total);
 void invalidate_pixels(std::uint64_t count);
 [[nodiscard]] ArtifactProgressTotals& transfers() noexcept { return transfers_; }
 void pixel_completed();
 void acquisition_complete();
 void source_activity(const BenchmarkDatasetSource source, std::string activity, bool foreground = true);
 [[nodiscard]] bool transfer_observer_enabled() const noexcept;
 [[nodiscard]] bool normalization_observer_enabled() const noexcept;
 [[nodiscard]] bool pixel_observer_enabled() const noexcept;
 void projected(const std::uint64_t bytes);
 void rejected(const std::uint64_t dropped, const std::uint64_t quarantined);
 void source_transfer(const DownloadProgress& update, const BenchmarkSourceProgress& aggregate);
 void source_images(BenchmarkDatasetSource source, std::uint64_t completed, std::uint64_t total, std::string activity = {});
 void source_complete(const BenchmarkDatasetSource source, const bool cache_hit);

private:
 [[nodiscard]] static std::string default_phase_activity(const DatasetCompilePhase phase);
 void set_activity_unlocked(std::string activity);
 static void add_progress(std::uint64_t& target, const std::uint64_t value, const char* context);
 BenchmarkSourceProgress& source_progress(const BenchmarkDatasetSource source);
 void update_acquisition();
 void update_labels();
 void set_source_activity_unlocked(BenchmarkDatasetSource source, std::string activity, bool foreground = true);
 void select_acquisition_source(BenchmarkDatasetSource source);
 void trace_activity(std::optional<BenchmarkDatasetSource> source, std::string_view activity);
 void emit();
 BenchmarkProgressCallback callback_;
 const BenchmarkTraceSink* trace_ = nullptr;
 BenchmarkCompileProgress state_;
 std::chrono::steady_clock::time_point activity_started_{};
 std::chrono::steady_clock::time_point pixel_started_{};
 std::uint64_t indexed_completed_ = 0, indexed_total_ = 0;
 ArtifactProgressTotals transfers_;
 std::unique_ptr<IndexingProgressTotals> indexing_;
 enum class LabelPlan { Waiting, Active, Complete };
 std::vector<LabelPlan> label_plans_;
 std::uint64_t label_completed_ = 0, label_active_ = 0;
 bool labels_admitted_ = false, indexing_known_ = false;
 std::mutex mutex_;
};
// One compile-owned instance. Each release contributes its full-import row
// count once across retained products and repairs; metadata never counts twice.
class IndexingProgressTotals final {
public:
 explicit IndexingProgressTotals(std::span<const std::uint64_t> totals);
 void update(std::size_t release, std::uint64_t completed, ProgressReporter& reporter);
 // The replaced importer must have returned or joined before withdrawal.
 void invalidate(std::size_t release, ProgressReporter& reporter);

private:
 struct Observation { std::uint64_t completed = 0, total = 0; };
 std::vector<Observation> releases_;
 std::uint64_t completed_ = 0, total_ = 0;
 std::mutex mutex_;
};
}  // namespace mmltk::backend::data::benchmark_internal
