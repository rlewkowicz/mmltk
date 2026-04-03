#include "cpu_affinity.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "dataset/compile/dataset_compiler_internal.h"
#include "spdmon/spdmon.hpp"
#include "test_fixture.h"

#include <algorithm>
#include "support/catch2_compat.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace mmltk;
using namespace mmltk::testsupport;

namespace fs = std::filesystem;

namespace {

using ProgressClock = spdmon::ProgressBar::clock_t;
using ProgressTimePoint = ProgressClock::time_point;

ProgressTimePoint g_progress_now = ProgressTimePoint{};

ProgressTimePoint test_progress_now() {
    return g_progress_now;
}

void set_progress_now(std::chrono::milliseconds elapsed) {
    g_progress_now = ProgressTimePoint{elapsed};
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
            had_existing_ = true;
            previous_value_ = existing;
        }
        if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error(std::string("setenv failed for ") + name_ + ": " + std::strerror(errno));
        }
    }

    ~ScopedEnvVar() {
        if (had_existing_) {
            (void)::setenv(name_.c_str(), previous_value_.c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::string previous_value_;
    bool had_existing_ = false;
};

class ScopedStderrCapture {
public:
    ScopedStderrCapture() {
        std::fflush(stderr);
        if (::pipe(pipe_fds_.data()) != 0) {
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }
        saved_stderr_fd_ = ::dup(STDERR_FILENO);
        if (saved_stderr_fd_ < 0) {
            cleanup_pipe();
            throw std::runtime_error(std::string("dup failed: ") + std::strerror(errno));
        }
        if (::dup2(pipe_fds_[1], STDERR_FILENO) < 0) {
            cleanup_pipe();
            cleanup_saved_fd();
            throw std::runtime_error(std::string("dup2 failed: ") + std::strerror(errno));
        }
        ::close(pipe_fds_[1]);
        pipe_fds_[1] = -1;
    }

    ~ScopedStderrCapture() {
        if (!finished_) {
            (void)finish();
        }
    }

    ScopedStderrCapture(const ScopedStderrCapture&) = delete;
    ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

    std::string finish() {
        if (finished_) {
            return captured_output_;
        }
        std::fflush(stderr);
        restore_stderr();
        captured_output_ = read_all();
        cleanup_pipe();
        finished_ = true;
        return captured_output_;
    }

private:
    std::array<int, 2> pipe_fds_{-1, -1};
    int saved_stderr_fd_ = -1;
    bool finished_ = false;
    std::string captured_output_;

    void cleanup_pipe() {
        if (pipe_fds_[0] >= 0) {
            ::close(pipe_fds_[0]);
            pipe_fds_[0] = -1;
        }
        if (pipe_fds_[1] >= 0) {
            ::close(pipe_fds_[1]);
            pipe_fds_[1] = -1;
        }
    }

    void cleanup_saved_fd() {
        if (saved_stderr_fd_ >= 0) {
            ::close(saved_stderr_fd_);
            saved_stderr_fd_ = -1;
        }
    }

    void restore_stderr() {
        if (saved_stderr_fd_ >= 0) {
            (void)::dup2(saved_stderr_fd_, STDERR_FILENO);
            cleanup_saved_fd();
        }
    }

    [[nodiscard]] std::string read_all() const {
        std::string output;
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t bytes_read = ::read(pipe_fds_[0], buffer.data(), buffer.size());
            if (bytes_read > 0) {
                output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
                continue;
            }
            if (bytes_read == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        return output;
    }
};

std::vector<std::string> normalize_terminal_output(const std::string& output) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t index = 0; index < output.size();) {
        if (output[index] == '\r') {
            current.clear();
            ++index;
            continue;
        }
        if (output.compare(index, 4, "\033[2K") == 0) {
            current.clear();
            index += 4;
            continue;
        }
        if (output[index] == '\n') {
            lines.push_back(current);
            current.clear();
            ++index;
            continue;
        }
        current.push_back(output[index++]);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

std::size_t count_substring(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t search_from = 0;
    while (true) {
        const std::size_t found = haystack.find(needle, search_from);
        if (found == std::string::npos) {
            break;
        }
        ++count;
        search_from = found + needle.size();
    }
    return count;
}

std::string make_unique_root_dir(const std::string& prefix) {
    std::string pattern = (fs::temp_directory_path() / (prefix + "_XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error("mkdtemp failed to create a temporary directory");
    }
    return {created};
}

void expect_compile_failure(const FixtureSpec& fixture,
                            const std::function<void(const fs::path&)>& mutate,
                            const std::string& expected_error,
                            int target_width = -1,
                            int target_height = -1) {
    create_synthetic_dataset(fixture);
    const fs::path annotation_path = fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl";
    mutate(annotation_path);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = static_cast<uint32_t>(target_width > 0 ? target_width : fixture.width);
    config.target_height = static_cast<uint32_t>(target_height > 0 ? target_height : fixture.height);
    config.num_workers = 2;

    bool threw = false;
    try {
        DatasetCompiler::compile(config);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find(expected_error) != std::string::npos);
    }
    assert(threw);
}

void overwrite_annotation(const fs::path& annotation_path, const std::string& record) {
    std::ofstream file(annotation_path, std::ios::trunc);
    assert(file.is_open());
    file << record << "\n";
}

void test_vanished_masks_are_dropped_and_reported() {
    const FixtureSpec fixture{
        make_unique_root_dir("mmltk_compile_drop_vanished_mask"),
        "train",
        16,
        16,
        20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,1,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:1","image_size_wh":[16,16]})");

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = 8;
    config.target_height = 8;
    config.num_workers = 2;

    std::vector<size_t> observed_drops;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_drops.push_back(progress.dropped_annotations);
    });

    assert(!observed_drops.empty());
    assert(observed_drops.back() == 1);
    assert(std::any_of(observed_drops.begin(),
                       observed_drops.end(),
                       [](size_t dropped) { return dropped == 1; }));

    DatasetLoader::Config loader_config;
    loader_config.compiled_path = compiled_bin_path(fixture);
    loader_config.batch_size = 1;
    loader_config.shuffle = false;
    DatasetLoader loader(loader_config);
    assert(loader.num_label_instances() == 9);
    assert(loader.label_index()[10].num_instances == 0);
}

void test_partial_mask_vanish_keeps_instance() {
    const FixtureSpec fixture{
        make_unique_root_dir("mmltk_compile_keep_partial_mask"),
        "train",
        16,
        16,
        20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,3,3],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:2 16:2 34:1","image_size_wh":[16,16]})");

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = 8;
    config.target_height = 8;
    config.num_workers = 2;

    std::vector<size_t> observed_drops;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_drops.push_back(progress.dropped_annotations);
    });

    assert(!observed_drops.empty());
    assert(observed_drops.back() == 0);

    DatasetLoader::Config loader_config;
    loader_config.compiled_path = compiled_bin_path(fixture);
    loader_config.batch_size = 1;
    loader_config.shuffle = false;
    DatasetLoader loader(loader_config);
    assert(loader.num_label_instances() == 10);
    const LabelIndexEntry& entry = loader.label_index()[10];
    assert(entry.num_instances == 1);
    const PackedInstance& instance = loader.label_data()[entry.label_begin];
    assert(instance.mask_rle_pairs > 0);
}

