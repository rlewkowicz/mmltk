#pragma once

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace mmltk::backend::models::rfdetr {

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
    int64_t automatic_num_queries_cap = 0;
    int64_t num_classes = 0;
    bool has_masks = false;
};

[[nodiscard]] inline std::string format_shape(const std::vector<int64_t>& shape) {
    std::ostringstream stream;
    stream << "[";
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index > 0) { stream << ", "; }
        stream << shape[index];
    }
    stream << "]";
    return stream.str();
}

// Infers the RF-DETR output layout from the output tensor shapes: a [batch, queries, 4] output carries the
// query count, any other rank-3 output carries the class count, and a rank-4 output marks mask support.
inline void infer_rfdetr_output_layout(ModelInfo& info) {
    for (const TensorInfo& output : info.outputs) {
        if (output.shape.size() == 3 && output.shape.back() == 4) {
            info.num_queries = output.shape[1];
        } else if (output.shape.size() == 3) {
            info.num_classes = output.shape[2];
        } else if (output.shape.size() == 4) {
            info.has_masks = true;
        }
    }
}

}  // namespace mmltk::backend::models::rfdetr
