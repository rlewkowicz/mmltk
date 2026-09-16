module;
#include <cstdint>
#include "src/backend/models/rfdetr/core/model_info.h"
#include <filesystem>
#include <memory>
#include <stop_token>
#include <span>
#include <string_view>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
export module mmltk.backend.models.rfdetr.model_export;


namespace mmltk::backend::models::rfdetr {
void write_onnx_model_bytes(std::string_view serialized_model, const std::filesystem::path& output_path, const ModelClassLayout& layout);
}
export namespace mmltk::backend::models::rfdetr {
using ModelExportStatus = std::int32_t;
inline constexpr ModelExportStatus kModelExportSuccess = 0;
class ExportOnnxSession final {
   public:
    ExportOnnxSession();
    ~ExportOnnxSession();
    ExportOnnxSession(const ExportOnnxSession&) = delete;
    ExportOnnxSession& operator=(const ExportOnnxSession&) = delete;
    void Run(const ExportOnnxRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream, std::stop_token stop = {});
    [[nodiscard]] ModelExportStatus Close() noexcept;

   private:
    struct State;
    std::shared_ptr<State> state_;
};
void export_onnx(const ExportOnnxRequest& request);
ModelInfo load_onnx_model_info(const std::filesystem::path& model_path, std::span<const RfdetrNamedOutputRole> roles = {});
void simplify_onnx_model_file(const std::filesystem::path& model_path);
}  // namespace mmltk::backend::models::rfdetr
