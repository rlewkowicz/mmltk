#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/layers/ms_deform_attn.h"
#include <torch/torch.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/core/InferenceMode.h>
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <limits>
#include <catch2/generators/catch_generators.hpp>
#include "src/test_support/cuda_test_utils.hpp"
namespace {
namespace layers = mmltk::backend::ml::layers;
using AttentionInputs = std::array<torch::Tensor, 3>;
void require_attention_values(const torch::Tensor& actual, const torch::Tensor& expected, double tolerance = 2e-4) {
 REQUIRE(actual.scalar_type() == expected.scalar_type());
 REQUIRE(torch::equal(torch::isfinite(actual), torch::isfinite(expected)));
 REQUIRE(torch::equal(torch::isnan(actual), torch::isnan(expected)));
 REQUIRE(torch::allclose(actual, expected, tolerance, tolerance, true));
}
void require_attention_vjp(const torch::Tensor& actual, const AttentionInputs& operands, const torch::Tensor& shapes, const torch::Tensor& incoming) {
 AttentionInputs reference;
 for (size_t i = 0; i < operands.size(); ++i) reference[i] = operands[i].detach().clone().requires_grad_(true);
 const auto expected = layers::ms_deform_attn_reference(reference[0].to(torch::kFloat32), shapes, reference[1].to(torch::kFloat32), reference[2].to(torch::kFloat32)).to(actual.scalar_type());
 const double tolerance = actual.scalar_type() == torch::kBFloat16 ? 8e-3 : actual.scalar_type() == torch::kFloat16 ? 1e-3 : 2e-4;
 require_attention_values(actual, expected, tolerance);
 for (const double scale : {1., .75}) {
  const auto cotangent = incoming * scale;
  const auto actual_grads = torch::autograd::grad({actual}, {operands[0], operands[1], operands[2]}, {cotangent}, true);
  const auto expected_grads = torch::autograd::grad({expected}, {reference[0], reference[1], reference[2]}, {cotangent}, true);
  for (size_t i = 0; i < operands.size(); ++i) require_attention_values(actual_grads[i], expected_grads[i], tolerance);
 }
}
void compare_attention(const AttentionInputs& operands, const torch::Tensor& shapes, const torch::Tensor& starts, int64_t step, const torch::Tensor& incoming) {
 const auto actual = layers::ms_deform_attn_cuda_autograd(operands[0], shapes, starts, operands[1], operands[2], step);
 require_attention_vjp(actual, operands, shapes, incoming);
}
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
TEST_CASE("Deformable attention retains selected support derivatives across reduction families", "[backend][ml][layers][parity]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; support VJPs remain unverified");
 const int channels = GENERATE(1, 7, 32, 64, 65, 1024, 1025, 2048);
 const int extent = GENERATE(1, 2, 4);
 const int axis = GENERATE(0, 1);
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
 const auto shapes = torch::tensor({{extent, extent}}, torch::kInt64), starts = torch::tensor({0}, torch::kInt64);
 const float half_pixel = .5F / static_cast<float>(extent);
 const float boundary = -half_pixel;
 const std::vector<float> coordinates{std::nextafter(boundary, -1.F), boundary, std::nextafter(boundary, 0.F), 0.F, half_pixel, .5F, 1.F, 1.F + half_pixel};
 auto locations = torch::full({1, static_cast<int64_t>(coordinates.size()), 1, 1, 2, 2}, half_pixel, options);
 locations.select(-1, axis).copy_(torch::tensor(coordinates, options).view({1, -1, 1, 1, 1}).expand_as(locations.select(-1, axis)));
 AttentionInputs inputs{
  (torch::arange(extent * extent * channels, options).reshape({1, extent * extent, 1, channels}) / (extent * extent * channels) + .125).requires_grad_(true), locations.requires_grad_(true),
  torch::full({1, static_cast<int64_t>(coordinates.size()), 1, 1, 2}, .5, options).requires_grad_(true)
 };
 compare_attention(inputs, shapes, starts, 64, torch::ones({1, static_cast<int64_t>(coordinates.size()), channels}, options));
}
TEST_CASE("Deformable attention preserves exceptional sampled products and VJPs", "[backend][ml][layers][parity]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; exceptional attention semantics remain unverified");
 const int channels = GENERATE(1, 7, 64, 65, 1025, 2048);
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
 const auto shapes = torch::tensor({{2, 2}}, torch::kInt64), starts = torch::tensor({0}, torch::kInt64);
 const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
 for (const float attention : {1.F, 0.F, inf, -inf, nan})
  for (const float gradient : {1.F, inf, nan})
   for (const float pixel : {0.F, 1.F, inf, nan}) {
    CAPTURE(channels, attention, gradient, pixel);
    auto locations = torch::tensor({-.25F, .25F, .25F, .25F, .5F, .5F, 2.F, 2.F, inf, .5F, nan, .5F}, options).reshape({1, 6, 1, 1, 1, 2}).repeat({1, 1, 1, 1, 2, 1});
    // The second point retains ordinary valid support even when the first
    // point's footprint is empty. Zero pixels expose per-corner 0 * Inf.
    locations.select(4, 1).fill_(.5F);
    auto value = torch::ones({1, 4, 1, channels}, options);
    value.select(1, 0).fill_(pixel);
    AttentionInputs inputs{value.requires_grad_(true), locations.requires_grad_(true), torch::full({1, 6, 1, 1, 2}, attention, options).requires_grad_(true)};
    compare_attention(inputs, shapes, starts, 1, torch::full({1, 6, channels}, gradient, options));
   }
}
TEST_CASE("Deformable attention launches bounded tail chunks on the caller stream", "[backend][ml][layers][parity]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; attention chunk tails remain unverified");
 const int batch = GENERATE(5, 63, 64, 65, 127, 128, 129);
 const int step = GENERATE(3, 64);
 const auto stream = c10::cuda::getStreamFromPool();
 const c10::cuda::CUDAStreamGuard guard(stream);
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
 const auto shapes = torch::tensor({{2, 2}, {1, 2}}, torch::kInt64), starts = torch::tensor({0, 4}, torch::kInt64);
 AttentionInputs inputs{
  torch::randn({batch, 6, 2, 7}, options).requires_grad_(true), torch::rand({batch, 2, 2, 2, 2, 2}, options).requires_grad_(true), torch::rand({batch, 2, 2, 2, 2}, options).requires_grad_(true)
 };
 compare_attention(inputs, shapes, starts, step, torch::randn({batch, 2, 14}, options));
 stream.synchronize();
}
TEST_CASE("Dispatcher attention capture preserves opaque work and mixed precision VJPs", "[backend][ml][layers][parity][compilation]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; captured attention remains unverified");
 for (const auto& [device, precision] : mmltk::backend::ml::testsupport::available_amp_precisions()) {
  if (!device.is_cuda()) continue;
  CAPTURE(device, precision);
  const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
  const auto shapes = torch::tensor({{2, 2}}, torch::kInt64), starts = torch::tensor({0}, torch::kInt64);
  const auto make_inputs = [&](int replay) {
   auto locations = torch::full({1, 3, 2, 1, 2, 2}, .31 + replay * .027, options);
   locations.select(1, 0).select(-1, 0).fill_(-.25F);
   locations.select(1, 0).select(-1, 1).fill_(.25F);
   return AttentionInputs{
    (torch::arange(56, options).reshape({1, 2, 4, 7}) * .031 + replay * .07).transpose(1, 2).to(precision).requires_grad_(true), locations.requires_grad_(true),
    torch::full({1, 3, 2, 1, 2}, .4 + replay * .01, options.dtype(precision)).requires_grad_(true)
   };
  };
  mmltk::backend::ml::cuda::TorchAutocastScope amp(precision != torch::kFloat32, precision);
  auto operands = make_inputs(0);
  auto trace = torch::jit::tracer::trace({operands[0], shapes, starts, operands[1], operands[2]}, [](torch::jit::Stack args) {
   return torch::jit::Stack{layers::ms_deform_attn_cuda_autograd(args[0].toTensor(), args[1].toTensor(), args[2].toTensor(), args[3].toTensor(), args[4].toTensor(), 64)};
  }, [](const torch::autograd::Variable&) { return std::string{}; }, false, false);
  const auto graph_text = trace.first->graph->toString();
  REQUIRE(graph_text.find("mmltk::ms_deform_attn") != std::string::npos);
  REQUIRE(graph_text.find("aten::empty") == std::string::npos);
  const auto incoming = torch::linspace(-.37, .82, 42, options).reshape({1, 3, 14}).to(precision);
  require_attention_vjp(trace.second[0].toTensor(), operands, shapes, incoming);
  torch::jit::GraphExecutor executor(trace.first->graph, "attention_dispatch");
  for (int replay = 1; replay <= 4; ++replay) {
   CAPTURE(replay);
   operands = make_inputs(replay);
   torch::jit::Stack stack{operands[0], shapes, starts, operands[1], operands[2]};
   executor.run(stack);
   require_attention_vjp(stack[0].toTensor(), operands, shapes, incoming);
  }
  // Autograd-free dispatch must still upcast operands and restore value dtype.
  c10::InferenceMode inference;
  const auto actual = layers::ms_deform_attn_cuda_autograd(operands[0], shapes, starts, operands[1], operands[2], 64);
  const auto expected = layers::ms_deform_attn_reference(operands[0].to(torch::kFloat32), shapes, operands[1], operands[2].to(torch::kFloat32)).to(precision);
  require_attention_values(actual, expected, precision == torch::kBFloat16 ? 8e-3 : 1e-3);
  REQUIRE_FALSE(actual.requires_grad());
 }
}
TEST_CASE("Dispatcher attention preserves explicit input and layout failures", "[backend][ml][layers]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; attention admission remains unverified");
 const auto options = torch::TensorOptions().device(torch::kCUDA).dtype(torch::kFloat32);
 const auto value = torch::ones({1, 4, 1, 1}, options), locations = torch::full({1, 1, 1, 1, 1, 2}, .5, options), weights = torch::ones({1, 1, 1, 1, 1}, options);
 const auto shapes = torch::tensor({{2, 2}}, torch::kInt64), starts = torch::tensor({0}, torch::kInt64);
 for (const bool inference : {false, true}) {
  const c10::InferenceMode mode(inference);
  REQUIRE_THROWS(layers::ms_deform_attn_cuda_autograd(value.to(torch::kFloat64), shapes, starts, locations, weights, 64));
  REQUIRE_THROWS(layers::ms_deform_attn_cuda_autograd(value, shapes, starts, locations.cpu(), weights, 64));
  REQUIRE_THROWS(layers::ms_deform_attn_cuda_autograd(value, shapes.to(options.device()), starts, locations, weights, 64));
  REQUIRE_THROWS(layers::ms_deform_attn_cuda_autograd(value, shapes, starts + 1, locations, weights, 64));
  REQUIRE_THROWS(layers::ms_deform_attn_cuda_autograd(value, shapes, starts, locations, weights, 0));
 }
}
