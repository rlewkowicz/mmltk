#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/detail/selective_compilation.h"
#include "src/backend/models/rfdetr/core/detail/decoder_attention.h"
#include "src/backend/models/rfdetr/core/detail/modules_technical.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <ATen/CPUGeneratorImpl.h>
#include <ATen/cuda/CUDAContext.h>
#include <torch/script.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <ATen/autocast_mode.h>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
namespace {
namespace rf = mmltk::backend::models::rfdetr;
namespace tensor_fixture = mmltk::backend::ml::testsupport;
using Region = rf::detail::SelectiveTensorRegion;
TEST_CASE("Native inference clones rebind module owners and share only immutable weights", "[rfdetr][compilation][gpu]") {
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 torch::NoGradGuard no_grad;
 auto config = rf::native_config_from_preset(rf::model_presets().front());
 config.resolution = 64;
 config.num_queries = 3;
 config.num_select = 3;
 config.num_classes = 2;
 rf::NativeRfDetrModel master(config);
 master.to(torch::Device(torch::kCUDA, 0));
 master.eval();
 const auto cuda = torch::TensorOptions().device(torch::kCUDA);
 rf::NestedTensor input{torch::rand({1, 3, 64, 64}, cuda), torch::zeros({1, 64, 64}, cuda.dtype(torch::kBool))};
 const auto reference = master.forward(input, false).main.pred_logits.to(torch::kCPU);
 for (const auto mode : {rf::CompilationMode::kNone, rf::CompilationMode::kSelective}) {
  auto first = master.make_inference_clone(1, mode);
  auto second = first->make_inference_clone(1, mode);
  const auto original = master.named_parameters();
  const auto frozen = first->named_parameters();
  const auto sibling = second->named_parameters();
  for (const auto& entry : original) {
   REQUIRE(frozen[entry.key()].data_ptr() != entry.value().data_ptr());
   REQUIRE(frozen[entry.key()].data_ptr() == sibling[entry.key()].data_ptr());
   CHECK_FALSE(frozen[entry.key()].requires_grad());
  }
  const auto before = first->forward(input, false).main.pred_logits.to(torch::kCPU);
  CHECK(torch::allclose(before, reference, 2e-4, 2e-4));
  original["class_embed.bias"].add_(3);
  CHECK_FALSE(torch::allclose(master.forward(input, false).main.pred_logits.to(torch::kCPU), before));
  CHECK(torch::allclose(first->forward(input, false).main.pred_logits.to(torch::kCPU), before, 2e-4, 2e-4));
  second->invalidate_compilation();
  CHECK(torch::allclose(second->forward(input, false).main.pred_logits.to(torch::kCPU), before, 2e-4, 2e-4));
  CHECK_THROWS_AS(first->train(), std::logic_error);
  CHECK_THROWS_AS(first->to(torch::kCPU), std::logic_error);
  original["class_embed.bias"].sub_(3);
 }
 master.freeze_inference_weights();
 auto sealed = master.make_inference_clone(1, rf::CompilationMode::kNone);
 CHECK(sealed->parameters().front().data_ptr() == master.parameters().front().data_ptr());
}
struct AttentionPair {
 torch::nn::MultiheadAttention actual{torch::nn::MultiheadAttentionOptions(4, 2).dropout(0.0)};
 torch::nn::MultiheadAttention oracle{torch::nn::MultiheadAttentionOptions(4, 2).dropout(0.0)};
 explicit AttentionPair(torch::Device device) {
  actual->to(device);
  oracle->to(device);
  torch::NoGradGuard guard;
  for (const auto& item : actual->named_parameters()) oracle->named_parameters()[item.key()].copy_(item.value());
 }
};
TEST_CASE("Selective tensor slots record once preserve live weights and guard mismatches", "[rfdetr][compilation]") {
 Region region("fixture");
 torch::nn::Linear linear(4, 4);
 int forwards = 0;
 bool fail = false;
 const auto run = [&](const Region::Tensors& inputs) {
  ++forwards;
  if (fail) throw std::runtime_error("injected candidate failure");
  return Region::Tensors{linear->forward(inputs[0]) + torch::rand_like(inputs[0])};
 };
 region.prepare(true, true, 2);
 auto input = torch::ones({2, 4}).set_requires_grad(true);
 auto generator = at::detail::getDefaultCPUGenerator();
 const auto before = generator.get_state();
 const auto first = region.invoke(true, {input}, {linear.get()}, run).front();
 const auto after = generator.get_state();
 REQUIRE(forwards == 1);
 const auto* identity = region.identity(true);
 REQUIRE(identity);
 generator.set_state(before);
 const auto expected = run({input}).front();
 REQUIRE(torch::equal(first, expected));
 REQUIRE(torch::equal(after, generator.get_state()));
 first.sum().backward();
 REQUIRE(linear->weight.grad().defined());
 REQUIRE(input.grad().defined());
 {
  torch::NoGradGuard guard;
  linear->weight.add_(0.2);
 }
 generator.set_state(before);
 const auto live = region.invoke(true, {input}, {linear.get()}, run).front();
 generator.set_state(before);
 REQUIRE(torch::allclose(live, run({input}).front()));
 region.prepare(true, true, 2);
 REQUIRE(region.identity(true) == identity);
 const auto calls = forwards;
 region.invoke(true, {torch::ones({1, 4})}, {linear.get()}, run);
 REQUIRE(forwards == calls + 1);
 REQUIRE(region.identity(true) == identity);
 region.prepare(true, true, 3);
 fail = true;
 REQUIRE_THROWS(region.invoke(true, {torch::ones({3, 4}).set_requires_grad(true)}, {linear.get()}, run));
 REQUIRE(region.identity(true) == identity);
 const auto failed_calls = forwards;
 REQUIRE_NOTHROW(region.invoke(true, {input}, {linear.get()}, run));
 REQUIRE(forwards == failed_calls);
 fail = false;
 region.prepare(true, false, 3);
 const auto disabled_calls = forwards;
 region.invoke(true, {input}, {linear.get()}, run);
 REQUIRE(forwards == disabled_calls + 1);
 REQUIRE_FALSE(region.identity(false));
 region.prepare(false, true, 2);
 region.invoke(false, {input}, {linear.get()}, run);
 REQUIRE(region.identity(false));
 region.invalidate();
 REQUIRE_FALSE(region.identity(true));
 REQUIRE_FALSE(region.identity(false));
}
TEST_CASE("Fixed tensor regions leave the selected batch armed after an initial tail", "[rfdetr][compilation]") {
 Region region("tail_first");
 torch::nn::Linear linear(4, 4);
 int calls = 0;
 const auto ordinary = [&](const Region::Tensors& inputs) {
  ++calls;
  return Region::Tensors{linear->forward(inputs[0]) + inputs[1]};
 };
 region.prepare(false, true, 2);
 const auto tail = torch::ones({1, 4});
 const auto full = torch::ones({2, 4});
 region.invoke(false, {tail, tail}, {linear.get()}, ordinary);
 REQUIRE(calls == 1);
 REQUIRE_FALSE(region.identity(false));
 region.invoke(false, {full, full}, {linear.get()}, ordinary);
 REQUIRE(calls == 2);
 const auto* identity = region.identity(false);
 REQUIRE(identity);
 region.invoke(false, {full, full}, {linear.get()}, ordinary);
 REQUIRE(calls == 2);
 REQUIRE(region.identity(false) == identity);
 region.invoke(false, {tail, tail}, {linear.get()}, ordinary);
 REQUIRE(calls == 3);
 REQUIRE(region.identity(false) == identity);
 // Every fixed-region input participates in admission, even when ordinary
 // broadcasting could produce a result with the selected leading extent.
 region.invoke(false, {full, tail}, {linear.get()}, ordinary);
 REQUIRE(calls == 4);
 REQUIRE(region.identity(false) == identity);
}
TEST_CASE("Query tensor regions reuse varying extents and independent promoted input dtypes", "[rfdetr][compilation]") {
 for (const auto& [device, dtype] : tensor_fixture::available_amp_precisions()) {
  Region region("query", true);
  torch::nn::Linear linear(4, 4);
  linear->to(device);
  region.prepare(true, true, 2);
  int calls = 0;
  const auto operation = [&](const Region::Tensors& inputs) {
   ++calls;
   return Region::Tensors{inputs[0] + torch::relu(linear->forward(inputs[1]))};
  };
  const void* identity = nullptr;
  std::vector<torch::Tensor> retained;
  for (const auto queries : {3, 7, 1}) {
   const int64_t batch = queries == 3 ? 1 : queries == 7 ? 2 : 3;
   auto residual = torch::rand({batch, queries, 4}, torch::TensorOptions().device(device)).set_requires_grad(true);
   auto cross = torch::rand_like(residual).to(dtype).set_requires_grad(true);
   torch::Tensor actual, expected;
   {
    mmltk::backend::ml::cuda::TorchAutocastScope amp(device.is_cuda() && dtype != torch::kFloat32, dtype);
    const bool cache = at::autocast::is_autocast_cache_enabled();
    actual = region.invoke(true, {residual, cross}, {linear.get()}, operation).front();
    REQUIRE(at::autocast::is_autocast_cache_enabled() == cache);
    expected = residual + torch::relu(linear->forward(cross));
   }
   REQUIRE(torch::allclose(actual, expected, 2e-3, 2e-3));
   if (!identity) identity = region.identity(true);
   REQUIRE(region.identity(true) == identity);
   retained.push_back(actual.sum());
  }
  REQUIRE(calls == 1);
  for (auto& loss : retained) loss.backward();
  REQUIRE(linear->weight.grad().defined());
  REQUIRE(torch::isfinite(linear->weight.grad()).all().item<bool>());
 }
}
// An independent dense matrix equation: no MultiheadAttention or SDPA call.
torch::Tensor dense_attention(torch::nn::MultiheadAttention& module, const torch::Tensor& target, const torch::Tensor& position, const torch::Tensor& admitted = {}) {
 const auto width = target.size(-1);
 const auto heads = module->options.num_heads();
 const auto project = [&](const torch::Tensor& x, int64_t offset) {
  return torch::linear(x, module->in_proj_weight.narrow(0, offset * width, width), module->in_proj_bias.narrow(0, offset * width, width));
 };
 const auto split = [&](const torch::Tensor& x) { return x.view({x.size(0), x.size(1), heads, width / heads}).transpose(1, 2); };
 const auto q = split(project(target + position, 0));
 const auto k = split(project(target + position, 1));
 const auto v = split(project(target, 2));
 torch::Tensor joined;
 {
  // SDPA accumulates its half/BF16 intermediates in FP32. An autocast dense
  // equation rounds scores and probabilities at extra boundaries, changing
  // the reference gradients independently of the grouped attention logic.
  const mmltk::backend::ml::cuda::TorchAutocastScope full_precision(false, torch::kFloat32);
  const auto precision = q.scalar_type() == torch::kFloat16 || q.scalar_type() == torch::kBFloat16 ? torch::kFloat32 : q.scalar_type();
  auto scores = torch::matmul(q.to(precision), k.to(precision).transpose(-1, -2)) / std::sqrt(static_cast<double>(width / heads));
  if (admitted.defined()) scores = scores.masked_fill(~admitted.unsqueeze(1), -std::numeric_limits<float>::infinity());
  const auto probabilities = torch::softmax(scores, -1);
  joined = torch::matmul(probabilities, v.to(precision)).to(v.scalar_type()).transpose(1, 2).reshape_as(target);
 }
 return torch::linear(joined, module->out_proj->weight, module->out_proj->bias);
}
TEST_CASE("Decoder SDPA preserves independent positional and target gradients including coincident values", "[rfdetr][attention][compilation]") {
 const tensor_fixture::FullMatrixPrecision precision;
 for (const auto& [device, dtype] : tensor_fixture::available_amp_precisions()) {
  for (const int64_t queries : {1, 3}) {
   AttentionPair pair(device);
   auto& actual = pair.actual;
   auto& oracle = pair.oracle;
   auto target = torch::rand({2, queries, 4}, torch::TensorOptions().device(device)).set_requires_grad(true);
   auto position = torch::zeros_like(target).set_requires_grad(true);
   auto reference_target = target.detach().clone().set_requires_grad(true);
   auto reference_position = position.detach().clone().set_requires_grad(true);
   rf::DecoderQueryLayout layout;
   layout.ordinary = {1, queries};
   torch::Tensor output, expected;
   {
    mmltk::backend::ml::cuda::TorchAutocastScope amp(device.is_cuda() && dtype != torch::kFloat32, dtype);
    auto unit = std::make_shared<torch::jit::CompilationUnit>();
    auto type = torch::jit::ClassType::create("__torch__.AttentionRouteEvidence", unit, true);
    torch::jit::Module recorded(unit, type);
    size_t index = 0;
    for (const auto& item : actual->named_parameters()) recorded.register_parameter("parameter_" + std::to_string(index++), item.value(), false);
    const bool cache = at::autocast::is_autocast_cache_enabled();
    at::autocast::set_autocast_cache_enabled(false);
    const auto trace = torch::jit::tracer::trace(
     {target, position}, [&](torch::jit::Stack arguments) { return torch::jit::Stack{rf::isolated_group_self_attention(actual, arguments[0].toTensor(), arguments[1].toTensor(), layout)}; },
     [](const torch::autograd::Variable&) { return ""; }, false, false, &recorded);
    at::autocast::set_autocast_cache_enabled(cache);
    output = trace.second.front().toTensor();
    bool sdpa_operator = false;
    for (const auto* node : trace.first->graph->nodes()) sdpa_operator |= std::string_view(node->kind().toQualString()).find("scaled_dot_product") != std::string_view::npos;
    REQUIRE(sdpa_operator);
    expected = dense_attention(oracle, reference_target, reference_position);
   }
   REQUIRE(torch::allclose(output, expected, 2e-2, 3e-3));
   output.square().sum().backward();
   expected.square().sum().backward();
   REQUIRE(torch::allclose(target.grad(), reference_target.grad(), 2e-2, 3e-3));
   REQUIRE(torch::allclose(position.grad(), reference_position.grad(), 2e-2, 3e-3));
   if (queries == 1) {
    REQUIRE(position.grad().count_nonzero().item<int64_t>() == 0);
    REQUIRE(target.grad().abs().sum().item<float>() > 0.0F);
   }
   for (const auto& item : actual->named_parameters()) REQUIRE(torch::allclose(item.value().grad(), oracle->named_parameters()[item.key()].grad(), 2e-2, 3e-3));
   torch::optim::SGD update(actual->parameters(), torch::optim::SGDOptions(0.01));
   torch::optim::SGD reference_update(oracle->parameters(), torch::optim::SGDOptions(0.01));
   update.step();
   reference_update.step();
   for (const auto& item : actual->named_parameters()) REQUIRE(torch::allclose(item.value(), oracle->named_parameters()[item.key()], 2e-2, 3e-3));
  }
 }
}
TEST_CASE("Ordinary and DN grouped SDPA match masked dense equations with retained AMP backwards", "[rfdetr][attention][compilation]") {
 const tensor_fixture::FullMatrixPrecision precision;
 for (const auto& [device, dtype] : tensor_fixture::available_amp_precisions()) {
  for (bool dn : {false, true}) {
   CAPTURE(device.str(), dtype, dn);
   const auto options = torch::TensorOptions().device(device);
   rf::DecoderQueryLayout layout;
   layout.ordinary = {2, 3};
   if (dn) {
    layout.denoising_groups = 2;
    layout.denoising_queries_per_group = 2;
    layout.denoising_valid_slots = torch::tensor({{{true, false}, {true, true}}, {{false, false}, {false, false}}}, options.dtype(torch::kBool));
    layout.denoising_key_padding = torch::tensor({{{false, true}, {false, false}}, {{false, true}, {false, true}}}, options.dtype(torch::kBool));
   }
   AttentionPair pair(device);
   auto& actual = pair.actual;
   auto& oracle = pair.oracle;
   std::vector<torch::Tensor> actual_losses, oracle_losses;
   std::vector<std::pair<torch::Tensor, torch::Tensor>> inputs;
   for (const bool equal_inputs : {false, true}) {
    auto target = torch::rand({2, layout.total_queries(), 4}, options).set_requires_grad(true);
    auto position = (equal_inputs ? target.detach().clone() : torch::zeros_like(target)).set_requires_grad(true);
    auto reference_target = target.detach().clone().set_requires_grad(true);
    auto reference_position = position.detach().clone().set_requires_grad(true);
    auto admitted = torch::zeros({2, layout.total_queries(), layout.total_queries()}, options.dtype(torch::kBool));
    for (int64_t group = 0; group < 2; ++group) admitted.narrow(1, group * 3, 3).narrow(2, group * 3, 3).fill_(true);
    auto clean_target = reference_target;
    auto clean_position = reference_position;
    if (dn) {
     const auto valid = layout.denoising_valid_slots.reshape({2, 4, 1});
     clean_target = torch::cat({reference_target.narrow(1, 0, 6), torch::where(valid, reference_target.narrow(1, 6, 4), torch::zeros_like(reference_target.narrow(1, 6, 4)))}, 1);
     clean_position = torch::cat({reference_position.narrow(1, 0, 6), torch::where(valid, reference_position.narrow(1, 6, 4), torch::zeros_like(reference_position.narrow(1, 6, 4)))}, 1);
     for (int64_t group = 0; group < 2; ++group)
      admitted.narrow(1, 6 + group * 2, 2).narrow(2, 6 + group * 2, 2).copy_((~layout.denoising_key_padding.select(1, group)).unsqueeze(1).expand({2, 2, 2}));
    }
    torch::Tensor output, expected;
    {
     mmltk::backend::ml::cuda::TorchAutocastScope amp(device.is_cuda() && dtype != torch::kFloat32, dtype);
     output = rf::isolated_group_self_attention(actual, target, position, layout);
     expected = dense_attention(oracle, clean_target, clean_position, admitted);
    }
    REQUIRE(torch::allclose(output, expected, 1e-2, 3e-3));
    actual_losses.push_back(output.square().sum());
    oracle_losses.push_back(expected.square().sum());
    inputs.emplace_back(target, reference_target);
    inputs.emplace_back(position, reference_position);
   }
   for (size_t index = 0; index < actual_losses.size(); ++index) {
    actual_losses[index].backward();
    oracle_losses[index].backward();
   }
   for (const auto& [actual_input, expected_input] : inputs) REQUIRE(torch::allclose(actual_input.grad(), expected_input.grad(), 2e-2, 5e-3));
   for (const auto& item : actual->named_parameters()) {
    const auto expected_gradient = oracle->named_parameters()[item.key()].grad();
    const auto difference = (item.value().grad() - expected_gradient).abs();
    CAPTURE(item.key(), difference.max().item<float>(), (difference / (expected_gradient.abs() * 2e-2 + 5e-3)).max().item<float>());
    CAPTURE(item.value().grad(), expected_gradient);
    REQUIRE(torch::allclose(item.value().grad(), expected_gradient, 2e-2, 5e-3));
   }
   torch::optim::SGD optimizer(actual->parameters(), torch::optim::SGDOptions(0.001));
   torch::optim::SGD expected_optimizer(oracle->parameters(), torch::optim::SGDOptions(0.001));
   optimizer.step();
   expected_optimizer.step();
   for (const auto& item : actual->named_parameters()) REQUIRE(torch::allclose(item.value(), oracle->named_parameters()[item.key()], 2e-3, 3e-4));
  }
 }
}
TEST_CASE("Actual decoder tail matches independent residual norm and feedforward VJPs", "[rfdetr][compilation][parity]") {
 for (const auto& [device, dtype] : tensor_fixture::available_amp_precisions()) {
  const auto layer = rf::test_support::DecoderTailTestAccess::make(4, 7);
  layer->to(device);
  layer->train();
  std::unordered_map<std::string, torch::Tensor> reference;
  {
   torch::NoGradGuard guard;
   for (const auto& item : layer->named_parameters()) {
    item.value().copy_((torch::arange(item.value().numel(), item.value().options()) * 0.017 + 0.13).reshape_as(item.value()));
    reference.emplace(item.key(), item.value().detach().clone().set_requires_grad(true));
   }
  }
  const auto normalization = [&](const torch::Tensor& x, const std::string& prefix) {
   const auto full = x.to(torch::kFloat32);
   const auto centered = full - full.mean(-1, true);
   return centered * torch::rsqrt(centered.square().mean(-1, true) + 1e-5) * reference.at(prefix + ".weight") + reference.at(prefix + ".bias");
  };
  std::vector<std::pair<torch::Tensor, torch::Tensor>> arguments, losses;
  // Ordinary and combined DN extents share the exact private method. The
  // last repeated extent reaches the executor's later optimized path.
  for (const int64_t queries : {3, 7, 7, 7}) {
   auto residual = torch::rand({2, queries, 4}, torch::TensorOptions().device(device)).set_requires_grad(true);
   auto cross = torch::rand_like(residual).to(dtype).set_requires_grad(true);
   auto expected_residual = residual.detach().clone().set_requires_grad(true);
   auto expected_cross = cross.detach().clone().set_requires_grad(true);
   torch::Tensor actual, expected;
   {
    mmltk::backend::ml::cuda::TorchAutocastScope amp(device.is_cuda() && dtype != torch::kFloat32, dtype);
    actual = rf::test_support::DecoderTailTestAccess::invoke(*layer, residual, cross, true);
    const auto normalized = normalization(expected_residual + expected_cross, "norm2");
    const auto hidden = torch::relu(torch::linear(normalized, reference.at("linear1.weight"), reference.at("linear1.bias")));
    const auto projected = torch::linear(hidden, reference.at("linear2.weight"), reference.at("linear2.bias"));
    expected = normalization(normalized + projected, "norm3");
   }
   REQUIRE(torch::allclose(actual, expected, 5e-3, 5e-3));
   arguments.emplace_back(residual, expected_residual);
   arguments.emplace_back(cross, expected_cross);
   // Nonuniform upstream cotangents prevent layer normalization's sum from
   // making every gradient vanish in the oracle.
   const auto cotangent = torch::arange(actual.numel(), actual.options()).reshape_as(actual) * 0.01;
   losses.emplace_back((actual * cotangent).sum(), (expected * cotangent).sum());
  }
  for (const auto& [actual, expected] : losses) {
   actual.backward();
   expected.backward();
  }
  for (const auto& [actual, expected] : arguments) REQUIRE(torch::allclose(actual.grad(), expected.grad(), 2e-2, 5e-3));
  for (const auto& item : layer->named_parameters()) {
   if (!reference.at(item.key()).grad().defined()) {
    REQUIRE_FALSE(item.value().grad().defined());
    continue;
   }
   REQUIRE(item.value().grad().defined());
   REQUIRE(torch::allclose(item.value().grad(), reference.at(item.key()).grad(), 2e-2, 5e-3));
  }
 }
}
}  // namespace
