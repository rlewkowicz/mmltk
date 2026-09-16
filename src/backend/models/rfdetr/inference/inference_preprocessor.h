#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include "src/backend/data/dataset_loader.h"
#include "src/backend/ml/torch/detail/torch_api.h"
namespace mmltk::backend::models::rfdetr {
class InferenceBatchPreprocessor final {
   public:
    InferenceBatchPreprocessor(const std::int64_t capacity, const int height, const int width, const int device,
                               const mmltk::backend::ml::torch_api::ScalarType type)
        : capacity_(capacity),
          height_(height),
          width_(width),
          device_(device),
          output_(type == mmltk::backend::ml::torch_api::kFloat
                      ? mmltk::backend::ml::torch_api::Tensor{}
                      : mmltk::backend::ml::torch_api::empty(
                            {capacity, 3, height, width},
                            mmltk::backend::ml::torch_api::TensorOptions().dtype(type).device(
                                mmltk::backend::ml::torch_api::kCUDA, static_cast<mmltk::backend::ml::torch_api::DeviceIndex>(device)))) {}
    [[nodiscard]] mmltk::backend::ml::torch_api::Tensor Run(const mmltk::backend::data::Batch& batch) {
        const auto active = static_cast<std::int64_t>(batch.num_images);
        if (active <= 0 || active > capacity_ || batch.device_images == nullptr) {
            throw std::invalid_argument("invalid RF-DETR inference preprocessing batch");
        }
        const std::array<std::int64_t, 4> shape{active, 3, height_, width_};
        const auto input = mmltk::backend::ml::torch_api::from_blob(
            const_cast<float*>(batch.device_images), mmltk::backend::ml::torch_api::IntArrayRef{shape},
            mmltk::backend::ml::torch_api::TensorOptions()
                .dtype(mmltk::backend::ml::torch_api::kFloat)
                .device(mmltk::backend::ml::torch_api::kCUDA, static_cast<mmltk::backend::ml::torch_api::DeviceIndex>(device_)));
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
    mmltk::backend::ml::torch_api::Tensor output_;
};
}  // namespace mmltk::backend::models::rfdetr
