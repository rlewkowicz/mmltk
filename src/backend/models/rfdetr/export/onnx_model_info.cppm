module;
#include <filesystem>

#include "src/backend/models/rfdetr/core/model_info.h"

export module mmltk.backend.models.rfdetr.model_export:onnx_model_info;

export namespace mmltk::backend::models::rfdetr {

ModelInfo load_onnx_model_info(const std::filesystem::path& model_path);

}
