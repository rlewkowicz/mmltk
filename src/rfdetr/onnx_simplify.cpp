#include "rfdetr/onnx_simplify.h"
#include "mmltk_logging.h"

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

#include <onnx/checker.h>
#include <onnx/shape_inference/implementation.h>

#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

void run_onnx_simplify(ONNX_NAMESPACE::ModelProto& model) {
    try {
        mmltk::logging::logger("rfdetr.onnx_simplify")->info("onnx: checking exported model...");
        ONNX_NAMESPACE::checker::check_model(model);
        mmltk::logging::logger("rfdetr.onnx_simplify")->info("onnx: running shape inference...");
        ONNX_NAMESPACE::shape_inference::InferShapes(model, ONNX_NAMESPACE::OpSchemaRegistry::Instance());
        mmltk::logging::logger("rfdetr.onnx_simplify")->info("onnx: shape inference complete");
    } catch (const ONNX_NAMESPACE::checker::ValidationError& error) {
        throw std::runtime_error(std::string("RF-DETR ONNX simplify failed: ONNX checker rejected the model: ") +
                                 error.what());
    } catch (const ONNX_NAMESPACE::InferenceError& error) {
        throw std::runtime_error(
            std::string("RF-DETR ONNX simplify failed: ONNX shape inference rejected the model: ") + error.what());
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("RF-DETR ONNX simplify failed: ") + error.what());
    }
}

}  // namespace mmltk::rfdetr
