#include "rfdetr/onnx_model_io.h"
#include "rfdetr/onnx_simplify.h"
#include "rfdetr/onnx_tool_shared.h"

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

#include <string>

int main(int argc, char** argv) {
    return mmltk::rfdetr::run_single_onnx_tool_main(
        argc, argv,
        "usage: mmltk-rfdetr-onnx-simplify [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx",
        "mmltk-rfdetr-onnx-simplify", "rfdetr.onnx_simplify",
        "mmltk rfdetr onnx simplify error: ", [](const std::string& model_path) {
            ONNX_NAMESPACE::ModelProto model = mmltk::rfdetr::load_onnx_model(model_path);
            mmltk::rfdetr::run_onnx_simplify(model);
            mmltk::rfdetr::write_onnx_model(model, model_path);
        });
}
