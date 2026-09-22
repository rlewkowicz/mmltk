#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/layers/ms_deform_attn.h"
#include <torch/torch.h>
#include <limits>
#include <catch2/generators/catch_generators.hpp>
#include "src/test_support/cuda_test_utils.hpp"
namespace {
void test_cuda_ms_deform_attn_matches_reference() {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
 const auto device = torch::Device(torch::kCUDA, 0);
 auto value = torch::randn({2, 8, 2, 4}, torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
 const auto spatial_shapes = torch::tensor({{2, 2}, {1, 4}}, torch::TensorOptions().dtype(torch::kInt64));
 const auto level_start_index = torch::tensor({0, 4}, torch::TensorOptions().dtype(torch::kInt64));
 auto sampling_locations = torch::rand({2, 3, 2, 2, 4, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
 auto attention_weights = torch::rand({2, 3, 2, 2, 4}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
 attention_weights = (attention_weights / attention_weights.sum({3, 4}, true)).detach().requires_grad_(true);
 auto value_ref = value.detach().clone().requires_grad_(true);
 auto sampling_ref = sampling_locations.detach().clone().requires_grad_(true);
 auto attention_ref = attention_weights.detach().clone().requires_grad_(true);
 const auto actual = mmltk::backend::ml::layers::ms_deform_attn_cuda_autograd(value, spatial_shapes, level_start_index, sampling_locations, attention_weights, 64);
 const auto expected = mmltk::backend::ml::layers::ms_deform_attn_reference(value_ref, spatial_shapes, sampling_ref, attention_ref);
 REQUIRE((torch::allclose(actual, expected, 1.0e-4, 1.0e-4)));
 const auto grad = torch::randn_like(actual);
 (actual * grad).sum().backward();
 (expected * grad).sum().backward();
 REQUIRE((torch::allclose(value.grad(), value_ref.grad(), 2.0e-4, 2.0e-4)));
 REQUIRE((torch::allclose(sampling_locations.grad(), sampling_ref.grad(), 2.0e-4, 2.0e-4)));
 REQUIRE((torch::allclose(attention_weights.grad(), attention_ref.grad(), 2.0e-4, 2.0e-4)));
}
}  // namespace
TEST_CASE("test_cuda_ms_deform_attn_matches_reference", "[backend][ml][layers]") { test_cuda_ms_deform_attn_matches_reference(); }
TEST_CASE("Deformable attention overwrites every output across chunks and empty support", "[backend][ml][layers]") {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA unavailable; attention overwrite coverage remains unverified");
 const int step = GENERATE(1, 2, 4, 64);
 const int heads = GENERATE(1, 3);
 const int channels = GENERATE(1, 7, 32);
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::Device(torch::kCUDA, 0));
 const auto shapes = torch::tensor({{2, 2}}, torch::kInt64);
 const auto starts = torch::tensor({0}, torch::kInt64);
 const auto value = torch::arange(4 * 4 * heads * channels, options).view({4, 4, heads, channels}) / 16;
 const auto weights = torch::ones({4, 5, heads, 1, 1}, options);
 auto locations = torch::empty({4, 5, heads, 1, 1, 2}, options);
 for (int repeat = 0; repeat < 3; ++repeat) {
  locations.fill_(.5F);
  locations.select(1, 1).fill_(0.F);
  locations.select(1, 2).fill_(1.F);
  locations.select(1, 3).fill_(-.25F);
  locations.select(1, 4).fill_(1.25F);
  const auto output = mmltk::backend::ml::layers::ms_deform_attn_cuda_autograd(value, shapes, starts, locations, weights, step);
  const auto expected = mmltk::backend::ml::layers::ms_deform_attn_reference(value, shapes, locations, weights);
  REQUIRE(torch::equal(output, expected));
  for (const float coordinate : {-2.F, 2.F, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
   locations.fill_(coordinate);
   const auto empty = mmltk::backend::ml::layers::ms_deform_attn_cuda_autograd(value, shapes, starts, locations, weights, step);
   REQUIRE(empty.numel() == 4 * 5 * heads * channels);
   REQUIRE(empty.eq(0).all().item<bool>());
   REQUIRE(torch::signbit(empty).any().item<bool>() == false);
  }
 }
}
