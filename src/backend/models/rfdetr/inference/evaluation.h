#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
struct EvaluateRequest : ModelArtifactRequest, InferenceExecutionConfig {
    std::filesystem::path compiled_path;
    std::string backend = "auto";
    std::size_t limit_images = 0U;
    std::size_t num_queries = 0U;
    std::size_t eval_max_dets = 0U;
    std::size_t batch_size = 1U;
    int lanes = 0;
    bool progress_bar = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};
MMLTK_REFLECT_FIELDS(EvaluateRequest)
struct EvaluationRunResult {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;
    std::size_t image_count = 0U;
    std::size_t category_count = 0U;
    ValidationBackendResult result;
};
[[nodiscard]] EvaluateRequest finalize_evaluate_request(EvaluateRequest request);
EvaluationRunResult run_evaluation(const EvaluateRequest& request);
void print_evaluation_summary(const EvaluateRequest& request, const EvaluationRunResult& result);
}  // namespace mmltk::backend::models::rfdetr
