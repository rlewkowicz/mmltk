#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <algorithm>
#include <array>
#include "src/frameworks/serialization/reflected_json.h"
#include <cstddef>
#include <limits>
#include <meta>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
namespace {
void require_valid_fields(const auto& request, const char* message) {
 if (mmltk::frameworks::reflection::validate_reflected_fields(request)) throw std::runtime_error(message);
}
}  // namespace
void validate_build_engine_request(const BuildEngineRequest& request) {
 require_valid_fields(request, "invalid RF-DETR build-engine fields");
 if (request.output_path.empty() || request.onnx_path.empty() || !request.weights_path.empty() || !request.tensorrt_path.empty()) {
  throw std::runtime_error("rfdetr build-engine requires --onnx and --output");
 }
}
void validate_export_onnx_request(const ExportOnnxRequest& request) {
 require_valid_fields(request, "invalid RF-DETR export-onnx fields");
 if (request.output_path.empty() || request.weights_path.empty() || !request.onnx_path.empty() || !request.tensorrt_path.empty()) {
  throw std::runtime_error("rfdetr export-onnx requires --weights and --output");
 }
}
void validate_predict_request(const PredictRequest& request) {
 require_valid_fields(request, "invalid RF-DETR predict fields");
 (void)derive_execution_facts(request, 0);
 const bool compiled_source = request.source_kind == PredictSourceKind::CompiledDataset && !request.compiled_path.empty() && request.image_inputs.empty() && request.video_path.empty();
 const bool image_source = request.source_kind == PredictSourceKind::ImageFiles && request.compiled_path.empty() && !request.image_inputs.empty() && request.video_path.empty();
 const bool video_source = request.source_kind == PredictSourceKind::VideoFile && !request.video_path.empty() && request.compiled_path.empty() && request.image_inputs.empty();
 if ((!compiled_source && !image_source && !video_source) || request.selected_input_count() != 1U) {
  throw std::runtime_error("rfdetr predict requires exactly one source (compiled dataset, images, or video), and one model artifact");
 }
}
void validate_validate_request(const ValidateRequest& request) {
 require_valid_fields(request, "invalid RF-DETR validation fields");
 (void)derive_execution_facts(request, 0);
 if (request.compiled_path.empty() || request.selected_input_count() == 0U || (request.recompile && request.source_dir.empty()))
  throw std::runtime_error("rfdetr validate requires compiled input and a model artifact");
}
void validate_train_placement(const TrainRequest& request) {
 if (request.numa_node < -1 || (!request.numa_nodes.empty() && (request.numa_nodes.size() != request.device_ids.size() || request.numa_node != -1)) ||
     (request.device_ids.size() > 1 && request.numa_node != -1) || std::ranges::any_of(request.numa_nodes, [](int node) { return node < -1; }))
  throw std::invalid_argument("use --numa-nodes with one node per --device-ids rank; --numa-node is single-device only");
}
void validate_train_request(const TrainRequest& request) {
 require_valid_fields(request, "invalid RF-DETR training fields");
 validate_train_placement(request);
 (void)derive_execution_facts(request, 0);
 (void)derive_training_validation_facts(request, 0);
 if (!training_supervision_config_valid(request.training_supervision)) { throw std::runtime_error("invalid RF-DETR training supervision configuration"); }
 if (training_supervision_enabled(request.training_supervision) && request.compilation_mode == CompilationMode::kFullTrace) {
  throw std::runtime_error("RF-DETR Match-Free and denoising supervision do not support full-trace compilation");
 }
 if (const PresetCatalogEntry* preset = find_preset_catalog_entry(request.preset_name)) {
  if (const auto error = training_supervision_admission_error(preset->task, preset->decoder_layer_count, request.training_supervision)) throw std::runtime_error(std::string(*error));
  NativeRfDetrConfig model_config = native_config_from_preset(*preset);
  if (!native_model_config_valid(model_config)) throw std::runtime_error("RF-DETR preset has invalid task or head topology");
  model_config.training_supervision = request.training_supervision;
  if (training_supervision_enabled(request.training_supervision) && request.num_queries != 0U) {
   if (request.num_queries > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("RF-DETR supervision query layout exceeds the native model limit");
   model_config.num_queries = static_cast<int>(request.num_queries);
  }
  if (!training_supervision_model_config_valid(model_config)) throw std::runtime_error("RF-DETR training supervision is incompatible with the selected model");
 }
 if (request.train_compiled_path.empty() || request.val_compiled_path.empty() || request.output_dir.empty()) {
  throw std::runtime_error("RF-DETR train requires --train-compiled, --val-compiled, and --output-dir");
 }
 if (request.from_scratch && (find_preset_catalog_entry(request.preset_name) == nullptr || !request.class_layout_path.empty())) {
  throw std::runtime_error("rfdetr train --from-scratch requires a known --preset and no --class-layout");
 }
 if (!request.from_scratch && !request.encoder_weights_path.empty()) throw std::runtime_error("encoder weights are valid only for --from-scratch training");
 const std::size_t input_count = static_cast<std::size_t>(!request.weights_path.empty()) + static_cast<std::size_t>(!request.resume_path.empty()) + static_cast<std::size_t>(request.from_scratch);
 const bool distributed = request.distributed_worker ? request.distributed_rank >= 0 && request.distributed_world_size > 1 && !request.distributed_store_path.empty()
                                                     : request.distributed_rank == 0 && request.distributed_world_size == 1 && request.distributed_store_path.empty();
 if (input_count != 1U || !gpu_augmentation_config_valid(request.gpu_augmentation) || !mmltk::frameworks::reflection::unique_nonnegative_identifiers(request.device_ids) ||
     !mmltk::frameworks::reflection::enum_contains(request.recipe.lr_scheduler) || !distributed) {
  throw std::runtime_error("rfdetr train requires train/validation data, output, and exactly one of --weights, --from-scratch, or --resume");
 }
}
std::string encode_train_request_json(const TrainRequest& request) {
 validate_train_request(request);
 std::array<std::byte, kMaximumTrainRequestJsonBytes> scratch;
 auto text = mmltk::frameworks::serialization::reflected_json(request, scratch, {.max_bytes = scratch.size(), .max_items = 4096, .max_depth = 32}).dump();
 if (text.size() > kMaximumTrainRequestJsonBytes) throw std::invalid_argument("training request JSON exceeds 64 KiB");
 return text;
}
void migrate_train_request_record(nlohmann::json& record) {
 if (!record.is_object()) throw std::invalid_argument("training request must be an object");
 if (!record.contains("from_scratch")) record["from_scratch"] = false;
 if (!record.contains("encoder_weights_path")) record["encoder_weights_path"] = "";
}
TrainRequest reusable_train_request(TrainRequest request, const TrainRequest* original) {
 if (original) {
  request.from_scratch = original->from_scratch;
  request.weights_path = original->weights_path;
  request.class_layout_path = original->class_layout_path;
  request.resume_path = original->resume_path;
 }
 request.encoder_weights_path.clear();
 return request;
}
void normalize_train_resume_request(TrainRequest& request, const std::filesystem::path& checkpoint) {
 request.from_scratch = false;
 request.encoder_weights_path.clear();
 request.weights_path.clear();
 request.class_layout_path.clear();
 request.resume_path = checkpoint;
}
TrainRequest decode_train_request_json(const std::string_view text) {
 if (text.empty() || text.size() > kMaximumTrainRequestJsonBytes) throw std::invalid_argument("training request JSON must fit 64 KiB");
 constexpr mmltk::frameworks::serialization::wire::Limits limits{.max_bytes = kMaximumTrainRequestJsonBytes, .max_items = 4096, .max_depth = 32};
 auto document = mmltk::frameworks::serialization::parse_bounded_json(text, limits);
 migrate_train_request_record(document);
 auto request = mmltk::frameworks::serialization::decode_reflected_json<TrainRequest>(document.dump(), limits);
 validate_train_request(request);
 return request;
}
}  // namespace mmltk::backend::models::rfdetr
