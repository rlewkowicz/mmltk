#include <filesystem>
#include <string>
#include "detail/onnx_tool_main.h"
#include "src/backend/models/rfdetr/core/model_info.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.common.logging.mmltk_logging;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
void log_model_info(const std::filesystem::path& model_path) {
 const auto info = rfdetr::load_onnx_model_info(model_path);
 mmltk::common::logging::info("rfdetr.onnx_info", [&info](auto& logger) {
  logger.info(
   "model[{}]: path={} input={} {} {} outputs={} queries={} "
   "classes={} dataset_images=0 dataset_classes=0",
   info.backend, info.model_path, info.input.name, rfdetr::format_shape(info.input.shape), info.input.dtype, info.outputs.size(), info.num_queries, info.num_classes);
  for (const auto& output : info.outputs) { logger.info("  output: {} {} {}", output.name, rfdetr::format_shape(output.shape), output.dtype); }
 });
}
}  // namespace
int main(const int argc, char** argv) {
 return mmltk::entrypoints::tools::run_onnx_tool_main(argc, argv,
  {
   .usage = "usage: mmltk-rfdetr-onnx-info [--log-level LEVEL] "
            "[--log-file PATH] [--log-dir PATH] MODEL.onnx",
   .application_name = "mmltk-rfdetr-onnx-info",
   .logger_name = "rfdetr.onnx_info",
   .error_prefix = "mmltk rfdetr onnx info error: ",
  },
  &log_model_info);
}
