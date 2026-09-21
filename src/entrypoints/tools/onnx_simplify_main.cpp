#include <filesystem>
#include "detail/onnx_tool_main.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.common.logging.mmltk_logging;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
void simplify_model(const std::filesystem::path& model_path) { rfdetr::simplify_onnx_model_file(model_path); }
}  // namespace
int main(const int argc, char** argv) {
 return mmltk::entrypoints::tools::run_onnx_tool_main(argc, argv,
                                                      {
                                                       .usage = "usage: mmltk-rfdetr-onnx-simplify [--log-level LEVEL] "
                                                                "[--log-file PATH] [--log-dir PATH] MODEL.onnx",
                                                       .application_name = "mmltk-rfdetr-onnx-simplify",
                                                       .logger_name = "rfdetr.onnx_simplify",
                                                       .error_prefix = "mmltk rfdetr onnx simplify error: ",
                                                      },
                                                      &simplify_model);
}
