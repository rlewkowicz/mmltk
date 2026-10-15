// RF-DETR GroupPose operations, pinned core e2b5f096, Apache-2.0.
#include "detail/keypoint_head.h"
#include "detail/linear.h"
#include <ATen/TensorIndexing.h>
#include <algorithm>
#include <numeric>
#include <set>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr::detail {
using namespace torch::indexing;
ConditionalQueryInitializer::ConditionalQueryInitializer(int64_t dimension, int64_t count, int64_t output_dimension) {
 queries_ = register_parameter("queries", torch::randn({count, output_dimension}));
 query_norm_ = register_module("query_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({output_dimension}).elementwise_affine(false)));
 modulation_ = register_module("adaLN_modulation", torch::nn::Sequential(torch::nn::Linear(dimension, dimension), torch::nn::GELU(), torch::nn::Linear(dimension, output_dimension * 3)));
 output_ = register_module("out_proj", torch::nn::Linear(output_dimension, output_dimension));
 torch::NoGradGuard guard;
 modulation_->at<torch::nn::LinearImpl>(2).weight.zero_();
 modulation_->at<torch::nn::LinearImpl>(2).bias.zero_();
}
torch::Tensor ConditionalQueryInitializer::forward(const torch::Tensor& condition) {
 const auto hidden = modulation_->at<torch::nn::GELUImpl>(1).forward(linear_forward(modulation_->at<torch::nn::LinearImpl>(0), condition.unsqueeze(-2)));
 const auto modulation = linear_forward(modulation_->at<torch::nn::LinearImpl>(2), hidden).chunk(3, -1);
 return linear_forward(*output_, (modulation[0] + 1.0) * query_norm_->forward(queries_) + modulation[1]) * modulation[2] + queries_;
}
int64_t compact_keypoint_count(std::span<const std::uint16_t> counts) { return std::accumulate(counts.begin(), counts.end(), int64_t{0}); }
torch::Tensor keypoint_active_mask(std::span<const std::uint16_t> counts) {
 const int64_t maximum = counts.empty() ? 0 : *std::ranges::max_element(counts);
 auto result = torch::zeros({static_cast<int64_t>(counts.size()), maximum}, torch::kBool);
 for (size_t index = 0; index < counts.size(); ++index) result[index].narrow(0, 0, counts[index]).fill_(true);
 return result;
}
torch::Tensor keypoint_class_mask(std::span<const std::uint16_t> counts) {
 const auto total = compact_keypoint_count(counts);
 auto result = torch::ones({total + 1, total + 1}, torch::kBool);
 result[0].fill_(false);
 result.select(1, 0).fill_(false);
 int64_t offset = 1;
 for (auto count : counts) {
  result.narrow(0, offset, count).narrow(1, offset, count).fill_(false);
  offset += count;
 }
 return result;
}
torch::Tensor refine_keypoints(const torch::Tensor& delta, const torch::Tensor& references) {
 const auto safe = torch::nan_to_num(delta, 0.0, 0.0, 0.0);
 return torch::cat({safe.narrow(-1, 0, 2) * references.narrow(-1, 2, 2).unsqueeze(-2) + references.narrow(-1, 0, 2).unsqueeze(-2), safe.narrow(-1, 2, kKeypointPredictionDimension - 2)}, -1);
}
torch::Tensor padded_keypoints(const torch::Tensor& compact, std::span<const std::uint16_t> counts) {
 TORCH_CHECK(!counts.empty() && compact.dim() >= 4 && compact.size(-2) == compact_keypoint_count(counts) && compact.size(-1) == kKeypointPredictionDimension, "GroupPose compact output disagrees with admitted schema");
 const int64_t maximum = *std::ranges::max_element(counts);
 std::vector<torch::Tensor> classes;
 classes.reserve(counts.size());
 int64_t offset = 0;
 for (auto count : counts) {
  auto joints = compact.narrow(-2, offset, count);
  if (count < maximum) {
   auto shape = compact.sizes().vec();
   shape[shape.size() - 2] = maximum - count;
   joints = torch::cat({joints, torch::zeros(shape, compact.options())}, -2);
  }
  classes.push_back(joints);
  offset += count;
 }
 return torch::cat(classes, -2);
}
torch::Tensor keypoint_class_logits(const torch::Tensor& padded, const torch::Tensor& active, int64_t classes) {
 auto shape = padded.sizes().vec();
 shape.resize(shape.size() - 2);
 shape.push_back(active.size(0));
 shape.push_back(active.size(1));
 auto boost = (padded.select(-1, 7).reshape(shape) * active.to(padded.scalar_type())).sum(-1);
 if (active.size(0) < classes) {
  shape = boost.sizes().vec();
  shape.back() = classes - active.size(0);
  boost = torch::cat({boost, torch::zeros(shape, boost.options())}, -1);
 }
 return boost;
}
std::vector<std::uint16_t> keypoint_counts_from_mask(const torch::Tensor& mask) {
 namespace catalog = mmltk::backend::data::catalog;
 if (!mask.defined() || mask.scalar_type() != torch::kBool || mask.dim() != 2 || mask.size(0) > static_cast<int64_t>(kMaximumClassOutputSlots) || mask.size(1) > static_cast<int64_t>(catalog::kKeypointsPerClassCapacity)) throw std::invalid_argument("invalid RF-DETR active-keypoint mask shape or dtype");
 const auto cpu = mask.to(torch::kCPU).contiguous();
 const auto* values = cpu.data_ptr<bool>();
 std::vector<std::uint16_t> counts(mask.size(0));
 for (int64_t row = 0; row < mask.size(0); ++row) {
  bool inactive = false;
  for (int64_t column = 0; column < mask.size(1); ++column) {
   const bool active = values[row * mask.size(1) + column];
   if (inactive && active) throw std::invalid_argument("active-keypoint mask must contain contiguous leading joints");
   inactive = inactive || !active;
   counts[row] += active;
  }
 }
 if (std::ranges::count_if(counts, [](auto count) { return count != 0; }) > static_cast<int64_t>(catalog::kClassCatalogCapacity)) throw std::invalid_argument("active-keypoint mask exceeds foreground class capacity");
 const auto maximum = counts.empty() ? 0U : *std::ranges::max_element(counts);
 if (mask.size(1) != maximum) throw std::invalid_argument("active-keypoint mask has noncanonical padding width");
 return counts;
}
std::vector<std::pair<int64_t, int64_t>> keypoint_transfer_rows(const ResolvedClassLayout& source, std::span<const std::uint16_t> source_counts, const ResolvedClassLayout& destination, std::span<const std::uint16_t> destination_counts) {
 source.require_execution(true);
 destination.require_execution(true);
 validate_keypoint_layout(source, source_counts);
 validate_keypoint_layout(destination, destination_counts);
 std::vector<int64_t> source_offsets(source.catalog()->size(), -1);
 int64_t offset = 0;
 for (size_t slot = 0; slot < source_counts.size(); ++slot) {
  if (const auto index = source.record().slots[slot].foreground_index) source_offsets[*index] = offset;
  offset += source_counts[slot];
 }
 std::vector<std::pair<int64_t, int64_t>> rows;
 offset = 0;
 for (size_t slot = 0; slot < destination_counts.size(); ++slot) {
  const auto count = destination_counts[slot];
  const auto index = destination.record().slots[slot].foreground_index;
  if (index && count) {
   const auto source_index = source.catalog()->resolve(destination.catalog()->names()[*index]);
   if (source_index && source_offsets[*source_index] >= 0) {
    const auto& from = source.catalog()->keypoints(*source_index);
    const auto& to = destination.catalog()->keypoints(*index);
    const auto shared_edges = [](const auto& schema, const auto& other) {
     std::set<std::pair<std::string_view, std::string_view>> edges;
     for (const auto& edge : schema.skeleton) {
      std::string_view first = schema.names[edge.first], second = schema.names[edge.second];
      if (std::ranges::find(other.names, first) == other.names.end() || std::ranges::find(other.names, second) == other.names.end()) continue;
      if (second < first) std::swap(first, second);
      edges.emplace(first, second);
     }
     return edges;
    };
    if (shared_edges(from, to) != shared_edges(to, from)) throw std::invalid_argument("Transfer has conflicting skeleton meaning for shared named joints");
    for (size_t joint = 0; joint < to.names.size(); ++joint) {
     const auto match = std::ranges::find(from.names, to.names[joint]);
     if (match == from.names.end()) continue;
     const auto source_joint = static_cast<size_t>(match - from.names.begin());
     if (from.oks_sigmas[source_joint] != to.oks_sigmas[joint] || from.names[from.horizontal_flip[source_joint]] != to.names[to.horizontal_flip[joint]]) throw std::invalid_argument("Transfer has conflicting meaning for named joint " + to.names[joint]);
     rows.emplace_back(source_offsets[*source_index] + source_joint, offset + joint);
    }
   }
  }
  offset += count;
 }
 return rows;
}
void validate_keypoint_layout(const ResolvedClassLayout& layout, std::span<const std::uint16_t> counts) {
 if (counts.size() > layout.output_width()) throw std::invalid_argument("keypoint schema exceeds classifier width");
 if (!layout.semantic()) return;
 for (size_t slot = 0; slot < layout.output_width(); ++slot) {
  const auto& entry = layout.record().slots[slot];
  const auto expected = entry.foreground_index ? layout.catalog()->keypoints(*entry.foreground_index).names.size() : 0U;
  const auto actual = slot < counts.size() ? counts[slot] : 0U;
  if (actual != expected) throw std::invalid_argument("active-keypoint mask disagrees with named class/joint schema at output slot " + std::to_string(slot));
 }
}
}
