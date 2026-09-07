module;
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"

export module mmltk.backend.models.rfdetr.model_export;

export import :onnx_model_info;
export import :onnx_simplify;

namespace mmltk::backend::models::rfdetr {

void write_onnx_model_bytes(std::string_view serialized_model, const std::filesystem::path& output_path);

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

    void Run(const ExportOnnxRequest& request, mmltk::backend::ml::runtime::BorrowedCommandStream command_stream);
    [[nodiscard]] ModelExportStatus Close() noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};

void export_onnx(const ExportOnnxRequest& request);
}  // namespace mmltk::backend::models::rfdetr
