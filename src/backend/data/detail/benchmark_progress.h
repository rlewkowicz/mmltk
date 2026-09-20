#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "benchmark_cache.h"
#include "benchmark_download.h"
namespace mmltk::backend::data::benchmark_internal {
class ProgressReporter {
   public:
    ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace);
    void phase(const DatasetCompilePhase phase, const std::uint64_t completed = 0U, const std::uint64_t total = 0U);
    void activity(std::string activity);
    void pixel_attempt(const std::uint64_t completed, const std::uint64_t total, const std::string_view split, const std::uint64_t split_total);
    void pixel_completed();
    void source_activity(const BenchmarkDatasetSource source, std::string activity);
    [[nodiscard]] bool transfer_observer_enabled() const noexcept;
    [[nodiscard]] bool pixel_observer_enabled() const noexcept;
    void projected(const std::uint64_t bytes);
    void rejected(const std::uint64_t dropped, const std::uint64_t quarantined);
    void source_transfer(BenchmarkDatasetSource source, const DownloadProgress& update, std::uint64_t completed, std::uint64_t total);
    void add_source_images(BenchmarkDatasetSource source, std::uint64_t count, std::uint64_t total, std::string activity = {});
    void rollback_source_images(BenchmarkDatasetSource source, std::uint64_t count, std::uint64_t total, std::string activity = {});
    void source_images(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total);
    void source_complete(const BenchmarkDatasetSource source, const bool cache_hit);

   private:
    [[nodiscard]] static std::string default_phase_activity(const DatasetCompilePhase phase);
    void set_activity_unlocked(std::string activity);
    static void add_progress(std::uint64_t& target, const std::uint64_t value, const char* context);
    BenchmarkSourceProgress& source_progress(const BenchmarkDatasetSource source);
    void update_source_phase_progress();
    void set_source_activity_unlocked(BenchmarkDatasetSource source, std::string activity);
    void select_acquisition_source(BenchmarkDatasetSource source);
    void trace_activity(std::optional<BenchmarkDatasetSource> source, std::string_view activity);
    void emit();
    BenchmarkProgressCallback callback_;
    const BenchmarkTraceSink* trace_ = nullptr;
    BenchmarkCompileProgress state_;
    std::chrono::steady_clock::time_point activity_started_{};
    std::string_view pixel_split_;
    std::uint64_t pixel_split_total_ = 0U;
    std::uint64_t pixel_completed_ = 0U;
    std::uint64_t pixel_completed_before_ = 0U;
    std::mutex mutex_;
};
}  // namespace mmltk::backend::data::benchmark_internal
