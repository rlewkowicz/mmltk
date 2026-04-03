#include "rfdetr/onnx_model_io.h"
#include "rfdetr/onnx_simplify.h"
#include "rfdetr/onnx_tool_args.h"
#include "mmltk_logging.h"

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void report_onnx_simplify_error(std::string_view message) noexcept {
    try {
        mmltk::logging::logger("rfdetr.onnx_simplify")->error("{}", message);
        return;
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
        return;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const mmltk::rfdetr::SingleOnnxToolArgs parsed = mmltk::rfdetr::parse_single_onnx_tool_args(
            argc,
            argv,
            "usage: mmltk-rfdetr-onnx-simplify [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx");
        mmltk::logging::initialize(
            mmltk::logging::merge(mmltk::logging::config_from_env("mmltk-rfdetr-onnx-simplify"),
                                  parsed.logging));
        ONNX_NAMESPACE::ModelProto model = mmltk::rfdetr::load_onnx_model(parsed.model_path);

        mmltk::rfdetr::run_onnx_simplify(model);
        mmltk::rfdetr::write_onnx_model(model, parsed.model_path);
    } catch (const std::exception& error) {
        report_onnx_simplify_error(std::string("mmltk rfdetr onnx simplify error: ") + error.what());
        return 1;
    }

    return 0;
}
