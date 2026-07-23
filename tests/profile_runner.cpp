#include "cuda_test_utils.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "execution_policy.h"
#include "profile_utils.h"
#include "test_fixture.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace mmltk;
using namespace mmltk::testsupport;

namespace {

struct Options {
    std::string test_dir = "/tmp/mmltk_profile";
    bool keep_artifacts = false;
    int width = 384;
    int height = 384;
    int num_images = 256;
    int batch_size = 32;
    int num_epochs = 2;
    int compile_workers = -1;
    int shuffle_prefetch = 3;
    int repetitions = 10;
    int warmup_runs = 1;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--keep-artifacts") == 0) {
            opts.keep_artifacts = true;
        } else if (std::strcmp(argv[i], "--test-dir") == 0 && i + 1 < argc) {
            opts.test_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            opts.width = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            opts.height = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--num-images") == 0 && i + 1 < argc) {
            opts.num_images = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--batch-size") == 0 && i + 1 < argc) {
            opts.batch_size = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--num-epochs") == 0 && i + 1 < argc) {
            opts.num_epochs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--compile-workers") == 0 && i + 1 < argc) {
            opts.compile_workers = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--shuffle-prefetch") == 0 && i + 1 < argc) {
            opts.shuffle_prefetch = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--repetitions") == 0 && i + 1 < argc) {
            opts.repetitions = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--warmup-runs") == 0 && i + 1 < argc) {
            opts.warmup_runs = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr,
                         "Usage: %s [--keep-artifacts] [--test-dir PATH] [--width N] [--height N] "
                         "[--num-images N] [--batch-size N] [--num-epochs N] "
                         "[--compile-workers N] [--shuffle-prefetch N] [--repetitions N] "
                         "[--warmup-runs N]\n",
                         argv[0]);
            std::exit(1);
        }
    }

    if (opts.width <= 0 || opts.height <= 0 || opts.num_images <= 0 || opts.batch_size <= 0 || opts.num_epochs <= 0 ||
        opts.shuffle_prefetch <= 0 || opts.repetitions <= 0 || opts.warmup_runs < 0) {
        std::fprintf(stderr, "numeric options must be positive except warmup-runs, which may be zero\n");
        std::exit(1);
    }
    return opts;
}

double seconds_since(const std::chrono::steady_clock::time_point& start,
                     const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double>(end - start).count();
}

std::string metric_name(const char* label, const char* suffix) {
    return std::string("benchmark.") + label + "." + suffix;
}

void record_duration_metric(const char* label, const char* suffix, const std::chrono::steady_clock::time_point& start,
                            const std::chrono::steady_clock::time_point& end) {
    profile_record_duration_ns(
        metric_name(label, suffix).c_str(),
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
}

void record_value_metric(const char* label, const char* suffix, std::uint64_t value) {
    profile_add_value(metric_name(label, suffix).c_str(), value);
}

void run_loader_case(const char* label, const DatasetLoader::Config& cfg, int num_epochs, bool overlap_consumer) {
    const auto init_start = std::chrono::steady_clock::now();
    DatasetLoader loader(cfg);
    const auto init_end = std::chrono::steady_clock::now();

    std::printf("%s init_sec=%.6f\n", label, seconds_since(init_start, init_end));
    record_duration_metric(label, "init", init_start, init_end);
    record_value_metric(label, "prefetch_factor", static_cast<std::uint64_t>(cfg.prefetch_factor));

    cudaStream_t consumer_stream = nullptr;
    if (overlap_consumer) {
        CUDA_ASSERT_OK(cudaSetDevice(cfg.device_id));
        CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&consumer_stream, cudaStreamNonBlocking));
    }

    for (int epoch = 0; epoch < num_epochs; ++epoch) {
        const auto epoch_start = std::chrono::steady_clock::now();
        loader.begin_epoch();
        Batch batch{};
        size_t total = 0;
        while (loader.next_batch(batch)) {
            total += batch.num_images;
            if (overlap_consumer) {
                loader.handoff_batch(batch, consumer_stream);
                CUDA_ASSERT_OK(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0,
                                               batch.num_images * loader.image_stride(), consumer_stream));
                loader.release_batch(batch, consumer_stream);
            } else {
                loader.release_batch(batch);
            }
        }
        if (overlap_consumer) {
            CUDA_ASSERT_OK(cudaStreamSynchronize(consumer_stream));
        }
        loader.cuda().sync();
        const auto epoch_end = std::chrono::steady_clock::now();
        const double secs = seconds_since(epoch_start, epoch_end);
        std::printf("%s epoch=%d images=%zu sec=%.6f img_per_sec=%.2f\n", label, epoch, total, secs,
                    static_cast<double>(total) / secs);
        record_duration_metric(label, "epoch", epoch_start, epoch_end);
        record_value_metric(label, "epoch_images", total);
        record_value_metric(label, "epoch_img_per_sec_x100",
                            static_cast<std::uint64_t>((static_cast<double>(total) / secs) * 100.0));
    }

    if (consumer_stream) {
        CUDA_ASSERT_OK(cudaStreamDestroy(consumer_stream));
    }
}

