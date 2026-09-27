#include "detail/decoder_attention.h"
#include <ATen/ops/scaled_dot_product_attention.h>
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace {
torch::Tensor grouped_attention(const torch::Tensor& query, const torch::Tensor& key, const torch::Tensor& value, int64_t heads, int64_t groups, const torch::Tensor& excluded_keys = {}) {
 const auto batch = value.size(0), count = value.size(1), width = value.size(2);
 const auto split = [&](const torch::Tensor& input) { return input.reshape({batch * groups, count / groups, heads, width / heads}).transpose(1, 2); };
 // MultiheadAttention exclusion true means forbidden; SDPA true means admitted.
 const auto admitted = excluded_keys.defined() ? (~excluded_keys).unsqueeze(1).unsqueeze(1) : torch::Tensor{};
 const auto output = at::scaled_dot_product_attention(split(query), split(key), split(value), admitted.defined() ? std::optional<torch::Tensor>(admitted) : std::nullopt, 0.0, false);
 return output.transpose(1, 2).reshape({batch, count, width});
}
}  // namespace
torch::Tensor isolated_group_self_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& query_position, const DecoderQueryLayout& layout) {
 if (target.dim() != 3 || query_position.sizes() != target.sizes() || target.size(1) != layout.total_queries() || layout.ordinary.groups <= 0 || layout.ordinary.queries_per_group <= 0) {
  throw std::runtime_error("decoder query tensors do not match their typed group layout");
 }
 const int64_t batch = target.size(0);
 const int64_t width = target.size(2);
 const int64_t ordinary_count = layout.ordinary.total_queries();
 const auto heads = attention->options.num_heads();
 TORCH_CHECK(heads > 0 && width == attention->options.embed_dim() && width % heads == 0 && attention->in_proj_weight.defined() && !attention->bias_k.defined() && !attention->bias_v.defined() &&
              !attention->options.add_zero_attn() && attention->options.dropout() == 0.0,
  "decoder attention requires packed equal-width zero-dropout projections");
 auto clean_target = target;
 auto clean_position = query_position;
 torch::Tensor key_padding;
 if (layout.has_denoising()) {
  const std::array<int64_t, 3> expected_padding{batch, layout.denoising_groups, layout.denoising_queries_per_group};
  if (!layout.denoising_key_padding.defined() || layout.denoising_key_padding.sizes() != c10::IntArrayRef(expected_padding) || layout.denoising_key_padding.scalar_type() != torch::kBool ||
      layout.denoising_key_padding.device() != target.device() || !layout.denoising_valid_slots.defined() || layout.denoising_valid_slots.sizes() != c10::IntArrayRef(expected_padding) ||
      layout.denoising_valid_slots.scalar_type() != torch::kBool || layout.denoising_valid_slots.device() != target.device()) {
   throw std::runtime_error("DN key padding does not match its typed group layout");
  }
  const auto invalid_slots = (~layout.denoising_valid_slots).reshape({batch, layout.denoising_queries(), 1});
  clean_target = torch::cat({target.narrow(1, 0, ordinary_count), target.narrow(1, ordinary_count, layout.denoising_queries()).masked_fill(invalid_slots, 0)}, 1);
  clean_position = torch::cat({query_position.narrow(1, 0, ordinary_count), query_position.narrow(1, ordinary_count, layout.denoising_queries()).masked_fill(invalid_slots, 0)}, 1);
  key_padding = layout.denoising_key_padding.reshape({batch * layout.denoising_groups, layout.denoising_queries_per_group});
 }
 // Apply each shared projection once over all queries, as in upstream MHA.
 // Splitting projections by attention block separately rounds AMP parameter
 // gradients. Only the attention matrix needs block isolation.
 const auto project = [&](const torch::Tensor& input, int64_t part) {
  const auto bias = attention->in_proj_bias.defined() ? attention->in_proj_bias.narrow(0, part * width, width) : torch::Tensor{};
  return torch::linear(input, attention->in_proj_weight.narrow(0, part * width, width), bias);
 };
 const auto positioned = clean_target + clean_position;
 const auto q = project(positioned, 0), k = project(positioned, 1), v = project(clean_target, 2);
 const auto attend = [&](int64_t offset, int64_t count, int64_t groups, const torch::Tensor& padding = torch::Tensor{}) {
  return grouped_attention(q.narrow(1, offset, count), k.narrow(1, offset, count), v.narrow(1, offset, count), heads, groups, padding);
 };
 // DN-DETR Equation 7 blocks matching queries from observing DN queries and
 // blocks cross-DN-group traffic. RF-DETR deliberately makes the boundary
 // symmetric and retains its pre-existing isolation between ordinary
 // Group-DETR groups; reshaping groups into the batch dimension realizes
 // those blocks without a quadratic all-query mask.
 auto output = attend(0, ordinary_count, layout.ordinary.groups);
 if (layout.has_denoising()) { output = torch::cat({output, attend(ordinary_count, layout.denoising_queries(), layout.denoising_groups, key_padding)}, 1); }
 return attention->out_proj->forward(output);
}
}  // namespace mmltk::backend::models::rfdetr
