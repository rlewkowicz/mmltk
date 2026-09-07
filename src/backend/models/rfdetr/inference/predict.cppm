module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"

export module mmltk.backend.models.rfdetr.inference.prediction;

export import mmltk.backend.models.rfdetr.inference.runtime_backend;

export namespace mmltk::backend::models::rfdetr {

struct PredictionRecord {
    std::int64_t dataset_index = 0;
    std::int64_t image_id = 0;
    std::string source_name;
    std::vector<Prediction> detections;
};

struct PredictionRunResult {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;
    std::vector<std::string> class_names;
    std::vector<PredictionRecord> records;
    std::size_t processed_images = 0;
    PhaseTiming timing;
};

class PredictionSession final {
   public:
    PredictionSession();
    ~PredictionSession();
    PredictionSession(const PredictionSession&) = delete;
    PredictionSession& operator=(const PredictionSession&) = delete;

    PredictionRunResult Run(const PredictRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream);
    [[nodiscard]] std::size_t RunAndWrite(const PredictRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream);
    PredictionRunResult RunResolved(const PredictRequest& request, const ResolvedInferenceArtifact& artifact,
                                    mmltk::backend::ml::runtime::BorrowedCommandStream command_stream);
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] PredictRequest finalize_predict_request(PredictRequest request);
PredictionRunResult run_prediction(const PredictRequest& request);
PredictionRunResult run_resolved_prediction(const PredictRequest& request, const ResolvedInferenceArtifact& artifact);
void write_prediction_json(const PredictRequest& request, const PredictionRunResult& result);
void print_prediction_summary(const PredictRequest& request, const PredictionRunResult& result);

}  // namespace mmltk::backend::models::rfdetr
