#pragma once

#include "mmltk/rfdetr/model_info.h"

#include <filesystem>

namespace mmltk::rfdetr {

ModelInfo load_onnx_model_info(const std::filesystem::path& model_path);

} // namespace mmltk::rfdetr
