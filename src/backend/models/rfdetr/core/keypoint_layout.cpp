// RF-DETR GroupPose operations, pinned core e2b5f096, Apache-2.0.
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/catalog/keypoint_schema.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
namespace mmltk::backend::models::rfdetr {
namespace catalog = mmltk::backend::data::catalog;
KeypointTensorLayout::KeypointTensorLayout(std::span<const std::uint16_t> counts) {
 if (counts.size() > kMaximumClassOutputSlots) throw std::invalid_argument("keypoint schema exceeds physical slot capacity");
 std::size_t active_classes = 0;
 for (const auto count : counts) {
  if (count > catalog::kKeypointsPerClassCapacity) throw std::invalid_argument("keypoint schema exceeds per-class joint capacity");
  active_classes += count != 0;
 }
 if (active_classes > catalog::kClassCatalogCapacity) throw std::invalid_argument("keypoint schema exceeds foreground class capacity");
 counts_.assign(counts.begin(), counts.end());
 compact_offsets_.reserve(counts.size() + 1);
 compact_offsets_.push_back(0);
 for (const auto count : counts) {
  compact_offsets_.push_back(compact_offsets_.back() + count);
  maximum_joints_ = std::max(maximum_joints_, static_cast<std::int64_t>(count));
 }
 // Capacity admission above bounds both products well within tensor indexing.
 padded_count_ = slot_count() * maximum_joints_;
}
std::int64_t KeypointTensorLayout::padded_offset(std::size_t slot) const {
 if (slot > counts_.size()) throw std::out_of_range("keypoint physical slot exceeds schema extent");
 return static_cast<std::int64_t>(slot) * maximum_joints_;
}
torch::Tensor KeypointTensorLayout::active_mask() const {
 auto result = torch::zeros({slot_count(), maximum_joints_}, torch::kBool);
 for (std::size_t index = 0; index < counts_.size(); ++index) result[index].narrow(0, 0, counts_[index]).fill_(true);
 return result;
}
torch::Tensor KeypointTensorLayout::class_mask() const {
 const auto total = compact_count();
 auto result = torch::ones({total + 1, total + 1}, torch::kBool);
 result[0].fill_(false);
 result.select(1, 0).fill_(false);
 for (std::size_t slot = 0; slot < counts_.size(); ++slot) {
  const auto offset = compact_offsets_[slot] + 1;
  result.narrow(0, offset, counts_[slot]).narrow(1, offset, counts_[slot]).fill_(false);
 }
 return result;
}
torch::Tensor refine_keypoints(const torch::Tensor& delta, const torch::Tensor& references) {
 constexpr auto coordinates = KeypointPredictionPacket::index<^^KeypointPrediction::x>();
 constexpr auto logits = KeypointPredictionPacket::index<^^KeypointPrediction::findable_logit>();
 const auto safe = torch::nan_to_num(delta, 0.0, 0.0, 0.0);
 return torch::cat({safe.narrow(-1, coordinates, logits - coordinates) * references.narrow(-1, 2, 2).unsqueeze(-2) + references.narrow(-1, 0, 2).unsqueeze(-2), safe.narrow(-1, logits, KeypointPredictionPacket::size - logits)}, -1);
}
torch::Tensor KeypointTensorLayout::pad(const torch::Tensor& compact) const {
 TORCH_CHECK(!counts_.empty() && compact.dim() >= 4 && compact.size(-2) == compact_count() && compact.size(-1) == static_cast<std::int64_t>(KeypointPredictionPacket::size), "GroupPose compact output disagrees with admitted schema");
 std::vector<torch::Tensor> classes;
 classes.reserve(counts_.size());
 for (std::size_t slot = 0; slot < counts_.size(); ++slot) {
  const auto count = counts_[slot];
  auto joints = compact.narrow(-2, compact_offsets_[slot], count);
  if (count < maximum_joints_) {
   auto shape = compact.sizes().vec();
   shape[shape.size() - 2] = maximum_joints_ - count;
   joints = torch::cat({joints, torch::zeros(shape, compact.options())}, -2);
  }
  classes.push_back(joints);
 }
 return torch::cat(classes, -2);
}
torch::Tensor KeypointTensorLayout::class_logits(const torch::Tensor& padded, const torch::Tensor& active, std::int64_t classes) const {
 TORCH_CHECK(padded.dim() >= 4 && padded.size(-2) == padded_count_ && padded.size(-1) == static_cast<std::int64_t>(KeypointPredictionPacket::size), "GroupPose padded output disagrees with admitted schema");
 TORCH_CHECK(active.dim() == 2 && active.scalar_type() == torch::kBool && active.size(0) == slot_count() && active.size(1) == maximum_joints_ && classes >= slot_count() && classes <= static_cast<std::int64_t>(kMaximumClassOutputSlots), "GroupPose classifier or active mask disagrees with admitted schema");
 auto shape = padded.sizes().vec();
 shape.resize(shape.size() - 2);
 shape.push_back(slot_count());
 shape.push_back(maximum_joints_);
 auto boost = (KeypointPredictionPacket::select<^^KeypointPrediction::class_logit>(padded, -1).reshape(shape) * active.to(padded.scalar_type())).sum(-1);
 if (slot_count() < classes) {
  shape = boost.sizes().vec();
  shape.back() = classes - slot_count();
  boost = torch::cat({boost, torch::zeros(shape, boost.options())}, -1);
 }
 return boost;
}
KeypointTensorLayout KeypointTensorLayout::from_active_mask(const torch::Tensor& mask) {
 if (!mask.defined() || mask.scalar_type() != torch::kBool || mask.dim() != 2 || mask.size(0) > static_cast<std::int64_t>(kMaximumClassOutputSlots) || mask.size(1) > static_cast<std::int64_t>(catalog::kKeypointsPerClassCapacity)) throw std::invalid_argument("invalid RF-DETR active-keypoint mask shape or dtype");
 const auto cpu = mask.to(torch::kCPU).contiguous();
 const auto* values = cpu.data_ptr<bool>();
 std::vector<std::uint16_t> counts(mask.size(0));
 for (std::int64_t row = 0; row < mask.size(0); ++row) {
  bool inactive = false;
  for (std::int64_t column = 0; column < mask.size(1); ++column) {
   const bool active = values[row * mask.size(1) + column];
   if (inactive && active) throw std::invalid_argument("active-keypoint mask must contain contiguous leading joints");
   inactive = inactive || !active;
   counts[row] += active;
  }
 }
 KeypointTensorLayout result(counts);
 if (mask.size(1) != result.maximum_joints()) throw std::invalid_argument("active-keypoint mask has noncanonical padding width");
 return result;
}
void KeypointTensorLayout::require_class_layout(const ResolvedClassLayout& layout) const {
 if (counts_.size() > layout.output_width()) throw std::invalid_argument("keypoint schema exceeds classifier width");
 if (!layout.semantic()) return;
 for (std::size_t slot = 0; slot < layout.output_width(); ++slot) {
  const auto& entry = layout.record().slots[slot];
  const auto expected = entry.foreground_index ? layout.catalog()->keypoints(*entry.foreground_index).names.size() : 0U;
  const auto actual = slot < counts_.size() ? counts_[slot] : 0U;
  if (actual != expected) throw std::invalid_argument("active-keypoint mask disagrees with named class/joint schema at output slot " + std::to_string(slot));
 }
}
std::vector<std::pair<std::int64_t, std::int64_t>> keypoint_transfer_rows(const ResolvedClassLayout& source, const KeypointTensorLayout& source_joints, const ResolvedClassLayout& destination, const KeypointTensorLayout& destination_joints) {
 source.require_execution(true);
 destination.require_execution(true);
 source_joints.require_class_layout(source);
 destination_joints.require_class_layout(destination);
 std::vector<std::int64_t> source_offsets(source.catalog()->size(), -1);
 for (std::size_t slot = 0; slot < source_joints.counts().size(); ++slot)
  if (const auto index = source.record().slots[slot].foreground_index) source_offsets[*index] = source_joints.compact_offset(slot);
 std::vector<std::pair<std::int64_t, std::int64_t>> rows;
 for (std::size_t slot = 0; slot < destination_joints.counts().size(); ++slot) {
  const auto index = destination.record().slots[slot].foreground_index;
  if (!index || !destination_joints.counts()[slot]) continue;
  const auto source_index = source.catalog()->resolve(destination.catalog()->names()[*index]);
  if (!source_index || source_offsets[*source_index] < 0) continue;
  const auto& from = source.catalog()->keypoints(*source_index);
  const auto& to = destination.catalog()->keypoints(*index);
  for (const auto [source_joint, destination_joint] : catalog::shared_keypoint_correspondence(from, to))
   rows.emplace_back(source_offsets[*source_index] + source_joint, destination_joints.compact_offset(slot) + destination_joint);
 }
 return rows;
}
}  // namespace mmltk::backend::models::rfdetr
