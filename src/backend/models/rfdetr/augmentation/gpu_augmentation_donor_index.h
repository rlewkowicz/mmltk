#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
namespace mmltk::backend::models::rfdetr::detail {
// One O(C) index per cached donor batch; each exact circular choice is O(1).
class CachedAugmentationDonorIndex final {
public:
 void rebuild(std::span<const GpuAugmentationDonor> donors);
 [[nodiscard]] std::int64_t select(std::size_t start, std::uint32_t source) const noexcept;

private:
 // Donor metadata stays borrowed for the current executor call.
 std::span<const GpuAugmentationDonor> donors_;
 std::vector<std::size_t> valid_;
 std::vector<std::int64_t> first_valid_;
 std::vector<std::int64_t> next_different_;
};
}  // namespace mmltk::backend::models::rfdetr::detail
