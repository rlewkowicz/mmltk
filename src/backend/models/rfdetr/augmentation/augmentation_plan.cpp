#include "augmentation_plan.h"
#include "gpu_augmentation_donor_index.h"
#include "detail/gpu_augment_plan_math.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace mmltk::backend::models::rfdetr {
namespace {
GpuAugmentationGroupLaunchConfig launch_group(const AugmentationGroupConfig& group) { return {group.probability, group.min_strength, group.max_strength}; }
void prepare_image_plan(AugmentationImagePlan& plan, const GpuAugmentationLaunchConfig& config, const std::uint64_t key, float* parameters) {
 const augment_math::GeometryPlan geometry = augment_math::solve_image_plan(parameters, config, key);
 plan.erasure = augment_math::spatial_erasure(parameters, key);
 plan.resize_scale = geometry.resize_scale;
 plan.resize_offset_x = geometry.resize_offset_x;
 plan.resize_offset_y = geometry.resize_offset_y;
 std::copy_n(geometry.forward, plan.forward.size(), plan.forward.begin());
 std::copy_n(geometry.inverse, plan.inverse.size(), plan.inverse.begin());
 plan.area_scale = geometry.area_scale;
}
}  // namespace
GpuAugmentationLaunchConfig detail::augmentation_launch_config(const GpuAugmentationConfig& config) {
 return {
  config.enabled ? 1 : 0, launch_group(config.geometry), launch_group(config.resize), launch_group(config.color), launch_group(config.noise), launch_group(config.blur), launch_group(config.occlusion)
 };
}
bool augmentation_paste_admitted(const GpuAugmentationConfig& config, const std::uint64_t key) noexcept {
 return config.enabled && augment_math::uniform01(key, 0x4000ULL) < config.copy_paste_probability;
}
void CachedAugmentationDonorIndex::rebuild(const std::span<const GpuAugmentationDonor> donors) {
 donors_ = donors;
 valid_.clear();
 valid_.reserve(donors.size());
 first_valid_.assign(donors.size(), -1);
 next_different_.assign(donors.size(), -1);
 for (std::size_t slot = 0; slot < donors.size(); ++slot)
  if (donors[slot].label >= 0) valid_.push_back(slot);
 if (valid_.empty()) return;
 auto next = static_cast<std::int64_t>(valid_.front());
 for (std::size_t slot = donors.size(); slot-- != 0;) {
  if (donors[slot].label >= 0) next = static_cast<std::int64_t>(slot);
  first_valid_[slot] = next;
 }
 // Begin at a dataset boundary so no same-dataset run straddles the
 // traversal. Invalid cache entries were removed without changing order.
 std::size_t pivot = 0;
 while (pivot < valid_.size() && donors[valid_[pivot]].dataset_index == donors[valid_[(pivot + valid_.size() - 1) % valid_.size()]].dataset_index) ++pivot;
 if (pivot == valid_.size()) return;
 for (std::size_t begin = 0; begin < valid_.size();) {
  std::size_t end = begin + 1;
  const auto source = donors[valid_[(pivot + begin) % valid_.size()]].dataset_index;
  while (end < valid_.size() && donors[valid_[(pivot + end) % valid_.size()]].dataset_index == source) ++end;
  const auto following = static_cast<std::int64_t>(valid_[(pivot + end) % valid_.size()]);
  for (auto item = begin; item < end; ++item) next_different_[valid_[(pivot + item) % valid_.size()]] = following;
  begin = end;
 }
}
std::int64_t CachedAugmentationDonorIndex::select(const std::size_t start, const std::uint32_t source) const noexcept {
 if (start >= first_valid_.size()) return -1;
 const auto first = first_valid_[start];
 if (first < 0) return -1;
 const auto slot = static_cast<std::size_t>(first);
 return donors_[slot].dataset_index == source ? next_different_[slot] : first;
}
std::int64_t CachedAugmentationDonorIndex::select_for_image(const std::uint64_t key, const std::uint32_t source) const noexcept {
 if (donors_.empty()) return -1;
 const auto start = std::min<std::size_t>(static_cast<std::size_t>(augment_math::uniform01(key, 0x4001ULL) * static_cast<float>(donors_.size())), donors_.size() - 1U);
 return select(start, source);
}
void detail::prepare_augmentation_image(AugmentationImagePlan& plan, const GpuAugmentationConfig& config, std::uint64_t key, std::uint32_t dataset_index, const GpuAugmentationDonor* selected,
 std::int64_t donor_slot, float* image_parameters, float* paste) {
 plan = {};
 prepare_image_plan(plan, augmentation_launch_config(config), key, image_parameters);
 plan.cache_choice = augment_math::uniform01(key, 0x5000ULL);
 plan.cache_source_dataset_index = dataset_index;
 std::fill_n(paste, kGpuCopyPasteParameterCount, 0.0F);
 paste[0] = -1.0F;
 if (!selected || selected->label < 0 || selected->dataset_index == dataset_index || !augmentation_paste_admitted(config, key)) return;
 const auto& donor = *selected;
 const float scale = 0.5F + augment_math::uniform01(key, 0x4002ULL);
 const float destination_x = augment_math::uniform01(key, 0x4003ULL);
 const float destination_y = augment_math::uniform01(key, 0x4004ULL);
 const float source_x = (donor.box[0] + donor.box[2]) * 0.5F;
 const float source_y = (donor.box[1] + donor.box[3]) * 0.5F;
 const float translate_x = destination_x - scale * source_x;
 const float translate_y = destination_y - scale * source_y;
 const float inverse_scale = 1.0F / scale;
 const float output_x0 = augment_math::clamp01(std::fma(scale, donor.box[0], translate_x));
 const float output_y0 = augment_math::clamp01(std::fma(scale, donor.box[1], translate_y));
 const float output_x1 = augment_math::clamp01(std::fma(scale, donor.box[2], translate_x));
 const float output_y1 = augment_math::clamp01(std::fma(scale, donor.box[3], translate_y));
 if (output_x1 <= output_x0 || output_y1 <= output_y0) return;
 plan.paste_donor_slot = donor_slot;
 plan.paste_masked = donor.has_mask;
 plan.paste_label = donor.label;
 plan.paste_sampling_identity = donor.sampling_identity;
 plan.paste_source_area = donor.area;
 plan.paste_source_box = donor.box;
 plan.paste_output_box = {output_x0, output_y0, output_x1, output_y1};
 plan.paste_inverse = {inverse_scale, 0.0F, -translate_x * inverse_scale, 0.0F, inverse_scale, -translate_y * inverse_scale};
 paste[0] = static_cast<float>(donor_slot);
 paste[1] = donor.has_mask ? 1.0F : 2.0F;
 std::copy(plan.paste_inverse.begin(), plan.paste_inverse.end(), paste + 2);
}
AugmentationImagePlan plan_augmentation_image(const GpuAugmentationConfig& config, std::uint64_t key, std::uint32_t dataset_index, const GpuAugmentationDonor* donor, std::int64_t slot) {
 AugmentationImagePlan plan;
 std::array<float, kGpuAugmentationParameterCount> parameters{};
 std::array<float, kGpuCopyPasteParameterCount> paste{};
 detail::prepare_augmentation_image(plan, config, key, dataset_index, donor, slot, parameters.data(), paste.data());
 return plan;
}
}  // namespace mmltk::backend::models::rfdetr
