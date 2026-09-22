#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include <exception>
#include "src/test_support/profile_runner_common.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/system/execution_policy.h"
using namespace mmltk::testsupport;
namespace {
struct Options : CommonProfileOptions {
 std::string compiled_path;
 std::string checkpoint_path;
 int limit_images = 0;
};
struct EvaluateRun {
 std::uint64_t elapsed_ns = 0;
 size_t images = 0;
 double bbox_ap = 0.0;
 std::optional<double> mask_ap;
};
Options parse_options(int argc, char** argv) {
 Options options;
 CliOptionTable table;
 table.add_string("--compiled-path", options.compiled_path);
 table.add_string("--checkpoint-path", options.checkpoint_path);
 add_common_profile_options(table, options);
 table.add_integer("--limit-images", options.limit_images);
 table.parse_or_exit(argc, argv,
  "--compiled-path PATH --checkpoint-path PATH [--repetitions N] "
  "[--warmup-runs N] [--device-id N] [--workers N] [--batch-size N] "
  "[--cpu-affinity LIST] [--limit-images N]");
 if (options.compiled_path.empty() || options.checkpoint_path.empty()) { throw std::runtime_error("RF-DETR profile runner requires compiled and checkpoint paths"); }
 if (options.repetitions <= 0 || options.warmup_runs < 0 || options.batch_size <= 0 || options.limit_images < 0) {
  throw std::runtime_error("repetitions, warmup-runs, and batch-size must be positive/non-negative");
 }
 return options;
}
EvaluateRun run_checkpoint_backend(const Options& options) {
 mmltk::backend::models::rfdetr::EvaluateRequest evaluate_options;
 evaluate_options.compiled_path = options.compiled_path;
 evaluate_options.weights_path = options.checkpoint_path;
 evaluate_options.batch_size = static_cast<size_t>(std::max(1, options.batch_size));
 evaluate_options.limit_images = static_cast<std::size_t>(options.limit_images);
 evaluate_options.device_id = options.device_id;
 evaluate_options.workers = options.workers;
 evaluate_options.cpu_affinity = options.cpu_affinity;
 const auto started = std::chrono::steady_clock::now();
 const mmltk::backend::models::rfdetr::EvaluationRunResult result = mmltk::backend::models::rfdetr::run_evaluation(evaluate_options);
 const std::uint64_t elapsed_ns = elapsed_ns_since(started);
 mmltk::backend::models::rfdetr::print_evaluation_summary(evaluate_options, result);
 return EvaluateRun{
  elapsed_ns,
  result.result.timing.images,
  result.result.summary.bbox.ap,
  result.result.summary.mask.has_value() ? std::optional<double>(result.result.summary.mask->ap) : std::nullopt,
 };
}
void record_evaluate_metrics(const EvaluateRun& run) {
 record_duration_metric("rfdetr.evaluate.checkpoint.total", run.elapsed_ns);
 record_value_metric("rfdetr.evaluate.checkpoint.images", run.images);
 record_value_metric("rfdetr.evaluate.checkpoint.img_per_sec_x100", img_per_sec_x100(run.elapsed_ns, run.images));
 record_value_metric("rfdetr.evaluate.checkpoint.bbox_ap_x10000", x10000_metric(run.bbox_ap));
 record_optional_x10000_metric("rfdetr.evaluate.checkpoint.mask_ap_x10000", run.mask_ap);
}
void print_iteration_line(const char* phase, int index, int total, const EvaluateRun& run) {
 std::printf("%s=rfdetr.evaluate.checkpoint %d/%d pt=%.3fs bbox=%.4f mask=%s\n", phase, index, total, seconds_from_ns(run.elapsed_ns), run.bbox_ap, optional_metric_text(run.mask_ap).c_str());
 std::fflush(stdout);
}
}  // namespace
int main(int argc, char** argv) {
 mmltk::common::logging::profile_enable();
 mmltk::common::logging::profile_set_process_label("profile.rfdetr.evaluate");
 mmltk::common::logging::profile_set_run_label("rfdetr.evaluate.checkpoint");
 try {
  const mmltk::common::system::ExecutionPolicySnapshot execution_snapshot = mmltk::common::system::apply_process_execution_policy();
  mmltk::common::logging::trace([&](auto& logger) {
   logger.trace(
    "event=profile.execution_policy executable=mmltk_rfdetr_profile_runner online_cpu_count={} "
    "nice_value={} scheduler_policy={} scheduler_priority={} io_class={} io_priority_data={}",
    execution_snapshot.online_cpu_count, execution_snapshot.nice_value, execution_snapshot.scheduler_policy, execution_snapshot.scheduler_priority, execution_snapshot.io_class,
    execution_snapshot.io_priority_data);
  });
  const Options options = parse_options(argc, argv);
  ensure_file_exists("compiled dataset", options.compiled_path);
  ensure_file_exists("checkpoint", options.checkpoint_path);
  run_profile_phases(
   "rfdetr.evaluate.checkpoint", options.warmup_runs, options.repetitions, [&options](bool, int) { return run_checkpoint_backend(options); }, record_evaluate_metrics, print_iteration_line);
  mmltk::common::logging::profile_flush();
  return 0;
 } catch (const std::exception& error) {
  std::fprintf(stderr, "mmltk_rfdetr_profile_runner error: %s\n", error.what());
  return 1;
 }
}
