#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/models/rfdetr/core/detail/position_embedding.h"
#include <array>
#include <cstdint>
namespace {
namespace fixture = mmltk::backend::ml::testsupport;
using mmltk::backend::models::rfdetr::detail::interpolate_position_embedding;
TEST_CASE("Absolute positions preserve constant rectangular grids, prefix and gradients", "[rfdetr][position_embedding]") {
 const std::array<std::array<std::int64_t, 3>, 2> geometries{{{3, 5, 7}, {5, 2, 3}}};
 for (const auto& [device, dtype] : fixture::available_amp_precisions()) {
  const auto options = torch::TensorOptions().device(device).dtype(dtype);
  const auto prefix = torch::tensor({11.25F, -7.5F, -0.F}, options).reshape({1, 1, 3});
  const auto constant = torch::tensor({0.125F, -1.75F, 3.5F}, options).reshape({1, 1, 3});
  for (const auto& [side, height, width] : geometries) {
   CAPTURE(device, dtype, side, height, width);
   auto positions = torch::cat({prefix, constant.expand({1, side * side, 3})}, 1).set_requires_grad(true);
   const auto output = interpolate_position_embedding(positions, side, height, width);
   REQUIRE(output.sizes() == torch::IntArrayRef({1, 1 + height * width, 3}));
   REQUIRE(output.scalar_type() == dtype);
   REQUIRE(output.device() == device);
   CHECK(torch::equal(output.narrow(1, 0, 1), prefix));
   // Equality alone would miss a changed sign bit on the prefix's negative zero.
   CHECK(torch::equal(torch::signbit(output.narrow(1, 0, 1)), torch::signbit(prefix)));
   CHECK(torch::allclose(output.narrow(1, 1, height * width), constant.expand({1, height * width, 3}), 2e-6, 2e-6));
   REQUIRE(output.requires_grad());
   output.to(torch::kFloat32).sum().backward();
   fixture::require_finite_gradient(positions);
   CHECK(torch::equal(positions.grad().narrow(1, 0, 1), torch::ones_like(prefix)));
   const auto spatial_gradient = positions.grad().narrow(1, 1, side * side).to(torch::kFloat32);
   CHECK(spatial_gradient.count_nonzero().item<std::int64_t>() == side * side * 3);
   // A constant shift in each input channel shifts every output token equally.
   // The input gradient rounds once when restored to the source precision.
   const double tolerance = dtype == torch::kBFloat16 ? 8e-3 : dtype == torch::kFloat16 ? 1e-3 : 2e-6;
   CHECK(torch::allclose(spatial_gradient.sum(1), torch::full({1, 3}, height * width, options.dtype(torch::kFloat32)), tolerance, tolerance));
  }
 }
}
TEST_CASE("Absolute position interpolation restores source precision after FP32 resizing", "[rfdetr][position_embedding]") {
 for (const auto& [device, dtype] : fixture::available_amp_precisions()) {
  CAPTURE(device, dtype);
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  const auto positions = (torch::linspace(-5.125, 7.625, 51, options).sin() * 3.75).reshape({1, 17, 3}).to(dtype);
  const auto actual = interpolate_position_embedding(positions, 4, 7, 3);
  // Precision invariance uses the same already-quantized source values; it
  // detects interpolation or intermediate rounding in the reduced dtype.
  const auto fp32 = interpolate_position_embedding(positions.to(torch::kFloat32), 4, 7, 3);
  REQUIRE(actual.scalar_type() == dtype);
  CHECK(actual.device() == device);
  CHECK(torch::equal(actual, fp32.to(dtype)));
 }
}
}  // namespace
