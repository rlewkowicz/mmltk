#pragma once

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

namespace ONNX_NAMESPACE {
class ModelProto;
}

namespace mmltk::rfdetr {

void run_onnx_simplify(ONNX_NAMESPACE::ModelProto& model);

}
