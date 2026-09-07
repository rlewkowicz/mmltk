#include "src/backend/models/rfdetr/contract/workflow_requests.h"

#include <algorithm>
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
    const bool compiled_source =
        request.source_kind == PredictSourceKind::CompiledDataset && !request.compiled_path.empty() && request.image_inputs.empty();
    const bool image_source =
        request.source_kind == PredictSourceKind::ImageFiles && request.compiled_path.empty() && !request.image_inputs.empty();
    if ((!compiled_source && !image_source) || request.output_path.empty() || request.selected_input_count() != 1U) {
        throw std::runtime_error("rfdetr predict requires compiled input, output, and one model artifact");
    }
}

void validate_validate_request(const ValidateRequest& request) {
    require_valid_fields(request, "invalid RF-DETR validation fields");
    if (request.compiled_path.empty() || request.selected_input_count() == 0U || (request.recompile && request.source_dir.empty()))
        throw std::runtime_error("rfdetr validate requires compiled input and a model artifact");
}

void validate_train_placement(const TrainRequest& request) {
    if (request.numa_node < -1 ||
        (!request.numa_nodes.empty() && (request.numa_nodes.size() != request.device_ids.size() || request.numa_node != -1)) ||
        (request.device_ids.size() > 1 && request.numa_node != -1) ||
        std::ranges::any_of(request.numa_nodes, [](int node) { return node < -1; }))
        throw std::invalid_argument("use --numa-nodes with one node per --device-ids rank; --numa-node is single-device only");
}

void validate_train_request(const TrainRequest& request) {
    require_valid_fields(request, "invalid RF-DETR training fields");
    validate_train_placement(request);
    if (!training_supervision_config_valid(request.training_supervision)) {
        throw std::runtime_error("invalid RF-DETR training supervision configuration");
    }
    if (training_supervision_enabled(request.training_supervision) && request.compilation_mode == CompilationMode::kFullTrace) {
        throw std::runtime_error("RF-DETR Match-Free and denoising supervision do not support full-trace compilation");
    }
    if (request.training_supervision.assignment == TrainAssignmentKind::MatchFree) {
        const PresetCatalogEntry* preset = find_model_preset(request.preset_name);
        if (preset != nullptr && preset->task == ModelTask::Segmentation) {
            throw std::runtime_error("RF-DETR Match-Free supervision does not support segmentation");
        }
    }
    if (const PresetCatalogEntry* preset = find_model_preset(request.preset_name)) {
        NativeRfDetrConfig model_config = native_config_from_preset(*preset);
        model_config.training_supervision = request.training_supervision;
        if (training_supervision_enabled(request.training_supervision) && request.num_queries != 0U) {
            if (request.num_queries > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("RF-DETR supervision query layout exceeds the native model limit");
            model_config.num_queries = static_cast<int>(request.num_queries);
        }
        if (!training_supervision_model_config_valid(model_config))
            throw std::runtime_error("RF-DETR training supervision is incompatible with the selected model");
    }
    if (request.train_compiled_path.empty() || request.val_compiled_path.empty() || request.output_dir.empty()) {
        throw std::runtime_error("RF-DETR train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const std::size_t input_count =
        static_cast<std::size_t>(!request.weights_path.empty()) + static_cast<std::size_t>(!request.resume_path.empty());
    const bool distributed =
        request.distributed_worker
            ? request.distributed_rank >= 0 && request.distributed_world_size > 1 && !request.distributed_store_path.empty()
            : request.distributed_rank == 0 && request.distributed_world_size == 1 && request.distributed_store_path.empty();
    if (input_count != 1U || !gpu_augmentation_config_valid(request.gpu_augmentation) ||
        !mmltk::frameworks::reflection::unique_nonnegative_identifiers(request.device_ids) ||
        !mmltk::frameworks::reflection::enum_contains(request.lr_scheduler) || !distributed) {
        throw std::runtime_error("rfdetr train requires train/validation data, output, and one checkpoint input");
    }
}

}  // namespace mmltk::backend::models::rfdetr
