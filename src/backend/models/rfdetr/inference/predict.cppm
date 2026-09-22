module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <stop_token>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
export module mmltk.backend.models.rfdetr.inference.prediction;
export import mmltk.backend.models.rfdetr.inference.runtime_backend;
export namespace mmltk::backend::models::rfdetr {
class PredictionJsonWriter final {
public:
 explicit PredictionJsonWriter(const PredictRequest&);
 ~PredictionJsonWriter();
 PredictionJsonWriter(const PredictionJsonWriter&) = delete;
 PredictionJsonWriter& operator=(const PredictionJsonWriter&) = delete;
 void Begin(const PredictionRunResult&);
 void Append(const PredictionRecord&);
 void Complete();

private:
 struct State;
 std::unique_ptr<State> state_;
};
class PredictionSession final {
public:
 PredictionSession();
 ~PredictionSession();
 PredictionSession(const PredictionSession&) = delete;
 PredictionSession& operator=(const PredictionSession&) = delete;
 PredictionRunResult Run(const PredictRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream, const PredictionDelivery& delivery = {});
 [[nodiscard]] PredictionRunResult RunAndWrite(const PredictRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream, const PredictionDelivery& delivery = {});
 PredictionRunResult RunResolved(
  const PredictRequest& request, const ResolvedInferenceArtifact& artifact, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream, const PredictionDelivery& delivery = {});
 // Sticky through Close; the run owner captures this before replacing or
 // destroying the session. Ordinary contained preview failures leave it false.
 [[nodiscard]] bool HasUnsafeCustody() const noexcept;
 [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;

private:
 struct State;
 std::shared_ptr<State> state_;
};
[[nodiscard]] PredictRequest finalize_predict_request(PredictRequest request);
PredictionRunResult run_prediction(const PredictRequest& request);
PredictionRunResult run_resolved_prediction(const PredictRequest& request, const ResolvedInferenceArtifact& artifact);
void print_prediction_summary(const PredictRequest& request, const PredictionRunResult& result);
}  // namespace mmltk::backend::models::rfdetr
