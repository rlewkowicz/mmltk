#pragma once

#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE onnx_torch
#endif

#include <onnx/onnx_pb.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

std::runtime_error onnx_model_io_error(const char* message, const std::filesystem::path& path);
ONNX_NAMESPACE::ModelProto load_onnx_model(const std::filesystem::path& model_path);
void write_onnx_model(const ONNX_NAMESPACE::ModelProto& model, const std::filesystem::path& output_path);
std::string serialize_onnx_model_proto(
    const std::shared_ptr<ONNX_NAMESPACE::ModelProto>& model_proto);
void write_onnx_model_proto(const std::shared_ptr<ONNX_NAMESPACE::ModelProto>& model_proto,
                            const std::filesystem::path& output_path);

} // namespace mmltk::rfdetr
