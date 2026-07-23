#pragma once

#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/train_recipe.h"
#include "mmltk/rfdetr/validate.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

struct BuildEngineRequest : ModelArtifactRequest {
    std::filesystem::path output_path;
    int device_id = 0;
    bool allow_fp16 = true;
};

struct ExportOnnxRequest : ModelArtifactRequest {
    std::filesystem::path output_path;
    int device_id = 0;
    int opset_version = 19;
    bool simplify = false;
};

struct PredictRequest : ModelArtifactRequest, PredictRuntimeConfig {
    PredictSourceKind source_kind = PredictSourceKind::CompiledDataset;
    std::filesystem::path compiled_path;
    std::vector<PredictImageInput> image_inputs;
    std::filesystem::path output_path;
    std::string backend = "auto";
    std::size_t batch_size = 1;
    std::size_t max_dets_per_image = 500;
};

struct ValidateRequest : ValidationOptions {};

struct TrainRequest : TrainOptions {
    TrainRecipeFieldOverrides recipe_overrides;
};

template <typename Request>
inline std::size_t selected_model_input_count(const Request& request) {
    return static_cast<std::size_t>(!request.weights_path.empty()) +
           static_cast<std::size_t>(!request.onnx_path.empty()) +
           static_cast<std::size_t>(!request.tensorrt_path.empty());
}

void validate_build_engine_request(const BuildEngineRequest& request);

void validate_export_onnx_request(const ExportOnnxRequest& request);

void validate_predict_request(const PredictRequest& request);

void finalize_predict_request(PredictRequest& request);

PredictOptions to_predict_options(const PredictRequest& request, std::string_view preset_name = {});

void validate_validate_request(const ValidateRequest& request);

void finalize_validate_request(ValidateRequest& request);

ValidationOptions to_validate_options(const ValidateRequest& request);

void validate_train_request(const TrainRequest& request);

TrainOptions to_train_options(const TrainRequest& request);

TrainRequest train_request_from_options(const TrainOptions& options,
                                        const TrainRecipeFieldOverrides& recipe_overrides = {});

std::string infer_train_recipe_preset_name(const TrainRequest& request);

void finalize_train_request(TrainRequest& request, std::string_view fallback_preset_name = {});

}  
