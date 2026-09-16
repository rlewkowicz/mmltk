#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <string_view>
#include "profile_runner_common.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/ml/torch/detail/torch_api.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/training/distributed_train_launcher.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/training/train_recipe.h"
#include "src/common/system/execution_policy.h"
#include "test_fixture.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
using namespace mmltk::testsupport;
namespace {
using mmltk::backend::data::CompilerConfig;
using mmltk::backend::data::DatasetCompilePlan;
using mmltk::backend::data::DatasetCompiler;
using mmltk::backend::data::testsupport::FixtureSpec;
using mmltk::backend::models::rfdetr::TrainAssignmentKind;
using mmltk::backend::models::rfdetr::TrainRequest;
using mmltk::backend::models::rfdetr::TrainRunResult;
constexpr int kProfileFixtureImages = 24;
constexpr std::array<std::string_view, 3> kMatcherMetrics{
    "rfdetr.matcher.cost_copy_syncs",
    "rfdetr.matcher.lsap",
    "rfdetr.matcher.indices_to_device",
};
struct Options : CommonProfileOptions {
    std::string test_dir = "/tmp/mmltk_rfdetr_train_profile";
    std::string weights_path;
    bool keep_artifacts = false;
    int width = 432;
    int height = 432;
    int num_images = kProfileFixtureImages;
    int epochs = 1;
    int lanes = 0;
    int prefetch_factor = 2;
    int compile_workers = -1;
    int seed = 42;
    std::string assignment = std::string{mmltk::backend::models::rfdetr::cli_enum_spelling(TrainAssignmentKind::Hungarian)};
    bool denoising = false;
};
struct TrainRun {
    std::uint64_t elapsed_ns = 0;
    double train_loss = 0.0;
    double bbox_ap = 0.0;
    std::optional<double> mask_ap;
    std::optional<std::uint64_t> peak_allocated_cuda_bytes;
};
Options parse_options(int argc, char** argv) {
    Options options;
    options.workers = 16;
    CliOptionTable table;
    table.add_string("--test-dir", options.test_dir);
    table.add_string("--weights-path", options.weights_path);
    table.add_flag("--keep-artifacts", options.keep_artifacts);
    table.add_integer("--width", options.width);
    table.add_integer("--height", options.height);
    table.add_integer("--num-images", options.num_images);
    table.add_integer("--epochs", options.epochs);
    table.add_integer("--lanes", options.lanes);
    table.add_integer("--prefetch-factor", options.prefetch_factor);
    table.add_integer("--compile-workers", options.compile_workers);
    table.add_integer("--seed", options.seed);
    table.add_string("--assignment", options.assignment);
    table.add_flag("--dn", options.denoising);
    add_common_profile_options(table, options);
    table.parse_or_exit(argc, argv,
                        "--weights-path PATH [--test-dir PATH] [--keep-artifacts] "
                        "[--width N] [--height N] [--num-images N] [--batch-size N] [--epochs N] "
                        "[--device-id N] [--workers N] [--lanes N] [--prefetch-factor N] [--compile-workers N] "
                        "[--seed N] [--assignment hungarian|match-free] [--dn] "
                        "[--repetitions N] [--warmup-runs N] [--cpu-affinity LIST]");
    if (options.weights_path.empty()) { throw std::runtime_error("RF-DETR train profile runner requires --weights-path"); }
    if (options.width <= 0 || options.height <= 0 || options.num_images <= 0 || options.batch_size <= 0 || options.epochs <= 0 || options.device_id < 0 ||
        options.workers < 0 || options.lanes < 0 || options.prefetch_factor <= 0 || options.repetitions <= 0 || options.warmup_runs < 0) {
        throw std::runtime_error("numeric options must be positive except lanes and warmup-runs, which may be zero");
    }
    if (options.num_images < options.batch_size) { throw std::runtime_error("num-images must be at least batch-size for RF-DETR train profile"); }
    if (options.num_images != kProfileFixtureImages) { throw std::runtime_error("RF-DETR matcher-route profile requires the complete 24-image fixture"); }
    if (!mmltk::backend::models::rfdetr::train_assignment_from_spelling(options.assignment).has_value()) {
        throw std::runtime_error("assignment must use the canonical spelling 'hungarian' or 'match-free'");
    }
    return options;
}
[[nodiscard]] TrainAssignmentKind selected_assignment(const Options& options) {
    return *mmltk::backend::models::rfdetr::train_assignment_from_spelling(options.assignment);
}
[[nodiscard]] std::string profile_run_label(const Options& options) {
    return "rfdetr.train.assignment-" + options.assignment + (options.denoising ? ".dn-on" : ".dn-off");
}
void build_fixture(const Options& options, const FixtureSpec& fixture) {
    mmltk::backend::data::testsupport::create_synthetic_dataset(fixture);
    CompilerConfig config;
    config.source_dir = mmltk::backend::data::testsupport::dataset_dir(fixture);
    config.output_dir = mmltk::backend::data::testsupport::compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = static_cast<uint32_t>(fixture.width);
    config.target_height = static_cast<uint32_t>(fixture.height);
    config.num_workers = options.compile_workers;
    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {config.split});
    DatasetCompiler::compile(plan, 0U);
    const auto compiled = mmltk::backend::data::inspect_compiled_dataset(mmltk::backend::data::testsupport::compiled_bin_path(fixture));
    if (compiled.image_count != static_cast<std::uint32_t>(kProfileFixtureImages) || compiled.max_instances_per_image == 0U) {
        throw std::runtime_error("RF-DETR profile fixture must compile all 24 images and positive targets");
    }
}
void reseed_ambient_torch_rng(const Options& options) {
    const auto seed = static_cast<std::uint64_t>(options.seed);
    mmltk::backend::ml::torch_api::manual_seed(seed);
    ::torch::cuda::manual_seed_all(seed);
}
void reset_cuda_peak(const Options& options) { c10::cuda::CUDACachingAllocator::resetPeakStats(static_cast<c10::DeviceIndex>(options.device_id)); }
[[nodiscard]] std::uint64_t peak_allocated_cuda_bytes(const Options& options) {
    const auto stats = c10::cuda::CUDACachingAllocator::getDeviceStats(static_cast<c10::DeviceIndex>(options.device_id));
    const auto peak = stats.allocated_bytes[static_cast<std::size_t>(c10::CachingAllocator::StatType::AGGREGATE)].peak;
    if (peak < 0) { throw std::runtime_error("CUDA allocator reported a negative allocated-byte peak"); }
    return static_cast<std::uint64_t>(peak);
}
TrainRun run_train_iteration(const Options& options, const std::string& compiled_path, const fs::path& output_dir, const bool measured) {
    fs::remove_all(output_dir);
    reseed_ambient_torch_rng(options);
    if (measured) { reset_cuda_peak(options); }
    TrainRequest train_options;
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
    train_options.seed = options.seed;
    train_options.training_supervision.assignment = selected_assignment(options);
    train_options.training_supervision.denoising.enabled = options.denoising;
    train_options.print_freq = 1;
    train_options.progress_bar = false;
    train_options.amp = true;
    train_options.validation_loss = false;
    train_options.validation_profile = true;
    const auto started = std::chrono::steady_clock::now();
    const TrainRunResult result = mmltk::backend::models::rfdetr::run_training(train_options);
    const std::uint64_t elapsed_ns = elapsed_ns_since(started);
    if (result.history.empty()) { throw std::runtime_error("RF-DETR train profile produced no epoch history"); }
    const auto& last = result.history.back();
    return TrainRun{
        elapsed_ns,
        last.train_loss,
        last.val_summary.bbox.ap,
        last.val_summary.mask.has_value() ? std::optional<double>(last.val_summary.mask->ap) : std::nullopt,
        measured ? std::optional<std::uint64_t>(peak_allocated_cuda_bytes(options)) : std::nullopt,
    };
}
void record_train_metrics(const TrainRun& run, const Options& options) {
    const size_t profiled_train_images = static_cast<size_t>(options.num_images) * static_cast<size_t>(options.epochs);
    record_duration_metric("rfdetr.train.total", run.elapsed_ns);
    record_value_metric("rfdetr.train.total_ns", run.elapsed_ns);
    record_value_metric("rfdetr.train.images", profiled_train_images);
    record_value_metric("rfdetr.train.img_per_sec_x100", img_per_sec_x100(run.elapsed_ns, profiled_train_images));
    record_value_metric("rfdetr.train.loss_x10000", x10000_metric(run.train_loss));
    record_value_metric("rfdetr.train.val_bbox_ap_x10000", x10000_metric(run.bbox_ap));
    record_optional_x10000_metric("rfdetr.train.val_mask_ap_x10000", run.mask_ap);
    if (run.peak_allocated_cuda_bytes.has_value()) {
        mmltk::common::logging::profile_add_value("rfdetr.train.peak_allocated_cuda_bytes", *run.peak_allocated_cuda_bytes);
    }
}
void print_iteration_line(const Options& options, const char* phase, int index, int total, const TrainRun& run) {
    std::printf("%s=rfdetr.train mode=%s dn=%s %d/%d total=%.3fs train_loss=%.4f bbox=%.4f mask=%s\n", phase, options.assignment.c_str(),
                options.denoising ? "on" : "off", index, total, seconds_from_ns(run.elapsed_ns), run.train_loss, run.bbox_ap,
                optional_metric_text(run.mask_ap).c_str());
    std::fflush(stdout);
}
[[nodiscard]] bool metric_line(const std::string_view line, const std::string_view metric) {
    return line == metric || (line.starts_with(metric) && line.size() > metric.size() && line[metric.size()] == ' ');
}
[[nodiscard]] std::uint64_t metric_field(const std::string_view line, const std::string_view field) {
    const std::string key = std::string(field) + "=";
    const auto begin_pos = line.find(key);
    if (begin_pos == std::string_view::npos) { throw std::runtime_error("profile metric lacks required " + key + " field"); }
    const auto value_begin = begin_pos + key.size();
    const auto value_end = line.find(' ', value_begin);
    const auto value = line.substr(value_begin, value_end - value_begin);
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) { throw std::runtime_error("profile metric contains malformed " + key + " field"); }
    return parsed;
}
struct RouteEvidence {
    bool found = false;
    bool positive_layers = false;
    bool positive_valid_pairs = false;
    std::array<bool, kMatcherMetrics.size()> positive_matcher{};
    std::array<bool, kMatcherMetrics.size()> matcher_line{};
};
void verify_profile_route(const std::string& log_path, const std::string& run_label, const Options& options) {
    std::ifstream input(log_path);
    if (!input) { throw std::runtime_error("failed to read flushed RF-DETR profile log: " + log_path); }
    std::vector<RouteEvidence> repetitions(static_cast<std::size_t>(options.repetitions));
    std::size_t measured_blocks = 0U;
    RouteEvidence* current = nullptr;
    std::string line;
    while (std::getline(input, line)) {
        const std::string_view view{line};
        if (view.starts_with("=== mmltk profile ")) {
            current = nullptr;
            if (view.starts_with("=== mmltk profile aggregate ") || view.find(" run=" + run_label + " ") == std::string_view::npos) { continue; }
            for (int repetition = 0; repetition < options.repetitions; ++repetition) {
                const std::string expected = " iteration=" + iteration_label_for_run(run_label.c_str(), repetition + 1) + " ";
                if (view.find(expected) != std::string_view::npos) {
                    auto& evidence = repetitions[static_cast<std::size_t>(repetition)];
                    if (evidence.found) { throw std::runtime_error("profile log contains a duplicate measured repetition block"); }
                    evidence.found = true;
                    ++measured_blocks;
                    current = &evidence;
                    break;
                }
            }
            if (current == nullptr) { throw std::runtime_error("profile log contains an unexpected measured repetition block"); }
            continue;
        }
        if (current == nullptr || view.empty()) { continue; }
        if (metric_line(view, "rfdetr.supervision.match_free.layers")) {
            current->positive_layers = metric_field(view, "value_sum") > 0U;
        } else if (metric_line(view, "rfdetr.supervision.match_free.valid_pairs")) {
            current->positive_valid_pairs = metric_field(view, "value_sum") > 0U;
        }
        for (std::size_t metric = 0; metric < kMatcherMetrics.size(); ++metric) {
            if (!metric_line(view, kMatcherMetrics[metric])) { continue; }
            current->matcher_line[metric] = true;
            current->positive_matcher[metric] = metric_field(view, metric == 1U ? "calls" : "value_sum") > 0U;
        }
    }
    if (input.bad()) { throw std::runtime_error("failed while reading flushed RF-DETR profile log"); }
    if (measured_blocks != repetitions.size()) { throw std::runtime_error("profile log does not contain exactly the requested measured blocks"); }
    const bool match_free = selected_assignment(options) == TrainAssignmentKind::MatchFree;
    for (std::size_t repetition = 0; repetition < repetitions.size(); ++repetition) {
        const auto& evidence = repetitions[repetition];
        if (!evidence.found) { throw std::runtime_error("profile log is missing a requested measured repetition block"); }
        if (match_free) {
            if (!evidence.positive_layers || !evidence.positive_valid_pairs) {
                throw std::runtime_error("Match-Free profile repetition lacks positive supervised-layer and valid-pair evidence");
            }
            if (std::ranges::any_of(evidence.matcher_line, std::identity{})) {
                throw std::runtime_error("Match-Free profile repetition contains native matcher activity");
            }
        } else if (!std::ranges::all_of(evidence.positive_matcher, std::identity{})) {
            throw std::runtime_error("Hungarian profile repetition lacks positive native matcher activity");
        }
    }
}
}  // namespace
int main(int argc, char** argv) {
    mmltk::common::logging::profile_enable();
    mmltk::common::logging::profile_set_process_label("profile.rfdetr.train");
    mmltk::common::logging::profile_set_run_label("rfdetr.train");
    try {
        const mmltk::common::system::ExecutionPolicySnapshot execution_snapshot = mmltk::common::system::apply_process_execution_policy();
        mmltk::common::logging::trace([&](auto& logger) {
            logger.trace(
                "event=profile.execution_policy executable=mmltk_rfdetr_train_profile_runner online_cpu_count={} "
                "nice_value={} scheduler_policy={} scheduler_priority={} io_class={} io_priority_data={}",
                execution_snapshot.online_cpu_count, execution_snapshot.nice_value, execution_snapshot.scheduler_policy, execution_snapshot.scheduler_priority,
                execution_snapshot.io_class, execution_snapshot.io_priority_data);
        });
        const Options options = parse_options(argc, argv);
        ensure_file_exists("weights checkpoint", options.weights_path);
        const char* profile_log = std::getenv("MMLTK_PROFILE_LOG");
        if (profile_log == nullptr || profile_log[0] == '\0') { throw std::runtime_error("RF-DETR train profile runner requires nonempty MMLTK_PROFILE_LOG"); }
        const std::string run_label = profile_run_label(options);
        const FixtureSpec fixture{
            options.test_dir, "train", options.width, options.height, options.num_images,
        };
        build_fixture(options, fixture);
        const std::string compiled_path = mmltk::backend::data::testsupport::compiled_bin_path(fixture);
        run_profile_phases(
            run_label.c_str(), options.warmup_runs, options.repetitions,
            [&options, &compiled_path, &fixture](const bool is_warmup, const int index) {
                const std::string run_name = (is_warmup ? "run-warmup-" : "run-") + std::to_string(index);
                return run_train_iteration(options, compiled_path, fs::path(fixture.root_dir) / run_name, !is_warmup);
            },
            [&options](const TrainRun& run) { record_train_metrics(run, options); },
            [&options](const char* phase, const int index, const int total, const TrainRun& run) { print_iteration_line(options, phase, index, total, run); });
        mmltk::common::logging::profile_flush();
        verify_profile_route(profile_log, run_label, options);
        if (!options.keep_artifacts) { fs::remove_all(fixture.root_dir); }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mmltk_rfdetr_train_profile_runner error: %s\n", error.what());
        return 1;
    }
}
