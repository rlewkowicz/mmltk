#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/layers/ms_deform_attn.h"
#include <torch/torch.h>
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
