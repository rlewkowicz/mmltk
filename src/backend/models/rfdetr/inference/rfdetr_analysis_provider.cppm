module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
export module mmltk.backend.models.rfdetr.inference.analysis_provider;
export namespace mmltk::backend::models::rfdetr {
struct RfdetrAnalysisOptions final {
    ModelArtifactRequest artifacts;
    std::string backend = "auto";
    std::int32_t device = 0;
    mmltk::backend::ml::runtime::BorrowedCommandStream command_stream{};
    std::uint32_t static_resolution = 0U;
    std::size_t maximum_detections = 500U;
    bool allow_fp16 = true;
};
[[nodiscard]] std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> MakeRfdetrAnalysisProvider(const RfdetrAnalysisOptions& options);
}  // namespace mmltk::backend::models::rfdetr
