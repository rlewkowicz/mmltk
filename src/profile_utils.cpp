#include "profile_utils.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mmltk {

#if MMLTK_ENABLE_PROFILING

namespace {

using Clock = std::chrono::steady_clock;

struct Metric {
    std::uint64_t calls = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t min_ns = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ns = 0;

    bool has_value = false;
    std::uint64_t value_count = 0;
    std::uint64_t value_sum = 0;
    std::uint64_t value_min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value_max = 0;
    std::uint64_t value_last = 0;
};

struct RunSnapshot {
    std::string run_label;
    std::string iteration_label;
    std::uint64_t total_ns = 0;
    std::vector<std::pair<std::string, Metric>> items;
};

struct AggregateMetric {
    std::uint64_t runs = 0;

    bool has_duration = false;
    std::uint64_t calls_total = 0;
    std::uint64_t total_ns_sum = 0;
    std::uint64_t total_ns_min_per_run = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total_ns_max_per_run = 0;

    bool has_value = false;
    std::uint64_t value_runs = 0;
    std::uint64_t value_count_total = 0;
    std::uint64_t value_sum_total = 0;
    std::uint64_t value_last_sum = 0;
    std::uint64_t value_min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value_max = 0;
};

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

bool has_metric_data(const Metric& metric) {
    return metric.calls > 0 || metric.has_value;
}

std::vector<std::pair<std::string, Metric>> sorted_metric_items(
    const std::unordered_map<std::string, Metric>& metrics) {
    std::vector<std::pair<std::string, Metric>> items;
    items.reserve(metrics.size());
    for (const auto& entry : metrics) {
        if (has_metric_data(entry.second)) {
            items.emplace_back(entry.first, entry.second);
        }
    }
    std::ranges::sort(items, [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    return items;
}

std::string default_log_path() {
    ::mkdir("profiles", 0755);
    std::array<char, 64> path{};
    std::snprintf(path.data(), path.size(), "profiles/%lld.log", static_cast<long long>(std::time(nullptr)));
    return path.data();
}

void write_metric_line(FILE* out, const std::string& name, const Metric& metric) {
    std::fprintf(out, "%s", name.c_str());
    if (metric.calls > 0) {
        const double total_ms = static_cast<double>(metric.total_ns) / 1.0e6;
        const double avg_ms = total_ms / static_cast<double>(metric.calls);
        const double min_ms = static_cast<double>(metric.min_ns) / 1.0e6;
        const double max_ms = static_cast<double>(metric.max_ns) / 1.0e6;
        std::fprintf(out, " calls=%llu total_ms=%.3f avg_ms=%.3f min_ms=%.3f max_ms=%.3f",
                     static_cast<unsigned long long>(metric.calls), total_ms, avg_ms, min_ms, max_ms);
    }
    if (metric.has_value) {
        std::fprintf(
            out, " value_count=%llu value_sum=%llu value_last=%llu value_min=%llu value_max=%llu",
            static_cast<unsigned long long>(metric.value_count), static_cast<unsigned long long>(metric.value_sum),
            static_cast<unsigned long long>(metric.value_last), static_cast<unsigned long long>(metric.value_min),
            static_cast<unsigned long long>(metric.value_max));
    }
    std::fputc('\n', out);
}

void write_run_block(FILE* out, const RunSnapshot& run) {
    std::fprintf(out, "=== mmltk profile build=%s pid=%d run=%s iteration=%s total_ms=%.3f ===\n", MMLTK_BUILD_CONFIG,
                 static_cast<int>(::getpid()), run.run_label.c_str(), run.iteration_label.c_str(),
                 static_cast<double>(run.total_ns) / 1.0e6);
    for (const auto& entry : run.items) {
        write_metric_line(out, entry.first, entry.second);
    }
    std::fputc('\n', out);
}

void accumulate_aggregate_metric(const Metric& metric, AggregateMetric& aggregate) {
    ++aggregate.runs;
    if (metric.calls > 0) {
        aggregate.has_duration = true;
        aggregate.calls_total += metric.calls;
        aggregate.total_ns_sum += metric.total_ns;
        aggregate.total_ns_min_per_run = std::min(aggregate.total_ns_min_per_run, metric.total_ns);
        aggregate.total_ns_max_per_run = std::max(aggregate.total_ns_max_per_run, metric.total_ns);
    }
    if (metric.has_value) {
        aggregate.has_value = true;
        ++aggregate.value_runs;
        aggregate.value_count_total += metric.value_count;
        aggregate.value_sum_total += metric.value_sum;
        aggregate.value_last_sum += metric.value_last;
        aggregate.value_min = std::min(aggregate.value_min, metric.value_min);
        aggregate.value_max = std::max(aggregate.value_max, metric.value_max);
    }
}

void write_aggregate_block(FILE* out, const std::string& run_label, const std::vector<RunSnapshot>& runs,
                           std::uint64_t process_total_ns) {
    std::unordered_map<std::string, AggregateMetric> aggregate_metrics;
    for (const RunSnapshot& run : runs) {
        for (const auto& entry : run.items) {
            accumulate_aggregate_metric(entry.second, aggregate_metrics[entry.first]);
        }
    }

    std::vector<std::pair<std::string, AggregateMetric>> items;
    items.reserve(aggregate_metrics.size());
    for (const auto& entry : aggregate_metrics) {
        items.emplace_back(entry.first, entry.second);
    }
    std::ranges::sort(items, [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::fprintf(out, "=== mmltk profile aggregate build=%s pid=%d run=%s runs=%zu process_total_ms=%.3f ===\n",
                 MMLTK_BUILD_CONFIG, static_cast<int>(::getpid()), run_label.c_str(), runs.size(),
                 static_cast<double>(process_total_ns) / 1.0e6);
    for (const auto& entry : items) {
        const AggregateMetric& metric = entry.second;
        std::fprintf(out, "%s runs=%llu", entry.first.c_str(), static_cast<unsigned long long>(metric.runs));
        if (metric.has_duration) {
            const double total_ms_avg_per_run =
                static_cast<double>(metric.total_ns_sum) / (1.0e6 * static_cast<double>(metric.runs));
            const double avg_ms_per_call =
                static_cast<double>(metric.total_ns_sum) / (1.0e6 * static_cast<double>(metric.calls_total));
            const double total_ms_min_per_run = static_cast<double>(metric.total_ns_min_per_run) / 1.0e6;
            const double total_ms_max_per_run = static_cast<double>(metric.total_ns_max_per_run) / 1.0e6;
            std::fprintf(out,
                         " calls_total=%llu calls_avg_per_run=%.3f total_ms_avg_per_run=%.3f avg_ms_per_call=%.3f "
                         "total_ms_min_per_run=%.3f total_ms_max_per_run=%.3f",
                         static_cast<unsigned long long>(metric.calls_total),
                         static_cast<double>(metric.calls_total) / static_cast<double>(metric.runs),
                         total_ms_avg_per_run, avg_ms_per_call, total_ms_min_per_run, total_ms_max_per_run);
        }
        if (metric.has_value) {
            std::fprintf(
                out, " value_count_total=%llu value_avg=%.3f value_last_avg=%.3f value_min=%llu value_max=%llu",
                static_cast<unsigned long long>(metric.value_count_total),
                static_cast<double>(metric.value_sum_total) / static_cast<double>(metric.value_count_total),
                static_cast<double>(metric.value_last_sum) / static_cast<double>(metric.value_runs),
                static_cast<unsigned long long>(metric.value_min), static_cast<unsigned long long>(metric.value_max));
        }
        std::fputc('\n', out);
    }
    std::fputc('\n', out);
}

class ProfileRegistry {
   public:
    static ProfileRegistry& instance() {
        static ProfileRegistry registry;
        return registry;
    }

    void record_duration(const char* name, std::uint64_t elapsed_ns) {
        std::lock_guard<std::mutex> lock(mtx_);
        Metric& metric = metrics_[name];
        ++metric.calls;
        metric.total_ns += elapsed_ns;
        metric.min_ns = std::min(metric.min_ns, elapsed_ns);
        metric.max_ns = std::max(metric.max_ns, elapsed_ns);
    }

    void add_value(const char* name, std::uint64_t delta) {
        std::lock_guard<std::mutex> lock(mtx_);
        Metric& metric = metrics_[name];
        metric.has_value = true;
        ++metric.value_count;
        metric.value_sum += delta;
        metric.value_min = std::min(metric.value_min, delta);
        metric.value_max = std::max(metric.value_max, delta);
        metric.value_last = delta;
    }

    void set_value(const char* name, std::uint64_t value) {
        add_value(name, value);
    }

    void set_process_label(const char* label) {
        std::lock_guard<std::mutex> lock(mtx_);
        process_label_ = (label && label[0]) ? label : std::string();
    }

    void reset_iteration() {
        std::lock_guard<std::mutex> lock(mtx_);
        metrics_.clear();
        iteration_start_ns_ = now_ns();
    }

    void capture_iteration(const char* label) {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::uint64_t captured_ns = now_ns();
        const auto items = sorted_metric_items(metrics_);
        if (!items.empty()) {
            runs_.push_back(RunSnapshot{
                run_label_,
                (label && label[0]) ? label : run_label_,
                captured_ns - iteration_start_ns_,
                items,
            });
        }
        metrics_.clear();
        iteration_start_ns_ = captured_ns;
    }

    void set_run_label(const char* label) {
        std::lock_guard<std::mutex> lock(mtx_);
        run_label_ = (label && label[0]) ? label : "unnamed";
    }

    void flush() {
        bool expected = false;
        if (!flush_started_.compare_exchange_strong(expected, true)) {
            return;
        }

        const char* env_path = std::getenv("MMLTK_PROFILE_LOG");
        std::string fallback_path;
        const char* log_path = env_path;
        if (!log_path || !log_path[0]) {
            fallback_path = default_log_path();
            log_path = fallback_path.c_str();
        }
        const char* append_env = std::getenv("MMLTK_PROFILE_APPEND");
        const bool append = append_env && std::strcmp(append_env, "0") != 0;

        FILE* out = std::fopen(log_path, append ? "a" : "w");
        if (!out) {
            return;
        }

        std::vector<RunSnapshot> runs;
        std::string process_label;
        bool had_captured_runs = false;
        const std::uint64_t captured_ns = now_ns();
        const std::uint64_t process_total_ns = captured_ns - process_start_ns_;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            runs = runs_;
            had_captured_runs = !runs_.empty();
            const auto current_items = sorted_metric_items(metrics_);
            if (!current_items.empty()) {
                runs.push_back(RunSnapshot{
                    run_label_,
                    run_label_,
                    captured_ns - iteration_start_ns_,
                    current_items,
                });
            }
            process_label = process_label_;
        }

        for (const RunSnapshot& run : runs) {
            write_run_block(out, run);
        }
        if (had_captured_runs) {
            std::unordered_map<std::string, std::vector<RunSnapshot>> grouped_runs;
            grouped_runs.reserve(runs.size());
            for (const auto& run : runs) {
                grouped_runs[run.run_label].push_back(run);
            }

            std::vector<std::string> group_labels;
            group_labels.reserve(grouped_runs.size());
            for (const auto& entry : grouped_runs) {
                group_labels.push_back(entry.first);
            }
            std::ranges::sort(group_labels);
            for (const auto& label : group_labels) {
                write_aggregate_block(out, label, grouped_runs.at(label), process_total_ns);
            }

            const std::string overall_label = process_label.empty() ? "overall" : process_label + ".overall";
            write_aggregate_block(out, overall_label, runs, process_total_ns);
        }
        std::fclose(out);
    }

   private:
    ProfileRegistry() {
        std::atexit(&ProfileRegistry::flush_atexit);
    }

    static void flush_atexit() {
        ProfileRegistry::instance().flush();
    }

    std::mutex mtx_;
    std::unordered_map<std::string, Metric> metrics_;
    std::vector<RunSnapshot> runs_;
    std::string process_label_;
    std::string run_label_ = "unnamed";
    std::atomic<bool> flush_started_{false};
    const std::uint64_t process_start_ns_ = now_ns();
    std::uint64_t iteration_start_ns_ = process_start_ns_;
};

}  // namespace

ScopedProfile::ScopedProfile(const char* name) : name_(name), start_ns_(now_ns()) {}

ScopedProfile::~ScopedProfile() {
    ProfileRegistry::instance().record_duration(name_, now_ns() - start_ns_);
}

void profile_add_value(const char* name, std::uint64_t delta) {
    ProfileRegistry::instance().add_value(name, delta);
}

void profile_set_value(const char* name, std::uint64_t value) {
    ProfileRegistry::instance().set_value(name, value);
}

void profile_record_duration_ns(const char* name, std::uint64_t elapsed_ns) {
    ProfileRegistry::instance().record_duration(name, elapsed_ns);
}

void profile_set_process_label(const char* label) {
    ProfileRegistry::instance().set_process_label(label);
}

void profile_set_run_label(const char* label) {
    ProfileRegistry::instance().set_run_label(label);
}

void profile_reset_iteration() {
    ProfileRegistry::instance().reset_iteration();
}

void profile_capture_iteration(const char* label) {
    ProfileRegistry::instance().capture_iteration(label);
}

void profile_flush() {
    ProfileRegistry::instance().flush();
}

#endif

}  // namespace mmltk
