#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <torch/types.h>
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/keypoint_prediction.h"
#include "src/backend/models/rfdetr/core/reflected_tensor_packet.h"
namespace mmltk::backend::models::rfdetr {
using KeypointPredictionPacket = ReflectedTensorPacket<KeypointPrediction>;
static_assert(std::is_standard_layout_v<KeypointPrediction> && sizeof(KeypointPrediction) == KeypointPredictionPacket::size * sizeof(float));
// Immutable physical-slot interpretation; owns no tensors or catalog lifetime.
// Schema slots can omit trailing classifier slots. Compact offsets count only
// real joints; padded offsets retain each schema slot and its inactive zeros.
class KeypointTensorLayout final {
public:
 explicit KeypointTensorLayout(std::span<const std::uint16_t> counts = {});
 [[nodiscard]] static KeypointTensorLayout from_active_mask(const torch::Tensor& mask);
 [[nodiscard]] std::span<const std::uint16_t> counts() const noexcept { return counts_; }
 [[nodiscard]] std::int64_t slot_count() const noexcept { return static_cast<std::int64_t>(counts_.size()); }
 [[nodiscard]] std::int64_t maximum_joints() const noexcept { return maximum_joints_; }
 [[nodiscard]] std::int64_t compact_count() const noexcept { return compact_offsets_.back(); }
 [[nodiscard]] std::int64_t padded_count() const noexcept { return padded_count_; }
 // The one-past-last slot is accepted as the corresponding total extent.
 [[nodiscard]] std::int64_t compact_offset(std::size_t slot) const { return compact_offsets_.at(slot); }
 [[nodiscard]] std::int64_t padded_offset(std::size_t slot) const;
 void require_class_layout(const ResolvedClassLayout& layout) const;
 [[nodiscard]] torch::Tensor active_mask() const;
 [[nodiscard]] torch::Tensor class_mask() const;
 [[nodiscard]] torch::Tensor pad(const torch::Tensor& compact) const;
 [[nodiscard]] torch::Tensor class_logits(const torch::Tensor& padded, const torch::Tensor& active, std::int64_t classes) const;

private:
 std::vector<std::uint16_t> counts_;
 std::vector<std::int64_t> compact_offsets_;
 std::int64_t maximum_joints_ = 0;
 std::int64_t padded_count_ = 0;
};
[[nodiscard]] torch::Tensor refine_keypoints(const torch::Tensor& delta, const torch::Tensor& references);
// Source/destination compact rows, retaining destination class/joint order.
[[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> keypoint_transfer_rows(
 const ResolvedClassLayout& source, const KeypointTensorLayout& source_joints,
 const ResolvedClassLayout& destination, const KeypointTensorLayout& destination_joints);
}  // namespace mmltk::backend::models::rfdetr
