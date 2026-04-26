#pragma once

#include "profile_utils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

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

inline std::string iteration_label_for_run(const char* run_label, int repetition) {
    std::array<char, 96> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s[%02d]", run_label, repetition);
    return buffer.data();
}

inline double seconds_from_ns(std::uint64_t value) {
    return static_cast<double>(value) / 1.0e9;
}

inline std::uint64_t x10000_metric(double value) {
    return static_cast<std::uint64_t>(std::llround(value * 10000.0));
}

inline std::uint64_t img_per_sec_x100(std::uint64_t elapsed_ns, size_t images) {
    if (elapsed_ns == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::llround((static_cast<double>(images) * 100.0 * 1.0e9) / static_cast<double>(elapsed_ns)));
}

inline std::string benchmark_metric_name(const char* suffix) {
    return std::string("benchmark.") + suffix;
}

inline void record_duration_metric(const char* suffix, std::uint64_t elapsed_ns) {
    mmltk::profile_record_duration_ns(benchmark_metric_name(suffix).c_str(), elapsed_ns);
}

inline void record_value_metric(const char* suffix, std::uint64_t value) {
    mmltk::profile_add_value(benchmark_metric_name(suffix).c_str(), value);
}

inline void ensure_file_exists(const char* label, const std::string& path) {
    if (!fs::exists(path)) {
        throw std::runtime_error(std::string("missing ") + label + ": " + path);
    }
}

}  // namespace mmltk::testsupport
