#pragma once

#include <torch/torch.h>

#include <utility>

namespace mmltk::rfdetr {

inline std::pair<torch::Tensor, torch::Tensor> make_normalization_tensors(int device_id) {
    return {
        torch::tensor({0.485f, 0.456f, 0.406f},
                      torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id))
            .view({1, 3, 1, 1}),
        torch::tensor({0.229f, 0.224f, 0.225f},
                      torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id))
            .view({1, 3, 1, 1}),
    };
}

}  // namespace mmltk::rfdetr
