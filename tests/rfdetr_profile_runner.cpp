#include "profile_utils.h"
#include "execution_policy.h"
#include "mmltk/rfdetr/predict.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string compiled_path;
    std::string checkpoint_path;
    int repetitions = 1;
    int warmup_runs = 0;
    int device_id = 0;
    int workers = 0;
    int batch_size = 1;
    size_t limit_images = 0;
    std::string cpu_affinity;
};

struct EvaluateRun {
    std::uint64_t elapsed_ns = 0;
    size_t images = 0;
    double bbox_ap = 0.0;
    double mask_ap = 0.0;
};

std::string iteration_label_for_run(const char* run_label, int repetition) {
    std::array<char, 96> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s[%02d]", run_label, repetition);
    return buffer.data();
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--compiled-path") == 0 && index + 1 < argc) {
            options.compiled_path = argv[++index];
        } else if (std::strcmp(argv[index], "--checkpoint-path") == 0 && index + 1 < argc) {
            options.checkpoint_path = argv[++index];
        } else if (std::strcmp(argv[index], "--repetitions") == 0 && index + 1 < argc) {
            options.repetitions = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--warmup-runs") == 0 && index + 1 < argc) {
            options.warmup_runs = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--device-id") == 0 && index + 1 < argc) {
            options.device_id = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--workers") == 0 && index + 1 < argc) {
            options.workers = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--batch-size") == 0 && index + 1 < argc) {
            options.batch_size = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--cpu-affinity") == 0 && index + 1 < argc) {
            options.cpu_affinity = argv[++index];
        } else if (std::strcmp(argv[index], "--limit-images") == 0 && index + 1 < argc) {
            options.limit_images = static_cast<size_t>(std::strtoull(argv[++index], nullptr, 10));
        } else {
            std::fprintf(stderr,
                         "Usage: %s --compiled-path PATH --checkpoint-path PATH [--repetitions N] "
                         "[--warmup-runs N] [--device-id N] [--workers N] [--batch-size N] "
                         "[--cpu-affinity LIST] [--limit-images N]\n",
                         argv[0]);
            std::exit(1);
        }
    }

    if (options.compiled_path.empty() || options.checkpoint_path.empty()) {
        throw std::runtime_error("RF-DETR profile runner requires compiled and checkpoint paths");
    }
    if (options.repetitions <= 0 || options.warmup_runs < 0 || options.batch_size <= 0) {
        throw std::runtime_error("repetitions, warmup-runs, and batch-size must be positive/non-negative");
    }
    return options;
}

void ensure_file_exists(const char* label, const std::string& path) {
    if (!fs::exists(path)) {
        throw std::runtime_error(std::string("missing ") + label + ": " + path);
    }
}

double seconds_from_ns(std::uint64_t value) {
    return static_cast<double>(value) / 1.0e9;
}

std::uint64_t x10000_metric(double value) {
    return static_cast<std::uint64_t>(std::llround(value * 10000.0));
}

std::uint64_t img_per_sec_x100(std::uint64_t elapsed_ns, size_t images) {
    if (elapsed_ns == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(
        (static_cast<double>(images) * 100.0 * 1.0e9) / static_cast<double>(elapsed_ns)));
}

std::string metric_name(const char* suffix) {
    return std::string("benchmark.") + suffix;
}

void record_duration_metric(const char* suffix, std::uint64_t elapsed_ns) {
    mmltk::profile_record_duration_ns(metric_name(suffix).c_str(), elapsed_ns);
}

void record_value_metric(const char* suffix, std::uint64_t value) {
    mmltk::profile_add_value(metric_name(suffix).c_str(), value);
}