void test_compile_default_workers_use_full_cpuset() {
    const std::vector<int> allowed = allowed_cpu_set();
    assert(!allowed.empty());
    assert(compiler_internal::resolve_num_workers(0) == static_cast<int>(allowed.size()));
    assert(compiler_internal::resolve_num_workers(-1) == static_cast<int>(allowed.size()));
}

void test_invalid_annotations_fail_loud() {
    expect_compile_failure(
        FixtureSpec{
            make_unique_root_dir("mmltk_compile_invalid_json"),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            assert(file.is_open());
            file << "{invalid json}\n";
        },
        "invalid JSON annotation record");

    expect_compile_failure(
        FixtureSpec{
            make_unique_root_dir("mmltk_compile_unknown_class"),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            assert(file.is_open());
            file << R"({"class":"unknown","bbox_xyxy":[10,10,20,20],"mask_rle_encoding":"row_major_start_length","mask_rle":"660:10","image_size_wh":[65,65]})"
                 << "\n";
        },
        "is not declared in categories.json");

}

void test_format_compile_postfix_uses_expected_spinner_and_metrics() {
    CompileProgress progress{};
    progress.phase = CompileProgressPhase::kPixels;
    progress.active = 2;
    progress.dropped_annotations = 3;

    assert(spdmon::format_compile_postfix(progress, 0) == "pixels 2 active drop=3 |");
    assert(spdmon::format_compile_postfix(progress, 1) == "pixels 2 active drop=3 /");
    assert(spdmon::format_compile_postfix(progress, 2) == "pixels 2 active drop=3 -");
    assert(spdmon::format_compile_postfix(progress, 3) == "pixels 2 active drop=3 \\");
}

