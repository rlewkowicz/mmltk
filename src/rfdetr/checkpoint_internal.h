#pragma once

#include "fastloader/rfdetr/checkpoint.h"

#include <torch/serialize.h>

namespace fastloader::rfdetr::detail {

torch::Tensor prepare_tensor_for_checkpoint_write(const torch::Tensor& tensor);
void write_state_archive(torch::serialize::OutputArchive& archive,
                         const char* key,
                         const std::vector<StateDictEntry>& state_dict);

} // namespace fastloader::rfdetr::detail
