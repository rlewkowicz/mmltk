#pragma once

#include "rfdetr/postprocess.h"

#include <filesystem>
#include <memory>
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

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual const ModelInfo& info() const = 0;
    virtual OutputTensors run(const torch::Tensor& normalized_input) = 0;
    virtual void* stream() const = 0;
    virtual void save_engine(const std::filesystem::path& path);
};

std::unique_ptr<InferenceBackend> make_onnx_backend(const std::filesystem::path& model_path,
                                                    int device_id);
std::vector<std::unique_ptr<InferenceBackend>> make_onnx_backend_lanes(
    const std::filesystem::path& model_path,
    int device_id,
    int lane_count);

std::unique_ptr<InferenceBackend> make_tensorrt_backend(const std::filesystem::path& model_path,
                                                        int device_id,
                                                        bool allow_fp16,
                                                        const std::filesystem::path& save_engine_path = {});
std::vector<std::unique_ptr<InferenceBackend>> make_tensorrt_backend_lanes(
    const std::filesystem::path& model_path,
    int device_id,
    bool allow_fp16,
    int lane_count,
    const std::filesystem::path& save_engine_path = {});

} // namespace fastloader::rfdetr
