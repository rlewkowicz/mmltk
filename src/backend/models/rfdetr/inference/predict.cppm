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
    bool cancelled = false;
    std::size_t processed_images = 0;
    PhaseTiming timing;
};

// Current-record pixels are either CHW device storage or owned decoded RGB8.
// GPU receiver copies run on `stream`, so source reuse follows every read.
// `custody` owns the exact source and compact annotations for failed-copy retirement.
// A receiver that cannot settle that stream must retain custody, stop source workers,
// and propagate CudaOperationError; it may not return a contained visual failure.
struct PredictionPixels final {
    const float* chw = nullptr;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    int device = -1;
    std::uintptr_t stream = 0U;
    const std::uint8_t* rgb8 = nullptr;
    std::shared_ptr<void> custody;
    void (*stop_source)(void*) = nullptr;
    void* source_control = nullptr;
    std::string_view preview_failure;
};

struct PredictionDelivery final {
    std::stop_token stop{};
    // Compact annotation planes accompany requested current pixels only. Without
    // raw preview demand, completed still receives every semantic record (including RLE),
    // while its annotation storage has no available device planes.
    bool source_pixels = false;
    std::uint32_t maximum_pixel_width = UINT32_MAX;
    std::uint32_t maximum_pixel_height = UINT32_MAX;
    std::function<bool(std::optional<double>, double)> before_frame{};
    std::function<void(const PredictionRunResult&)> begin{};
    std::function<void(const PredictionRecord&, PredictionPixels, const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&)> completed{};
    std::function<void(std::size_t completed, std::size_t total)> progress{};
    std::function<void(std::size_t decoded, std::size_t total)> decoded{};
};

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
    PredictionRunResult RunResolved(const PredictRequest& request, const ResolvedInferenceArtifact& artifact,
                                    mmltk::backend::ml::runtime::BorrowedCommandStream command_stream, const PredictionDelivery& delivery = {});
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] PredictRequest finalize_predict_request(PredictRequest request);
PredictionRunResult run_prediction(const PredictRequest& request);
PredictionRunResult run_resolved_prediction(const PredictRequest& request, const ResolvedInferenceArtifact& artifact);
void print_prediction_summary(const PredictRequest& request, const PredictionRunResult& result);

}  // namespace mmltk::backend::models::rfdetr
