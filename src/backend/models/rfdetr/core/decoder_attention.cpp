#include "detail/decoder_attention.h"
#include <ATen/ops/scaled_dot_product_attention.h>
#include <stdexcept>
#include <optional>
#include <array>
namespace mmltk::backend::models::rfdetr {
namespace {
// Inputs are sequence-major grouped tensors. Projection always follows tensor
// identity: equal numerical values never merge target and positional gradients.
torch::Tensor projected_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& query, const torch::Tensor& value, const torch::Tensor& excluded_keys) {
 const auto width = value.size(2);
 const auto heads = attention->options.num_heads();
 TORCH_CHECK(heads > 0 && width == attention->options.embed_dim() && width % heads == 0 && attention->in_proj_weight.defined() && !attention->bias_k.defined() && !attention->bias_v.defined() && !attention->options.add_zero_attn() && attention->options.dropout() == 0.0, "decoder attention requires packed equal-width zero-dropout projections");
 const auto project = [&](const torch::Tensor& input, int64_t part) {
  const auto bias = attention->in_proj_bias.defined() ? attention->in_proj_bias.narrow(0, part * width, width) : torch::Tensor{};
  return torch::linear(input, attention->in_proj_weight.narrow(0, part * width, width), bias)
   .view({input.size(0), input.size(1), heads, width / heads}).permute({1, 2, 0, 3});
 };
 const auto q = project(query, 0);
 const auto k = project(query, 1);
 const auto v = project(value, 2);
 // MultiheadAttention exclusion true means forbidden; SDPA true means admitted.
 const auto admitted = excluded_keys.defined() ? (~excluded_keys).unsqueeze(1).unsqueeze(1) : torch::Tensor{};
 auto output = at::scaled_dot_product_attention(q, k, v, admitted.defined() ? std::optional<torch::Tensor>(admitted) : std::nullopt, 0.0, false);
 output = output.permute({2, 0, 1, 3}).reshape({value.size(0), value.size(1), width});
 return attention->out_proj->forward(output);
}
torch::Tensor grouped_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& position, const torch::Tensor& key_padding = {}) {
 const auto batch = target.size(0), groups = target.size(1), queries = target.size(2), width = target.size(3);
 const auto query = (target + position).permute({2, 0, 1, 3}).reshape({queries, batch * groups, width});
 const auto value = target.permute({2, 0, 1, 3}).reshape({queries, batch * groups, width});
 const auto output = projected_attention(attention, query, value, key_padding);
 return output.view({queries, batch, groups, width}).permute({1, 2, 0, 3}).reshape({batch, groups * queries, width});
}
}
torch::Tensor isolated_group_self_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& query_position, const DecoderQueryLayout& layout) {
 if (target.dim() != 3 || query_position.sizes() != target.sizes() || target.size(1) != layout.total_queries() || layout.ordinary.groups <= 0 || layout.ordinary.queries_per_group <= 0) {
  throw std::runtime_error("decoder query tensors do not match their typed group layout");
 }
 const int64_t batch = target.size(0);
 const int64_t width = target.size(2);
 const int64_t ordinary_count = layout.ordinary.total_queries();
 // DN-DETR Equation 7 blocks matching queries from observing DN queries and
 // blocks cross-DN-group traffic. RF-DETR deliberately makes the boundary
 // symmetric and retains its pre-existing isolation between ordinary
 // Group-DETR groups; reshaping groups into the batch dimension realizes
 // those blocks without a quadratic all-query mask.
 const auto ordinary_target = target.narrow(1, 0, ordinary_count).view({batch, layout.ordinary.groups, layout.ordinary.queries_per_group, width});
 const auto ordinary_position = query_position.narrow(1, 0, ordinary_count).view({batch, layout.ordinary.groups, layout.ordinary.queries_per_group, width});
 const auto ordinary_output = grouped_attention(attention, ordinary_target, ordinary_position);
 if (!layout.has_denoising()) { return ordinary_output; }
 const std::array<int64_t, 3> expected_padding{batch, layout.denoising_groups, layout.denoising_queries_per_group};
 if (!layout.denoising_key_padding.defined() || layout.denoising_key_padding.sizes() != c10::IntArrayRef(expected_padding) || layout.denoising_key_padding.scalar_type() != torch::kBool ||
     layout.denoising_key_padding.device() != target.device() || !layout.denoising_valid_slots.defined() || layout.denoising_valid_slots.sizes() != c10::IntArrayRef(expected_padding) ||
     layout.denoising_valid_slots.scalar_type() != torch::kBool || layout.denoising_valid_slots.device() != target.device()) {
  throw std::runtime_error("DN key padding does not match its typed group layout");
 }
 auto denoising_target = target.narrow(1, ordinary_count, layout.denoising_queries()).view({batch, layout.denoising_groups, layout.denoising_queries_per_group, width});
 auto denoising_position = query_position.narrow(1, ordinary_count, layout.denoising_queries()).view({batch, layout.denoising_groups, layout.denoising_queries_per_group, width});
 const auto invalid_slots = (~layout.denoising_valid_slots).unsqueeze(-1);
 denoising_target = denoising_target.masked_fill(invalid_slots, 0);
 denoising_position = denoising_position.masked_fill(invalid_slots, 0);
 const auto key_padding = layout.denoising_key_padding.reshape({batch * layout.denoising_groups, layout.denoising_queries_per_group});
 const auto denoising_output = grouped_attention(attention, denoising_target, denoising_position, key_padding);
 return torch::cat({ordinary_output, denoising_output}, 1);
}
}
