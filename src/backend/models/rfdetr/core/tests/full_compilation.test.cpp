#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/backend/models/rfdetr/core/detail/modules_technical.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/test_support/error_expectation_test_utils.hpp"
#include <ATen/CPUGeneratorImpl.h>
#include <ATen/cuda/CUDAGeneratorImpl.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>
namespace {
namespace rf = mmltk::backend::models::rfdetr;
namespace fixture = mmltk::backend::ml::testsupport;
using Access = rf::test_support::FullTraceTestAccess;
rf::NativeRfDetrConfig compilation_config(bool masks = false) {
 auto config = rf::native_config_from_preset(rf::model_presets().front());
 config.resolution = 64;
 config.num_queries = config.num_select = 3;
 config.num_classes = 3;
 config.group_detr = 2;
 config.dec_layers = 2;
 config.aux_loss = true;
 config.segmentation = masks;
 return config;
}
bool contains_operation(const torch::jit::Block* block, std::string_view operation) {
 for (const auto* node : block->nodes()) {
  if (node->kind().toQualString() == operation) return true;
  for (const auto* child : node->blocks())
   if (contains_operation(child, operation)) return true;
  if (node->hasAttribute(torch::jit::attr::Subgraph) && contains_operation(node->g(torch::jit::attr::Subgraph)->block(), operation)) return true;
 }
 return false;
}
std::vector<torch::Tensor> prediction_tensors(const rf::ModelOutputs& outputs) {
 std::vector<torch::Tensor> result;
 const auto append = [&](const rf::OutputLayer& layer) {
  result.push_back(layer.pred_logits);
  result.push_back(layer.pred_boxes);
  if (layer.pred_masks) result.push_back(*layer.pred_masks);
  if (layer.sparse_pred_masks) {
   result.push_back(layer.sparse_pred_masks->spatial_features);
   result.push_back(layer.sparse_pred_masks->query_features);
   result.push_back(layer.sparse_pred_masks->bias);
  }
 };
 append(outputs.main);
 for (const auto& layer : outputs.aux_outputs) append(layer);
 if (outputs.enc_outputs) append(*outputs.enc_outputs);
 return result;
}
void compare_tensor(const torch::Tensor& actual, const torch::Tensor& expected, c10::ScalarType precision) {
 REQUIRE(actual.sizes() == expected.sizes());
 REQUIRE(actual.scalar_type() == expected.scalar_type());
 REQUIRE(torch::equal(torch::isfinite(actual), torch::isfinite(expected)));
 const double tolerance = precision == torch::kFloat32 ? 4e-4 : precision == torch::kFloat16 ? 4e-3 : .02;
 const auto difference = (actual.detach().to(torch::kFloat64) - expected.detach().to(torch::kFloat64)).abs();
 CAPTURE(difference.max().item<double>(), expected.detach().abs().max().item<double>(), precision);
 CHECK(torch::allclose(actual, expected, tolerance, tolerance));
}
torch::Tensor prediction_vjp(const rf::ModelOutputs& actual, const rf::ModelOutputs& expected, c10::ScalarType dtype, bool use_actual) {
 REQUIRE(actual.aux_outputs.size() == expected.aux_outputs.size());
 REQUIRE(actual.enc_outputs.has_value() == expected.enc_outputs.has_value());
 const auto observed = prediction_tensors(actual), reference = prediction_tensors(expected);
 REQUIRE(observed.size() == reference.size());
 auto total = torch::zeros({}, reference.front().options().dtype(torch::kFloat32));
 for (std::size_t index = 0; index < observed.size(); ++index) {
  if (use_actual) compare_tensor(observed[index], reference[index], dtype);
  const auto& tensor = use_actual ? observed[index] : reference[index];
  const auto cotangent = torch::sin(torch::arange(tensor.numel(), tensor.options().dtype(torch::kFloat32)) + static_cast<double>(index)).reshape(tensor.sizes()) / tensor.numel();
  total = total + (tensor.to(torch::kFloat32) * cotangent).sum();
 }
 return total;
}
TEST_CASE("Full CUDA traces preserve every prediction head and fixed-vector derivatives through replacement", "[rfdetr][compilation][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; full production-kernel parity remains unverified");
 const fixture::FullMatrixPrecision full_precision;
 for (const auto& [device, dtype] : fixture::available_amp_precisions()) {
  if (!device.is_cuda()) continue;
  for (const bool masks : {false, true}) {
   CAPTURE(dtype, masks);
   const auto config = compilation_config(masks);
   torch::manual_seed(428);
   rf::NativeRfDetrModel eager(config);
   torch::manual_seed(428);
   rf::NativeRfDetrModel compiled(config);
   eager.to(device);
   compiled.to(device);
   eager.train();
   auto generator = at::cuda::detail::getDefaultCUDAGenerator(device.index());
   auto cpu_generator = at::detail::getDefaultCPUGenerator();
   const auto cpu_state = cpu_generator.get_state(), cuda_state = generator.get_state();
   compiled.optimize_for_inference(2, true, rf::CompilationMode::kFullTrace);
   CHECK(torch::equal(cpu_state, cpu_generator.get_state()));
   CHECK(torch::equal(cuda_state, generator.get_state()));
   CHECK_FALSE(Access::identity(compiled, true));
   CHECK_FALSE(Access::reference_attention(compiled));
   auto image = torch::rand({2, 3, 64, 64}, torch::TensorOptions().device(device)).set_requires_grad(true);
   auto traced_image = image.detach().clone().set_requires_grad(true);
   std::vector<torch::Tensor> eager_losses, compiled_losses;
   std::shared_ptr<torch::jit::Graph> retained_graph;
   for (int invocation = 0; invocation < 5; ++invocation) {
    CAPTURE(invocation);
    mmltk::backend::ml::cuda::TorchAutocastScope amp(dtype != torch::kFloat32, dtype);
    const auto before = generator.get_state();
    const auto expected = eager.forward({image + invocation * .013, {}}, masks);
    const auto after = generator.get_state();
    generator.set_state(before);
    const auto actual = compiled.forward({traced_image + invocation * .013, {}}, masks);
    REQUIRE(torch::equal(after, generator.get_state()));
    REQUIRE(actual.aux_outputs.size() == 1);
    REQUIRE(actual.enc_outputs);
    CHECK(actual.main.sparse_pred_masks.has_value() == masks);
    eager_losses.push_back(prediction_vjp(actual, expected, dtype, false));
    compiled_losses.push_back(prediction_vjp(actual, expected, dtype, true));
    const auto graph = Access::graph(compiled, true);
    REQUIRE(graph);
    REQUIRE(contains_operation(graph->block(), "mmltk::ms_deform_attn"));
    if (masks) REQUIRE(contains_operation(graph->block(), "mmltk::segmentation_depthwise"));
    if (invocation == 4) {
     const auto executed = torch::jit::lastExecutedOptimizedGraph();
     REQUIRE(executed);
     REQUIRE(contains_operation(executed->block(), "mmltk::ms_deform_attn"));
     if (masks) REQUIRE(contains_operation(executed->block(), "mmltk::segmentation_depthwise"));
    }
    if (retained_graph) CHECK(graph == retained_graph);
    retained_graph = graph;
   }
   // A new preparation records while earlier recorded and optimized results
   // are still live. Both generations must remain differentiable.
   compiled.optimize_for_inference(1, true, rf::CompilationMode::kFullTrace);
   {
    mmltk::backend::ml::cuda::TorchAutocastScope amp(dtype != torch::kFloat32, dtype);
    const auto expected = eager.forward({image.narrow(0, 0, 1), {}}, masks);
    const auto actual = compiled.forward({traced_image.narrow(0, 0, 1), {}}, masks);
    eager_losses.push_back(prediction_vjp(actual, expected, dtype, false));
    compiled_losses.push_back(prediction_vjp(actual, expected, dtype, true));
   }
   CHECK(Access::graph(compiled, true) != retained_graph);
   compiled.invalidate_compilation();
   retained_graph.reset();
   for (std::size_t index = 0; index < eager_losses.size(); ++index) {
    eager_losses[index].backward();
    compiled_losses[index].backward();
   }
   compare_tensor(traced_image.grad(), image.grad(), dtype);
   const auto actual_parameters = compiled.named_parameters();
   for (const auto& item : eager.named_parameters()) {
    CAPTURE(item.key());
    const auto& actual = actual_parameters[item.key()];
    REQUIRE(actual.grad().defined() == item.value().grad().defined());
    if (actual.grad().defined()) compare_tensor(actual.grad(), item.value().grad(), dtype);
   }
  }
 }
}
TEST_CASE("Full model admission retains mode tails live weights and ordinary geometry errors", "[rfdetr][compilation][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; full trace guards remain unverified");
 const fixture::FullMatrixPrecision precision;
 rf::NativeRfDetrModel model(compilation_config(true));
 model.to(torch::Device(torch::kCUDA, 0));
 model.optimize_for_inference(2, true, rf::CompilationMode::kFullTrace);
 const auto options = torch::TensorOptions().device(torch::kCUDA);
 const auto input = torch::rand({2, 3, 64, 64}, options);
 const auto tail = input.narrow(0, 0, 1);
 static_cast<void>(model.forward({tail, {}}, true));
 CHECK_FALSE(Access::identity(model, true));
 const auto initial = model.forward({input, {}}, true);
 auto graph = Access::graph(model, true);
 REQUIRE(graph);
 // Different input dtype and input/ambient gradient policy take the checked
 // ordinary path while preserving the admitted FP32 training graph.
 compare_tensor(model.forward({input.to(torch::kFloat16), {}}, true).main.pred_logits, model.forward({input.to(torch::kFloat16).to(torch::kFloat32), {}}, true).main.pred_logits, torch::kFloat32);
 CHECK(Access::graph(model, true) == graph);
 auto differentiable_input = input.detach().clone().set_requires_grad(true);
 CHECK(model.forward({differentiable_input, {}}, true).main.pred_logits.requires_grad());
 CHECK(Access::graph(model, true) == graph);
 {
  torch::NoGradGuard no_grad;
  CHECK_FALSE(model.forward({input, {}}, true).main.pred_logits.requires_grad());
  CHECK(Access::graph(model, true) == graph);
 }
 const auto no_masks = model.forward({input, {}}, false);
 for (const auto* layer : {&no_masks.main, &no_masks.aux_outputs.front(), &*no_masks.enc_outputs}) {
  CHECK_FALSE(layer->pred_masks);
  CHECK_FALSE(layer->sparse_pred_masks);
 }
 CHECK(Access::graph(model, true) == graph);
 const auto mask = torch::zeros({2, 64, 64}, options.dtype(torch::kBool));
 static_cast<void>(model.forward({input, mask}, true));
 CHECK(Access::graph(model, true) == graph);
 {
  torch::NoGradGuard no_grad;
  model.named_parameters()["class_embed.bias"].add_(.5);
 }
 const auto updated = model.forward({input, {}}, true);
 CHECK(torch::allclose(updated.main.pred_logits, initial.main.pred_logits + .5, 2e-4, 2e-4));
 CHECK(Access::graph(model, true) == graph);
 // Direct requires_grad changes must invalidate even without an owner call.
 for (auto& parameter : model.parameters()) parameter.set_requires_grad(false);
 const auto frozen = model.forward({input, {}}, true);
 CHECK_FALSE(frozen.main.pred_logits.requires_grad());
 CHECK(Access::graph(model, true) != graph);
 graph = Access::graph(model, true);
 model.named_parameters()["class_embed.weight"].set_requires_grad(true);
 CHECK(model.forward({input, {}}, true).main.pred_logits.requires_grad());
 CHECK(Access::graph(model, true) != graph);
 graph = Access::graph(model, true);
 model.optimize_for_inference(3, true, rf::CompilationMode::kFullTrace);
 REQUIRE_THROWS(model.forward({torch::rand({3, 3, 96, 96}, options), {}}, true));
 CHECK(Access::graph(model, true) == graph);
 CHECK_NOTHROW(model.forward({input, {}}, true));
 model.optimize_for_inference(2, false, rf::CompilationMode::kFullTrace);
 {
  torch::NoGradGuard no_grad;
  const auto eval = model.forward({input, {}}, true);
  REQUIRE(eval.main.pred_masks);
  CHECK_FALSE(eval.main.sparse_pred_masks);
  CHECK(eval.main.pred_logits.size(1) == model.config().num_queries);
  REQUIRE(Access::graph(model, false));
  CHECK(Access::graph(model, true) == graph);
  CHECK(model.forward({tail, {}}, true).main.pred_logits.size(0) == 1);
  REQUIRE_THROWS(model.forward({torch::rand({2, 3, 96, 96}, options), {}}, true));
 }
 model.train();
 CHECK(model.forward({input, {}}, true).main.pred_logits.size(1) == model.config().num_queries * model.config().group_detr);
 model.set_force_pytorch_deformable_attn(true);
 CHECK_FALSE(Access::graph(model, true));
 CHECK_FALSE(Access::graph(model, false));
 static_cast<void>(model.forward({input, {}}, true));
 CHECK_FALSE(contains_operation(Access::graph(model, true)->block(), "mmltk::ms_deform_attn"));
}
TEST_CASE("Lazy FullTrace rejects target-dependent supervision before recording", "[rfdetr][compilation]") {
 for (const bool match_free : {false, true}) {
  auto config = compilation_config();
  config.training_supervision.assignment = match_free ? rf::TrainAssignmentKind::MatchFree : rf::TrainAssignmentKind::Hungarian;
  config.training_supervision.denoising.enabled = !match_free;
  const auto layout = rf::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"first", "second"}));
  rf::NativeRfDetrModel model(config, layout);
  model.initialize_training_supervision(128);
  model.optimize_for_inference(1, true, rf::CompilationMode::kFullTrace);
  CHECK_FALSE(Access::graph(model, true));
  const rf::NestedTensor input{torch::zeros({1, 3, 64, 64}), {}};
  mmltk::testsupport::expect_runtime_error_contains([&] {
   if (match_free)
    static_cast<void>(model.forward_for_match_free(input));
   else
    static_cast<void>(model.forward_with_denoising(input, {}, {128, 0, 0, 0}));
  }, "full-trace RF-DETR cannot");
  CHECK_FALSE(Access::graph(model, true));
 }
}
}  // namespace
