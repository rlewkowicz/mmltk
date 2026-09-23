#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/subsystems/system/compute_runtime.h"
namespace mmltk::controller {
// One admitted GUI run. Backend requests retain their independent CLI contracts.
struct ExportRunRequest final {
 mmltk::backend::models::rfdetr::ExportOnnxRequest onnx;
 std::filesystem::path output_directory;
 bool export_onnx = true;
 bool build_tensorrt = true;
 bool allow_fp16 = true;
};
// Producers return only after their file readers settle, including failure and cancellation.
[[nodiscard]] contracts::ComputeTerminal execute_export_run(const ExportRunRequest&, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink&,
 std::function_ref<void(const mmltk::backend::models::rfdetr::ExportOnnxRequest&, const ComputeArtifactSink&)> export_onnx,
 std::function_ref<void(const mmltk::backend::models::rfdetr::BuildEngineRequest&, const ComputeArtifactSink&)> build_engine);
}  // namespace mmltk::controller
