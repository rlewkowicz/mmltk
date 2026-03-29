#include "rfdetr/onnx_simplify.h"

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

#include <onnx/checker.h>
#include <onnx/shape_inference/implementation.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace fastloader::rfdetr {

void run_onnx_simplify(ONNX_NAMESPACE::ModelProto& model) {
    try {
        std::fprintf(stderr, "onnx: checking exported model...\n");
        ONNX_NAMESPACE::checker::check_model(model);
        std::fprintf(stderr, "onnx: running shape inference...\n");
        ONNX_NAMESPACE::shape_inference::InferShapes(
            model,
            ONNX_NAMESPACE::OpSchemaRegistry::Instance());
        std::fprintf(stderr, "onnx: shape inference complete\n");
    } catch (const ONNX_NAMESPACE::checker::ValidationError& error) {
        throw std::runtime_error(
            std::string("RF-DETR ONNX simplify failed: ONNX checker rejected the model: ") +
            error.what());
    } catch (const ONNX_NAMESPACE::InferenceError& error) {
        throw std::runtime_error(
            std::string("RF-DETR ONNX simplify failed: ONNX shape inference rejected the model: ") +
            error.what());
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("RF-DETR ONNX simplify failed: ") + error.what());
    }
}

} // namespace fastloader::rfdetr
