module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/model_info.h"

export module mmltk.backend.models.rfdetr.inference.runtime_backend;

export namespace mmltk::backend::models::rfdetr {

struct RfdetrRuntimeBackendOptions final {
    ModelArtifactRequest artifacts;
    std::string backend = "auto";
    std::int32_t device = 0;
    mmltk::backend::ml::runtime::BorrowedCommandStream command_stream{};
    std::uint32_t static_resolution = 0U;
    std::size_t maximum_detections = 500U;
    std::filesystem::path save_compiled_model_path;
    bool allow_fp16 = true;
};

enum class InferenceArtifactKind : std::uint8_t {
    Weights,
    Onnx,
    TensorRt,
};

struct ResolvedInferenceArtifact final {
    InferenceArtifactKind kind = InferenceArtifactKind::Weights;
    std::string backend_name;
    std::filesystem::path path;
    bool compile_onnx_to_tensorrt = false;
};

[[nodiscard]] ResolvedInferenceArtifact resolve_inference_artifact(const ModelArtifactRequest& artifacts, std::string backend);
[[nodiscard]] ModelArtifactRequest select_inference_artifact(const ModelArtifactRequest& artifacts,
                                                             const ResolvedInferenceArtifact& resolved);
[[nodiscard]] ResolvedModelArtifacts describe_inference_artifact(const ModelArtifactRequest& request,
                                                                 const ResolvedInferenceArtifact& artifact, std::uint32_t resolution);

// Deferred masks pair selected top-k queries with their model-resolution logits.
// Materialize on the lane stream before its next Run. Custody retains the exact
// allocation through exceptional retirement; it is not a replay or immutable cache.
struct RfdetrMaskSelection final {
    mmltk::backend::ml::runtime::RuntimeTensorBuffer query_indices;
    std::optional<mmltk::backend::ml::runtime::RuntimeTensorBuffer> mask_logits;
    std::shared_ptr<void> custody;
};

class RfdetrRuntimeBackend final {
   public:
    ~RfdetrRuntimeBackend();

    RfdetrRuntimeBackend(const RfdetrRuntimeBackend&) = delete;
    RfdetrRuntimeBackend& operator=(const RfdetrRuntimeBackend&) = delete;
    RfdetrRuntimeBackend(RfdetrRuntimeBackend&&) noexcept;
    RfdetrRuntimeBackend& operator=(RfdetrRuntimeBackend&&) noexcept;

    [[nodiscard]] const std::string& backend_name() const noexcept;
    [[nodiscard]] std::uint32_t static_resolution() const noexcept;
    [[nodiscard]] std::int32_t device() const noexcept;
    [[nodiscard]] std::uintptr_t stream() const noexcept;
    [[nodiscard]] bool has_masks() const noexcept;
    [[nodiscard]] const mmltk::backend::ml::runtime::RuntimeShape& logits_shape() const noexcept;
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeElementType input_element_type() const noexcept;
    [[nodiscard]] const mmltk::backend::ml::runtime::RuntimeModelInfo& model_info() const noexcept;

    // Mask capacity does not imply demand. Existing analysis consumers remain bbox-only.
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeSubmission Run(
        const mmltk::backend::ml::runtime::RuntimeTensorBuffer& input,
        std::span<mmltk::backend::ml::runtime::AnalysisAnnotationStorage> annotations,
        std::span<RfdetrMaskSelection> selections = {}, bool include_masks = false);
    void ReleaseAfterCompletion(mmltk::backend::ml::runtime::RuntimeSubmission&& submission);
    [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;
    [[nodiscard]] std::shared_ptr<RfdetrRuntimeBackend> MakeLane() const;

   private:
    struct State;

    explicit RfdetrRuntimeBackend(std::shared_ptr<mmltk::backend::ml::runtime::RuntimeBackend> lane, std::string backend_name,
                                  std::uint32_t static_resolution, std::size_t maximum_detections);

    std::shared_ptr<mmltk::backend::ml::runtime::RuntimeBackend> lane_;
    std::string backend_name_;
    std::uint32_t static_resolution_ = 0U;
    std::size_t maximum_detections_ = 0U;
    std::unique_ptr<State> state_;

    friend std::shared_ptr<RfdetrRuntimeBackend> make_rfdetr_runtime_backend(const RfdetrRuntimeBackendOptions& options);
};

[[nodiscard]] std::shared_ptr<RfdetrRuntimeBackend> make_rfdetr_runtime_backend(const RfdetrRuntimeBackendOptions& options);

[[nodiscard]] std::vector<std::shared_ptr<RfdetrRuntimeBackend>> make_rfdetr_runtime_backend_lanes(
    const RfdetrRuntimeBackendOptions& options, std::size_t lane_count);

[[nodiscard]] ModelInfo inspect_tensorrt_model(const ModelArtifactRequest& artifacts, int device_id);

void build_tensorrt_engine(const BuildEngineRequest& request);

void build_tensorrt_engine(const BuildEngineRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream);

}  // namespace mmltk::backend::models::rfdetr