EvaluateRun run_checkpoint_backend(const Options& options) {
    mmltk::rfdetr::EvaluateOptions evaluate_options;
    evaluate_options.compiled_path = options.compiled_path;
    evaluate_options.weights_path = options.checkpoint_path;
    evaluate_options.batch_size = static_cast<size_t>(std::max(1, options.batch_size));
    evaluate_options.limit_images = options.limit_images;
    evaluate_options.device_id = options.device_id;
    evaluate_options.workers = options.workers;
    evaluate_options.cpu_affinity = options.cpu_affinity;

    const auto started = std::chrono::steady_clock::now();
    const mmltk::rfdetr::EvaluationRunResult result = mmltk::rfdetr::run_evaluation(evaluate_options);
    const std::uint64_t elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    mmltk::rfdetr::print_evaluation_summary(evaluate_options, result);

    return EvaluateRun{
        elapsed_ns,
        result.result.timing.images,
        result.result.summary.bbox.ap,
        result.result.summary.mask.ap,
    };
}

void record_evaluate_metrics(const EvaluateRun& run) {
    record_duration_metric("rfdetr.evaluate.checkpoint.total", run.elapsed_ns);
    record_value_metric("rfdetr.evaluate.checkpoint.images", run.images);
    record_value_metric("rfdetr.evaluate.checkpoint.img_per_sec_x100", img_per_sec_x100(run.elapsed_ns, run.images));
    record_value_metric("rfdetr.evaluate.checkpoint.bbox_ap_x10000", x10000_metric(run.bbox_ap));
    record_value_metric("rfdetr.evaluate.checkpoint.mask_ap_x10000", x10000_metric(run.mask_ap));
}

void print_iteration_line(const char* phase, int index, int total, const EvaluateRun& run) {
    std::printf("%s=rfdetr.evaluate.checkpoint %d/%d pt=%.3fs bbox=%.4f mask=%.4f\n",
                phase,
                index,
                total,
                seconds_from_ns(run.elapsed_ns),
                run.bbox_ap,
                run.mask_ap);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    MMLTK_PROFILE_PROCESS_LABEL("profile.rfdetr.evaluate");
    MMLTK_PROFILE_RUN_LABEL("rfdetr.evaluate.checkpoint");

    try {
        const mmltk::ExecutionPolicySnapshot execution_snapshot =
            mmltk::apply_process_execution_policy();
        mmltk::log_process_execution_policy(
            "mmltk_rfdetr_profile_runner",
            execution_snapshot,
            false,
            true);
        const Options options = parse_options(argc, argv);
        ensure_file_exists("compiled dataset", options.compiled_path);
        ensure_file_exists("checkpoint", options.checkpoint_path);

        std::vector<EvaluateRun> warmup_runs;
        warmup_runs.reserve(static_cast<size_t>(std::max(0, options.warmup_runs)));
        std::vector<EvaluateRun> repetition_runs;
        repetition_runs.reserve(static_cast<size_t>(std::max(1, options.repetitions)));

        for (int warmup = 0; warmup < options.warmup_runs; ++warmup) {
            MMLTK_PROFILE_RESET_ITERATION();
            warmup_runs.push_back(run_checkpoint_backend(options));
        }

        for (int repetition = 0; repetition < options.repetitions; ++repetition) {
            mmltk::profile_set_run_label("rfdetr.evaluate.checkpoint");
            mmltk::profile_reset_iteration();
            const EvaluateRun run = run_checkpoint_backend(options);
            record_evaluate_metrics(run);
            repetition_runs.push_back(run);
            mmltk::profile_capture_iteration(
                iteration_label_for_run("rfdetr.evaluate.checkpoint", repetition + 1).c_str());
        }

        for (int warmup = 0; warmup < options.warmup_runs; ++warmup) {
            print_iteration_line("warmup", warmup + 1, options.warmup_runs, warmup_runs[static_cast<size_t>(warmup)]);
        }

        for (int repetition = 0; repetition < options.repetitions; ++repetition) {
            print_iteration_line(
                "repetition",
                repetition + 1,
                options.repetitions,
                repetition_runs[static_cast<size_t>(repetition)]);
        }

        MMLTK_PROFILE_FLUSH();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mmltk_rfdetr_profile_runner error: %s\n", error.what());
        return 1;
    }
}
