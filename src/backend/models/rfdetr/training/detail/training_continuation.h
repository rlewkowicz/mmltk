#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "torch_api.h"
namespace mmltk::backend::models::rfdetr {
struct ResumeContinuationManifest {
    bool ema_requested = false;
    bool ema_present = false;
    std::optional<double> scaler_scale;
    std::optional<int64_t> scaler_growth_tracker;
};
void validate_resume_continuation_manifest(const ResumeContinuationManifest& manifest);
namespace detail {
// These are the continuation values distinct from the canonical request.
// Member names are the current native archive keys, including provenance.
struct TrainingContinuationValues {
    int64_t epoch = 0;
    double best_regular_metric = 0.0;
    double best_ema_metric = 0.0;
    double grad_scaler_scale = 1.0;
    int64_t grad_scaler_growth_tracker = 0;
    std::string training_attempt_id;
    std::string training_original_descriptor;
};
struct TrainingContinuation {
    TrainRequest configuration;
    TrainingContinuationValues values;
};
void write_training_configuration(mmltk::backend::ml::torch_api::OutputArchive&, const TrainRequest&);
[[nodiscard]] TrainRequest read_training_configuration(mmltk::backend::ml::torch_api::InputArchive&);
void write_training_continuation(mmltk::backend::ml::torch_api::OutputArchive&, const TrainRequest&, const TrainingContinuationValues&);
// Empty only for weights-only archives. Requires complete, consistent scalar
// continuation; the caller admits the optimizer and ordered EMA tensor state.
[[nodiscard]] std::optional<TrainingContinuation> read_training_continuation(mmltk::backend::ml::torch_api::InputArchive&);
void require_active_training_continuation(const TrainingContinuation&, const TrainRequest&);
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
