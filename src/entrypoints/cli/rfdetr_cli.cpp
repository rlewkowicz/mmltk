#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/common/system/runtime_paths.h"
#include "tool_launch.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <meta>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include "cli_output.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "spdmon/spdmon.hpp"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/models/rfdetr/training/training_partition.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/common/system/runtime_paths.h"
#include "src/controller/services/train_command.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
import mmltk.common.logging.mmltk_logging;
namespace data = mmltk::backend::data;
namespace logging = mmltk::common::logging;
namespace reflection = mmltk::frameworks::reflection;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace services = mmltk::controller::services;
namespace mmltk::entrypoints::cli {
namespace {
using reflection::custom_option;
using reflection::negative_flag;
using reflection::option;
using reflection::option_with_item_policy;
using rfdetr::AugmentationGroupConfig;
using rfdetr::BuildEngineRequest;
using rfdetr::DenoisingSupervisionConfig;
using rfdetr::EvaluateRequest;
using rfdetr::ExportOnnxRequest;
using rfdetr::GpuAugmentationConfig;
using rfdetr::MatchFreeSupervisionConfig;
using rfdetr::PredictRequest;
using rfdetr::TrainDataPolicy;
using rfdetr::TrainingSupervisionConfig;
using rfdetr::TrainLaneConfiguration;
using rfdetr::TrainRecipeCatalog;
using rfdetr::TrainRecipeSettings;
using rfdetr::TrainRequest;
using rfdetr::ValidateRequest;
struct CompileCliRequest final {
 MMLTK_MAX_PATH_BYTES std::filesystem::path source_dir;
 MMLTK_MAX_PATH_BYTES std::filesystem::path output_dir;
 MMLTK_MAX_PATH_BYTES std::filesystem::path cache_dir;
 MMLTK_MINIMUM(int, 1) MMLTK_MAXIMUM(int, static_cast<int>(data::MAX_IMAGE_EXTENT)) int resolution = 432;
 MMLTK_MINIMUM(int, 0) MMLTK_MAXIMUM(int, static_cast<int>(data::MAX_IMAGE_EXTENT)) int benchmark_resolution = 0;
 MMLTK_MINIMUM(int, -1) int num_workers = -1;
 MMLTK_MINIMUM(int, 0) int cuda_mask_batch_size = 0;
 MMLTK_MINIMUM(int, 0) int cuda_device_id = 0;
 bool overwrite = false;
 bool perceptual_downscale = false;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
};
struct InfoCliRequest final {
 MMLTK_MAX_PATH_BYTES std::filesystem::path onnx_path;
 MMLTK_MAX_PATH_BYTES std::filesystem::path tensorrt_path;
 MMLTK_MINIMUM(int, 0) int device_id = 0;
};
struct NormalizeWeightsRequest final {
 MMLTK_MAX_PATH_BYTES std::filesystem::path class_layout_path;
 MMLTK_MAX_PATH_BYTES std::filesystem::path input_path;
 MMLTK_MAX_PATH_BYTES std::filesystem::path output_path;
};
struct PredictCliRequest final {
 PredictRequest request;
 MMLTK_MAX_ITEMS(mmltk::backend::models::rfdetr::kMaximumCliImageInputs) std::vector<std::filesystem::path> image_paths;
};
struct TrainCliRequest final {
 MMLTK_MAX_BYTES(rfdetr::kMaximumTrainRequestJsonBytes) std::string request_json;
 TrainRequest request;
};
MMLTK_REFLECT_FIELDS(CompileCliRequest)
MMLTK_REFLECT_FIELDS(InfoCliRequest)
MMLTK_REFLECT_FIELDS(NormalizeWeightsRequest)
MMLTK_REFLECT_FIELDS(PredictCliRequest)
MMLTK_REFLECT_FIELDS(TrainCliRequest)
// Typed prefixes preserve the existing member identity and policy machinery.
template <auto... Members>
inline constexpr auto train_member = reflection::member_path<&TrainCliRequest::request, Members...>;
template <auto... Members>
inline constexpr auto recipe_member = train_member<&TrainRequest::recipe, Members...>;
template <auto... Members>
inline constexpr auto augmentation_member = train_member<&TrainRequest::gpu_augmentation, Members...>;
template <auto... Members>
inline constexpr auto supervision_member = train_member<&TrainRequest::training_supervision, Members...>;
template <auto... Members>
inline constexpr auto predict_member = reflection::member_path<&PredictCliRequest::request, Members...>;
inline constexpr std::array kCompileOptions{option<CompileCliRequest, &CompileCliRequest::resize_mode>("--resize-mode", "Image geometry: Stretch or Letterbox", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::perceptual_downscale>("--perceptual-downscale", "Perceptual shrinking", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::source_dir>("--source-dir", "Source dataset root", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::output_dir>("--output-dir", "Compiled output directory", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::cache_dir>("--cache-dir", "Benchmark cache root", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::benchmark_resolution>("--compile-benchmark-dataset", "Compile benchmark dataset", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::overwrite>("--overwrite", "Replace benchmark output", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::resolution>("--resolution", "Square image resolution", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::num_workers>("--workers", "CPU worker budget", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::cuda_mask_batch_size>("--cuda-mask-batch-size", "CUDA mask batch size", "Dataset"),
 option<CompileCliRequest, &CompileCliRequest::cuda_device_id>("--cuda-device-id", "CUDA device id", "Dataset")};
inline constexpr std::array kInfoOptions{option<InfoCliRequest, &InfoCliRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
 option<InfoCliRequest, &InfoCliRequest::tensorrt_path>("--tensorrt", "TensorRT engine path", "Model input"),
 option<InfoCliRequest, &InfoCliRequest::device_id>("--device-id", "CUDA device id", "Execution")};
inline constexpr std::array kNormalizeOptions{option<NormalizeWeightsRequest, &NormalizeWeightsRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
 option<NormalizeWeightsRequest, &NormalizeWeightsRequest::input_path>("--input", "Input upstream checkpoint", "Input and output"),
 option<NormalizeWeightsRequest, &NormalizeWeightsRequest::output_path>("--output", "Output native checkpoint", "Input and output")};
inline constexpr std::array kBuildEngineOptions{option<BuildEngineRequest, &BuildEngineRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
 option<BuildEngineRequest, &BuildEngineRequest::onnx_path>("--onnx", "ONNX model path", "Input and output"),
 option<BuildEngineRequest, &BuildEngineRequest::output_path>("--output", "Output TensorRT engine path", "Input and output"),
 option<BuildEngineRequest, &BuildEngineRequest::preset_name>("--preset", "Declared RF-DETR preset", "Input and output"),
 option<BuildEngineRequest, &BuildEngineRequest::resolution>("--resolution", "Square model resolution", "Input and output"),
 option<BuildEngineRequest, &BuildEngineRequest::device_id>("--device-id", "CUDA device id", "Execution"),
 option<BuildEngineRequest, &BuildEngineRequest::allow_fp16>("--fp16", "Enable FP16 TensorRT kernels", "Execution", {}, "--no-fp16")};
inline constexpr std::array kExportOnnxOptions{option<ExportOnnxRequest, &ExportOnnxRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
 option<ExportOnnxRequest, &ExportOnnxRequest::weights_path>("--weights", "RF-DETR checkpoint path", "Input and output"),
 option<ExportOnnxRequest, &ExportOnnxRequest::output_path>(
  // CLEANUP-IGNORE: Export and TensorRT build expose separate reflected request schemas despite shared artifact
  // fields.
  "--output", "Output ONNX path", "Input and output"),
 option<ExportOnnxRequest, &ExportOnnxRequest::preset_name>("--preset", "Declared RF-DETR preset", "Input and output"),
 option<ExportOnnxRequest, &ExportOnnxRequest::resolution>("--resolution", "Square model resolution", "Input and output"),
 option<ExportOnnxRequest, &ExportOnnxRequest::device_id>("--device-id", "CUDA device id", "Execution"),
 option<ExportOnnxRequest, &ExportOnnxRequest::opset_version>("--opset-version", "ONNX opset version", "Execution"),
 option<ExportOnnxRequest, &ExportOnnxRequest::simplify>(
  // CLEANUP-IGNORE: Adjacent command arrays join an Export tail to separately typed Evaluate loading options.
  "--simplify", "Run ONNX validation", "Execution")};
inline constexpr std::array kEvaluateOptions{option<EvaluateRequest, &EvaluateRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<EvaluateRequest, &EvaluateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::numa_node>("--numa-node", "GPU-local NUMA node (-1 automatic)", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::compiled_path>("--compiled", "Compiled dataset split", "Dataset"),
 option<EvaluateRequest, &EvaluateRequest::weights_path>("--weights", "RF-DETR checkpoint path", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::tensorrt_path>("--tensorrt", "TensorRT engine path", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::preset_name>("--preset", "Declared preset", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::resolution>("--resolution", "Square model resolution", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::candidate_count>("--candidate-count", "Selected candidate count (0: admitted model default)", "Model input"),
 option<EvaluateRequest, &EvaluateRequest::batch_size>("--batch-size", "Evaluation batch size", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::device_id>("--device-id", "CUDA device id", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::limit_images>("--limit-images", "Image limit", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::eval_max_dets>("--eval-max-dets", "Detection cap", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::workers>("--workers", "Dataset worker count", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::lanes>("--lanes", "Parallel backend lanes", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::cpu_affinity>("--cpu-affinity", "Linux CPU list", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::backend>("--backend", "Backend preference", "Execution"),
 option<EvaluateRequest, &EvaluateRequest::allow_fp16>("--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
 option<EvaluateRequest, &EvaluateRequest::progress_bar>("--progress", "Render progress", "Execution", {}, "--no-progress"),
 option<EvaluateRequest, &EvaluateRequest::compilation_mode>("--compile-mode", "Native compilation mode", "Execution")};
inline constexpr std::array kPredictOptions{option<PredictCliRequest, predict_member<&PredictRequest::class_layout_path>>("--class-layout", "Digest-bound class descriptor", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::numa_node>>("--numa-node", "GPU-local NUMA node (-1 automatic)", "Execution"),
 negative_flag<PredictCliRequest, predict_member<&PredictRequest::h2d_dataloader>>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::compiled_path>>("--compiled", "Compiled dataset split (.bin)", "Dataset"),
 option_with_item_policy<PredictCliRequest, &PredictCliRequest::image_paths, &rfdetr::PredictImageInput::image_path>("--image", "Input image path; repeat for multiple images", "Dataset"),
 option<PredictCliRequest, predict_member<&PredictRequest::output_path>>("--output", "Prediction JSON output path", "Dataset"),
 option<PredictCliRequest, predict_member<&PredictRequest::weights_path>>("--weights", "RF-DETR checkpoint path", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::onnx_path>>("--onnx", "ONNX model path", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::tensorrt_path>>("--tensorrt", "TensorRT engine path", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::preset_name>>("--preset", "Declared RF-DETR preset architecture", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::resolution>>("--resolution", "Square model input resolution", "Model input"),
 option<PredictCliRequest, predict_member<&PredictRequest::batch_size>>("--batch-size", "Batch size for inference", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::max_dets_per_image>>("--max-dets-per-image", "Maximum saved detections per image", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::device_id>>("--device-id", "CUDA device id", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::threshold>>("--threshold", "Minimum score threshold", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::workers>>("--workers", "Dataset worker count", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::lanes>>("--lanes", "Parallel backend lane count", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::cpu_affinity>>("--cpu-affinity", "Linux CPU list", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::backend>>("--backend", "Backend preference", "Execution"),
 option<PredictCliRequest, predict_member<&PredictRequest::allow_fp16>>("--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
 option<PredictCliRequest, predict_member<&PredictRequest::progress_bar>>("--progress", "Render interactive progress", "Execution", {}, "--no-progress"),
 option<PredictCliRequest, predict_member<&PredictRequest::compilation_mode>>("--compile-mode", "Native compilation mode", "Execution")};
inline constexpr std::array kValidateOptions{option<ValidateRequest, &ValidateRequest::compile_resize_mode>("--resize-mode", "Compile image geometry: Stretch or Letterbox", "Dataset"),
 option<ValidateRequest, &ValidateRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<ValidateRequest, &ValidateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 option<ValidateRequest, &ValidateRequest::numa_node>("--numa-node", "GPU-local NUMA node (-1 automatic)", "Execution"),
 option<ValidateRequest, &ValidateRequest::compiled_path>("--compiled", "Compiled dataset split (.bin)", "Dataset"),
 option<ValidateRequest, &ValidateRequest::source_dir>("--source", "Source dataset root", "Dataset"), option<ValidateRequest, &ValidateRequest::split>("--split", "Source split", "Dataset"),
 option<ValidateRequest, &ValidateRequest::resolution>("--resolution", "Square resolution", "Dataset"),
 option<ValidateRequest, &ValidateRequest::recompile>("--recompile", "Recompile source dataset", "Dataset"),
 option<ValidateRequest, &ValidateRequest::compile_workers>("--compile-workers", "Compile worker count", "Dataset"),
 option<ValidateRequest, &ValidateRequest::compile_cuda_mask_batch_size>("--compile-cuda-mask-batch-size", "CUDA mask batch size", "Dataset"),
 option<ValidateRequest, &ValidateRequest::compile_cuda_device_id>("--compile-cuda-device-id", "CUDA device id", "Dataset"),
 option<ValidateRequest, &ValidateRequest::weights_path>("--weights", "RF-DETR checkpoint path", "Model input"),
 option<ValidateRequest, &ValidateRequest::preset_name>("--preset", "Declared RF-DETR preset", "Model input"),
 option<ValidateRequest, &ValidateRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
 option<ValidateRequest, &ValidateRequest::tensorrt_path>("--tensorrt", "TensorRT engine path", "Model input"),
 option<ValidateRequest, &ValidateRequest::save_engine_path>("--save-engine", "Write generated TensorRT engine", "Model input"),
 option<ValidateRequest, &ValidateRequest::report_json_path>("--report-json", "Validation report JSON path", "Output"),
 option<ValidateRequest, &ValidateRequest::eval_order>("--eval-order", "Backend evaluation order", "Output"),
 option<ValidateRequest, &ValidateRequest::batch_size>("--batch-size", "Evaluation batch size", "Execution"),
 option<ValidateRequest, &ValidateRequest::limit_images>("--limit-images", "Image limit", "Execution"),
 option<ValidateRequest, &ValidateRequest::candidate_count>("--candidate-count", "Selected candidate count (0: admitted model default)", "Execution"),
 option<ValidateRequest, &ValidateRequest::eval_max_dets>("--eval-max-dets", "Detection cap", "Execution"),
 option<ValidateRequest, &ValidateRequest::alignment_images>("--alignment-images", "Backend alignment sample count", "Execution"),
 option<ValidateRequest, &ValidateRequest::prefetch_factor>("--prefetch-factor", "Dataset prefetch factor", "Execution"),
 option<ValidateRequest, &ValidateRequest::device_id>("--device-id", "CUDA device id", "Execution"),
 option<ValidateRequest, &ValidateRequest::workers>("--workers", "Dataset worker count", "Execution"),
 option<ValidateRequest, &ValidateRequest::lanes>("--lanes", "Parallel backend lanes", "Execution"),
 option<ValidateRequest, &ValidateRequest::cpu_affinity>("--cpu-affinity", "Linux CPU list", "Execution"),
 option<ValidateRequest, &ValidateRequest::allow_fp16>("--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
 option<ValidateRequest, &ValidateRequest::log_mode>("--log-mode", "Validation logging mode", "Execution"),
 option<ValidateRequest, &ValidateRequest::profile>("--profile", "Collect validation profile", "Execution"),
 option<ValidateRequest, &ValidateRequest::write_report_json>("--write-report-json", "Write validation report", "Output", {}, "--no-write-report-json")};
template <auto Member, bool Unique>
std::expected<void, reflection::ParseError> assign_train_integer_list(TrainCliRequest& state, const std::string_view text, bool, const reflection::FieldConstraint) {
 std::array<int, mmltk::backend::models::rfdetr::kMaximumTrainingDevices> ids{};
 std::size_t count = 0U;
 std::size_t start = 0U;
 while (start <= text.size()) {
  if (count == ids.size()) { return std::unexpected(reflection::ParseError{reflection::ParseErrorCode::InvalidValue, text, "integer list exceeds the selected-device capacity"}); }
  const std::size_t comma = text.find(',', start);
  const std::string_view item = text.substr(start, comma == std::string_view::npos ? text.size() - start : comma - start);
  int id = -1;
  const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), id);
  if (item.empty() || error != std::errc{} || end != item.data() + item.size() || id < (Unique ? 0 : -1) || (Unique && std::find(ids.begin(), ids.begin() + count, id) != ids.begin() + count)) {
   return std::unexpected(
    reflection::ParseError{reflection::ParseErrorCode::InvalidValue, text, Unique ? "--device-ids requires unique non-negative integers" : "--numa-nodes requires integers >= -1"});
  }
  ids[count++] = id;
  if (comma == std::string_view::npos) break;
  start = comma + 1U;
 }
 (state.request.*Member).assign(ids.begin(), ids.begin() + count);
 return {};
}
template <auto Member>
void emit_train_integer_list(std::vector<std::string>& arguments, const TrainCliRequest& state, const std::string_view name, std::string_view, reflection::OptionKind, bool) {
 if ((state.request.*Member).empty()) return;
 std::string joined;
 for (const int id : state.request.*Member) {
  if (!joined.empty()) joined.push_back(',');
  joined += std::to_string(id);
 }
 arguments.emplace_back(name);
 arguments.emplace_back(std::move(joined));
}
inline constexpr std::array kTrainOptions{option<TrainCliRequest, &TrainCliRequest::request_json>("--request-json", "Complete canonical training request (maximum 64 KiB)", "Training"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::nesterov>>("--nesterov", "SGD Nesterov momentum", "Optimization", {}, "--no-nesterov"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::warmup_bias_lr>>("--warmup-bias-lr", "Absolute SGD bias warmup learning rate", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::class_layout_path>>("--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<TrainCliRequest, train_member<&TrainRequest::h2d_dataloader>>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 custom_option<TrainCliRequest, train_member<&TrainRequest::numa_nodes>, &assign_train_integer_list<&TrainRequest::numa_nodes, false>, &emit_train_integer_list<&TrainRequest::numa_nodes>>(
  "--numa-nodes", "Comma-separated NUMA overrides in selected device order (-1 automatic)", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::train_compiled_path>>("--train-compiled", "Compiled training split", "Dataset"),
 option<TrainCliRequest, train_member<&TrainRequest::val_compiled_path>>("--val-compiled", "Compiled validation split", "Dataset"),
 option<TrainCliRequest, train_member<&TrainRequest::test_compiled_path>>("--test-compiled", "Compiled test split", "Dataset"),
 option<TrainCliRequest, train_member<&TrainRequest::resolution>>("--resolution", "Square model input resolution", "Dataset"),
 option<TrainCliRequest, train_member<&TrainRequest::output_dir>>("--output-dir", "Output directory", "Checkpoint"),
 option<TrainCliRequest, train_member<&TrainRequest::weights_path>>("--weights", "Source checkpoint", "Checkpoint"),
 option<TrainCliRequest, train_member<&TrainRequest::resume_path>>("--resume", "Native checkpoint to resume", "Checkpoint"),
 option<TrainCliRequest, train_member<&TrainRequest::preset_name>>("--preset", "Declared preset", "Checkpoint"),
 option<TrainCliRequest, train_member<&TrainRequest::num_queries>>("--num-queries", "Native query count", "Checkpoint"),
 option<TrainCliRequest, train_member<&TrainRequest::batch_size>>("--batch-size", "Global microbatch image count", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::val_batch_size>>("--val-batch-size", "Validation batch size", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::epochs>>("--epochs", "Epoch count", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::grad_accum_steps>>("--grad-accum-steps", "Gradient accumulation", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::optimizer>>("--optimizer", "adamw, muon, or sgd", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr>>("--lr", "Decoder learning rate", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr_encoder>>("--lr-encoder", "Encoder learning rate", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::momentum>>("--momentum", "Muon or SGD momentum", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::freeze_encoder>>("--freeze-encoder", "Freeze encoder", "Optimization", {}, "--no-freeze-encoder"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr_component_decay>>("--lr-component-decay", "Component decay", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::encoder_layer_decay>>("--encoder-layer-decay", "Layer decay", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::weight_decay>>("--weight-decay", "Weight decay", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr_drop>>("--lr-drop", "Step drop epoch", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr_scheduler>>("--lr-scheduler", "step, cosine, or ultralytics-linear", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::lr_min_factor>>("--lr-min-factor", "Minimum LR multiplier", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::warmup_epochs>>("--warmup-epochs", "Warmup duration", "Optimization"),
 option<TrainCliRequest, recipe_member<&TrainRecipeSettings::warmup_momentum>>("--warmup-momentum", "Warmup momentum", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::clip_max_norm>>("--clip-max-norm", "Gradient clipping norm", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::fused_optimizer>>("--fused-optimizer", "Use fused AdamW backend (AdamW only)", "Optimization", {}, "--no-fused-optimizer"),
 option<TrainCliRequest, train_member<&TrainRequest::use_ema>>("--use-ema", "Maintain EMA", "Optimization", {}, "--no-ema"),
 option<TrainCliRequest, train_member<&TrainRequest::validation_loss>>("--validation-loss", "Calculate validation loss", "Optimization", {}, "--no-validation-loss"),
 option<TrainCliRequest, train_member<&TrainRequest::validation_profile>>("--validation-profile", "Write validation profile", "Optimization", {}, "--no-validation-profile"),
 option<TrainCliRequest, train_member<&TrainRequest::ema_decay>>("--ema-decay", "EMA decay", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::ema_tau>>("--ema-tau", "EMA tau", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::eval_max_dets>>("--eval-max-dets", "Detection cap", "Optimization"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::assignment>>("--assignment", "hungarian or match-free", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::match_free, &MatchFreeSupervisionConfig::rho>>("--match-free-rho", "Sparse correspondence threshold", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::match_free, &MatchFreeSupervisionConfig::correspondence_weight>>(
  "--match-free-correspondence-weight", "Correspondence objective weight", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::match_free, &MatchFreeSupervisionConfig::query_weight>>("--match-free-query-weight", "Query objective weight", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::denoising, &DenoisingSupervisionConfig::enabled>>("--dn", "Enable denoising supervision", "Supervision", {}, "--no-dn"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::denoising, &DenoisingSupervisionConfig::groups>>("--dn-groups", "Denoising group count", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::denoising, &DenoisingSupervisionConfig::label_noise_ratio>>(
  "--dn-label-noise-ratio", "Denoising label flip probability", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::denoising, &DenoisingSupervisionConfig::center_noise_scale>>(
  "--dn-center-noise-scale", "Denoising center noise scale", "Supervision"),
 option<TrainCliRequest, supervision_member<&TrainingSupervisionConfig::denoising, &DenoisingSupervisionConfig::size_noise_scale>>(
  "--dn-size-noise-scale", "Denoising size noise scale", "Supervision"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::enabled>>("--gpu-augment", "Apply GPU augmentation", "GPU augmentation", {}, "--no-gpu-augment"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::perceptual_downscale>>(
  "--aug-perceptual-downscale", "Perceptual shrinking", "GPU augmentation", {}, "--no-aug-perceptual-downscale"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::geometry, &AugmentationGroupConfig::probability>>("--aug-geometry-prob", "geometry selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::geometry, &AugmentationGroupConfig::min_strength>>("--aug-geometry-min-strength", "geometry minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::geometry, &AugmentationGroupConfig::max_strength>>("--aug-geometry-max-strength", "geometry maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::resize, &AugmentationGroupConfig::probability>>("--aug-resize-prob", "resize selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::resize, &AugmentationGroupConfig::min_strength>>("--aug-resize-min-strength", "resize minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::resize, &AugmentationGroupConfig::max_strength>>("--aug-resize-max-strength", "resize maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::color, &AugmentationGroupConfig::probability>>("--aug-color-prob", "color selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::color, &AugmentationGroupConfig::min_strength>>("--aug-color-min-strength", "color minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::color, &AugmentationGroupConfig::max_strength>>("--aug-color-max-strength", "color maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::noise, &AugmentationGroupConfig::probability>>("--aug-noise-prob", "noise selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::noise, &AugmentationGroupConfig::min_strength>>("--aug-noise-min-strength", "noise minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::noise, &AugmentationGroupConfig::max_strength>>("--aug-noise-max-strength", "noise maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::blur, &AugmentationGroupConfig::probability>>("--aug-blur-prob", "blur selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::blur, &AugmentationGroupConfig::min_strength>>("--aug-blur-min-strength", "blur minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::blur, &AugmentationGroupConfig::max_strength>>("--aug-blur-max-strength", "blur maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::occlusion, &AugmentationGroupConfig::probability>>("--aug-occlusion-prob", "occlusion selection probability", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::occlusion, &AugmentationGroupConfig::min_strength>>(
  "--aug-occlusion-min-strength", "occlusion minimum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::occlusion, &AugmentationGroupConfig::max_strength>>(
  "--aug-occlusion-max-strength", "occlusion maximum strength", "GPU augmentation"),
 option<TrainCliRequest, augmentation_member<&GpuAugmentationConfig::copy_paste_probability>>("--aug-copy-paste-prob", "Copy-paste probability", "GPU augmentation"),
 option<TrainCliRequest, train_member<&TrainRequest::numa_node>>("--numa-node", "GPU-local NUMA node override (-1 selects known locality)", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::device_id>>("--device-id", "Single CUDA device", "Execution"),
 custom_option<TrainCliRequest, train_member<&TrainRequest::device_ids>, &assign_train_integer_list<&TrainRequest::device_ids, true>, &emit_train_integer_list<&TrainRequest::device_ids>>(
  "--device-ids", "Comma-separated CUDA device ids", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::workers>>("--workers", "Dataset worker count", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::lanes>>("--lanes", "Parallel backend lanes", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::validation_lanes>>("--validation-lanes", "Parallel training validation lanes", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::unfreeze_encoder_last_epochs>>("--unfreeze-encoder-last-epochs", "Unfreeze the encoder for the final epochs", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::disable_augmentation_last_epochs>>("--disable-augmentation-last-epochs", "Disable augmentation for the final epochs", "Optimization"),
 option<TrainCliRequest, train_member<&TrainRequest::cpu_affinity>>("--cpu-affinity", "Linux CPU list", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::prefetch_factor>>("--prefetch-factor", "Prefetch factor", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::print_freq>>("--print-freq", "Logging frequency", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::seed>>("--seed", "Random seed", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::amp>>("--amp", "Automatic mixed precision", "Execution", {}, "--no-amp"),
 option<TrainCliRequest, train_member<&TrainRequest::progress_bar>>("--progress", "Render progress", "Execution", {}, "--no-progress"),
 option<TrainCliRequest, train_member<&TrainRequest::compilation_mode>>("--compile-mode", "Native compilation mode", "Execution"),
 option<TrainCliRequest, train_member<&TrainRequest::distributed_worker>>("--dist-worker", "Internal distributed worker", "Distributed (internal)"),
 option<TrainCliRequest, train_member<&TrainRequest::distributed_rank>>("--dist-rank", "Worker rank", "Distributed (internal)"),
 option<TrainCliRequest, train_member<&TrainRequest::distributed_world_size>>("--dist-world-size", "Worker world size", "Distributed (internal)"),
 option<TrainCliRequest, train_member<&TrainRequest::distributed_store_path>>("--dist-store-file", "Rendezvous file", "Distributed (internal)")};
[[nodiscard]] consteval bool train_descriptor_relation_is_complete() {
 using Relation = reflection::catalog_provider_relation<TrainRecipeCatalog>;
 constexpr auto selector = recipe_member<&TrainRecipeSettings::optimizer>;
 std::array<reflection::ReflectedMemberIdentity, Relation::member_count> relation_identities{};
 std::size_t count = 0U;
 Relation::VisitMembers([&]<class Entry>() {
  constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, TrainRecipeSettings>(selector, Entry::destination);
  const auto identity = reflection::accessor_member_identity<TrainCliRequest, destination>();
  (void)reflection::unique_descriptor_index(kTrainOptions, identity);
  relation_identities[count++] = identity;
 });
 constexpr auto device_id = train_member<&TrainRequest::device_id>;
 constexpr auto device_ids = train_member<&TrainRequest::device_ids>;
 const auto single_identity = reflection::accessor_member_identity<TrainCliRequest, device_id>();
 const auto list_identity = reflection::accessor_member_identity<TrainCliRequest, device_ids>();
 (void)reflection::unique_descriptor_index(kTrainOptions, single_identity);
 (void)reflection::unique_descriptor_index(kTrainOptions, list_identity);
 if (count != Relation::member_count || single_identity == list_identity) return false;
 for (const auto& identity : relation_identities) {
  if (identity == single_identity || identity == list_identity) return false;
 }
 return true;
}
static_assert(train_descriptor_relation_is_complete());
static_assert((reflection::audit_descriptors(kCompileOptions), true));
static_assert((reflection::audit_descriptors(kInfoOptions), true));
static_assert((reflection::audit_descriptors(kNormalizeOptions), true));
inline constexpr std::array kBuildEngineUnexposed{reflection::unexposed<BuildEngineRequest, &BuildEngineRequest::weights_path>("build-engine accepts the selected ONNX artifact kind"),
 reflection::unexposed<BuildEngineRequest, &BuildEngineRequest::tensorrt_path>("build-engine accepts the selected ONNX artifact kind")};
inline constexpr std::array kExportOnnxUnexposed{reflection::unexposed<ExportOnnxRequest, &ExportOnnxRequest::onnx_path>("export-onnx accepts the selected native-weights artifact kind"),
 reflection::unexposed<ExportOnnxRequest, &ExportOnnxRequest::tensorrt_path>("export-onnx accepts the selected native-weights artifact kind")};
inline constexpr std::array kPredictUnexposed{reflection::unexposed<PredictCliRequest, predict_member<&PredictRequest::video_path>>("local video selection belongs to the GUI prediction workflow"),
 reflection::unexposed<PredictCliRequest, predict_member<&PredictRequest::image_inputs>>("the CLI bounded image_paths collection derives the final image-input "
                                                                                         "records once in finalize_predict_request")};
static_assert((reflection::audit_descriptors(kBuildEngineOptions, kBuildEngineUnexposed), true));
static_assert((reflection::audit_descriptors(kExportOnnxOptions, kExportOnnxUnexposed), true));
static_assert((reflection::audit_descriptors(kEvaluateOptions), true));
static_assert((reflection::audit_descriptors(kPredictOptions, kPredictUnexposed), true));
static_assert((reflection::audit_descriptors(kValidateOptions), true));
inline constexpr std::array kTrainUnexposed{
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::lane_configuration, &TrainLaneConfiguration::models>>("--request-json owns the complete model recipes and identities"),
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::lane_configuration, &TrainLaneConfiguration::next_model_id>>("--request-json owns the model identity frontier"),
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::lane_configuration, &TrainLaneConfiguration::merge_rounds>>("--request-json owns the complete merge policy"),
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::data_policy, &TrainDataPolicy::rare_threshold>>("--request-json owns the complete sampling policy"),
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::data_policy, &TrainDataPolicy::maximum_repeat_factor>>("--request-json owns the complete sampling policy"),
 reflection::unexposed<TrainCliRequest, train_member<&TrainRequest::data_policy, &TrainDataPolicy::maximum_draw_multiplier>>("--request-json owns the complete sampling policy")};
static_assert((reflection::audit_descriptors(kTrainOptions, kTrainUnexposed), true));
void print_command_help(std::string rendered_options) { std::puts(rendered_options.c_str()); }
template <class Request, class Options>
[[nodiscard]] Request parse_request(const std::span<const std::string_view> arguments, const Options& options) {
 auto parsed = reflection::parse<Request>(arguments, options);
 if (!parsed) throw parsed.error();
 return std::move(parsed->request);
}
void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence);
[[nodiscard]] TrainCliRequest parse_train_request(const std::span<const std::string_view> arguments) {
 auto parsed = reflection::parse<TrainCliRequest>(arguments, kTrainOptions);
 if (!parsed) throw parsed.error();
 apply_train_presence(parsed->request, parsed->presence);
 return std::move(parsed->request);
}
void print_compile_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr compile [options]", command.description, kCompileOptions)); }
void print_info_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr info [options]", command.description, kInfoOptions)); }
void print_build_engine_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr build-engine [options]", command.description, kBuildEngineOptions)); }
void print_export_onnx_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr export-onnx [options]", command.description, kExportOnnxOptions)); }
void print_predict_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr predict [options]", command.description, kPredictOptions)); }
void print_evaluate_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr evaluate [options]", command.description, kEvaluateOptions)); }
void print_validate_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr validate [options]", command.description, kValidateOptions)); }
void print_train_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr train [options]", command.description, kTrainOptions)); }
void print_normalize_help(const rfdetr::RfdetrCommandDescriptor& command) { print_command_help(reflection::help("mmltk rfdetr normalize-weights [options]", command.description, kNormalizeOptions)); }
void finalize_compile_request(CompileCliRequest& request) {
 if (request.benchmark_resolution != 0) {
  if (request.output_dir.empty()) request.output_dir = "./compiled";
  return;
 }
 if (request.source_dir.empty() || request.output_dir.empty()) { throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir"); }
 if (!request.cache_dir.empty()) {
  throw std::runtime_error(
   "rfdetr compile --cache-dir requires "
   "--compile-benchmark-dataset");
 }
 if (request.overwrite) {
  throw std::runtime_error(
   "rfdetr compile --overwrite requires "
   "--compile-benchmark-dataset");
 }
}
std::atomic<bool> benchmark_cancel_requested{false};
void request_benchmark_cancel(int) noexcept { benchmark_cancel_requested.store(true, std::memory_order_relaxed); }
class BenchmarkSignalScope final {
public:
 BenchmarkSignalScope() {
  benchmark_cancel_requested.store(false, std::memory_order_relaxed);
  struct sigaction action{};
  action.sa_handler = &request_benchmark_cancel;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGINT, &action, &previous_interrupt_) != 0) { throw std::system_error(errno, std::generic_category(), "cannot install benchmark signal handler"); }
  if (::sigaction(SIGTERM, &action, &previous_terminate_) != 0) {
   const int failure = errno;
   (void)::sigaction(SIGINT, &previous_interrupt_, nullptr);
   throw std::system_error(failure, std::generic_category(), "cannot install benchmark signal handler");
  }
 }
 ~BenchmarkSignalScope() {
  (void)::sigaction(SIGTERM, &previous_terminate_, nullptr);
  (void)::sigaction(SIGINT, &previous_interrupt_, nullptr);
 }
 BenchmarkSignalScope(const BenchmarkSignalScope&) = delete;
 BenchmarkSignalScope& operator=(const BenchmarkSignalScope&) = delete;

private:
 struct sigaction previous_interrupt_{};
 struct sigaction previous_terminate_{};
};
void run_compile(const CompileCliRequest& request) {
 if (request.benchmark_resolution != 0) {
  BenchmarkSignalScope signal_scope;
  data::BenchmarkCompilerConfig config;
  config.output_dir = request.output_dir;
  config.cache_dir = request.cache_dir;
  config.resolution = static_cast<std::uint32_t>(request.benchmark_resolution);
  config.num_workers = request.num_workers;
  config.overwrite = request.overwrite;
  config.perceptual_downscale = request.perceptual_downscale;
  config.resize_mode = request.resize_mode;
  config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(benchmark_cancel_requested);
  config.progress = [](const data::BenchmarkCompileProgress& progress) {
   spdmon::ProgressBar::log(data::format_dataset_compile_tracks(progress.tracks));
   if (!progress.activity.empty()) spdmon::ProgressBar::log(progress.activity);
   for (const auto& source : progress.sources)
    if (!source.activity.empty() || source.transfer || source.complete) {
     spdmon::ProgressBar::log(std::string(data::benchmark_source_label(source.source)) + " · " + data::format_benchmark_source_status(source, "Acquiring") + " · " +
                              std::to_string(source.completed_bytes) + "/" + (source.byte_total_known ? std::to_string(source.total_bytes) : "?") + " bytes · " +
                              std::to_string(source.completed_images) + "/" + std::to_string(source.total_images) + " images · " + std::to_string(source.invalidated_images) + " invalidated");
    }
  };
  if (logging::enabled(spdlog::level::trace)) {
   config.trace = [](const std::string_view event, const std::string_view fields) {
    if (fields.empty()) { return; }
    logging::log_if_enabled("rfdetr.benchmark", spdlog::level::trace, [&](auto& current) { current.trace("{{\"event\":\"{}\",\"fields\":{}}}", event, fields); });
   };
  }
  data::compile_benchmark_dataset(std::move(config));
  return;
 }
 data::CompilerConfig config;
 config.perceptual_downscale = request.perceptual_downscale;
 config.resize_mode = request.resize_mode;
 config.source_dir = request.source_dir.string();
 config.output_dir = request.output_dir.string();
 config.target_width = static_cast<std::uint32_t>(request.resolution);
 config.target_height = static_cast<std::uint32_t>(request.resolution);
 config.num_workers = request.num_workers;
 config.cuda_mask_batch_size = request.cuda_mask_batch_size;
 config.cuda_device_id = request.cuda_device_id;
 const auto plan = data::DatasetCompiler::prepare(config, {"train", "val"});
 for (std::size_t split_index = 0U; split_index < plan.splits.size(); ++split_index) {
  std::size_t completed = 0U;
  spdmon::ProgressBar bar("compile " + plan.splits[split_index].split, 0U, "img");
  struct ProgressState final {
   std::size_t* completed;
   spdmon::ProgressBar* bar;
  } state{&completed, &bar};
  data::CompileTelemetry telemetry{plan.splits[split_index].image_count, {.context = &state, .report = [](void* context, const data::CompileProgress& progress) noexcept {
                                                                           auto& progress_state = *static_cast<ProgressState*>(context);
                                                                           progress_state.bar->set_total(progress.total);
                                                                           progress_state.bar->set_postfix(data::format_dataset_compile_tracks(progress.tracks));
                                                                           if (progress.done > *progress_state.completed) {
                                                                            progress_state.bar->add(progress.done - *progress_state.completed);
                                                                            *progress_state.completed = progress.done;
                                                                           }
                                                                          }}};
  data::DatasetCompiler::compile(plan, split_index, &telemetry);
  bar.close();
 }
}
[[noreturn]] void exec_onnx_info_tool(const std::filesystem::path& model_path, const logging::CliOverrides& logging_options) {
 const auto tool = mmltk::entrypoints::cli::resolve_sibling_tool_path("mmltk-rfdetr-onnx-info").string();
 std::vector<std::string> arguments{tool, model_path.string()};
 if (logging_options.level) {
  const auto level_name = spdlog::level::to_string_view(*logging_options.level);
  arguments.push_back("--log-level=" + std::string(level_name.data(), level_name.size()));
 }
 if (logging_options.log_file) arguments.push_back("--log-file=" + logging_options.log_file->string());
 if (logging_options.log_dir) arguments.push_back("--log-dir=" + logging_options.log_dir->string());
 auto argv = mmltk::entrypoints::cli::make_exec_argv(arguments);
 ::execv(argv.front(), argv.data());
 throw std::system_error(errno, std::generic_category(), "failed to exec ONNX info helper");
}
void finalize_predict_request(PredictCliRequest& state) {
 if (state.request.output_path.empty()) throw std::invalid_argument("rfdetr predict requires --output");
 state.request.image_inputs.clear();
 const auto count = state.image_paths.size();
 const auto source_kind = count == 0U ? rfdetr::PredictSourceKind::CompiledDataset : rfdetr::PredictSourceKind::ImageFiles;
 state.request.image_inputs.reserve(count);
 for (std::size_t index = 0U; index < count; ++index) {
  auto& path = state.image_paths[index];
  auto source_name = path.filename().string();
  state.request.image_inputs.push_back({
   .image_path = std::move(path),
   .source_name = std::move(source_name),
   .image_id = static_cast<std::int64_t>(index),
  });
 }
 decltype(state.image_paths){}.swap(state.image_paths);
 state.request.source_kind = source_kind;
 state.request = rfdetr::finalize_predict_request(std::move(state.request));
}
void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence) {
 constexpr auto json_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, &TrainCliRequest::request_json>());
 if (presence.test(json_index)) {
  for (std::size_t index = 0; index < kTrainOptions.size(); ++index)
   if (index != json_index && presence.test(index)) throw std::invalid_argument("--request-json is mutually exclusive with scalar train options");
  state.request = rfdetr::decode_train_request_json(state.request_json);
  return;
 }
 using Relation = reflection::catalog_provider_relation<TrainRecipeCatalog>;
 constexpr auto selector = recipe_member<&TrainRecipeSettings::optimizer>;
 Relation::VisitMembers([&]<class Entry>() {
  constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, TrainRecipeSettings>(selector, Entry::destination);
  constexpr auto index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, destination>());
  if (presence.test(index)) Relation::template set_override<Entry::destination>(state.request.recipe.overrides);
 });
 rfdetr::resolve_train_recipe(state.request.recipe);
 constexpr auto device_id = train_member<&TrainRequest::device_id>;
 constexpr auto device_ids = train_member<&TrainRequest::device_ids>;
 constexpr auto device_id_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_id>());
 constexpr auto device_ids_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_ids>());
 if (presence.test(device_id_index) && presence.test(device_ids_index)) { throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids"); }
}
class DistributedTrainingProcess final {
public:
 static int Run(const TrainRequest& request) {
  const auto partitions = rfdetr::select_distributed_training_partitions(request);
  if (partitions.size() < 2U) throw std::logic_error("distributed training requires multiple partitions");
  const auto store = std::filesystem::temp_directory_path() / ("mmltk_rfdetr_train_" + std::to_string(static_cast<long long>(::getpid())) + ".store");
  std::filesystem::remove(store);
  std::vector<pid_t> children;
  children.reserve(partitions.size());
  try {
   for (const auto& partition : partitions) {
    auto worker = request;
    worker.distributed_worker = true;
    worker.distributed_rank = partition.rank;
    worker.distributed_world_size = partition.world_size;
    worker.distributed_store_path = store;
    rfdetr::apply_training_partition(worker, partition);
    auto arguments = services::build_train_command_arguments(worker);
    arguments.insert(arguments.begin(), mmltk::common::system::runtime_paths::current_executable_path().string());
    const pid_t pid = ::fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "failed to fork RF-DETR worker");
    if (pid == 0) {
     std::vector<char*> argv;
     argv.reserve(arguments.size() + 1U);
     for (auto& argument : arguments) argv.push_back(argument.data());
     argv.push_back(nullptr);
     ::execv(argv.front(), argv.data());
     std::_Exit(127);
    }
    children.push_back(pid);
   }
  } catch (...) {
   for (const pid_t child : children) (void)::kill(child, SIGTERM);
   for (const pid_t child : children) (void)::waitpid(child, nullptr, 0);
   std::filesystem::remove(store);
   throw;
  }
  int result = 0;
  std::size_t remaining = children.size();
  while (remaining > 0U) {
   int status = 0;
   const pid_t completed = ::waitpid(-1, &status, 0);
   if (completed < 0) {
    if (errno == EINTR) continue;
    result = 1;
    for (const pid_t child : children) (void)::kill(child, SIGTERM);
    break;
   }
   --remaining;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    for (const pid_t child : children) {
     if (child != completed) (void)::kill(child, SIGTERM);
    }
   }
  }
  while (::waitpid(-1, nullptr, 0) > 0) {}
  std::filesystem::remove(store);
  return result;
 }
};
int dispatch_command(const rfdetr::RfdetrCommandDescriptor& descriptor, const std::span<const std::string_view> arguments, const bool help_requested, const logging::CliOverrides& logging_options) {
 switch (descriptor.command) {
  case rfdetr::RfdetrCommand::Compile: {
   if (help_requested) {
    print_compile_help(descriptor);
    return 0;
   }
   auto request = parse_request<CompileCliRequest>(arguments, kCompileOptions);
   finalize_compile_request(request);
   run_compile(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::Info: {
   if (help_requested) {
    print_info_help(descriptor);
    return 0;
   }
   const auto request = parse_request<InfoCliRequest>(arguments, kInfoOptions);
   if (static_cast<unsigned>(!request.onnx_path.empty()) + static_cast<unsigned>(!request.tensorrt_path.empty()) != 1U) {
    throw std::runtime_error(
     "rfdetr info requires exactly one of --onnx or "
     "--tensorrt");
   }
   if (!request.onnx_path.empty()) exec_onnx_info_tool(request.onnx_path, logging_options);
   rfdetr::ModelArtifactRequest artifacts;
   artifacts.tensorrt_path = request.tensorrt_path;
   const rfdetr::ModelInfo info = rfdetr::inspect_tensorrt_model(artifacts, request.device_id);
   rfdetr::print_model_metadata(info, 0U, 0U, rfdetr::ValidationLogMode::Interactive);
   return 0;
  }
  case rfdetr::RfdetrCommand::BuildEngine: {
   if (help_requested) {
    print_build_engine_help(descriptor);
    return 0;
   }
   const auto request = parse_request<BuildEngineRequest>(arguments, kBuildEngineOptions);
   rfdetr::build_tensorrt_engine(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::ExportOnnx: {
   if (help_requested) {
    print_export_onnx_help(descriptor);
    return 0;
   }
   const auto request = parse_request<ExportOnnxRequest>(arguments, kExportOnnxOptions);
   rfdetr::export_onnx(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::Predict: {
   if (help_requested) {
    print_predict_help(descriptor);
    return 0;
   }
   auto state = parse_request<PredictCliRequest>(arguments, kPredictOptions);
   finalize_predict_request(state);
   const auto result = rfdetr::run_prediction(state.request);
   rfdetr::print_prediction_summary(state.request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Evaluate: {
   if (help_requested) {
    print_evaluate_help(descriptor);
    return 0;
   }
   auto request = parse_request<EvaluateRequest>(arguments, kEvaluateOptions);
   if (request.compiled_path.empty()) throw std::runtime_error("rfdetr evaluate requires --compiled");
   request = rfdetr::finalize_evaluate_request(std::move(request));
   const auto result = rfdetr::run_evaluation(request);
   rfdetr::print_evaluation_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Validate: {
   if (help_requested) {
    print_validate_help(descriptor);
    return 0;
   }
   auto request = parse_request<ValidateRequest>(arguments, kValidateOptions);
   if (request.compiled_path.empty()) throw std::runtime_error("rfdetr validate requires --compiled");
   request = rfdetr::finalize_validate_request(std::move(request));
   const auto result = rfdetr::run_validation(request);
   rfdetr::write_validation_report(request, result);
   rfdetr::print_validation_run_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Train: {
   if (help_requested) {
    print_train_help(descriptor);
    return 0;
   }
   auto state = parse_train_request(arguments);
   auto request = rfdetr::finalize_train_request(std::move(state.request));
   if (!request.distributed_worker && request.device_ids.size() > 1U) { return DistributedTrainingProcess::Run(request); }
   const auto result = rfdetr::run_training(request);
   rfdetr::print_training_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::NormalizeWeights: {
   if (help_requested) {
    print_normalize_help(descriptor);
    return 0;
   }
   auto request = parse_request<NormalizeWeightsRequest>(arguments, kNormalizeOptions);
   if (request.input_path.empty() || request.output_path.empty()) { throw std::runtime_error("rfdetr normalize-weights requires --input and --output"); }
   request.input_path = std::filesystem::absolute(request.input_path).lexically_normal();
   request.output_path = std::filesystem::absolute(request.output_path).lexically_normal();
   const auto checkpoint = rfdetr::normalize_checkpoint_to_native(request.input_path, request.output_path, request.class_layout_path);
   if (logging::enabled(spdlog::level::info)) {
    logging::info("rfdetr.cli",
     [&](auto& current) { current.info("rfdetr.normalize-weights: wrote {} tensors for preset={} to {}", checkpoint.tensor_count(), checkpoint.metadata.preset_name, request.output_path.string()); });
   } else {
    std::printf("rfdetr.normalize-weights: wrote %zu tensors for preset=%s to %s\n", checkpoint.tensor_count(), checkpoint.metadata.preset_name.c_str(), request.output_path.c_str());
   }
   return 0;
  }
 }
 throw std::logic_error("RF-DETR command vocabulary is incomplete");
}
void print_rfdetr_help() {
 std::fputs(
  "RF-DETR model tooling for compilation, inference, evaluation, and "
  "training\nUsage: mmltk rfdetr <command>\nCommands:",
  stdout);
 for (const auto& command : rfdetr::kRfdetrCommands) mmltk::entrypoints::cli::print_command_help_line(command.name, command.description);
 std::fputc('\n', stdout);
}
}  // namespace
int handle_rfdetr_cli(const std::span<const std::string_view> arguments, const logging::CliOverrides& logging_options) {
 if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h") {
  print_rfdetr_help();
  return 0;
 }
 const auto command = rfdetr::parse_rfdetr_command(arguments.front());
 if (!command) {
  logging::report_fatal("mmltk rfdetr unknown command", arguments.front());
  return 1;
 }
 const auto* descriptor = rfdetr::rfdetr_command_descriptor(*command);
 if (descriptor == nullptr) throw std::logic_error("RF-DETR command descriptor is missing");
 const auto command_arguments = arguments.subspan(1U);
 const bool help_requested =
  std::ranges::find(command_arguments, std::string_view{"--help"}) != command_arguments.end() || std::ranges::find(command_arguments, std::string_view{"-h"}) != command_arguments.end();
 try {
  return dispatch_command(*descriptor, command_arguments, help_requested, logging_options);
 } catch (const rfdetr::TrainingPeerCancelled&) {
  // The session's rank-zero terminal owns the already-claimed first cause.
  return 1;
 } catch (const std::exception& error) { logging::report_fatal("mmltk rfdetr error", error.what(), std::nullopt, "rfdetr.cli"); } catch (...) {
  logging::report_fatal("mmltk rfdetr error", "unknown exception", std::nullopt, "rfdetr.cli");
 }
 return 1;
}
}  // namespace mmltk::entrypoints::cli
