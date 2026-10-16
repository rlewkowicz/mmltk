#pragma once
#include <torch/types.h>
#include <cstdint>
namespace mmltk::backend::models::rfdetr::detail {
// One prefix token followed by a row-major square grid: [1, 1 + grid * grid, D].
// Callers admit geometry and own no-op decisions. Spatial interpolation uses
// FP32 while retaining the source dtype, device, prefix and autograd connection.
[[nodiscard]] torch::Tensor interpolate_position_embedding(const torch::Tensor& positions, std::int64_t stored_grid, std::int64_t height, std::int64_t width);
}  // namespace mmltk::backend::models::rfdetr::detail
