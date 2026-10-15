#pragma once
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::controller {
// Captured at Start, private to the Live controller. The data plane receives
// only its model-neutral factory and the stream it owns.
// Only thrown after a no-custody provider admission failure and proved physical cleanup.
class LiveProviderAdmissionFailure final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
struct LiveAnalysisConfiguration final {
 mmltk::backend::models::rfdetr::PredictRequest request;
 bool keypoints = true, uncertainty = false;
};
}  // namespace mmltk::controller
