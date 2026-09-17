#include <cuda_runtime.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/data/tests/test_fixture.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/cuda_error.h"
using namespace mmltk::backend::data;
using namespace mmltk::common::logging;
using namespace mmltk::common::system;
using mmltk::frameworks::gpu::ensure_cuda_ok;
using namespace mmltk::backend::data::testsupport;
namespace fs = std::filesystem;
namespace {
[[noreturn]] void usage_error(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--keep-artifacts] [--test-dir PATH] [--width N] "
                 "[--height N] [--num-images N] [--batch-size N] "
                 "[--num-epochs N] [--compile-workers N] "
                 "[--shuffle-prefetch N] [--repetitions N] [--warmup-runs N]\n",
                 program);
    std::exit(1);
}
int parse_integer(const char* value, const char* program) {
    int parsed = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) { usage_error(program); }
    return parsed;
}
double seconds_since(const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}
std::string metric_name(const char* label, const char* suffix) { return std::string("benchmark.") + label + "." + suffix; }
void record_duration_metric(const char* label, const char* suffix, const std::chrono::steady_clock::time_point start,
                            const std::chrono::steady_clock::time_point end) {
    profile_record_duration_ns(metric_name(label, suffix).c_str(),
                               static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
}
void record_value_metric(const char* label, const char* suffix, const std::uint64_t value) { profile_add_value(metric_name(label, suffix).c_str(), value); }
std::string iteration_label(const int repetition) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "backend.data[%02d]", repetition);
    return buffer.data();
}
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
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--keep-artifacts") {
            opts.keep_artifacts = true;
            continue;
        }
        if (index + 1 >= argc) { usage_error(argv[0]); }
        const char* value = argv[++index];
        if (option == "--test-dir") {
            opts.test_dir = value;
        } else if (option == "--width") {
            opts.width = parse_integer(value, argv[0]);
        } else if (option == "--height") {
            opts.height = parse_integer(value, argv[0]);
        } else if (option == "--num-images") {
            opts.num_images = parse_integer(value, argv[0]);
        } else if (option == "--batch-size") {
            opts.batch_size = parse_integer(value, argv[0]);
        } else if (option == "--num-epochs") {
            opts.num_epochs = parse_integer(value, argv[0]);
        } else if (option == "--compile-workers") {
            opts.compile_workers = parse_integer(value, argv[0]);
        } else if (option == "--shuffle-prefetch") {
            opts.shuffle_prefetch = parse_integer(value, argv[0]);
        } else if (option == "--repetitions") {
            opts.repetitions = parse_integer(value, argv[0]);
        } else if (option == "--warmup-runs") {
            opts.warmup_runs = parse_integer(value, argv[0]);
        } else {
            usage_error(argv[0]);
        }
    }
    if (opts.width <= 0 || opts.height <= 0 || opts.num_images <= 0 || opts.batch_size <= 0 || opts.num_epochs <= 0 || opts.shuffle_prefetch <= 0 ||
        opts.repetitions <= 0 || opts.warmup_runs < 0) {
        std::fprintf(stderr, "numeric options must be positive except warmup-runs, which may be zero\n");
        std::exit(1);
    }
    return opts;
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
        ensure_cuda_ok(cudaSetDevice(cfg.device_id), "cudaSetDevice");
        ensure_cuda_ok(cudaStreamCreateWithFlags(&consumer_stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
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
                ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * loader.image_stride(), consumer_stream),
                               "cudaMemsetAsync");
                loader.release_batch(batch, consumer_stream);
            } else {
                loader.release_batch(batch);
            }
        }
        if (overlap_consumer) { ensure_cuda_ok(cudaStreamSynchronize(consumer_stream), "cudaStreamSynchronize"); }
        loader.synchronize();
        const auto epoch_end = std::chrono::steady_clock::now();
        const double secs = seconds_since(epoch_start, epoch_end);
        std::printf("%s epoch=%d images=%zu sec=%.6f img_per_sec=%.2f\n", label, epoch, total, secs, static_cast<double>(total) / secs);
        record_duration_metric(label, "epoch", epoch_start, epoch_end);
        record_value_metric(label, "epoch_images", total);
        record_value_metric(label, "epoch_img_per_sec_x100", static_cast<std::uint64_t>((static_cast<double>(total) / secs) * 100.0));
    }
    if (consumer_stream) { ensure_cuda_ok(cudaStreamDestroy(consumer_stream), "cudaStreamDestroy"); }
}
void run_profile_iteration(const FixtureSpec& fixture, const Options& opts) {
    auto ccfg = compiler_config(fixture);
    ccfg.num_workers = opts.compile_workers;
    const auto compile_start = std::chrono::steady_clock::now();
    const DatasetCompilePlan compile_plan = DatasetCompiler::prepare(ccfg, {ccfg.split});
    DatasetCompiler::compile(compile_plan, 0U);
    const auto compile_end = std::chrono::steady_clock::now();
    const std::string bin_path = compiled_bin_path(fixture);
    std::printf("compile sec=%.6f file_bytes=%zu\n", seconds_since(compile_start, compile_end), static_cast<size_t>(fs::file_size(bin_path)));
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
}  // namespace
int main(int argc, char** argv) {
    profile_enable();
    profile_set_process_label("profile.backend.data");
    profile_set_run_label("backend.data");
    try {
        const ExecutionPolicySnapshot execution_snapshot = apply_process_execution_policy();
        trace([&](auto& logger) {
            logger.trace(
                "event=profile.execution_policy executable=mmltk_backend_data_profile_runner online_cpu_count={} "
                "nice_value={} scheduler_policy={} scheduler_priority={} io_class={} io_priority_data={}",
                execution_snapshot.online_cpu_count, execution_snapshot.nice_value, execution_snapshot.scheduler_policy, execution_snapshot.scheduler_priority,
                execution_snapshot.io_class, execution_snapshot.io_priority_data);
        });
        const Options opts = parse_options(argc, argv);
        const FixtureSpec fixture{
            opts.test_dir, "train", opts.width, opts.height, opts.num_images,
        };
        create_synthetic_dataset(fixture);
        for (int warmup = 0; warmup < opts.warmup_runs; ++warmup) {
            profile_reset_iteration();
            std::printf("warmup=%d/%d\n", warmup + 1, opts.warmup_runs);
            run_profile_iteration(fixture, opts);
        }
        for (int repetition = 0; repetition < opts.repetitions; ++repetition) {
            profile_reset_iteration();
            std::printf("repetition=%d/%d\n", repetition + 1, opts.repetitions);
            run_profile_iteration(fixture, opts);
            profile_capture_iteration(iteration_label(repetition + 1).c_str());
        }
        profile_flush();
        if (!opts.keep_artifacts) { fs::remove_all(fixture.root_dir); }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mmltk_backend_data_profile_runner error: %s\n", error.what());
        return 1;
    }
}
