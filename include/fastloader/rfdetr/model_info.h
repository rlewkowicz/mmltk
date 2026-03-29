#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fastloader::rfdetr {

struct TensorInfo {
    std::string name;
    std::vector<int64_t> shape;
    std::string dtype;
};

struct ModelInfo {
    std::string backend;
    std::string model_path;
    TensorInfo input;
    std::vector<TensorInfo> outputs;
    int64_t num_queries = 0;
    int64_t num_classes = 0;
    bool has_masks = false;
};

} // namespace fastloader::rfdetr
