#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::entrypoints::cli::rfdetr_options {
namespace data = mmltk::backend::data;
namespace reflection = mmltk::frameworks::reflection;
namespace rfdetr = mmltk::backend::models::rfdetr;
using reflection::custom_option;
using reflection::negative_flag;
using reflection::option_with_item_policy;
using rfdetr::BuildEngineRequest;
using rfdetr::EvaluateRequest;
using rfdetr::ExportOnnxRequest;
using rfdetr::GpuAugmentationConfig;
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
using Compile = reflection::CliScope<CompileCliRequest>;
using Info = reflection::CliScope<InfoCliRequest>;
using Normalize = reflection::CliScope<NormalizeWeightsRequest>;
using Build = reflection::CliScope<BuildEngineRequest>;
using Export = reflection::CliScope<ExportOnnxRequest>;
using Evaluate = reflection::CliScope<EvaluateRequest>;
using Validate = reflection::CliScope<ValidateRequest>;
using Predict = reflection::CliScope<PredictCliRequest>::within<&PredictCliRequest::request>;
using TrainInput = reflection::CliScope<TrainCliRequest>;
using Train = TrainInput::within<&TrainCliRequest::request>;
using Recipe = Train::within<&TrainRequest::recipe>;
using Augmentation = Train::within<&TrainRequest::gpu_augmentation>;
using Supervision = Train::within<&TrainRequest::training_supervision>;
using MatchFree = Supervision::within<&TrainingSupervisionConfig::match_free>;
using Denoising = Supervision::within<&TrainingSupervisionConfig::denoising>;
inline constexpr std::array kCompileOptions{
 MMLTK_CLI_OPTION(Compile, resize_mode, "Image geometry: Stretch or Letterbox", "Dataset"), MMLTK_CLI_OPTION(Compile, perceptual_downscale, "Perceptual shrinking", "Dataset"),
 MMLTK_CLI_OPTION(Compile, source_dir, "Source dataset root", "Dataset"), MMLTK_CLI_OPTION(Compile, output_dir, "Compiled output directory", "Dataset"),
 MMLTK_CLI_OPTION(Compile, cache_dir, "Benchmark cache root", "Dataset"), MMLTK_CLI_NAMED(Compile, benchmark_resolution, "--compile-benchmark-dataset", "Compile benchmark dataset", "Dataset"),
 MMLTK_CLI_OPTION(Compile, overwrite, "Replace benchmark output", "Dataset"), MMLTK_CLI_OPTION(Compile, resolution, "Square image resolution", "Dataset"),
 MMLTK_CLI_NAMED(Compile, num_workers, "--workers", "CPU worker budget", "Dataset"), MMLTK_CLI_OPTION(Compile, cuda_mask_batch_size, "CUDA mask batch size", "Dataset"),
 MMLTK_CLI_OPTION(Compile, cuda_device_id, "CUDA device id", "Dataset")
};
inline constexpr std::array kInfoOptions{
 MMLTK_CLI_NAMED(Info, onnx_path, "--onnx", "ONNX model path", "Model input"), MMLTK_CLI_NAMED(Info, tensorrt_path, "--tensorrt", "TensorRT engine path", "Model input"),
 MMLTK_CLI_OPTION(Info, device_id, "CUDA device id", "Execution")
};
inline constexpr std::array kNormalizeOptions{
 MMLTK_CLI_NAMED(Normalize, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"),
 MMLTK_CLI_NAMED(Normalize, input_path, "--input", "Input upstream checkpoint", "Input and output"), MMLTK_CLI_NAMED(Normalize, output_path, "--output", "Output native checkpoint", "Input and output")
};
inline constexpr std::array kBuildEngineOptions{
 MMLTK_CLI_NAMED(Build, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"), MMLTK_CLI_NAMED(Build, onnx_path, "--onnx", "ONNX model path", "Input and output"),
 MMLTK_CLI_NAMED(Build, output_path, "--output", "Output TensorRT engine path", "Input and output"), MMLTK_CLI_NAMED(Build, preset_name, "--preset", "Declared RF-DETR preset", "Input and output"),
 MMLTK_CLI_OPTION(Build, resolution, "Square model resolution", "Input and output"), MMLTK_CLI_OPTION(Build, device_id, "CUDA device id", "Execution"),
 MMLTK_CLI_NAMED(Build, allow_fp16, "--fp16", "Enable FP16 TensorRT kernels", "Execution", {}, "--no-fp16")
};
inline constexpr std::array kExportOnnxOptions{
 MMLTK_CLI_NAMED(Export, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"),
 MMLTK_CLI_NAMED(Export, weights_path, "--weights", "RF-DETR checkpoint path", "Input and output"),
 // CLEANUP-IGNORE: Export and TensorRT build expose separate reflected request schemas despite shared artifact
 // fields.
 MMLTK_CLI_NAMED(Export, output_path, "--output", "Output ONNX path", "Input and output"), MMLTK_CLI_NAMED(Export, preset_name, "--preset", "Declared RF-DETR preset", "Input and output"),
 MMLTK_CLI_OPTION(Export, resolution, "Square model resolution", "Input and output"), MMLTK_CLI_OPTION(Export, device_id, "CUDA device id", "Execution"),
 MMLTK_CLI_OPTION(Export, opset_version, "ONNX opset version", "Execution"),
 // CLEANUP-IGNORE: Adjacent command arrays join an Export tail to separately typed Evaluate loading options.
 MMLTK_CLI_OPTION(Export, simplify, "Run ONNX validation", "Execution")
};
inline constexpr std::array kEvaluateOptions{
 MMLTK_CLI_NAMED(Evaluate, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<EvaluateRequest, &EvaluateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 MMLTK_CLI_OPTION(Evaluate, numa_node, "GPU-local NUMA node (-1 automatic)", "Execution"), MMLTK_CLI_NAMED(Evaluate, compiled_path, "--compiled", "Compiled dataset split", "Dataset"),
 MMLTK_CLI_NAMED(Evaluate, weights_path, "--weights", "RF-DETR checkpoint path", "Model input"), MMLTK_CLI_NAMED(Evaluate, onnx_path, "--onnx", "ONNX model path", "Model input"),
 MMLTK_CLI_NAMED(Evaluate, tensorrt_path, "--tensorrt", "TensorRT engine path", "Model input"), MMLTK_CLI_NAMED(Evaluate, preset_name, "--preset", "Declared preset", "Model input"),
 MMLTK_CLI_OPTION(Evaluate, resolution, "Square model resolution", "Model input"), MMLTK_CLI_OPTION(Evaluate, candidate_count, "Selected candidate count (0: admitted model default)", "Model input"),
 MMLTK_CLI_OPTION(Evaluate, batch_size, "Evaluation batch size", "Execution"), MMLTK_CLI_OPTION(Evaluate, device_id, "CUDA device id", "Execution"),
 MMLTK_CLI_OPTION(Evaluate, limit_images, "Image limit", "Execution"), MMLTK_CLI_OPTION(Evaluate, eval_max_dets, "Detection cap", "Execution"),
 MMLTK_CLI_OPTION(Evaluate, workers, "Dataset worker count", "Execution"), MMLTK_CLI_OPTION(Evaluate, lanes, "Parallel backend lanes", "Execution"),
 MMLTK_CLI_OPTION(Evaluate, cpu_affinity, "Linux CPU list", "Execution"), MMLTK_CLI_OPTION(Evaluate, backend, "Backend preference", "Execution"),
 MMLTK_CLI_NAMED(Evaluate, allow_fp16, "--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
 MMLTK_CLI_NAMED(Evaluate, progress_bar, "--progress", "Render progress", "Execution", {}, "--no-progress"),
 MMLTK_CLI_NAMED(Evaluate, compilation_mode, "--compile-mode", "Native compilation mode", "Execution")
};
inline constexpr std::array kPredictOptions{
 MMLTK_CLI_NAMED(Predict, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"), MMLTK_CLI_OPTION(Predict, numa_node, "GPU-local NUMA node (-1 automatic)", "Execution"),
 negative_flag<PredictCliRequest, Predict::path<&PredictRequest::h2d_dataloader>>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 MMLTK_CLI_NAMED(Predict, compiled_path, "--compiled", "Compiled dataset split (.bin)", "Dataset"),
 option_with_item_policy<PredictCliRequest, &PredictCliRequest::image_paths, &rfdetr::PredictImageInput::image_path>("--image", "Input image path; repeat for multiple images", "Dataset"),
 MMLTK_CLI_NAMED(Predict, output_path, "--output", "Prediction JSON output path", "Dataset"), MMLTK_CLI_NAMED(Predict, weights_path, "--weights", "RF-DETR checkpoint path", "Model input"),
 MMLTK_CLI_NAMED(Predict, onnx_path, "--onnx", "ONNX model path", "Model input"), MMLTK_CLI_NAMED(Predict, tensorrt_path, "--tensorrt", "TensorRT engine path", "Model input"),
 MMLTK_CLI_NAMED(Predict, preset_name, "--preset", "Declared RF-DETR preset architecture", "Model input"), MMLTK_CLI_OPTION(Predict, resolution, "Square model input resolution", "Model input"),
 MMLTK_CLI_OPTION(Predict, batch_size, "Batch size for inference", "Execution"), MMLTK_CLI_OPTION(Predict, max_dets_per_image, "Maximum saved detections per image", "Execution"),
 MMLTK_CLI_OPTION(Predict, device_id, "CUDA device id", "Execution"), MMLTK_CLI_OPTION(Predict, threshold, "Minimum score threshold", "Execution"),
 MMLTK_CLI_OPTION(Predict, workers, "Dataset worker count", "Execution"), MMLTK_CLI_OPTION(Predict, lanes, "Parallel backend lane count", "Execution"),
 MMLTK_CLI_OPTION(Predict, cpu_affinity, "Linux CPU list", "Execution"), MMLTK_CLI_OPTION(Predict, backend, "Backend preference", "Execution"),
 MMLTK_CLI_NAMED(Predict, allow_fp16, "--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
 MMLTK_CLI_NAMED(Predict, progress_bar, "--progress", "Render interactive progress", "Execution", {}, "--no-progress"),
 MMLTK_CLI_NAMED(Predict, compilation_mode, "--compile-mode", "Native compilation mode", "Execution")
};
inline constexpr std::array kValidateOptions{
 MMLTK_CLI_NAMED(Validate, compile_resize_mode, "--resize-mode", "Compile image geometry: Stretch or Letterbox", "Dataset"),
 MMLTK_CLI_NAMED(Validate, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<ValidateRequest, &ValidateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 MMLTK_CLI_OPTION(Validate, numa_node, "GPU-local NUMA node (-1 automatic)", "Execution"), MMLTK_CLI_NAMED(Validate, compiled_path, "--compiled", "Compiled dataset split (.bin)", "Dataset"),
 MMLTK_CLI_NAMED(Validate, source_dir, "--source", "Source dataset root", "Dataset"), MMLTK_CLI_OPTION(Validate, split, "Source split", "Dataset"),
 MMLTK_CLI_OPTION(Validate, resolution, "Square resolution", "Dataset"), MMLTK_CLI_OPTION(Validate, recompile, "Recompile source dataset", "Dataset"),
 MMLTK_CLI_OPTION(Validate, compile_workers, "Compile worker count", "Dataset"), MMLTK_CLI_OPTION(Validate, compile_cuda_mask_batch_size, "CUDA mask batch size", "Dataset"),
 MMLTK_CLI_OPTION(Validate, compile_cuda_device_id, "CUDA device id", "Dataset"), MMLTK_CLI_NAMED(Validate, weights_path, "--weights", "RF-DETR checkpoint path", "Model input"),
 MMLTK_CLI_NAMED(Validate, preset_name, "--preset", "Declared RF-DETR preset", "Model input"), MMLTK_CLI_NAMED(Validate, onnx_path, "--onnx", "ONNX model path", "Model input"),
 MMLTK_CLI_NAMED(Validate, tensorrt_path, "--tensorrt", "TensorRT engine path", "Model input"),
 MMLTK_CLI_NAMED(Validate, save_engine_path, "--save-engine", "Write generated TensorRT engine", "Model input"),
 MMLTK_CLI_NAMED(Validate, report_json_path, "--report-json", "Validation report JSON path", "Output"), MMLTK_CLI_OPTION(Validate, eval_order, "Backend evaluation order", "Output"),
 MMLTK_CLI_OPTION(Validate, batch_size, "Evaluation batch size", "Execution"), MMLTK_CLI_OPTION(Validate, limit_images, "Image limit", "Execution"),
 MMLTK_CLI_OPTION(Validate, candidate_count, "Selected candidate count (0: admitted model default)", "Execution"), MMLTK_CLI_OPTION(Validate, eval_max_dets, "Detection cap", "Execution"),
 MMLTK_CLI_OPTION(Validate, alignment_images, "Backend alignment sample count", "Execution"), MMLTK_CLI_OPTION(Validate, prefetch_factor, "Dataset prefetch factor", "Execution"),
 MMLTK_CLI_OPTION(Validate, device_id, "CUDA device id", "Execution"), MMLTK_CLI_OPTION(Validate, workers, "Dataset worker count", "Execution"),
 MMLTK_CLI_OPTION(Validate, lanes, "Parallel backend lanes", "Execution"), MMLTK_CLI_OPTION(Validate, cpu_affinity, "Linux CPU list", "Execution"),
 MMLTK_CLI_NAMED(Validate, allow_fp16, "--fp16", "Enable FP16", "Execution", {}, "--no-fp16"), MMLTK_CLI_OPTION(Validate, log_mode, "Validation logging mode", "Execution"),
 MMLTK_CLI_OPTION(Validate, profile, "Collect validation profile", "Execution"), MMLTK_CLI_OPTION(Validate, write_report_json, "Write validation report", "Output", {}, "--no-write-report-json")
};
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
// These six groups share exactly three CLI projections; keep their order local.
#define MMLTK_AUGMENTATION_OPTIONS(Member)                                                                                                                                \
 MMLTK_CLI_NAMED(Augmentation::within<&GpuAugmentationConfig::Member>, probability, "--aug-" #Member "-prob", #Member " selection probability", "GPU augmentation"),      \
  MMLTK_CLI_NAMED(Augmentation::within<&GpuAugmentationConfig::Member>, min_strength, "--aug-" #Member "-min-strength", #Member " minimum strength", "GPU augmentation"), \
  MMLTK_CLI_NAMED(Augmentation::within<&GpuAugmentationConfig::Member>, max_strength, "--aug-" #Member "-max-strength", #Member " maximum strength", "GPU augmentation")
inline constexpr std::array kTrainOptions{
 MMLTK_CLI_OPTION(TrainInput, request_json, "Complete canonical training request (maximum 64 KiB)", "Training"),
 MMLTK_CLI_OPTION(Recipe, nesterov, "SGD Nesterov momentum", "Optimization", {}, "--no-nesterov"), MMLTK_CLI_OPTION(Recipe, warmup_bias_lr, "Absolute SGD bias warmup learning rate", "Optimization"),
 MMLTK_CLI_NAMED(Train, class_layout_path, "--class-layout", "Digest-bound class descriptor", "Model input"),
 negative_flag<TrainCliRequest, Train::path<&TrainRequest::h2d_dataloader>>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 custom_option<TrainCliRequest, Train::path<&TrainRequest::numa_nodes>, &assign_train_integer_list<&TrainRequest::numa_nodes, false>, &emit_train_integer_list<&TrainRequest::numa_nodes>>(
  "--numa-nodes", "Comma-separated NUMA overrides in selected device order (-1 automatic)", "Execution"),
 MMLTK_CLI_NAMED(Train, train_compiled_path, "--train-compiled", "Compiled training split", "Dataset"),
 MMLTK_CLI_NAMED(Train, val_compiled_path, "--val-compiled", "Compiled validation split", "Dataset"), MMLTK_CLI_NAMED(Train, test_compiled_path, "--test-compiled", "Compiled test split", "Dataset"),
 MMLTK_CLI_OPTION(Train, resolution, "Square model input resolution", "Dataset"), MMLTK_CLI_OPTION(Train, output_dir, "Output directory", "Checkpoint"),
 MMLTK_CLI_NAMED(Train, weights_path, "--weights", "Source checkpoint", "Checkpoint"), MMLTK_CLI_NAMED(Train, resume_path, "--resume", "Native checkpoint to resume", "Checkpoint"),
 MMLTK_CLI_NAMED(Train, preset_name, "--preset", "Declared preset", "Checkpoint"), MMLTK_CLI_OPTION(Train, num_queries, "Native query count", "Checkpoint"),
 MMLTK_CLI_OPTION(Train, batch_size, "Global microbatch image count", "Optimization"), MMLTK_CLI_OPTION(Train, val_batch_size, "Validation batch size", "Optimization"),
 MMLTK_CLI_OPTION(Train, epochs, "Epoch count", "Optimization"), MMLTK_CLI_OPTION(Train, grad_accum_steps, "Gradient accumulation", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, optimizer, "adamw, muon, or sgd", "Optimization"), MMLTK_CLI_OPTION(Recipe, lr, "Decoder learning rate", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, lr_encoder, "Encoder learning rate", "Optimization"), MMLTK_CLI_OPTION(Recipe, momentum, "Muon or SGD momentum", "Optimization"),
 MMLTK_CLI_OPTION(Train, freeze_encoder, "Freeze encoder", "Optimization", {}, "--no-freeze-encoder"), MMLTK_CLI_OPTION(Recipe, lr_component_decay, "Component decay", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, encoder_layer_decay, "Layer decay", "Optimization"), MMLTK_CLI_OPTION(Recipe, weight_decay, "Weight decay", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, lr_drop, "Step drop epoch", "Optimization"), MMLTK_CLI_OPTION(Recipe, lr_scheduler, "step, cosine, or ultralytics-linear", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, lr_min_factor, "Minimum LR multiplier", "Optimization"), MMLTK_CLI_OPTION(Recipe, warmup_epochs, "Warmup duration", "Optimization"),
 MMLTK_CLI_OPTION(Recipe, warmup_momentum, "Warmup momentum", "Optimization"), MMLTK_CLI_OPTION(Train, clip_max_norm, "Gradient clipping norm", "Optimization"),
 MMLTK_CLI_OPTION(Train, fused_optimizer, "Use fused AdamW backend (AdamW only)", "Optimization", {}, "--no-fused-optimizer"),
 MMLTK_CLI_OPTION(Train, use_ema, "Maintain EMA", "Optimization", {}, "--no-ema"), MMLTK_CLI_OPTION(Train, validation_loss, "Calculate validation loss", "Optimization", {}, "--no-validation-loss"),
 MMLTK_CLI_OPTION(Train, validation_profile, "Write validation profile", "Optimization", {}, "--no-validation-profile"), MMLTK_CLI_OPTION(Train, ema_decay, "EMA decay", "Optimization"),
 MMLTK_CLI_OPTION(Train, ema_tau, "EMA tau", "Optimization"), MMLTK_CLI_OPTION(Train, eval_max_dets, "Detection cap", "Optimization"),
 MMLTK_CLI_OPTION(Supervision, assignment, "hungarian or match-free", "Supervision"), MMLTK_CLI_NAMED(MatchFree, rho, "--match-free-rho", "Sparse correspondence threshold", "Supervision"),
 MMLTK_CLI_NAMED(MatchFree, correspondence_weight, "--match-free-correspondence-weight", "Correspondence objective weight", "Supervision"),
 MMLTK_CLI_NAMED(MatchFree, query_weight, "--match-free-query-weight", "Query objective weight", "Supervision"),
 MMLTK_CLI_NAMED(Denoising, enabled, "--dn", "Enable denoising supervision", "Supervision", {}, "--no-dn"), MMLTK_CLI_NAMED(Denoising, groups, "--dn-groups", "Denoising group count", "Supervision"),
 MMLTK_CLI_NAMED(Denoising, label_noise_ratio, "--dn-label-noise-ratio", "Denoising label flip probability", "Supervision"),
 MMLTK_CLI_NAMED(Denoising, center_noise_scale, "--dn-center-noise-scale", "Denoising center noise scale", "Supervision"),
 MMLTK_CLI_NAMED(Denoising, size_noise_scale, "--dn-size-noise-scale", "Denoising size noise scale", "Supervision"),
 MMLTK_CLI_NAMED(Augmentation, enabled, "--gpu-augment", "Apply GPU augmentation", "GPU augmentation", {}, "--no-gpu-augment"),
 MMLTK_CLI_NAMED(Augmentation, perceptual_downscale, "--aug-perceptual-downscale", "Perceptual shrinking", "GPU augmentation", {}, "--no-aug-perceptual-downscale"),
 MMLTK_AUGMENTATION_OPTIONS(geometry), MMLTK_AUGMENTATION_OPTIONS(resize), MMLTK_AUGMENTATION_OPTIONS(color), MMLTK_AUGMENTATION_OPTIONS(noise), MMLTK_AUGMENTATION_OPTIONS(blur),
 MMLTK_AUGMENTATION_OPTIONS(occlusion), MMLTK_CLI_NAMED(Augmentation, copy_paste_probability, "--aug-copy-paste-prob", "Copy-paste probability", "GPU augmentation"),
 MMLTK_CLI_OPTION(Train, numa_node, "GPU-local NUMA node override (-1 selects known locality)", "Execution"), MMLTK_CLI_OPTION(Train, device_id, "Single CUDA device", "Execution"),
 custom_option<TrainCliRequest, Train::path<&TrainRequest::device_ids>, &assign_train_integer_list<&TrainRequest::device_ids, true>, &emit_train_integer_list<&TrainRequest::device_ids>>(
  "--device-ids", "Comma-separated CUDA device ids", "Execution"),
 MMLTK_CLI_OPTION(Train, workers, "Dataset worker count", "Execution"), MMLTK_CLI_OPTION(Train, lanes, "Parallel backend lanes", "Execution"),
 MMLTK_CLI_OPTION(Train, validation_lanes, "Parallel training validation lanes", "Execution"),
 MMLTK_CLI_OPTION(Train, unfreeze_encoder_last_epochs, "Unfreeze the encoder for the final epochs", "Optimization"),
 MMLTK_CLI_OPTION(Train, disable_augmentation_last_epochs, "Disable augmentation for the final epochs", "Optimization"), MMLTK_CLI_OPTION(Train, cpu_affinity, "Linux CPU list", "Execution"),
 MMLTK_CLI_OPTION(Train, prefetch_factor, "Prefetch factor", "Execution"), MMLTK_CLI_OPTION(Train, print_freq, "Logging frequency", "Execution"),
 MMLTK_CLI_OPTION(Train, seed, "Random seed", "Execution"), MMLTK_CLI_OPTION(Train, amp, "Automatic mixed precision", "Execution", {}, "--no-amp"),
 MMLTK_CLI_NAMED(Train, progress_bar, "--progress", "Render progress", "Execution", {}, "--no-progress"),
 MMLTK_CLI_NAMED(Train, compilation_mode, "--compile-mode", "Native compilation mode", "Execution"),
 MMLTK_CLI_NAMED(Train, distributed_worker, "--dist-worker", "Internal distributed worker", "Distributed (internal)"),
 MMLTK_CLI_NAMED(Train, distributed_rank, "--dist-rank", "Worker rank", "Distributed (internal)"),
 MMLTK_CLI_NAMED(Train, distributed_world_size, "--dist-world-size", "Worker world size", "Distributed (internal)"),
 MMLTK_CLI_NAMED(Train, distributed_store_path, "--dist-store-file", "Rendezvous file", "Distributed (internal)")
};
#undef MMLTK_AUGMENTATION_OPTIONS
[[nodiscard]] consteval bool train_descriptor_relation_is_complete() {
 using Relation = reflection::catalog_provider_relation<TrainRecipeCatalog>;
 constexpr auto selector = Recipe::path<&TrainRecipeSettings::optimizer>;
 std::array<reflection::ReflectedMemberIdentity, Relation::member_count> relation_identities{};
 std::size_t count = 0U;
 Relation::VisitMembers([&]<class Entry>() {
  constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, TrainRecipeSettings>(selector, Entry::destination);
  const auto identity = reflection::accessor_member_identity<TrainCliRequest, destination>();
  (void)reflection::unique_descriptor_index(kTrainOptions, identity);
  relation_identities[count++] = identity;
 });
 constexpr auto device_id = Train::path<&TrainRequest::device_id>;
 constexpr auto device_ids = Train::path<&TrainRequest::device_ids>;
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
inline constexpr std::array kBuildEngineUnexposed{
 reflection::unexposed<BuildEngineRequest, &BuildEngineRequest::weights_path>("build-engine accepts the selected ONNX artifact kind"),
 reflection::unexposed<BuildEngineRequest, &BuildEngineRequest::tensorrt_path>("build-engine accepts the selected ONNX artifact kind")
};
inline constexpr std::array kExportOnnxUnexposed{
 reflection::unexposed<ExportOnnxRequest, &ExportOnnxRequest::onnx_path>("export-onnx accepts the selected native-weights artifact kind"),
 reflection::unexposed<ExportOnnxRequest, &ExportOnnxRequest::tensorrt_path>("export-onnx accepts the selected native-weights artifact kind")
};
inline constexpr std::array kPredictUnexposed{
 reflection::unexposed<PredictCliRequest, Predict::path<&PredictRequest::video_path>>("local video selection belongs to the GUI prediction workflow"),
 reflection::unexposed<PredictCliRequest, Predict::path<&PredictRequest::image_inputs>>("the CLI bounded image_paths collection derives the final image-input "
                                                                                        "records once in finalize_predict_request")
};
static_assert((reflection::audit_descriptors(kBuildEngineOptions, kBuildEngineUnexposed), true));
static_assert((reflection::audit_descriptors(kExportOnnxOptions, kExportOnnxUnexposed), true));
static_assert((reflection::audit_descriptors(kEvaluateOptions), true));
static_assert((reflection::audit_descriptors(kPredictOptions, kPredictUnexposed), true));
static_assert((reflection::audit_descriptors(kValidateOptions), true));
inline constexpr std::array kTrainUnexposed{
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::lane_configuration, &TrainLaneConfiguration::models>>("--request-json owns the complete model recipes and identities"),
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::lane_configuration, &TrainLaneConfiguration::next_model_id>>("--request-json owns the model identity frontier"),
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::lane_configuration, &TrainLaneConfiguration::merge_rounds>>("--request-json owns the complete merge policy"),
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::data_policy, &TrainDataPolicy::rare_threshold>>("--request-json owns the complete sampling policy"),
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::data_policy, &TrainDataPolicy::maximum_repeat_factor>>("--request-json owns the complete sampling policy"),
 reflection::unexposed<TrainCliRequest, Train::path<&TrainRequest::data_policy, &TrainDataPolicy::maximum_draw_multiplier>>("--request-json owns the complete sampling policy")
};
static_assert((reflection::audit_descriptors(kTrainOptions, kTrainUnexposed), true));
}  // namespace mmltk::entrypoints::cli::rfdetr_options