void run_profile_iteration(const FixtureSpec& fixture, const Options& opts) {
    CompilerConfig ccfg;
    ccfg.source_dir = dataset_dir(fixture);
    ccfg.output_dir = compiled_dir(fixture);
    ccfg.split = fixture.split;
    ccfg.target_width = static_cast<uint32_t>(fixture.width);
    ccfg.target_height = static_cast<uint32_t>(fixture.height);
    ccfg.num_workers = opts.compile_workers;

    const auto compile_start = std::chrono::steady_clock::now();
    const DatasetCompilePlan compile_plan = DatasetCompiler::prepare(ccfg, {ccfg.split});
    DatasetCompiler::compile(compile_plan, 0U);
    const auto compile_end = std::chrono::steady_clock::now();
    const std::string bin_path = compiled_bin_path(fixture);

    std::printf("compile sec=%.6f file_bytes=%zu\n", seconds_since(compile_start, compile_end),
                static_cast<size_t>(fs::file_size(bin_path)));
    record_duration_metric("compile", "total", compile_start, compile_end);
    record_value_metric("compile", "file_bytes", static_cast<std::uint64_t>(fs::file_size(bin_path)));

    DatasetLoader::Config sequential_cfg;
    sequential_cfg.compiled_path = bin_path;
    sequential_cfg.batch_size = static_cast<size_t>(opts.batch_size);
    sequential_cfg.shuffle = false;
    sequential_cfg.prefetch_factor = 6;
    run_loader_case("loader_seq", sequential_cfg, opts.num_epochs, false);

    DatasetLoader::Config shuffle_cfg = sequential_cfg;
    shuffle_cfg.shuffle = true;
    shuffle_cfg.seed = 7;
    shuffle_cfg.prefetch_factor = opts.shuffle_prefetch;
    run_loader_case("loader_shuffle", shuffle_cfg, opts.num_epochs, false);
    run_loader_case("loader_shuffle_overlap", shuffle_cfg, opts.num_epochs, true);
}

}  

int main(int argc, char** argv) {
    mmltk::profile_enable();
    MMLTK_PROFILE_PROCESS_LABEL("profile.core");
    MMLTK_PROFILE_RUN_LABEL("core");
    const mmltk::ExecutionPolicySnapshot execution_snapshot = mmltk::apply_process_execution_policy();
    mmltk::log_process_execution_policy("mmltk_profile_runner", execution_snapshot, false, true);
    const Options opts = parse_options(argc, argv);

    const FixtureSpec fixture{
        opts.test_dir, "train", opts.width, opts.height, opts.num_images,
    };

    create_synthetic_dataset(fixture);

    for (int warmup = 0; warmup < opts.warmup_runs; ++warmup) {
        MMLTK_PROFILE_RESET_ITERATION();
        std::printf("warmup=%d/%d\n", warmup + 1, opts.warmup_runs);
        run_profile_iteration(fixture, opts);
    }

    for (int repetition = 0; repetition < opts.repetitions; ++repetition) {
        MMLTK_PROFILE_RESET_ITERATION();
        std::printf("repetition=%d/%d\n", repetition + 1, opts.repetitions);
        run_profile_iteration(fixture, opts);

        std::array<char, 64> iteration_label{};
        std::snprintf(iteration_label.data(), iteration_label.size(), "core[%02d]", repetition + 1);
        MMLTK_PROFILE_CAPTURE_ITERATION(iteration_label.data());
    }

    MMLTK_PROFILE_FLUSH();
    if (!opts.keep_artifacts) {
        fs::remove_all(fixture.root_dir);
    }
    return 0;
}
