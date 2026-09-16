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
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/models/rfdetr/training/distributed_train_launcher.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/training/train_recipe.h"
#include "src/common/system/runtime_paths.h"
#include "src/controller/services/train_command.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

import mmltk.backend.models.rfdetr.core.tool_launch_utils;
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
import mmltk.backend.models.rfdetr.training.checkpoint;
import mmltk.common.logging.mmltk_logging;

namespace data = mmltk::backend::data;
namespace logging = mmltk::common::logging;
namespace reflection = mmltk::frameworks::reflection;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace services = mmltk::controller::services;

namespace mmltk::entrypoints::cli {
namespace {

struct CompileCliRequest final {
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path source_dir;
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path output_dir;
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path cache_dir;
    [[= reflection::Minimum<int>{1}]][[= reflection::Maximum<int>{static_cast<int>(data::MAX_IMAGE_EXTENT)}]] int resolution = 432;
    [[= reflection::Minimum<int>{0}]][[= reflection::Maximum<int>{static_cast<int>(data::MAX_IMAGE_EXTENT)}]] int benchmark_resolution = 0;
    [[= reflection::Minimum<int>{-1}]] int num_workers = -1;
    [[= reflection::Minimum<int>{0}]] int cuda_mask_batch_size = 0;
    [[= reflection::Minimum<int>{0}]] int cuda_device_id = 0;
    bool overwrite = false;
    bool perceptual_downscale = false;
};

struct InfoCliRequest final {
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path onnx_path;
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path tensorrt_path;
    [[= reflection::Minimum<int>{0}]] int device_id = 0;
};

struct NormalizeWeightsRequest final {
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path class_layout_path;
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path input_path;
    [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::filesystem::path output_path;
};

struct PredictCliRequest final {
    rfdetr::PredictRequest request;
    [[= reflection::MaxItems{reflection::kMaximumCliImageInputs}]] std::vector<std::filesystem::path> image_paths;
};

struct TrainCliRequest final {
    rfdetr::TrainRequest request;
};

MMLTK_REFLECT_FIELDS(CompileCliRequest)
MMLTK_REFLECT_FIELDS(InfoCliRequest)
MMLTK_REFLECT_FIELDS(NormalizeWeightsRequest)
MMLTK_REFLECT_FIELDS(PredictCliRequest)
MMLTK_REFLECT_FIELDS(TrainCliRequest)

inline constexpr std::array kCompileOptions{
    reflection::option<CompileCliRequest, &CompileCliRequest::perceptual_downscale>("--perceptual-downscale", "Perceptual shrinking", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::source_dir>("--source-dir", "Source dataset root", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::output_dir>("--output-dir", "Compiled output directory", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::cache_dir>("--cache-dir", "Benchmark cache root", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::benchmark_resolution>("--compile-benchmark-dataset",
                                                                                    "Compile benchmark dataset", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::overwrite>("--overwrite", "Replace benchmark output", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::resolution>("--resolution", "Square image resolution", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::num_workers>("--workers", "CPU worker budget", "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::cuda_mask_batch_size>("--cuda-mask-batch-size", "CUDA mask batch size",
                                                                                    "Dataset"),
    reflection::option<CompileCliRequest, &CompileCliRequest::cuda_device_id>("--cuda-device-id", "CUDA device id", "Dataset")};

inline constexpr std::array kInfoOptions{
    reflection::option<InfoCliRequest, &InfoCliRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
    reflection::option<InfoCliRequest, &InfoCliRequest::tensorrt_path>("--tensorrt", "TensorRT engine path", "Model input"),
    reflection::option<InfoCliRequest, &InfoCliRequest::device_id>("--device-id", "CUDA device id", "Execution")};

inline constexpr std::array kNormalizeOptions{reflection::option<NormalizeWeightsRequest, &NormalizeWeightsRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
                                              reflection::option<NormalizeWeightsRequest, &NormalizeWeightsRequest::input_path>(
                                                  "--input", "Input upstream checkpoint", "Input and output"),
                                              reflection::option<NormalizeWeightsRequest, &NormalizeWeightsRequest::output_path>(
                                                  "--output", "Output native checkpoint", "Input and output")};

inline constexpr std::array kBuildEngineOptions{
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::onnx_path>("--onnx", "ONNX model path", "Input and output"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::output_path>("--output", "Output TensorRT engine path",
                                                                                             "Input and output"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::preset_name>("--preset", "Declared RF-DETR preset",
                                                                                             "Input and output"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::resolution>("--resolution", "Square model resolution",
                                                                                            "Input and output"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::device_id>("--device-id", "CUDA device id", "Execution"),
    reflection::option<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::allow_fp16>("--fp16", "Enable FP16 TensorRT kernels",
                                                                                            "Execution", {}, "--no-fp16")};

inline constexpr std::array kExportOnnxOptions{
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::weights_path>("--weights", "RF-DETR checkpoint path",
                                                                                            "Input and output"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::output_path>(
        // CLEANUP-IGNORE: Export and TensorRT build expose separate reflected request schemas despite shared artifact
        // fields.
        "--output", "Output ONNX path", "Input and output"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::preset_name>("--preset", "Declared RF-DETR preset",
                                                                                           "Input and output"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::resolution>("--resolution", "Square model resolution",
                                                                                          "Input and output"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::device_id>("--device-id", "CUDA device id", "Execution"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::opset_version>("--opset-version", "ONNX opset version",
                                                                                             "Execution"),
    reflection::option<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::simplify>(
        // CLEANUP-IGNORE: Adjacent command arrays join an Export tail to separately typed Evaluate loading options.
        "--simplify", "Run ONNX validation", "Execution")};

inline constexpr std::array kEvaluateOptions{
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::negative_flag<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading",
                                                                                                 "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::numa_node>("--numa-node", "GPU-local NUMA node (-1 automatic)",
                                                                                     "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::compiled_path>("--compiled", "Compiled dataset split", "Dataset"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::weights_path>("--weights", "RF-DETR checkpoint path",
                                                                                        "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::tensorrt_path>("--tensorrt", "TensorRT engine path",
                                                                                         "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::preset_name>("--preset", "Declared preset", "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::resolution>("--resolution", "Square model resolution",
                                                                                      "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::num_queries>("--num-queries", "Native query count",
                                                                                       "Model input"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::batch_size>("--batch-size", "Evaluation batch size", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::device_id>("--device-id", "CUDA device id", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::limit_images>("--limit-images", "Image limit", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::eval_max_dets>("--eval-max-dets", "Detection cap", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::workers>("--workers", "Dataset worker count", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::lanes>("--lanes", "Parallel backend lanes", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::cpu_affinity>("--cpu-affinity", "Linux CPU list", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::backend>("--backend", "Backend preference", "Execution"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::allow_fp16>("--fp16", "Enable FP16", "Execution", {},
                                                                                      "--no-fp16"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::progress_bar>("--progress", "Render progress", "Execution", {},
                                                                                        "--no-progress"),
    reflection::option<rfdetr::EvaluateRequest, &rfdetr::EvaluateRequest::compilation_mode>("--compile-mode", "Native compilation mode",
                                                                                            "Execution")};

inline constexpr std::array kPredictOptions{
    reflection::option<rfdetr::PredictRequest, &rfdetr::PredictRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::numa_node>>(
        "--numa-node", "GPU-local NUMA node (-1 automatic)", "Execution"),
    reflection::negative_flag<PredictCliRequest,
                              reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::h2d_dataloader>>(
        "--gdrcopy", "Use GDRCopy image loading", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::compiled_path>>(
        "--compiled", "Compiled dataset split (.bin)", "Dataset"),
    reflection::option_with_item_policy<PredictCliRequest, &PredictCliRequest::image_paths, &rfdetr::PredictImageInput::image_path>(
        "--image", "Input image path; repeat for multiple images", "Dataset"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::output_path>>(
        "--output", "Prediction JSON output path", "Dataset"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::weights_path>>(
        "--weights", "RF-DETR checkpoint path", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::onnx_path>>(
        "--onnx", "ONNX model path", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::tensorrt_path>>(
        "--tensorrt", "TensorRT engine path", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::preset_name>>(
        "--preset", "Declared RF-DETR preset architecture", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::resolution>>(
        "--resolution", "Square model input resolution", "Model input"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::batch_size>>(
        "--batch-size", "Batch size for inference", "Execution"),
    reflection::option<PredictCliRequest,
                       reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::max_dets_per_image>>(
        "--max-dets-per-image", "Maximum saved detections per image", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::device_id>>(
        "--device-id", "CUDA device id", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::threshold>>(
        "--threshold", "Minimum score threshold", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::workers>>(
        "--workers", "Dataset worker count", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::lanes>>(
        "--lanes", "Parallel backend lane count", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::cpu_affinity>>(
        "--cpu-affinity", "Linux CPU list", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::backend>>(
        "--backend", "Backend preference", "Execution"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::allow_fp16>>(
        "--fp16", "Enable FP16", "Execution", {}, "--no-fp16"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::progress_bar>>(
        "--progress", "Render interactive progress", "Execution", {}, "--no-progress"),
    reflection::option<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::compilation_mode>>(
        "--compile-mode", "Native compilation mode", "Execution")};

inline constexpr std::array kValidateOptions{
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::negative_flag<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading",
                                                                                                 "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::numa_node>("--numa-node", "GPU-local NUMA node (-1 automatic)",
                                                                                     "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::compiled_path>("--compiled", "Compiled dataset split (.bin)",
                                                                                         "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::source_dir>("--source", "Source dataset root", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::split>("--split", "Source split", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::resolution>("--resolution", "Square resolution", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::recompile>("--recompile", "Recompile source dataset", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::compile_workers>("--compile-workers", "Compile worker count",
                                                                                           "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::compile_cuda_mask_batch_size>("--compile-cuda-mask-batch-size",
                                                                                                        "CUDA mask batch size", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::compile_cuda_device_id>("--compile-cuda-device-id",
                                                                                                  "CUDA device id", "Dataset"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::weights_path>("--weights", "RF-DETR checkpoint path",
                                                                                        "Model input"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::preset_name>("--preset", "Declared RF-DETR preset",
                                                                                       "Model input"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::onnx_path>("--onnx", "ONNX model path", "Model input"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::tensorrt_path>("--tensorrt", "TensorRT engine path",
                                                                                         "Model input"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::save_engine_path>(
        "--save-engine", "Write generated TensorRT engine", "Model input"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::report_json_path>("--report-json", "Validation report JSON path",
                                                                                            "Output"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::eval_order>("--eval-order", "Backend evaluation order", "Output"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::batch_size>("--batch-size", "Evaluation batch size", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::limit_images>("--limit-images", "Image limit", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::num_queries>("--num-queries", "Native query count", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::eval_max_dets>("--eval-max-dets", "Detection cap", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::alignment_images>("--alignment-images",
                                                                                            "Backend alignment sample count", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::prefetch_factor>("--prefetch-factor", "Dataset prefetch factor",
                                                                                           "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::device_id>("--device-id", "CUDA device id", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::workers>("--workers", "Dataset worker count", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::cpu_affinity>("--cpu-affinity", "Linux CPU list", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::allow_fp16>("--fp16", "Enable FP16", "Execution", {},
                                                                                      "--no-fp16"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::log_mode>("--log-mode", "Validation logging mode", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::profile>("--profile", "Collect validation profile", "Execution"),
    reflection::option<rfdetr::ValidateRequest, &rfdetr::ValidateRequest::write_report_json>(
        "--write-report-json", "Write validation report", "Output", {}, "--no-write-report-json")};

template <auto Member, bool Unique>
std::expected<void, reflection::ParseError> assign_train_integer_list(TrainCliRequest& state, const std::string_view text, bool,
                                                                      const reflection::FieldConstraint) {
    std::array<int, reflection::kMaximumTrainingDevices> ids{};
    std::size_t count = 0U;
    std::size_t start = 0U;
    while (start <= text.size()) {
        if (count == ids.size()) {
            return std::unexpected(reflection::ParseError{reflection::ParseErrorCode::InvalidValue, text,
                                                          "integer list exceeds the selected-device capacity"});
        }
        const std::size_t comma = text.find(',', start);
        const std::string_view item = text.substr(start, comma == std::string_view::npos ? text.size() - start : comma - start);
        int id = -1;
        const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), id);
        if (item.empty() || error != std::errc{} || end != item.data() + item.size() || id < (Unique ? 0 : -1) ||
            (Unique && std::find(ids.begin(), ids.begin() + count, id) != ids.begin() + count)) {
            return std::unexpected(reflection::ParseError{
                reflection::ParseErrorCode::InvalidValue, text,
                Unique ? "--device-ids requires unique non-negative integers" : "--numa-nodes requires integers >= -1"});
        }
        ids[count++] = id;
        if (comma == std::string_view::npos) break;
        start = comma + 1U;
    }
    (state.request.*Member).assign(ids.begin(), ids.begin() + count);
    return {};
}

template <auto Member>
void emit_train_integer_list(std::vector<std::string>& arguments, const TrainCliRequest& state, const std::string_view name,
                             std::string_view, reflection::OptionKind, bool) {
    if ((state.request.*Member).empty()) return;
    std::string joined;
    for (const int id : state.request.*Member) {
        if (!joined.empty()) joined.push_back(',');
        joined += std::to_string(id);
    }
    arguments.emplace_back(name);
    arguments.emplace_back(std::move(joined));
}

inline constexpr std::array kTrainOptions{
    reflection::option<TrainCliRequest, &TrainCliRequest::class_layout_path>("--class-layout", "Digest-bound class descriptor", "Model input"),
    reflection::negative_flag<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::h2d_dataloader>>(
        "--gdrcopy", "Use GDRCopy image loading", "Execution"),
    reflection::custom_option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::numa_nodes>,
                              &assign_train_integer_list<&rfdetr::TrainRequest::numa_nodes, false>,
                              &emit_train_integer_list<&rfdetr::TrainRequest::numa_nodes>>(
        "--numa-nodes", "Comma-separated NUMA overrides in selected device order (-1 automatic)", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::train_compiled_path>>(
        "--train-compiled", "Compiled training split", "Dataset"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::val_compiled_path>>(
        "--val-compiled", "Compiled validation split", "Dataset"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::test_compiled_path>>(
        "--test-compiled", "Compiled test split", "Dataset"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::resolution>>(
        "--resolution", "Square model input resolution", "Dataset"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::output_dir>>(
        "--output-dir", "Output directory", "Checkpoint"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::weights_path>>(
        "--weights", "Source checkpoint", "Checkpoint"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::resume_path>>(
        "--resume", "Native checkpoint to resume", "Checkpoint"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::preset_name>>(
        "--preset", "Declared preset", "Checkpoint"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::num_queries>>(
        "--num-queries", "Native query count", "Checkpoint"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::batch_size>>(
        "--batch-size", "Per-rank batch size", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::val_batch_size>>(
        "--val-batch-size", "Validation batch size", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::epochs>>(
        "--epochs", "Epoch count", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::grad_accum_steps>>(
        "--grad-accum-steps", "Gradient accumulation", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::optimizer>>(
        "--optimizer", "adamw or muon", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr>>(
        "--lr", "Decoder learning rate", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr_encoder>>(
        "--lr-encoder", "Encoder learning rate", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::momentum>>(
        "--momentum", "Muon momentum (AdamW only when explicitly supported)", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::freeze_encoder>>(
        "--freeze-encoder", "Freeze encoder", "Optimization", {}, "--no-freeze-encoder"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr_component_decay>>(
        "--lr-component-decay", "Component decay", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::encoder_layer_decay>>(
        "--encoder-layer-decay", "Layer decay", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::weight_decay>>(
        "--weight-decay", "Weight decay", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr_drop>>(
        "--lr-drop", "Step drop epoch", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr_scheduler>>(
        "--lr-scheduler", "step or cosine", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lr_min_factor>>(
        "--lr-min-factor", "Minimum LR multiplier", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::warmup_epochs>>(
        "--warmup-epochs", "Warmup duration", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::warmup_momentum>>(
        "--warmup-momentum", "Warmup momentum", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::clip_max_norm>>(
        "--clip-max-norm", "Gradient clipping norm", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::fused_optimizer>>(
        "--fused-optimizer", "Use fused AdamW backend", "Optimization", {}, "--no-fused-optimizer"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::use_ema>>(
        "--use-ema", "Maintain EMA", "Optimization", {}, "--no-ema"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::validation_loss>>(
        "--validation-loss", "Calculate validation loss", "Optimization", {}, "--no-validation-loss"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::validation_profile>>(
        "--validation-profile", "Write validation profile", "Optimization", {}, "--no-validation-profile"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::ema_decay>>(
        "--ema-decay", "EMA decay", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::ema_tau>>(
        "--ema-tau", "EMA tau", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::eval_max_dets>>(
        "--eval-max-dets", "Detection cap", "Optimization"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::assignment>>(
        "--assignment", "hungarian or match-free", "Supervision"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                               &rfdetr::TrainingSupervisionConfig::match_free, &rfdetr::MatchFreeSupervisionConfig::rho>>(
        "--match-free-rho", "Sparse correspondence threshold", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::match_free,
                                                                &rfdetr::MatchFreeSupervisionConfig::correspondence_weight>>(
        "--match-free-correspondence-weight", "Correspondence objective weight", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::match_free,
                                                                &rfdetr::MatchFreeSupervisionConfig::query_weight>>(
        "--match-free-query-weight", "Query objective weight", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::denoising,
                                                                &rfdetr::DenoisingSupervisionConfig::enabled>>(
        "--dn", "Enable denoising supervision", "Supervision", {}, "--no-dn"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                               &rfdetr::TrainingSupervisionConfig::denoising, &rfdetr::DenoisingSupervisionConfig::groups>>(
        "--dn-groups", "Denoising group count", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::denoising,
                                                                &rfdetr::DenoisingSupervisionConfig::label_noise_ratio>>(
        "--dn-label-noise-ratio", "Denoising label flip probability", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::denoising,
                                                                &rfdetr::DenoisingSupervisionConfig::center_noise_scale>>(
        "--dn-center-noise-scale", "Denoising center noise scale", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::training_supervision,
                                                                &rfdetr::TrainingSupervisionConfig::denoising,
                                                                &rfdetr::DenoisingSupervisionConfig::size_noise_scale>>(
        "--dn-size-noise-scale", "Denoising size noise scale", "Supervision"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                                                &rfdetr::GpuAugmentationConfig::enabled>>(
        "--gpu-augment", "Apply GPU augmentation", "GPU augmentation", {}, "--no-gpu-augment"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                       &rfdetr::GpuAugmentationConfig::perceptual_downscale>>(
        "--aug-perceptual-downscale", "Perceptual shrinking", "GPU augmentation", {}, "--no-aug-perceptual-downscale"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::geometry, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-geometry-prob", "geometry selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::geometry, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-geometry-min-strength", "geometry minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::geometry, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-geometry-max-strength", "geometry maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::resize, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-resize-prob", "resize selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::resize, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-resize-min-strength", "resize minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::resize, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-resize-max-strength", "resize maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::color, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-color-prob", "color selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::color, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-color-min-strength", "color minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::color, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-color-max-strength", "color maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::noise, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-noise-prob", "noise selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::noise, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-noise-min-strength", "noise minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::noise, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-noise-max-strength", "noise maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::blur, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-blur-prob", "blur selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::blur, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-blur-min-strength", "blur minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::blur, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-blur-max-strength", "blur maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::occlusion, &rfdetr::AugmentationGroupConfig::probability>>(
        "--aug-occlusion-prob", "occlusion selection probability", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::occlusion, &rfdetr::AugmentationGroupConfig::min_strength>>(
        "--aug-occlusion-min-strength", "occlusion minimum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest,
                       reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                               &rfdetr::GpuAugmentationConfig::occlusion, &rfdetr::AugmentationGroupConfig::max_strength>>(
        "--aug-occlusion-max-strength", "occlusion maximum strength", "GPU augmentation"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::gpu_augmentation,
                                                                &rfdetr::GpuAugmentationConfig::copy_paste_probability>>(
        "--aug-copy-paste-prob", "Copy-paste probability", "GPU augmentation"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::numa_node>>(
        "--numa-node", "GPU-local NUMA node override (-1 selects known locality)", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_id>>(
        "--device-id", "Single CUDA device", "Execution"),
    reflection::custom_option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_ids>,
                              &assign_train_integer_list<&rfdetr::TrainRequest::device_ids, true>,
                              &emit_train_integer_list<&rfdetr::TrainRequest::device_ids>>("--device-ids",
                                                                                           "Comma-separated CUDA device ids", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::workers>>(
        "--workers", "Dataset worker count", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::lanes>>(
        "--lanes", "Parallel backend lanes", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::cpu_affinity>>(
        "--cpu-affinity", "Linux CPU list", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::prefetch_factor>>(
        "--prefetch-factor", "Prefetch factor", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::print_freq>>(
        "--print-freq", "Logging frequency", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::seed>>(
        "--seed", "Random seed", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::amp>>(
        "--amp", "Automatic mixed precision", "Execution", {}, "--no-amp"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::progress_bar>>(
        "--progress", "Render progress", "Execution", {}, "--no-progress"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::compilation_mode>>(
        "--compile-mode", "Native compilation mode", "Execution"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::distributed_worker>>(
        "--dist-worker", "Internal distributed worker", "Distributed (internal)"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::distributed_rank>>(
        "--dist-rank", "Worker rank", "Distributed (internal)"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::distributed_world_size>>(
        "--dist-world-size", "Worker world size", "Distributed (internal)"),
    reflection::option<TrainCliRequest, reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::distributed_store_path>>(
        "--dist-store-file", "Rendezvous file", "Distributed (internal)")};

[[nodiscard]] consteval bool train_descriptor_relation_is_complete() {
    static_assert(kTrainOptions.size() == 82U);
    using Relation = reflection::catalog_provider_relation<rfdetr::TrainRecipeCatalog>;
    constexpr auto selector = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::optimizer>;
    std::array<reflection::ReflectedMemberIdentity, Relation::member_count> relation_identities{};
    std::size_t count = 0U;
    Relation::VisitMembers([&]<class Entry>() {
        constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, rfdetr::TrainRequest>(selector, Entry::destination);
        const auto identity = reflection::accessor_member_identity<TrainCliRequest, destination>();
        (void)reflection::unique_descriptor_index(kTrainOptions, identity);
        relation_identities[count++] = identity;
    });
    constexpr auto device_id = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_id>;
    constexpr auto device_ids = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_ids>;
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
    reflection::unexposed<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::weights_path>(
        "build-engine accepts the selected ONNX artifact kind"),
    reflection::unexposed<rfdetr::BuildEngineRequest, &rfdetr::BuildEngineRequest::tensorrt_path>(
        "build-engine accepts the selected ONNX artifact kind")};
inline constexpr std::array kExportOnnxUnexposed{
    reflection::unexposed<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::onnx_path>(
        "export-onnx accepts the selected native-weights artifact kind"),
    reflection::unexposed<rfdetr::ExportOnnxRequest, &rfdetr::ExportOnnxRequest::tensorrt_path>(
        "export-onnx accepts the selected native-weights artifact kind")};
inline constexpr std::array kPredictUnexposed{
    reflection::unexposed<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::video_path>>(
        "local video selection belongs to the GUI prediction workflow"),
    reflection::unexposed<PredictCliRequest, reflection::member_path<&PredictCliRequest::request, &rfdetr::PredictRequest::image_inputs>>(
        "the CLI bounded image_paths collection derives the final image-input "
        "records once in finalize_predict_request")};

static_assert((reflection::audit_descriptors(kBuildEngineOptions, kBuildEngineUnexposed), true));
static_assert((reflection::audit_descriptors(kExportOnnxOptions, kExportOnnxUnexposed), true));
static_assert((reflection::audit_descriptors(kEvaluateOptions), true));
static_assert((reflection::audit_descriptors(kPredictOptions, kPredictUnexposed), true));
static_assert((reflection::audit_descriptors(kValidateOptions), true));
static_assert((reflection::audit_descriptors(kTrainOptions), true));

void print_command_help(std::string rendered_options) { std::puts(rendered_options.c_str()); }

template <class Request, class Options>
[[nodiscard]] Request parse_request(const std::span<const std::string_view> arguments, const Options& options) {
    auto parsed = reflection::parse<Request>(arguments, options);
    if (!parsed) throw parsed.error();
    return std::move(parsed->request);
}

void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence);

class RfdetrCommandParser final {
   public:
    [[nodiscard]] static CompileCliRequest Compile(const std::span<const std::string_view> arguments) {
        return parse_request<CompileCliRequest>(arguments, kCompileOptions);
    }

    [[nodiscard]] static InfoCliRequest Info(const std::span<const std::string_view> arguments) {
        return parse_request<InfoCliRequest>(arguments, kInfoOptions);
    }

    [[nodiscard]] static rfdetr::BuildEngineRequest BuildEngine(const std::span<const std::string_view> arguments) {
        return parse_request<rfdetr::BuildEngineRequest>(arguments, kBuildEngineOptions);
    }

    [[nodiscard]] static rfdetr::ExportOnnxRequest ExportOnnx(const std::span<const std::string_view> arguments) {
        return parse_request<rfdetr::ExportOnnxRequest>(arguments, kExportOnnxOptions);
    }

    [[nodiscard]] static PredictCliRequest Predict(const std::span<const std::string_view> arguments) {
        return parse_request<PredictCliRequest>(arguments, kPredictOptions);
    }

    [[nodiscard]] static rfdetr::EvaluateRequest Evaluate(const std::span<const std::string_view> arguments) {
        return parse_request<rfdetr::EvaluateRequest>(arguments, kEvaluateOptions);
    }

    [[nodiscard]] static rfdetr::ValidateRequest Validate(const std::span<const std::string_view> arguments) {
        return parse_request<rfdetr::ValidateRequest>(arguments, kValidateOptions);
    }

    [[nodiscard]] static TrainCliRequest Train(const std::span<const std::string_view> arguments) {
        auto parsed = reflection::parse<TrainCliRequest>(arguments, kTrainOptions);
        if (!parsed) throw parsed.error();
        apply_train_presence(parsed->request, parsed->presence);
        return std::move(parsed->request);
    }

    [[nodiscard]] static NormalizeWeightsRequest NormalizeWeights(const std::span<const std::string_view> arguments) {
        return parse_request<NormalizeWeightsRequest>(arguments, kNormalizeOptions);
    }
};

void print_compile_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr compile [options]", command.description, kCompileOptions));
}

void print_info_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr info [options]", command.description, kInfoOptions));
}

void print_build_engine_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr build-engine [options]", command.description, kBuildEngineOptions));
}

void print_export_onnx_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr export-onnx [options]", command.description, kExportOnnxOptions));
}

void print_predict_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr predict [options]", command.description, kPredictOptions));
}

void print_evaluate_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr evaluate [options]", command.description, kEvaluateOptions));
}

void print_validate_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr validate [options]", command.description, kValidateOptions));
}

void print_train_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr train [options]", command.description, kTrainOptions));
}

void print_normalize_help(const rfdetr::RfdetrCommandDescriptor& command) {
    print_command_help(reflection::help("mmltk rfdetr normalize-weights [options]", command.description, kNormalizeOptions));
}

void finalize_compile_request(CompileCliRequest& request) {
    if (request.benchmark_resolution != 0) {
        if (request.output_dir.empty()) request.output_dir = "./compiled";
        return;
    }
    if (request.source_dir.empty() || request.output_dir.empty()) {
        throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir");
    }
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
        if (::sigaction(SIGINT, &action, &previous_interrupt_) != 0) {
            throw std::system_error(errno, std::generic_category(), "cannot install benchmark signal handler");
        }
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
        if (config.cache_dir.empty()) {
            if (const char* root = std::getenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT"); root != nullptr && root[0] != '\0') {
                config.cache_dir = root;
            }
        }
        config.resolution = static_cast<std::uint32_t>(request.benchmark_resolution);
        config.num_workers = request.num_workers;
        config.overwrite = request.overwrite;
        config.perceptual_downscale = request.perceptual_downscale;
        config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(benchmark_cancel_requested);
        config.progress = [](const data::BenchmarkCompileProgress& progress) {
            if (!progress.activity.empty()) spdmon::ProgressBar::log(progress.activity);
        };
        if (logging::enabled(spdlog::level::trace)) {
            config.trace = [](const std::string_view event, const std::string_view fields) {
                logging::log_if_enabled("rfdetr.benchmark", spdlog::level::trace,
                                        [&](auto& current) { current.trace("{{\"event\":\"{}\",\"fields\":{}}}", event, fields); });
            };
        }
        data::BenchmarkDatasetCompiler::compile(std::move(config));
        return;
    }

    data::CompilerConfig config;
    config.perceptual_downscale = request.perceptual_downscale;
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
        data::CompileTelemetry telemetry{plan.splits[split_index].image_count,
                                         {.context = &state, .report = [](void* context, const data::CompileProgress& progress) noexcept {
                                              auto& progress_state = *static_cast<ProgressState*>(context);
                                              progress_state.bar->set_total(progress.total);
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
    const auto tool = rfdetr::resolve_sibling_tool_path("mmltk-rfdetr-onnx-info").string();
    std::vector<std::string> arguments{tool, model_path.string()};
    if (logging_options.level) {
        const auto level_name = spdlog::level::to_string_view(*logging_options.level);
        arguments.push_back("--log-level=" + std::string(level_name.data(), level_name.size()));
    }
    if (logging_options.log_file) arguments.push_back("--log-file=" + logging_options.log_file->string());
    if (logging_options.log_dir) arguments.push_back("--log-dir=" + logging_options.log_dir->string());
    auto argv = rfdetr::make_exec_argv(arguments);
    ::execv(argv.front(), argv.data());
    throw std::system_error(errno, std::generic_category(), "failed to exec ONNX info helper");
}

void finalize_predict_request(PredictCliRequest& state) {
    state.request.image_inputs.clear();
    state.request.image_inputs.reserve(state.image_paths.size());
    for (std::size_t index = 0U; index < state.image_paths.size(); ++index) {
        const auto& path = state.image_paths[index];
        state.request.image_inputs.push_back({
            .image_path = path,
            .source_name = path.filename().string(),
            .image_id = static_cast<std::int64_t>(index),
        });
    }
    state.request.source_kind =
        state.image_paths.empty() ? rfdetr::PredictSourceKind::CompiledDataset : rfdetr::PredictSourceKind::ImageFiles;
    state.request = rfdetr::finalize_predict_request(std::move(state.request));
}

void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence) {
    using Relation = reflection::catalog_provider_relation<rfdetr::TrainRecipeCatalog>;
    constexpr auto selector = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::optimizer>;
    Relation::VisitMembers([&]<class Entry>() {
        constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, rfdetr::TrainRequest>(selector, Entry::destination);
        constexpr auto index =
            reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, destination>());
        if (presence.test(index)) Relation::template set_override<Entry::destination>(state.request.recipe_overrides);
    });
    constexpr auto device_id = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_id>;
    constexpr auto device_ids = reflection::member_path<&TrainCliRequest::request, &rfdetr::TrainRequest::device_ids>;
    constexpr auto device_id_index =
        reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_id>());
    constexpr auto device_ids_index =
        reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_ids>());
    if (presence.test(device_id_index) && presence.test(device_ids_index)) {
        throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids");
    }
}

class DistributedTrainingProcess final {
   public:
    static int Run(const rfdetr::TrainRequest& request) {
        const auto partitions = rfdetr::select_distributed_training_partitions(request);
        if (partitions.size() < 2U) throw std::logic_error("distributed training requires multiple partitions");
        const auto store = std::filesystem::temp_directory_path() /
                           ("mmltk_rfdetr_train_" + std::to_string(static_cast<long long>(::getpid())) + ".store");
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
                arguments.insert(arguments.begin(), services::current_executable_path().string());
                const pid_t pid = ::fork();
                if (pid < 0) throw std::system_error(errno, std::generic_category(), "failed to fork RF-DETR worker");
                if (pid == 0) {
                    std::vector<char*> argv;
                    argv.reserve(arguments.size() + 1U);
                    for (auto& argument : arguments)
                        argv.push_back(argument.data());
                    argv.push_back(nullptr);
                    ::execv(argv.front(), argv.data());
                    std::_Exit(127);
                }
                children.push_back(pid);
            }
        } catch (...) {
            for (const pid_t child : children)
                (void)::kill(child, SIGTERM);
            for (const pid_t child : children)
                (void)::waitpid(child, nullptr, 0);
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
                for (const pid_t child : children)
                    (void)::kill(child, SIGTERM);
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

int dispatch_command(const rfdetr::RfdetrCommandDescriptor& descriptor, const std::span<const std::string_view> arguments,
                     const bool help_requested, const logging::CliOverrides& logging_options) {
    switch (descriptor.command) {
        case rfdetr::RfdetrCommand::Compile: {
            if (help_requested) {
                print_compile_help(descriptor);
                return 0;
            }
            auto request = RfdetrCommandParser::Compile(arguments);
            finalize_compile_request(request);
            run_compile(request);
            return 0;
        }
        case rfdetr::RfdetrCommand::Info: {
            if (help_requested) {
                print_info_help(descriptor);
                return 0;
            }
            const auto request = RfdetrCommandParser::Info(arguments);
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
            const auto request = RfdetrCommandParser::BuildEngine(arguments);
            rfdetr::build_tensorrt_engine(request);
            return 0;
        }
        case rfdetr::RfdetrCommand::ExportOnnx: {
            if (help_requested) {
                print_export_onnx_help(descriptor);
                return 0;
            }
            const auto request = RfdetrCommandParser::ExportOnnx(arguments);
            rfdetr::export_onnx(request);
            return 0;
        }
        case rfdetr::RfdetrCommand::Predict: {
            if (help_requested) {
                print_predict_help(descriptor);
                return 0;
            }
            auto state = RfdetrCommandParser::Predict(arguments);
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
            auto request = RfdetrCommandParser::Evaluate(arguments);
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
            auto request = RfdetrCommandParser::Validate(arguments);
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
            auto state = RfdetrCommandParser::Train(arguments);
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
            auto request = RfdetrCommandParser::NormalizeWeights(arguments);
            if (request.input_path.empty() || request.output_path.empty()) {
                throw std::runtime_error("rfdetr normalize-weights requires --input and --output");
            }
            request.input_path = std::filesystem::absolute(request.input_path).lexically_normal();
            request.output_path = std::filesystem::absolute(request.output_path).lexically_normal();
            const auto checkpoint = rfdetr::normalize_checkpoint_to_native(request.input_path, request.output_path, request.class_layout_path);
            if (logging::enabled(spdlog::level::info)) {
                logging::info("rfdetr.cli", [&](auto& current) {
                    current.info("rfdetr.normalize-weights: wrote {} tensors for preset={} to {}", checkpoint.tensor_count(),
                                 checkpoint.metadata.preset_name, request.output_path.string());
                });
            } else {
                std::printf("rfdetr.normalize-weights: wrote %zu tensors for preset=%s to %s\n", checkpoint.tensor_count(),
                            checkpoint.metadata.preset_name.c_str(), request.output_path.c_str());
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
    for (const auto& command : rfdetr::kRfdetrCommands)
        mmltk::entrypoints::cli::print_command_help_line(command.name, command.description);
    std::fputc('\n', stdout);
}

}  // namespace

int handle_rfdetr_cli(const std::span<const std::string_view> arguments, int argc, char** argv) {
    if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h") {
        print_rfdetr_help();
        return 0;
    }
    const auto command = rfdetr::parse_rfdetr_command(arguments.front());
    if (!command) {
        std::fprintf(stderr, "unknown command: %.*s\n", static_cast<int>(arguments.front().size()), arguments.front().data());
        return 1;
    }
    const auto* descriptor = rfdetr::rfdetr_command_descriptor(*command);
    if (descriptor == nullptr) throw std::logic_error("RF-DETR command descriptor is missing");
    const auto command_arguments = arguments.subspan(1U);
    const bool help_requested = std::ranges::find(command_arguments, std::string_view{"--help"}) != command_arguments.end() ||
                                std::ranges::find(command_arguments, std::string_view{"-h"}) != command_arguments.end();
    try {
        return dispatch_command(*descriptor, command_arguments, help_requested, logging::scan_cli_overrides(argc, argv));
    } catch (const reflection::ParseError& error) { std::fprintf(stderr, "%s\n", error.what()); } catch (const std::exception& error) {
        if (logging::enabled(spdlog::level::err)) {
            logging::error("rfdetr.cli", [&](auto& current) { current.error("mmltk rfdetr error: {}", error.what()); });
        } else {
            std::fprintf(stderr, "mmltk rfdetr error: %s\n", error.what());
        }
    }
    return 1;
}

}  // namespace mmltk::entrypoints::cli