void test_progress_bar_uses_non_tty_fallback_width() {
    ScopedEnvVar columns("COLUMNS", "40");
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});

    {
        spdmon::ProgressBar bar("compile-progress-fallback-width-check", 10, "img", &test_progress_now);
        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1);
    }

    const std::vector<std::string> lines = normalize_terminal_output(capture.finish());
    assert(lines.size() == 1U);
    const std::string& visible_line = lines.back();
    assert(visible_line.find("compile-progress-fallback-width-check") == std::string::npos);
    assert(visible_line.size() <= 40U);
}

void test_progress_bar_throttles_redraws_until_interval_expires() {
    ScopedEnvVar columns("COLUMNS", "120");
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});

    {
        spdmon::ProgressBar bar("compile", 10, "img", &test_progress_now);
        bar.set_min_render_interval(std::chrono::seconds{10});

        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1);

        set_progress_now(std::chrono::milliseconds{500});
        bar.add(1);
    }

    const std::string output = capture.finish();
    assert(count_substring(output, "\r\033[2K") == 1U);
}

void test_progress_bar_log_preserves_lines() {
    ScopedEnvVar columns("COLUMNS", "120");
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});

    {
        spdmon::ProgressBar bar("compile", 3, "img", &test_progress_now);
        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1);
        spdmon::ProgressBar::log("worker started");
        set_progress_now(std::chrono::milliseconds{500});
        bar.close();
    }

    const std::vector<std::string> lines = normalize_terminal_output(capture.finish());
    assert(lines.size() == 2U);
    assert(lines.front() == "worker started");
    assert(lines.back().find("compile") != std::string::npos);
}

} // namespace

void test_compile_progress_reports_monotonic_updates() {
    const FixtureSpec fixture{
        make_unique_root_dir("mmltk_compile_progress"),
        "train",
        257,
        193,
        96,
    };
    create_synthetic_dataset(fixture);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = 97;
    config.target_height = 73;
    config.num_workers = 4;

    const std::thread::id main_thread_id = std::this_thread::get_id();
    std::vector<size_t> observed_done;
    std::vector<size_t> observed_totals;
    std::vector<CompileProgressPhase> observed_phases;
    std::vector<size_t> observed_active;
    std::vector<std::thread::id> callback_threads;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_done.push_back(progress.done);
        observed_totals.push_back(progress.total);
        observed_phases.push_back(progress.phase);
        observed_active.push_back(progress.active);
        callback_threads.push_back(std::this_thread::get_id());
    });

    const size_t expected_total = static_cast<size_t>(fixture.num_images) * 2 + 1;
    assert(!observed_done.empty());
    assert(observed_done.front() == 0);
    for (size_t index = 1; index < observed_done.size(); ++index) {
        assert(observed_done[index] >= observed_done[index - 1]);
    }
    assert(std::all_of(observed_totals.begin(),
                       observed_totals.end(),
                       [&](size_t total) { return total == expected_total; }));
    assert(observed_done.back() == expected_total);
    // With concurrent labels+pixels, we should see pixel phase reported
    // at some point when done >= num_images (pixels trailing)
    const bool saw_pixel_phase =
        std::any_of(observed_phases.begin(), observed_phases.end(),
                    [](CompileProgressPhase phase) { return phase == CompileProgressPhase::kPixels; });
    assert(saw_pixel_phase);
    const bool saw_active_pixel_worker =
        std::any_of(observed_active.begin(), observed_active.end(), [](size_t active) { return active > 0; });
    assert(saw_active_pixel_worker);

    assert(!callback_threads.empty());
    assert(std::all_of(callback_threads.begin(),
                       callback_threads.end(),
                       [&](const std::thread::id& id) { return id == callback_threads.front(); }));
    assert(callback_threads.front() != main_thread_id);

    test_compile_default_workers_use_full_cpuset();
    test_vanished_masks_are_dropped_and_reported();
    test_partial_mask_vanish_keeps_instance();
    test_invalid_annotations_fail_loud();
}

MMLTK_REGISTER_TEST_CASE("[core][compile_progress]", test_compile_progress_reports_monotonic_updates);
MMLTK_REGISTER_TEST_CASE("[core][compile_progress]", test_format_compile_postfix_uses_expected_spinner_and_metrics);
MMLTK_REGISTER_TEST_CASE("[core][compile_progress]", test_progress_bar_uses_non_tty_fallback_width);
MMLTK_REGISTER_TEST_CASE("[core][compile_progress]", test_progress_bar_throttles_redraws_until_interval_expires);
MMLTK_REGISTER_TEST_CASE("[core][compile_progress]", test_progress_bar_log_preserves_lines);
