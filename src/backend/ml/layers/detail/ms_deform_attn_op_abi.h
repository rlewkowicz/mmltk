#pragma once

#include <cstdint>

namespace mmltk::backend::ml::layers::detail {

void ms_deform_attn_cuda_autograd_abi(const void* value, const void* spatial_shapes, const void* level_start_index,
                                      const void* sampling_locations, const void* attention_weights, std::int64_t im2col_step,
                                      void* output);

}  // namespace mmltk::backend::ml::layers::detail
