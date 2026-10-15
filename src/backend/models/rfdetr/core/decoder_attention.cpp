#include "detail/decoder_attention.h"
#include "detail/linear.h"
#include <ATen/autocast_mode.h>
#include <ATen/ops/scaled_dot_product_attention.h>
#include <ATen/ops/_scaled_dot_product_attention_math.h>
#include "src/pch_std.h"
#include <limits>
namespace mmltk::backend::models::rfdetr {
torch::Tensor model_scaled_dot_product_attention(
 const torch::Tensor& query, const torch::Tensor& key, const torch::Tensor& value, const torch::Tensor& admitted_mask, std::optional<double> scale, bool force_math) {
 if (force_math) {
  // The pinned math primitive consumes additive masks (the public SDPA wrapper
  // converts its bool convention before dispatch). Select it without changing
  // the process-wide backend enablement used by concurrent training lanes.
  const auto device = query.device().type();
  const bool autocast = at::autocast::is_autocast_enabled(device);
  const auto cast = [&](const torch::Tensor& input) { return autocast ? at::autocast::cached_cast(at::autocast::get_autocast_dtype(device), input, device) : input; };
  const auto q = cast(query), k = cast(key), v = cast(value);
  auto mask = cast(admitted_mask);
  // Match the public SDPA autocast boundary: cast its operands once, then keep
  // the math primitive's FP32 intermediates outside ambient autocast.
  c10::impl::ExcludeDispatchKeyGuard precision(at::autocast::get_autocast_dispatch_key_from_device_type(device));
  if (mask.defined() && mask.scalar_type() == torch::kBool) mask = torch::zeros_like(mask, q.options()).masked_fill(~mask, -std::numeric_limits<double>::infinity());
  return std::get<0>(at::_scaled_dot_product_attention_math(q, k, v, mask.defined() ? std::optional<torch::Tensor>(mask) : std::nullopt, 0.0, false, std::nullopt, scale, false));
 }
 return at::scaled_dot_product_attention(query, key, value, admitted_mask.defined() ? std::optional<torch::Tensor>(admitted_mask) : std::nullopt, 0.0, false, scale);
}
torch::Tensor keypoint_self_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& position, const torch::Tensor& excluded, bool force_math) {
 const int64_t batch = target.size(0), count = target.size(1), width = target.size(2), heads = attention->options.num_heads();
 const auto positioned = (target + position).transpose(0, 1);
 const auto values = target.transpose(0, 1);
 const auto project = [&](const torch::Tensor& input, int64_t part) {
  return torch::linear(input, attention->in_proj_weight.narrow(0, part * width, width), attention->in_proj_bias.narrow(0, part * width, width))
   .reshape({count, batch, heads, width / heads}).permute({1, 2, 0, 3});
 };
 const auto mask = excluded.defined() ? torch::zeros_like(excluded, target.options()).masked_fill(excluded, -std::numeric_limits<double>::infinity()) : torch::Tensor{};
 auto result = model_scaled_dot_product_attention(project(positioned, 0), project(positioned, 1), project(values, 2), mask, std::nullopt, force_math);
 result = detail::linear_forward(*attention->out_proj, result.permute({2, 0, 1, 3}).reshape({count * batch, width}));
 return result.reshape({count, batch, width}).transpose(0, 1);
}
namespace {
torch::Tensor grouped_attention(
 const torch::Tensor& query, const torch::Tensor& key, const torch::Tensor& value, int64_t heads, int64_t groups, bool force_math, const torch::Tensor& excluded_keys = {}) {
 const auto batch = value.size(0), count = value.size(1), width = value.size(2);
 const auto split = [&](const torch::Tensor& input) { return input.reshape({batch * groups, count / groups, heads, width / heads}).transpose(1, 2); };
 // MultiheadAttention exclusion true means forbidden; SDPA true means admitted.
 const auto admitted = excluded_keys.defined() ? (~excluded_keys).unsqueeze(1).unsqueeze(1) : torch::Tensor{};
 return model_scaled_dot_product_attention(split(query), split(key), split(value), admitted, std::nullopt, force_math);
}
}  // namespace
torch::Tensor isolated_group_self_attention(
 torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& query_position, const DecoderQueryLayout& layout, bool force_math) {
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
 const auto project = [&](const torch::Tensor& input, int64_t part) {
  const auto bias = attention->in_proj_bias.defined() ? attention->in_proj_bias.narrow(0, part * width, width) : torch::Tensor{};
  return torch::linear(input, attention->in_proj_weight.narrow(0, part * width, width), bias);
 };
 if (!layout.has_denoising()) {
  const auto groups = layout.ordinary.groups, queries = layout.ordinary.queries_per_group;
  // Python batch-first MHA projects transposed [queries, groups * batch, width]
  // operands. Preserve both its group-major ordering and strides: ATen linear
  // chooses a different rounded bias operation for noncontiguous operands.
  const auto sequence_first = [&](const torch::Tensor& input) { return input.view({batch, groups, queries, width}).transpose(0, 1).reshape({groups * batch, queries, width}).transpose(0, 1); };
  const auto positioned = sequence_first(target + query_position);
  const auto q = project(positioned, 0), k = project(positioned, 1), v = project(sequence_first(target), 2);
  auto output = grouped_attention(q.transpose(0, 1), k.transpose(0, 1), v.transpose(0, 1), heads, 1, force_math);
  output = detail::linear_forward(*attention->out_proj, output.permute({2, 0, 1, 3}).reshape({queries * groups * batch, width}));
  return output.view({queries, groups, batch, width}).permute({2, 1, 0, 3}).reshape({batch, ordinary_count, width});
 }
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
 const auto positioned = clean_target + clean_position;
 const auto q = project(positioned, 0), k = project(positioned, 1), v = project(clean_target, 2);
 const auto attend = [&](int64_t offset, int64_t count, int64_t groups, const torch::Tensor& padding = torch::Tensor{}) {
  return grouped_attention(q.narrow(1, offset, count), k.narrow(1, offset, count), v.narrow(1, offset, count), heads, groups, force_math, padding).transpose(1, 2).reshape({batch, count, width});
 };
 // DN-DETR Equation 7 blocks matching queries from observing DN queries and
 // blocks cross-DN-group traffic. RF-DETR deliberately makes the boundary
 // symmetric and retains its pre-existing isolation between ordinary
 // Group-DETR groups; reshaping groups into the batch dimension realizes
 // those blocks without a quadratic all-query mask.
 auto output = attend(0, ordinary_count, layout.ordinary.groups);
 if (layout.has_denoising()) { output = torch::cat({output, attend(ordinary_count, layout.denoising_queries(), layout.denoising_groups, key_padding)}, 1); }
 return detail::linear_forward(*attention->out_proj, output);
}
}  // namespace mmltk::backend::models::rfdetr
