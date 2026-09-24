module;
#include <memory>
#include "src/backend/ml/runtime/analysis_provider.h"
export module mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
export namespace mmltk::backend::models::rfdetr {
[[nodiscard]] std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> MakeRfdetrAnalysisProvider(const RfdetrRuntimeBackendOptions& options);
}  // namespace mmltk::backend::models::rfdetr
