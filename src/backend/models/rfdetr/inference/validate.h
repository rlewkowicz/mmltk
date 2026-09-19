#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <stop_token>
#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/core/model_info.h"
namespace mmltk::backend::models::rfdetr {
struct ValidationBackendResult {
    ResolvedModelArtifacts artifacts;
    ModelInfo model_info;
    EvalSummary summary;
    std::vector<EvaluationMetricDetail> details;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog;
    PhaseTiming timing;
};
struct ValidationDeltaSummary {
    double bbox_ap = 0.0;
    double bbox_ap50 = 0.0;
    std::optional<double> mask_ap;
    std::optional<double> mask_ap50;
};
struct ValidationLimitResolution {
    std::uint32_t persisted_max_instances_per_image = 0;
    std::size_t resolved_candidate_count = 0;
    std::size_t resolved_eval_max_dets = 0;
    bool candidate_count_automatic = false;
    bool eval_max_dets_automatic = false;
};
struct ValidationSampleView final {
    const PredictionRecord& prediction;
    PredictionPixels pixels;
    const mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotations;
    std::span<const Prediction> ground_truth;
};
struct ValidationDelivery final {
    std::stop_token stop{};
    std::function<void(std::size_t, std::size_t)> progress{};
    bool mask_metrics = true;
    std::function<void(std::span<const std::uint32_t>, std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>)> samples_selected{};
    std::function<void(ValidationSampleView)> sample{};
};
struct ValidationRunResult {
    bool cancelled = false;
    std::size_t images = 0;            // Planned distinct dataset population.
    std::size_t processed_images = 0;  // Accepted image executions, summed across backends.
    std::size_t categories = 0;
    ValidationLimitResolution limits{};
    std::vector<std::string> eval_order{};
    std::unordered_map<std::string, ValidationBackendResult> backends{};
    std::optional<AlignmentStats> alignment_probe{};
    std::optional<ValidationDeltaSummary> delta_tensorrt_minus_onnx{};
    std::optional<PhaseTiming> total_timing{};
    void FinalizeTiming();
};
class ValidationSession final {
   public:
    ValidationSession();
    ~ValidationSession();
    ValidationSession(const ValidationSession&) = delete;
    ValidationSession& operator=(const ValidationSession&) = delete;
    ValidationRunResult Run(const ValidateRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream,
                            const ValidationDelivery& delivery = {});
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};
[[nodiscard]] ValidateRequest finalize_validate_request(ValidateRequest request);
ValidationRunResult run_validation(const ValidateRequest& request);
void write_validation_report(const ValidateRequest& request, const ValidationRunResult& result);
void print_model_metadata(const ModelInfo& info, std::size_t images, std::size_t categories, ValidationLogMode log_mode);
void print_validation_run_summary(const ValidateRequest& request, const ValidationRunResult& result);
}  // namespace mmltk::backend::models::rfdetr
