#pragma once

#include "mmltk/rfdetr/model_info.h"

#include "rfdetr/postprocess.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

class InferenceBackend {
   public:
    virtual ~InferenceBackend() = default;

    [[nodiscard]] virtual const ModelInfo& info() const = 0;
    virtual OutputTensors run(const torch::Tensor& normalized_input) = 0;
    [[nodiscard]] virtual void* stream() const = 0;
    [[nodiscard]] virtual std::vector<std::unique_ptr<InferenceBackend>> make_lanes(int count) const = 0;
    virtual void save_engine(const std::filesystem::path& path);
};

using TensorRtLogSink = std::function<void(const std::string&)>;

class ScopedTensorRtLogSink {
   public:
    explicit ScopedTensorRtLogSink(TensorRtLogSink sink);
    ~ScopedTensorRtLogSink();

    ScopedTensorRtLogSink(const ScopedTensorRtLogSink&) = delete;
    ScopedTensorRtLogSink& operator=(const ScopedTensorRtLogSink&) = delete;

   private:
    TensorRtLogSink previous_sink_;
};

std::unique_ptr<InferenceBackend> make_onnx_backend(const std::filesystem::path& model_path, int device_id);
std::vector<std::unique_ptr<InferenceBackend>> make_onnx_backend_lanes(const std::filesystem::path& model_path,
                                                                       int device_id, int lane_count);

std::unique_ptr<InferenceBackend> make_tensorrt_backend(const std::filesystem::path& model_path, int device_id,
                                                        bool allow_fp16,
                                                        const std::filesystem::path& save_engine_path = {});
std::vector<std::unique_ptr<InferenceBackend>> make_tensorrt_backend_lanes(
    const std::filesystem::path& model_path, int device_id, bool allow_fp16, int lane_count,
    const std::filesystem::path& save_engine_path = {});

}  
