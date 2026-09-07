#pragma once

#include <torch/torch.h>

namespace mmltk::backend::models::rfdetr {

void pack_bool_masks_cuda_into(const torch::Tensor& masks, torch::Tensor& packed_masks);

}
