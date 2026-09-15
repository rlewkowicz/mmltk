#pragma once

#include <filesystem>
#include <string>

#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
namespace mmltk::backend::models::rfdetr {

struct ResolvedModelArtifacts {
    std::string model_id;
    std::string preset_name;
    std::string input_kind;
    std::filesystem::path input_path;
    std::filesystem::path weights_path;
    std::filesystem::path artifact_root;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    NativeRfDetrConfig config;
    ModelClassLayout class_layout;
    std::string artifact_sha256;
    int automatic_num_queries_cap = 0;
    int source_num_queries = 0;
    int source_num_select = 0;
};

}  // namespace mmltk::backend::models::rfdetr
