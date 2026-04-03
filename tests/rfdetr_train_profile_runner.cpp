#include "dataset_compiler.h"
#include "execution_policy.h"
#include "profile_utils.h"
#include "mmltk/rfdetr/train.h"
#include "test_fixture.h"

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

namespace {

using mmltk::DatasetCompiler;
using mmltk::CompilerConfig;
using mmltk::rfdetr::TrainOptions;
using mmltk::rfdetr::TrainRunResult;
using mmltk::testsupport::FixtureSpec;

struct Options {
    std::string test_dir = "/tmp/mmltk_rfdetr_train_profile";
    std::string weights_path;
    bool keep_artifacts = false;
    int width = 432;
    int height = 432;
    int num_images = 24;
    int batch_size = 6;
    int epochs = 1;
    int device_id = 0;
    int workers = 16;
    int lanes = 0;
    int prefetch_factor = 2;
    int compile_workers = -1;
    int repetitions = 1;
    int warmup_runs = 0;
    std::string cpu_affinity;
};

struct TrainRun {
    std::uint64_t elapsed_ns = 0;
    double train_loss = 0.0;
    double bbox_ap = 0.0;
    double mask_ap = 0.0;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--test-dir") == 0 && index + 1 < argc) {
            options.test_dir = argv[++index];
        } else if (std::strcmp(argv[index], "--weights-path") == 0 && index + 1 < argc) {
            options.weights_path = argv[++index];
        } else if (std::strcmp(argv[index], "--keep-artifacts") == 0) {
            options.keep_artifacts = true;
        } else if (std::strcmp(argv[index], "--width") == 0 && index + 1 < argc) {
            options.width = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--height") == 0 && index + 1 < argc) {
            options.height = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--num-images") == 0 && index + 1 < argc) {
            options.num_images = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--batch-size") == 0 && index + 1 < argc) {
            options.batch_size = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--epochs") == 0 && index + 1 < argc) {
            options.epochs = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--device-id") == 0 && index + 1 < argc) {
            options.device_id = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--workers") == 0 && index + 1 < argc) {
            options.workers = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--lanes") == 0 && index + 1 < argc) {
            options.lanes = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--prefetch-factor") == 0 && index + 1 < argc) {
            options.prefetch_factor = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--compile-workers") == 0 && index + 1 < argc) {
            options.compile_workers = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--repetitions") == 0 && index + 1 < argc) {
            options.repetitions = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--warmup-runs") == 0 && index + 1 < argc) {
            options.warmup_runs = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--cpu-affinity") == 0 && index + 1 < argc) {
            options.cpu_affinity = argv[++index];
        } else {
            std::fprintf(stderr,
                         "Usage: %s --weights-path PATH [--test-dir PATH] [--keep-artifacts] "
                         "[--width N] [--height N] [--num-images N] [--batch-size N] [--epochs N] "
                         "[--device-id N] [--workers N] [--lanes N] [--prefetch-factor N] [--compile-workers N] "
                         "[--repetitions N] [--warmup-runs N] [--cpu-affinity LIST]\n",
                         argv[0]);
            std::exit(1);
        }
    }

    if (options.weights_path.empty()) {
        throw std::runtime_error("RF-DETR train profile runner requires --weights-path");
    }
    if (options.width <= 0 || options.height <= 0 || options.num_images <= 0 || options.batch_size <= 0 ||
        options.epochs <= 0 || options.device_id < 0 || options.workers < 0 || options.lanes < 0 || options.prefetch_factor <= 0 ||
        options.repetitions <= 0 || options.warmup_runs < 0) {
        throw std::runtime_error("numeric options must be positive except lanes and warmup-runs, which may be zero");
    }
    if (options.num_images < options.batch_size) {
        throw std::runtime_error("num-images must be at least batch-size for RF-DETR train profile");
    }
    return options;
}

void ensure_file_exists(const char* label, const std::string& path) {
    if (!fs::exists(path)) {
        throw std::runtime_error(std::string("missing ") + label + ": " + path);
    }
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

std::string compiled_path_for(const FixtureSpec& spec) {
    return mmltk::testsupport::compiled_bin_path(spec);
}

void build_fixture(const Options& options, const FixtureSpec& fixture) {
    mmltk::testsupport::create_synthetic_dataset(fixture);

    CompilerConfig config;
    config.source_dir = mmltk::testsupport::dataset_dir(fixture);
    config.output_dir = mmltk::testsupport::compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = static_cast<uint32_t>(fixture.width);
    config.target_height = static_cast<uint32_t>(fixture.height);
    config.num_workers = options.compile_workers;
    DatasetCompiler::compile(config);
}

TrainRun run_train_iteration(const Options& options,
                             const std::string& compiled_path,
                             const fs::path& output_dir) {
    fs::remove_all(output_dir);

    TrainOptions train_options;
    train_options.train_compiled_path = compiled_path;
    train_options.val_compiled_path = compiled_path;
    train_options.output_dir = output_dir;
    train_options.weights_path = options.weights_path;
    train_options.batch_size = static_cast<size_t>(options.batch_size);
    train_options.epochs = options.epochs;
    train_options.device_id = options.device_id;
    train_options.workers = options.workers;
    train_options.lanes = options.lanes;
    train_options.prefetch_factor = options.prefetch_factor;
    train_options.cpu_affinity = options.cpu_affinity;
    train_options.print_freq = 1;
    train_options.progress_bar = false;
    train_options.amp = true;

    const auto started = std::chrono::steady_clock::now();
    const TrainRunResult result = mmltk::rfdetr::run_training(train_options);
    const auto elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());

    if (result.history.empty()) {
        throw std::runtime_error("RF-DETR train profile produced no epoch history");
    }
    const auto& last = result.history.back();
    return TrainRun{
        elapsed_ns,
        last.train_loss,
        last.val_summary.bbox.ap,
        last.val_summary.mask.ap,
    };
}

