#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "cli_option_table.h"
namespace fs = std::filesystem;
namespace mmltk::testsupport {
struct CommonProfileOptions {
    int repetitions = 1;
    int warmup_runs = 0;
    int device_id = 0;
    int workers = 0;
    int batch_size = 1;
    std::string cpu_affinity;
};
void add_common_profile_options(CliOptionTable& table, CommonProfileOptions& options);
[[nodiscard]] std::string iteration_label_for_run(const char* run_label, int repetition);
[[nodiscard]] double seconds_from_ns(std::uint64_t value);
[[nodiscard]] std::uint64_t x10000_metric(double value);
[[nodiscard]] std::uint64_t img_per_sec_x100(std::uint64_t elapsed_ns, std::size_t images);
[[nodiscard]] std::uint64_t elapsed_ns_since(const std::chrono::steady_clock::time_point& started);
[[nodiscard]] double seconds_since(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end);
[[nodiscard]] std::string benchmark_metric_name(const char* suffix);
[[nodiscard]] std::string benchmark_metric_name(const char* label, const char* suffix);
void record_duration_metric(const char* suffix, std::uint64_t elapsed_ns);
void record_duration_metric(const char* label, const char* suffix, const std::chrono::steady_clock::time_point& start,
                            const std::chrono::steady_clock::time_point& end);
void record_value_metric(const char* suffix, std::uint64_t value);
void record_value_metric(const char* label, const char* suffix, std::uint64_t value);
void record_optional_x10000_metric(const char* suffix, const std::optional<double>& value);
[[nodiscard]] std::string optional_metric_text(const std::optional<double>& value);
void ensure_file_exists(const char* label, const std::string& path);
void reset_profile_iteration();
void select_profile_run(const char* run_label);
void capture_profile_iteration(const std::string& label);
// Shared warmup/repetition orchestration for the profile runners: runs the warmup iterations,
// then the recorded repetitions (with per-iteration profile capture), then prints one line per run.
// `run_iteration(is_warmup, one_based_index)` produces the per-run result object.
template <typename RunIteration, typename RecordMetrics, typename PrintLine>
inline void run_profile_phases(const char* run_label, const int warmup_run_count, const int repetition_count, RunIteration&& run_iteration,
                               RecordMetrics&& record_metrics, PrintLine&& print_line) {
    using Run = std::decay_t<std::invoke_result_t<RunIteration&, bool, int>>;
    std::vector<Run> warmup_runs;
    warmup_runs.reserve(static_cast<size_t>(std::max(0, warmup_run_count)));
    std::vector<Run> repetition_runs;
    repetition_runs.reserve(static_cast<size_t>(std::max(1, repetition_count)));
    for (int warmup = 0; warmup < warmup_run_count; ++warmup) {
        reset_profile_iteration();
        warmup_runs.push_back(run_iteration(true, warmup + 1));
    }
    for (int repetition = 0; repetition < repetition_count; ++repetition) {
        select_profile_run(run_label);
        reset_profile_iteration();
        Run run = run_iteration(false, repetition + 1);
        record_metrics(run);
        repetition_runs.push_back(std::move(run));
        capture_profile_iteration(iteration_label_for_run(run_label, repetition + 1));
    }
    for (int warmup = 0; warmup < warmup_run_count; ++warmup) { print_line("warmup", warmup + 1, warmup_run_count, warmup_runs[static_cast<size_t>(warmup)]); }
    for (int repetition = 0; repetition < repetition_count; ++repetition) {
        print_line("repetition", repetition + 1, repetition_count, repetition_runs[static_cast<size_t>(repetition)]);
    }
}
}  // namespace mmltk::testsupport
