#include "src/backend/models/rfdetr/inference/evaluation.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

import mmltk.backend.models.rfdetr.inference.runtime_backend;

namespace mmltk::backend::models::rfdetr {

EvaluateRequest finalize_evaluate_request(EvaluateRequest request) {
    if (request.compiled_path.empty() || request.selected_input_count() != 1U || request.batch_size == 0U) {
        throw std::invalid_argument("invalid RF-DETR evaluation request");
    }
    request.compiled_path = std::filesystem::absolute(request.compiled_path);
    return request;
}

EvaluationRunResult run_evaluation(const EvaluateRequest& request) {
    const auto options = finalize_evaluate_request(request);
    ValidateRequest validation;
    static_cast<ModelArtifactRequest&>(validation) = options;
    static_cast<InferenceExecutionConfig&>(validation) = options;
    validation.compiled_path = options.compiled_path;
    validation.batch_size = options.batch_size;
    validation.limit_images = options.limit_images;
    validation.num_queries = options.num_queries;
    validation.eval_max_dets = options.eval_max_dets;
    validation.eval_order = resolve_inference_artifact(options, options.backend).backend_name;
    auto validation_result = run_validation(validation);
    if (validation_result.backends.size() != 1U) { throw std::runtime_error("RF-DETR evaluation expected one selected backend"); }
    auto& [backend_name, backend_result] = *validation_result.backends.begin();
    EvaluationRunResult result;
    result.backend_name = backend_name;
    result.image_count = validation_result.images;
    result.category_count = validation_result.categories;
    result.artifacts = backend_result.artifacts;
    result.result = std::move(backend_result);
    return result;
}

void print_evaluation_summary(const EvaluateRequest&, const EvaluationRunResult& result) {
    std::cout << result.backend_name << ": bbox AP=";
    if (result.result.summary.bbox.available) std::cout << result.result.summary.bbox.ap;
    else std::cout << "unavailable";
    const auto& caps = result.result.summary.bbox.detection_limits;
    std::cout << " model budget=" << result.result.summary.model_detection_budget << " AR caps=" << caps[0] << '/' << caps[1] << '/' << caps[2] << '\n';
}

}  // namespace mmltk::backend::models::rfdetr
