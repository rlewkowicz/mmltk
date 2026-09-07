#include <cstddef>
#include <vector>

#include "detail/ms_deform_attn.h"
#include "detail/ms_deform_attn_op_abi.h"

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

namespace mmltk::backend::ml::layers {

namespace {

using ms_deform_attn_detail::Slice;

}  // namespace

MsDeformTensor ms_deform_attn_cuda_autograd(const MsDeformTensor& value, const MsDeformTensor& spatial_shapes,
                                            const MsDeformTensor& level_start_index, const MsDeformTensor& sampling_locations,
                                            const MsDeformTensor& attention_weights, const std::int64_t im2col_step) {
    MsDeformTensor output;
    detail::ms_deform_attn_cuda_autograd_abi(&value, &spatial_shapes, &level_start_index, &sampling_locations, &attention_weights,
                                             im2col_step, &output);
    mmltk::common::logging::profile_add_value("backend.ml.layers.ms_deform_attn.cuda_path", 1U);
    return output;
}

MsDeformTensor ms_deform_attn_reference(const MsDeformTensor& value, const MsDeformTensor& spatial_shapes,
                                        const MsDeformTensor& sampling_locations, const MsDeformTensor& attention_weights) {
    const int64_t batch = value.size(0);
    const int64_t num_heads = value.size(2);
    const int64_t head_dim = value.size(3);
    const int64_t len_q = sampling_locations.size(1);
    const int64_t num_levels = spatial_shapes.size(0);
    const int64_t num_points = sampling_locations.size(4);

    const auto sampling_grids = sampling_locations.mul(2.0).sub(1.0);
    std::vector<MsDeformTensor> sampled_values;
    sampled_values.reserve(static_cast<size_t>(num_levels));

    int64_t start = 0;
    for (int64_t level = 0; level < num_levels; ++level) {
        const auto height = spatial_shapes.index({level, 0}).item<int64_t>();
        const auto width = spatial_shapes.index({level, 1}).item<int64_t>();
        const auto value_level =
            value.narrow(1, start, height * width).flatten(2).transpose(1, 2).reshape({batch * num_heads, head_dim, height, width});
        const auto sampling_grid_level = sampling_grids.index({Slice(), Slice(), Slice(), level}).transpose(1, 2).flatten(0, 1);
        sampled_values.push_back(ms_deform_attn_detail::grid_sample(value_level, sampling_grid_level,
                                                                    ms_deform_attn_detail::GridSampleFuncOptions()
                                                                        .mode(ms_deform_attn_detail::kBilinear)
                                                                        .padding_mode(ms_deform_attn_detail::kZeros)
                                                                        .align_corners(false)));
        start += height * width;
    }

    const auto sampled = ms_deform_attn_detail::stack(sampled_values, -2).flatten(-2);
    const auto normalized_attention = attention_weights.transpose(1, 2).reshape({batch * num_heads, 1, len_q, num_levels * num_points});
    const auto output = sampled.mul(normalized_attention).sum(-1).view({batch, num_heads * head_dim, len_q});
    return output.transpose(1, 2).contiguous();
}

}  // namespace mmltk::backend::ml::layers