void record_train_metrics(const TrainRun& run, const Options& options) {
    const size_t profiled_train_images =
        static_cast<size_t>(options.num_images) * static_cast<size_t>(options.epochs);
    record_duration_metric("rfdetr.train.total", run.elapsed_ns);
    record_value_metric("rfdetr.train.images", profiled_train_images);
    record_value_metric("rfdetr.train.img_per_sec_x100", img_per_sec_x100(run.elapsed_ns, profiled_train_images));
    record_value_metric("rfdetr.train.loss_x10000", x10000_metric(run.train_loss));
    record_value_metric("rfdetr.train.val_bbox_ap_x10000", x10000_metric(run.bbox_ap));
    record_value_metric("rfdetr.train.val_mask_ap_x10000", x10000_metric(run.mask_ap));
}

std::string iteration_label_for_run(int repetition) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "rfdetr.train[%02d]", repetition);
    return buffer.data();
}

void print_iteration_line(const char* phase, int index, int total, const TrainRun& run) {
    std::printf("%s=rfdetr.train %d/%d total=%.3fs train_loss=%.4f bbox=%.4f mask=%.4f\n",
                phase,
                index,
                total,
                static_cast<double>(run.elapsed_ns) / 1.0e9,
                run.train_loss,
                run.bbox_ap,
                run.mask_ap);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    MMLTK_PROFILE_PROCESS_LABEL("profile.rfdetr.train");
    MMLTK_PROFILE_RUN_LABEL("rfdetr.train");

    try {
        const mmltk::ExecutionPolicySnapshot execution_snapshot =
            mmltk::apply_process_execution_policy();
        mmltk::log_process_execution_policy(
            "mmltk_rfdetr_train_profile_runner",
            execution_snapshot,
            false,
            true);
        const Options options = parse_options(argc, argv);
        ensure_file_exists("weights checkpoint", options.weights_path);

        const FixtureSpec fixture{
            options.test_dir,
            "train",
            options.width,
            options.height,
            options.num_images,
        };
        build_fixture(options, fixture);
        const std::string compiled_path = compiled_path_for(fixture);

        std::vector<TrainRun> warmup_runs;
        warmup_runs.reserve(static_cast<size_t>(std::max(0, options.warmup_runs)));
        std::vector<TrainRun> repetition_runs;
        repetition_runs.reserve(static_cast<size_t>(std::max(1, options.repetitions)));

        for (int warmup = 0; warmup < options.warmup_runs; ++warmup) {
            mmltk::profile_reset_iteration();
            warmup_runs.push_back(run_train_iteration(
                options, compiled_path, fs::path(fixture.root_dir) / ("run-warmup-" + std::to_string(warmup + 1))));
        }

        for (int repetition = 0; repetition < options.repetitions; ++repetition) {
            mmltk::profile_set_run_label("rfdetr.train");
            mmltk::profile_reset_iteration();
            const TrainRun run = run_train_iteration(
                options, compiled_path, fs::path(fixture.root_dir) / ("run-" + std::to_string(repetition + 1)));
            record_train_metrics(run, options);
            repetition_runs.push_back(run);
            mmltk::profile_capture_iteration(iteration_label_for_run(repetition + 1).c_str());
        }

        for (int warmup = 0; warmup < options.warmup_runs; ++warmup) {
            print_iteration_line("warmup", warmup + 1, options.warmup_runs, warmup_runs[static_cast<size_t>(warmup)]);
        }
        for (int repetition = 0; repetition < options.repetitions; ++repetition) {
            print_iteration_line(
                "repetition", repetition + 1, options.repetitions, repetition_runs[static_cast<size_t>(repetition)]);
        }

        mmltk::profile_flush();
        if (!options.keep_artifacts) {
            fs::remove_all(fixture.root_dir);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mmltk_rfdetr_train_profile_runner error: %s\n", error.what());
        return 1;
    }
}
