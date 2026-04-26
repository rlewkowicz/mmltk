#include "rfdetr/ms_deform_attn_op.h"

#include "profile_utils.h"
#include "rfdetr/ms_deform_attn_cuda.h"

#include <torch/autograd.h>

namespace mmltk::rfdetr {

namespace {

torch::Tensor cast_for_kernel(const torch::Tensor& tensor) {
    if (tensor.scalar_type() == torch::kFloat32) {
        return tensor.contiguous();
    }
    if (tensor.scalar_type() == torch::kHalf || tensor.scalar_type() == torch::kBFloat16) {
        return tensor.to(torch::kFloat32).contiguous();
    }
    TORCH_CHECK(false,
                "RF-DETR deformable attention CUDA kernel only supports float32 inputs or AMP upcasts from fp16/bf16");
}

torch::ScalarType restore_dtype(torch::autograd::AutogradContext* ctx, const char* key) {
    return static_cast<torch::ScalarType>(ctx->saved_data[key].toInt());
}

class MsDeformAttnAutograd : public torch::autograd::Function<MsDeformAttnAutograd> {
   public:
    static torch::autograd::variable_list forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& value,
                                                  const torch::Tensor& spatial_shapes,
                                                  const torch::Tensor& level_start_index,
                                                  const torch::Tensor& sampling_locations,
                                                  const torch::Tensor& attention_weights, int64_t im2col_step) {
        TORCH_CHECK(value.is_cuda(), "RF-DETR deformable attention CUDA op requires CUDA tensors");
        TORCH_CHECK(spatial_shapes.is_cuda(), "RF-DETR deformable attention spatial_shapes must be CUDA");
        TORCH_CHECK(level_start_index.is_cuda(), "RF-DETR deformable attention level_start_index must be CUDA");
        TORCH_CHECK(sampling_locations.is_cuda(), "RF-DETR deformable attention sampling_locations must be CUDA");
        TORCH_CHECK(attention_weights.is_cuda(), "RF-DETR deformable attention attention_weights must be CUDA");

        auto value_fp32 = cast_for_kernel(value);
        auto sampling_locations_fp32 = cast_for_kernel(sampling_locations);
        auto attention_weights_fp32 = cast_for_kernel(attention_weights);
        auto spatial_shapes_contig = spatial_shapes.contiguous();
        auto level_start_index_contig = level_start_index.contiguous();

        auto output = ms_deform_attn_cuda_forward(value_fp32, spatial_shapes_contig, level_start_index_contig,
                                                  sampling_locations_fp32, attention_weights_fp32, im2col_step);

        ctx->save_for_backward({
            value_fp32,
            spatial_shapes_contig,
            level_start_index_contig,
            sampling_locations_fp32,
            attention_weights_fp32,
        });
        ctx->saved_data["im2col_step"] = im2col_step;
        ctx->saved_data["value_dtype"] = static_cast<int64_t>(value.scalar_type());
        ctx->saved_data["sampling_dtype"] = static_cast<int64_t>(sampling_locations.scalar_type());
        ctx->saved_data["attention_dtype"] = static_cast<int64_t>(attention_weights.scalar_type());
        MMLTK_PROFILE_ADD("rfdetr.ms_deform_attn.cuda_path", 1);
        return {output.to(value.scalar_type())};
    }

    static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx,
                                                   torch::autograd::variable_list grad_outputs) {
        const auto saved = ctx->get_saved_variables();
        auto grad_output = grad_outputs.front();
        TORCH_CHECK(grad_output.defined(), "RF-DETR deformable attention backward requires grad_output");
        grad_output = cast_for_kernel(grad_output);

        auto grads = ms_deform_attn_cuda_backward(saved[0], saved[1], saved[2], saved[3], saved[4], grad_output,
                                                  ctx->saved_data["im2col_step"].toInt());

        auto grad_value = grads[0].to(restore_dtype(ctx, "value_dtype"));
        auto grad_sampling_locations = grads[1].to(restore_dtype(ctx, "sampling_dtype"));
        auto grad_attention_weights = grads[2].to(restore_dtype(ctx, "attention_dtype"));
        return {
            grad_value,      torch::Tensor(), torch::Tensor(), grad_sampling_locations, grad_attention_weights,
            torch::Tensor(),
        };
    }
};

}  // namespace

torch::Tensor ms_deform_attn_cuda_autograd(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                           const torch::Tensor& level_start_index,
                                           const torch::Tensor& sampling_locations,
                                           const torch::Tensor& attention_weights, int64_t im2col_step) {
    auto outputs = MsDeformAttnAutograd::apply(value, spatial_shapes, level_start_index, sampling_locations,
                                               attention_weights, im2col_step);
    return outputs.front();
}

}  // namespace mmltk::rfdetr
