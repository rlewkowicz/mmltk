#include "detail/position_embedding.h"
#include <torch/nn/functional/upsampling.h>
#include <vector>
namespace mmltk::backend::models::rfdetr::detail {
torch::Tensor interpolate_position_embedding(const torch::Tensor& positions, const std::int64_t stored_grid, const std::int64_t height, const std::int64_t width) {
 namespace F = torch::nn::functional;
 const auto dimension = positions.size(2);
 const auto spatial = positions.narrow(1, 1, positions.size(1) - 1).reshape({1, stored_grid, stored_grid, dimension}).permute({0, 3, 1, 2});
 const auto resized =
  F::interpolate(spatial.to(torch::kFloat32), F::InterpolateFuncOptions().size(std::vector<std::int64_t>{height, width}).mode(torch::kBicubic).align_corners(false).antialias(true))
   .to(positions.scalar_type());
 return torch::cat({positions.narrow(1, 0, 1), resized.permute({0, 2, 3, 1}).reshape({1, height * width, dimension})}, 1);
}
}  // namespace mmltk::backend::models::rfdetr::detail
