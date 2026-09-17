#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include "src/backend/data/dataset_loader.h"
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
class InferenceBatchPreprocessor final {
   public:
    InferenceBatchPreprocessor(const std::int64_t capacity, const int height, const int width, const int device, const at::ScalarType type)
        : capacity_(capacity),
          height_(height),
          width_(width),
          device_(device),
          output_(type == at::kFloat ? torch::Tensor{}
                                     : torch::empty({capacity, 3, height, width},
                                                    torch::TensorOptions().dtype(type).device(torch::kCUDA, static_cast<c10::DeviceIndex>(device)))) {}
    [[nodiscard]] torch::Tensor Run(const mmltk::backend::data::Batch& batch) {
        const auto active = static_cast<std::int64_t>(batch.num_images);
        if (active <= 0 || active > capacity_ || batch.device_images == nullptr) {
            throw std::invalid_argument("invalid RF-DETR inference preprocessing batch");
        }
        const std::array<std::int64_t, 4> shape{active, 3, height_, width_};
        const auto input = torch::from_blob(const_cast<float*>(batch.device_images), at::IntArrayRef{shape},
                                            torch::TensorOptions().dtype(at::kFloat).device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_)));
        if (!output_.defined()) return input;
        auto result = output_.narrow(0, 0, active);
        result.copy_(input);
        return result;
    }

   private:
    std::int64_t capacity_;
    int height_;
    int width_;
    int device_;
    torch::Tensor output_;
};
}  // namespace mmltk::backend::models::rfdetr
