#include "src/test_support/profile_runner_common.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::testsupport {
void add_common_profile_options(CliOptionTable& table, CommonProfileOptions& options) {
    table.add_integer("--repetitions", options.repetitions);
    table.add_integer("--warmup-runs", options.warmup_runs);
    table.add_integer("--device-id", options.device_id);
    table.add_integer("--workers", options.workers);
    table.add_integer("--batch-size", options.batch_size);
    table.add_string("--cpu-affinity", options.cpu_affinity);
}
std::string iteration_label_for_run(const char* const run_label, const int repetition) {
    std::array<char, 96> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s[%02d]", run_label, repetition);
    return buffer.data();
}
double seconds_from_ns(const std::uint64_t value) { return static_cast<double>(value) / 1.0e9; }
std::uint64_t x10000_metric(const double value) { return static_cast<std::uint64_t>(std::llround(value * 10000.0)); }
std::uint64_t img_per_sec_x100(const std::uint64_t elapsed_ns, const std::size_t images) {
    if (elapsed_ns == 0U) return 0U;
    return static_cast<std::uint64_t>(std::llround((static_cast<double>(images) * 100.0 * 1.0e9) / static_cast<double>(elapsed_ns)));
}
std::uint64_t elapsed_ns_since(const std::chrono::steady_clock::time_point& started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}
double seconds_since(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double>(end - start).count();
}
std::string benchmark_metric_name(const char* const suffix) { return std::string{"benchmark."} + suffix; }
std::string benchmark_metric_name(const char* const label, const char* const suffix) { return std::string{"benchmark."} + label + "." + suffix; }
void record_duration_metric(const char* const suffix, const std::uint64_t elapsed_ns) {
    mmltk::common::logging::profile_record_duration_ns(benchmark_metric_name(suffix).c_str(), elapsed_ns);
}
void record_duration_metric(const char* const label, const char* const suffix, const std::chrono::steady_clock::time_point& start,
                            const std::chrono::steady_clock::time_point& end) {
    mmltk::common::logging::profile_record_duration_ns(benchmark_metric_name(label, suffix).c_str(),
                                                       static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
}
void record_value_metric(const char* const suffix, const std::uint64_t value) {
    mmltk::common::logging::profile_add_value(benchmark_metric_name(suffix).c_str(), value);
}
void record_value_metric(const char* const label, const char* const suffix, const std::uint64_t value) {
    mmltk::common::logging::profile_add_value(benchmark_metric_name(label, suffix).c_str(), value);
}
void record_optional_x10000_metric(const char* const suffix, const std::optional<double>& value) {
    if (value.has_value()) record_value_metric(suffix, x10000_metric(*value));
}
std::string optional_metric_text(const std::optional<double>& value) {
    if (!value.has_value()) return "null";
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.4f", *value);
    return buffer.data();
}
void ensure_file_exists(const char* const label, const std::string& path) {
    if (!std::filesystem::exists(path)) throw std::runtime_error(std::string{"missing "} + label + ": " + path);
}
void reset_profile_iteration() { mmltk::common::logging::profile_reset_iteration(); }
void select_profile_run(const char* const run_label) { mmltk::common::logging::profile_set_run_label(run_label); }
void capture_profile_iteration(const std::string& label) { mmltk::common::logging::profile_capture_iteration(label.c_str()); }
}  // namespace mmltk::testsupport
