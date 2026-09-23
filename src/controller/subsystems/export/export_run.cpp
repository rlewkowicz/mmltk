#include "export_run.h"
#include <optional>
#include <stdexcept>
#include "src/common/io/staging_directory.h"
namespace mmltk::controller {
contracts::ComputeTerminal execute_export_run(const ExportRunRequest& request, const std::stop_token stop, const ComputeProgressSink& progress, const ComputeArtifactSink& published,
 std::function_ref<void(const mmltk::backend::models::rfdetr::ExportOnnxRequest&, const ComputeArtifactSink&)> export_onnx,
 std::function_ref<void(const mmltk::backend::models::rfdetr::BuildEngineRequest&, const ComputeArtifactSink&)> build_engine) {
 if (!request.export_onnx && !request.build_tensorrt) throw std::invalid_argument("Select ONNX or TensorRT for export");
 if (request.output_directory.empty()) throw std::invalid_argument("Export output directory is unavailable");
 const auto cancelled = [] { return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled); };
 if (stop.stop_requested()) return cancelled();
 auto onnx = request.onnx;
 onnx.output_path = request.output_directory / "model.onnx";
 std::optional<mmltk::common::io::StagingDirectory> intermediate;
 if (!request.export_onnx) {
  intermediate.emplace(onnx.output_path, ".", ".export-XXXXXX", "create temporary export directory");
  onnx.output_path = intermediate->path() / "model.onnx";
 }
 std::uint64_t sequence = 0U;
 std::uint64_t completed = 0U;
 const std::uint64_t total = request.build_tensorrt ? 2U : 1U;
 const auto stage = [&](const char* status) {
  if (progress) progress({.sequence = ++sequence, .completed = completed, .total = total, .status = status});
 };
 bool committed = false;
 const ComputeArtifactSink onnx_published = [&](const auto& path) {
  committed = true;
  if (request.export_onnx && published) published(path);
 };
 stage("Exporting ONNX");
 if (stop.stop_requested()) return cancelled();
 export_onnx(onnx, onnx_published);
 if (stop.stop_requested()) return cancelled();
 if (!committed) throw std::runtime_error("ONNX export returned without publishing its artifact");
 ++completed;
 if (request.build_tensorrt) {
  mmltk::backend::models::rfdetr::BuildEngineRequest engine{};
  engine.onnx_path = onnx.output_path;
  engine.preset_name = onnx.preset_name;
  engine.resolution = onnx.resolution;
  engine.device_id = onnx.device_id;
  engine.output_path = request.output_directory / "model.engine";
  engine.allow_fp16 = request.allow_fp16;
  // The exported ONNX owns embedded/automatic class facts. A weights descriptor
  // describes the source weights and must not be applied to the new ONNX identity.
  stage("Building TensorRT engine");
  if (stop.stop_requested()) return cancelled();
  committed = false;
  const ComputeArtifactSink engine_published = [&](const auto& path) {
   committed = true;
   if (published) published(path);
  };
  build_engine(engine, engine_published);
  if (stop.stop_requested()) return cancelled();
  if (!committed) throw std::runtime_error("TensorRT build returned without publishing its artifact");
  ++completed;
 }
 stage("Export complete");
 return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, completed, request.output_directory.string());
}
}  // namespace mmltk::controller
