#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "src/backend/ml/runtime/onnx_environment.h"
#include "src/common/system/numa_memory.h"
#include <ATen/TensorIndexing.h>
#include <ATen/ops/alias.h>
#include <ATen/ops/_scaled_dot_product_attention_math.h>
#include <ATen/ops/scaled_dot_product_attention.h>
#include "src/backend/models/rfdetr/core/detail/segmentation_depthwise.h"
#include "src/backend/models/rfdetr/core/detail/linear.h"
#include <ATen/ops/arange.h>
#include <ATen/ops/bitwise_and.h>
#include <ATen/ops/conv2d.h>
#include <ATen/ops/einsum.h>
#include <ATen/ops/layer_norm.h>
#include <ATen/ops/t.h>
#include <ATen/ops/unbind.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#define ONNX_NAMESPACE onnx_torch
#include <torch/csrc/jit/serialization/export.h>
#undef ONNX_NAMESPACE
#include <torch/script.h>
#include <torch/nn/functional/upsampling.h>
#include <torch/torch.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "src/test_support/error_expectation_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/training/tests/encoder_training_fixture.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include <c10/cuda/CUDAStream.h>
#include <c10/cuda/CUDAGuard.h>
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/backend/models/rfdetr/training/tests/optimizer_fixture.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/training/tests/model_state_fixture.h"
#endif
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.model_export.onnx_lowering;
namespace {
class BackendTestStream final {
public:
 BackendTestStream() : guard_(c10::cuda::getStreamFromExternal(stream_.get(), 0)) {}
 [[nodiscard]] cudaStream_t get() const noexcept { return stream_.get(); }
 [[nodiscard]] mmltk::backend::ml::runtime::BorrowedCommandStream command() const noexcept { return {reinterpret_cast<std::uintptr_t>(stream_.get()), true}; }

private:
 mmltk::testsupport::ScopedTestStream stream_;
 c10::cuda::CUDAStreamGuard guard_;
};
mmltk::backend::ml::runtime::RuntimeTensorBuffer runtime_buffer(const torch::Tensor& tensor) {
 mmltk::backend::ml::runtime::RuntimeTensorBuffer result;
 result.device_data = tensor.data_ptr();
 result.capacity_bytes = tensor.nbytes();
 result.shape.rank = static_cast<std::uint8_t>(tensor.dim());
 std::copy(tensor.sizes().begin(), tensor.sizes().end(), result.shape.extents.begin());
 return result;
}
// Every case in this suite registers under the same tag.
#define MMLTK_TEST_JIT_SYMBOL(name) (::c10::Symbol::fromQualString(name))
#define MMLTK_TEST_JIT_ATTR(name) (::c10::Symbol::attr(name))
#define kAtenTo MMLTK_TEST_JIT_SYMBOL("aten::to")
#define kAtenTypeAs MMLTK_TEST_JIT_SYMBOL("aten::type_as")
#define kAtenConvolution MMLTK_TEST_JIT_SYMBOL("aten::_convolution")
#define kAtenFlatten MMLTK_TEST_JIT_SYMBOL("aten::flatten")
#define kAtenT MMLTK_TEST_JIT_SYMBOL("aten::t")
#define kAtenTranspose MMLTK_TEST_JIT_SYMBOL("aten::transpose")
#define kAtenView MMLTK_TEST_JIT_SYMBOL("aten::view")
#define kAtenCat MMLTK_TEST_JIT_SYMBOL("aten::cat")
#define kAtenStack MMLTK_TEST_JIT_SYMBOL("aten::stack")
#define kAtenRepeat MMLTK_TEST_JIT_SYMBOL("aten::repeat")
#define kAtenSlice MMLTK_TEST_JIT_SYMBOL("aten::slice")
#define kAtenSelect MMLTK_TEST_JIT_SYMBOL("aten::select")
#define kAtenNarrow MMLTK_TEST_JIT_SYMBOL("aten::narrow")
#define kAtenDetach MMLTK_TEST_JIT_SYMBOL("aten::detach")
#define kAtenAlias MMLTK_TEST_JIT_SYMBOL("aten::alias")
#define kAtenDropout MMLTK_TEST_JIT_SYMBOL("aten::dropout")
#define kAtenLayerNorm MMLTK_TEST_JIT_SYMBOL("aten::layer_norm")
#define kAtenAddInplace MMLTK_TEST_JIT_SYMBOL("aten::add_")
#define kAtenArange MMLTK_TEST_JIT_SYMBOL("aten::arange")
#define kAtenSoftmax MMLTK_TEST_JIT_SYMBOL("aten::softmax")
#define kAtenTopk MMLTK_TEST_JIT_SYMBOL("aten::topk")
#define kAtenOnesLike MMLTK_TEST_JIT_SYMBOL("aten::ones_like")
#define kAtenEinsum MMLTK_TEST_JIT_SYMBOL("aten::einsum")
#define kAtenSplit MMLTK_TEST_JIT_SYMBOL("aten::split")
#define kAtenUnbind MMLTK_TEST_JIT_SYMBOL("aten::unbind")
#define kAtenAdd MMLTK_TEST_JIT_SYMBOL("aten::add")
#define kAtenBitwiseAnd MMLTK_TEST_JIT_SYMBOL("aten::bitwise_and")
#define kPrimGetAttr (::c10::prim::GetAttr)
#define kOnnxAdd MMLTK_TEST_JIT_SYMBOL("onnx::Add")
#define kOnnxAnd MMLTK_TEST_JIT_SYMBOL("onnx::And")
#define kOnnxCast MMLTK_TEST_JIT_SYMBOL("onnx::Cast")
#define kOnnxConv MMLTK_TEST_JIT_SYMBOL("onnx::Conv")
#define kOnnxLayerNormalization MMLTK_TEST_JIT_SYMBOL("onnx::LayerNormalization")
#define kOnnxRange MMLTK_TEST_JIT_SYMBOL("onnx::Range")
#define kOnnxMatMul MMLTK_TEST_JIT_SYMBOL("onnx::MatMul")
#define kOnnxReshape MMLTK_TEST_JIT_SYMBOL("onnx::Reshape")
#define kOnnxTranspose MMLTK_TEST_JIT_SYMBOL("onnx::Transpose")
#define kOnnxConcat MMLTK_TEST_JIT_SYMBOL("onnx::Concat")
#define kOnnxTile MMLTK_TEST_JIT_SYMBOL("onnx::Tile")
#define kOnnxSlice MMLTK_TEST_JIT_SYMBOL("onnx::Slice")
#define kOnnxSoftmax MMLTK_TEST_JIT_SYMBOL("onnx::Softmax")
#define kOnnxTopK MMLTK_TEST_JIT_SYMBOL("onnx::TopK")
#define kOnnxConstantOfShape MMLTK_TEST_JIT_SYMBOL("onnx::ConstantOfShape")
#define kOnnxSplit MMLTK_TEST_JIT_SYMBOL("onnx::Split")
#define kAttrTo MMLTK_TEST_JIT_ATTR("to")
#define kAttrKernelShape MMLTK_TEST_JIT_ATTR("kernel_shape")
#define kAttrStrides MMLTK_TEST_JIT_ATTR("strides")
#define kAttrPads MMLTK_TEST_JIT_ATTR("pads")
#define kAttrDilations MMLTK_TEST_JIT_ATTR("dilations")
#define kAttrGroup MMLTK_TEST_JIT_ATTR("group")
#define kAttrAxis MMLTK_TEST_JIT_ATTR("axis")
using OnnxInitializerMap = std::unordered_map<std::string, torch::Tensor>;
[[nodiscard]] const void* find_test_initializer(const void* const context, const std::string& name) noexcept {
 const auto& initializers = *static_cast<const OnnxInitializerMap*>(context);
 const auto found = initializers.find(name);
 return found == initializers.end() ? nullptr : &found->second;
}
void lower_test_graph(const std::shared_ptr<torch::jit::Graph>& graph, const OnnxInitializerMap* const initializers = nullptr) {
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
 mmltk::backend::models::rfdetr::lower_graph_for_onnx_export({
  .graph = graph.get(),
  .initializer_context = initializers,
  .find_initializer = initializers == nullptr ? nullptr : &find_test_initializer,
  .readback = &readback,
 });
}
std::shared_ptr<torch::jit::Graph> trace_unary_graph(const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn) {
 auto cu = std::make_shared<torch::jit::CompilationUnit>();
 auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLowering", cu, true);
 torch::jit::Module module(cu, cls);
 auto trace_res =
  torch::jit::tracer::trace({input}, [&](torch::jit::Stack args) -> torch::jit::Stack { return {fn(args[0].toTensor())}; }, [](const torch::autograd::Variable&) { return ""; }, false, false, &module);
 return trace_res.first->graph;
}
std::pair<std::shared_ptr<torch::jit::Graph>, OnnxInitializerMap> trace_unary_graph_with_parameters(
 const torch::Tensor& input, const std::vector<std::pair<std::string, torch::Tensor>>& parameters, const std::function<torch::Tensor(torch::jit::Module&, const torch::Tensor&)>& fn) {
 auto cu = std::make_shared<torch::jit::CompilationUnit>();
 auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLoweringWithParameters", cu, true);
 torch::jit::Module module(cu, cls);
 OnnxInitializerMap initializers;
 for (const auto& [name, value] : parameters) {
  module.register_parameter(name, value, false);
  initializers.emplace(name, value);
 }
 auto trace_res = torch::jit::tracer::trace(
  {input}, [&](torch::jit::Stack args) -> torch::jit::Stack { return {fn(module, args[0].toTensor())}; }, [](const torch::autograd::Variable&) { return ""; }, false, false, &module);
 return {trace_res.first->graph, std::move(initializers)};
}
std::pair<std::shared_ptr<torch::jit::Graph>, OnnxInitializerMap> trace_unary_graph_with_bias_attr(const torch::Tensor& input, const torch::Tensor& bias) {
 return trace_unary_graph_with_parameters(input, {{"bias", bias}}, [](torch::jit::Module& module, const torch::Tensor& traced_input) { return traced_input + module.attr("bias").toTensor(); });
}
bool block_contains_kind(const torch::jit::Block* block, c10::Symbol kind) {
 for (const auto* node : block->nodes()) {
  if (node->kind() == kind) { return true; }
  for (const auto* child : node->blocks()) {
   if (block_contains_kind(child, kind)) { return true; }
  }
 }
 return false;
}
torch::jit::Node* find_first_node_kind(torch::jit::Block* block, c10::Symbol kind) {
 for (auto* node : block->nodes()) {
  if (node->kind() == kind) { return node; }
  for (auto* child : node->blocks()) {
   if (auto* found = find_first_node_kind(child, kind)) { return found; }
  }
 }
 return nullptr;
}
// Requires every `removed_kinds` symbol in the traced graph, lowers it for ONNX export, then
// requires that every `removed_kinds` symbol is gone and every `emitted_kinds` symbol is present.
void assert_lowering_replaces_kinds(const std::shared_ptr<torch::jit::Graph>& graph, const OnnxInitializerMap* initializers, const std::initializer_list<c10::Symbol> removed_kinds,
 const std::initializer_list<c10::Symbol> emitted_kinds) {
 for (const c10::Symbol kind : removed_kinds) { REQUIRE((block_contains_kind(graph->block(), kind))); }
 lower_test_graph(graph, initializers);
 for (const c10::Symbol kind : removed_kinds) { REQUIRE((!block_contains_kind(graph->block(), kind))); }
 for (const c10::Symbol kind : emitted_kinds) { REQUIRE((block_contains_kind(graph->block(), kind))); }
}
std::shared_ptr<torch::jit::Graph> lower_unary_graph_and_check(const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn,
 const std::initializer_list<c10::Symbol> removed_kinds, const std::initializer_list<c10::Symbol> emitted_kinds = {}) {
 auto graph = trace_unary_graph(input, fn);
 assert_lowering_replaces_kinds(graph, nullptr, removed_kinds, emitted_kinds);
 return graph;
}
void check_lowered_cast_target(
 const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn, const c10::Symbol traced_kind, const mmltk::backend::models::rfdetr::OnnxTensorElementType expected_dtype) {
 const auto graph = lower_unary_graph_and_check(input, fn, {traced_kind});
 auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
 REQUIRE((cast != nullptr));
 REQUIRE((cast->i(kAttrTo) == mmltk::backend::models::rfdetr::onnx_tensor_data_type(expected_dtype)));
}
TEST_CASE("test_supported_opset_validation", "[model][rfdetr][onnx_lowering]") {
 mmltk::backend::models::rfdetr::validate_supported_onnx_export_opset(19);
 mmltk::testsupport::expect_runtime_error_contains([]() { mmltk::backend::models::rfdetr::validate_supported_onnx_export_opset(17); }, "supports opset 19");
}
TEST_CASE("test_dtype_mapping", "[model][rfdetr][onnx_lowering]") {
 using mmltk::backend::models::rfdetr::onnx_tensor_data_type;
 using mmltk::backend::models::rfdetr::OnnxTensorElementType;
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::Float16) == 10));
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::Float32) == 1));
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::Bool) == 9));
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::Int32) == 6));
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::Int64) == 7));
 REQUIRE((onnx_tensor_data_type(OnnxTensorElementType::BFloat16) == 16));
}
TEST_CASE("test_lower_to_emits_onnx_cast", "[model][rfdetr][onnx_lowering]") {
 check_lowered_cast_target(torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kBool); }, kAtenTo, mmltk::backend::models::rfdetr::OnnxTensorElementType::Bool);
}
TEST_CASE("test_lower_type_as_emits_onnx_cast", "[model][rfdetr][onnx_lowering]") {
 check_lowered_cast_target(torch::randn({2, 3}), [](const torch::Tensor& input) { return input.type_as(torch::ones({1}, torch::TensorOptions().dtype(torch::kFloat16))); }, kAtenTypeAs,
  mmltk::backend::models::rfdetr::OnnxTensorElementType::Float16);
}
TEST_CASE("test_lower_noop_to_removes_redundant_cast", "[model][rfdetr][onnx_lowering]") {
 const auto graph = lower_unary_graph_and_check(torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kFloat32); }, {kAtenTo});
 REQUIRE((!block_contains_kind(graph->block(), kOnnxCast)));
}
TEST_CASE("test_lower_six_input_to_emits_onnx_cast", "[model][rfdetr][onnx_lowering]") {
 check_lowered_cast_target(torch::randn({2, 3}), [](const torch::Tensor& input) {
  auto source = torch::ones({2, 3}, torch::TensorOptions().dtype(torch::kInt64).device(input.device()));
  return source.to(input.device(), input.dtype(), false, false);
 }, kAtenTo, mmltk::backend::models::rfdetr::OnnxTensorElementType::Float32);
}
TEST_CASE("test_lower_convolution_emits_onnx_conv", "[model][rfdetr][onnx_lowering]") {
 auto weight = torch::randn({8, 3, 3, 3});
 auto bias = torch::randn({8});
 const auto graph = lower_unary_graph_and_check(torch::randn({1, 3, 16, 16}), [weight, bias](const torch::Tensor& input) {
  const std::vector<int64_t> stride{2, 2};
  const std::vector<int64_t> padding{1, 1};
  const std::vector<int64_t> dilation{1, 1};
  return torch::conv2d(input, weight, bias, stride, padding, dilation, 1);
 }, {kAtenConvolution});
 auto* conv = find_first_node_kind(graph->block(), kOnnxConv);
 REQUIRE((conv != nullptr));
 REQUIRE((conv->is(kAttrKernelShape) == std::vector<int64_t>({3, 3})));
 REQUIRE((conv->is(kAttrStrides) == std::vector<int64_t>({2, 2})));
 REQUIRE((conv->is(kAttrPads) == std::vector<int64_t>({1, 1, 1, 1})));
 REQUIRE((conv->is(kAttrDilations) == std::vector<int64_t>({1, 1})));
 REQUIRE((conv->i(kAttrGroup) == 1));
}
TEST_CASE("test_lower_flatten_emits_onnx_reshape", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4, 5}), [](const torch::Tensor& input) { return input.flatten(2); }, {kAtenFlatten}, {kOnnxReshape});
}
TEST_CASE("test_lower_t_emits_onnx_transpose", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({3, 4}), [](const torch::Tensor& input) { return at::t(input); }, {kAtenT}, {kOnnxTranspose});
}
TEST_CASE("test_lower_transpose_emits_onnx_transpose", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.transpose(1, 2); }, {kAtenTranspose}, {kOnnxTranspose});
}
TEST_CASE("test_lower_view_emits_onnx_reshape", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.view({2, 12}); }, {kAtenView}, {kOnnxReshape});
}
TEST_CASE("test_lower_cat_emits_onnx_concat", "[model][rfdetr][onnx_lowering]") {
 const auto graph = lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::cat({input, input}, 1); }, {kAtenCat});
 auto* concat = find_first_node_kind(graph->block(), kOnnxConcat);
 REQUIRE((concat != nullptr));
 REQUIRE((concat->i(kAttrAxis) == 1));
}
TEST_CASE("test_lower_stack_emits_concat_path", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::stack({input, input}, 1); }, {kAtenStack}, {kOnnxConcat});
}
TEST_CASE("test_lower_repeat_emits_onnx_tile", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({1, 3, 4}), [](const torch::Tensor& input) { return input.repeat({2, 1, 1}); }, {kAtenRepeat}, {kOnnxTile});
}
TEST_CASE("test_lower_slice_and_select_emit_onnx_slice_path", "[model][rfdetr][onnx_lowering]") {
 auto graph = trace_unary_graph(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.index({torch::indexing::Slice(), 0, torch::indexing::Slice()}); });
 REQUIRE((block_contains_kind(graph->block(), kAtenSlice) || block_contains_kind(graph->block(), kAtenSelect)));
 lower_test_graph(graph);
 REQUIRE((!block_contains_kind(graph->block(), kAtenSlice)));
 REQUIRE((!block_contains_kind(graph->block(), kAtenSelect)));
 REQUIRE((block_contains_kind(graph->block(), kOnnxSlice) || block_contains_kind(graph->block(), kOnnxReshape)));
 for (const auto axis : {0, -3}) {
  auto [parameter_graph, initializers] = trace_unary_graph_with_parameters(torch::zeros({3, 4}), {{"weight", torch::arange(24, torch::kFloat).reshape({2, 3, 4})}},
   [axis](torch::jit::Module& module, const torch::Tensor& input) { return input + module.attr("weight").toTensor().select(axis, 1); });
  auto* attribute = find_first_node_kind(parameter_graph->block(), kPrimGetAttr);
  REQUIRE(attribute != nullptr);
  attribute->output()->setType(c10::TensorType::get());
  lower_test_graph(parameter_graph, &initializers);
  REQUIRE_FALSE(block_contains_kind(parameter_graph->block(), kAtenSelect));
  const auto* gather = find_first_node_kind(parameter_graph->block(), c10::Symbol::fromQualString("onnx::Gather"));
  REQUIRE(gather != nullptr);
  CHECK(gather->i(kAttrAxis) == 0);
 }
}
TEST_CASE("test_lower_slice_with_negative_axis_and_step_emits_onnx_slice", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return input.slice(-1, 0, c10::nullopt, 2); }, {kAtenSlice}, {kOnnxSlice});
}
torch::Tensor execute_unary_onnx(const std::shared_ptr<torch::jit::Graph>& graph, const torch::Tensor& input) {
 mmltk::backend::ml::runtime::OnnxEnvironment environment(ORT_LOGGING_LEVEL_ERROR, "numerical-lowering");
 Ort::SessionOptions options;
 options.SetIntraOpNumThreads(1);
 options.SetInterOpNumThreads(1);
 const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
 if (graph->inputs().front()->type()->kind() == c10::TypeKind::ClassType) graph->eraseInput(0);
 graph->inputs().front()->setDebugName("input");
 graph->outputs().front()->setDebugName("output");
 auto exported = torch::jit::export_onnx(graph, {}, 19, {}, false, ::torch::onnx::OperatorExportTypes::ONNX, true, false, {}, true, false, "");
 const auto bytes = torch::jit::serialize_model_proto_to_string(std::get<0>(exported));
 Ort::Session session(environment.get(), bytes.data(), bytes.size(), options);
 auto value = Ort::Value::CreateTensor<float>(memory, input.data_ptr<float>(), input.numel(), input.sizes().data(), input.dim());
 const char* input_name = "input";
 const char* output_name = "output";
 auto output = session.Run(Ort::RunOptions{}, &input_name, &value, 1, &output_name, 1);
 const auto shape = output.front().GetTensorTypeAndShapeInfo().GetShape();
 return torch::from_blob(output.front().GetTensorMutableData<float>(), shape, torch::kFloat).clone();
}
TEST_CASE("test_antialiased_bicubic_resize_preserves_torch_values", "[model][rfdetr][onnx_lowering]") {
 namespace F = torch::nn::functional;
 auto input = torch::sin(torch::arange(126, torch::kFloat)).reshape({1, 2, 7, 9});
 for (const auto& size : {std::vector<int64_t>{3, 5}, std::vector<int64_t>{11, 13}, std::vector<int64_t>{1, 1}}) {
  for (const bool align_corners : {false, true}) {
   CAPTURE(size, align_corners);
   const auto interpolate = [&](
                             const torch::Tensor& value) { return F::interpolate(value, F::InterpolateFuncOptions().size(size).mode(torch::kBicubic).align_corners(align_corners).antialias(true)); };
   auto graph = trace_unary_graph(input, interpolate);
   if (align_corners) {
    mmltk::testsupport::expect_runtime_error_contains([&] { lower_test_graph(graph); }, "requires align_corners=false");
    continue;
   }
   lower_test_graph(graph);
   const auto actual = execute_unary_onnx(graph, input);
   const auto expected = interpolate(input);
   REQUIRE(actual.sizes() == expected.sizes());
   CHECK(torch::allclose(actual, expected, 1.0e-5, 1.0e-6));
   const auto position = torch::cat({torch::zeros({1, 1, 2}), input.permute({0, 2, 3, 1}).reshape({1, 63, 2})}, 1);
   const auto pixels = torch::zeros_like(expected);
   auto [constant_graph, initializers] = trace_unary_graph_with_parameters(pixels, {{"position", position}}, [&](torch::jit::Module& module, const torch::Tensor& value) {
    const auto grid = module.attr("position").toTensor().slice(1, 1).reshape({1, 7, 9, 2}).permute({0, 3, 1, 2}).to(torch::kFloat32);
    return value + interpolate(grid);
   });
   assert_lowering_replaces_kinds(constant_graph, &initializers, {c10::Symbol::fromQualString("aten::_upsample_bicubic2d_aa")}, {kOnnxAdd});
   CHECK_FALSE(block_contains_kind(constant_graph->block(), c10::Symbol::fromQualString("onnx::Resize")));
   CHECK(torch::allclose(execute_unary_onnx(constant_graph, pixels), expected, 1.0e-5, 1.0e-6));
  }
 }
}
TEST_CASE("SiLU ONNX lowering preserves activation values", "[model][rfdetr][onnx_lowering]") {
 const auto input = torch::tensor({-80.0f, -20.0f, -5.0f, -1.0f, 0.0f, 1.0f, 5.0f, 20.0f, 80.0f});
 const auto graph = lower_unary_graph_and_check(input, [](const torch::Tensor& value) { return at::silu(value); }, {c10::Symbol::fromQualString("aten::silu")});
 CHECK(torch::allclose(execute_unary_onnx(graph, input), at::silu(input), 1e-5, 1e-6));
}
TEST_CASE("test_lower_inference_dropout_removes_dropout_node", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return torch::dropout(input, 0.1, false); }, {kAtenDropout});
}
TEST_CASE("test_lower_detach_removes_detach_node", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return input.detach(); }, {kAtenDetach});
}
TEST_CASE("test_lower_alias_removes_alias_node", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return at::alias(input); }, {kAtenAlias});
}
TEST_CASE("test_lower_layer_norm_emits_onnx_layer_normalization", "[model][rfdetr][onnx_lowering]") {
 const auto weight = torch::randn({8});
 const auto bias = torch::randn({8});
 auto [graph, initializers] = trace_unary_graph_with_parameters(torch::randn({2, 3, 8}),
  {
   {"weight", weight},
   {"bias", bias},
  },
  [](torch::jit::Module& module, const torch::Tensor& input) { return torch::layer_norm(input, {8}, module.attr("weight").toTensor(), module.attr("bias").toTensor(), 1.0e-5, false); });
 assert_lowering_replaces_kinds(graph, &initializers, {kAtenLayerNorm}, {kOnnxLayerNormalization});
}
TEST_CASE("test_lower_add_inplace_emits_onnx_add", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) {
  auto out = input + 1.0;
  return out.add_(input);
 }, {kAtenAddInplace}, {kOnnxAdd});
}
TEST_CASE("test_lower_arange_emits_onnx_range", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return at::arange(input.size(2), input.options().dtype(torch::kFloat32)); }, {kAtenArange}, {kOnnxRange});
}
TEST_CASE("test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 5, 4}), [](const torch::Tensor& input) { return input.narrow(1, 1, input.size(1) - 1); }, {kAtenNarrow}, {kOnnxSlice});
}
TEST_CASE("test_lower_softmax_emits_onnx_softmax", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.softmax(-1, c10::nullopt); }, {kAtenSoftmax}, {kOnnxSoftmax});
}
TEST_CASE("test_lower_topk_emits_onnx_topk", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return std::get<1>(input.topk(2, 1, true, true)); }, {kAtenTopk}, {kOnnxTopK});
}
TEST_CASE("test_lower_ones_like_emits_constant_of_shape", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::ones_like(input); }, {kAtenOnesLike}, {kOnnxConstantOfShape});
}
TEST_CASE("test_lower_einsum_emits_onnx_einsum", "[model][rfdetr][onnx_lowering]") {
 const auto query = torch::randn({2, 7, 3});
 lower_unary_graph_and_check(torch::randn({2, 3, 4, 5}), [query](const torch::Tensor& input) { return at::einsum("bchw,bnc->bnhw", {input, query}); }, {kAtenEinsum}, {kOnnxMatMul, kOnnxReshape});
}
TEST_CASE("test_lower_split_list_unpack_emits_onnx_split", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return input.split(1, 1)[0]; }, {kAtenSplit}, {kOnnxSplit});
}
TEST_CASE("test_lower_unbind_list_unpack_emits_onnx_split", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return at::unbind(input, 1)[0]; }, {kAtenUnbind}, {kOnnxSplit});
}
TEST_CASE("test_lower_add_with_bias_attr_emits_onnx_add", "[model][rfdetr][onnx_lowering]") {
 auto bias = torch::randn({1, 3, 1, 1});
 auto [graph, initializers] = trace_unary_graph_with_bias_attr(torch::randn({1, 3, 8, 8}), bias);
 REQUIRE((block_contains_kind(graph->block(), kPrimGetAttr)));
 assert_lowering_replaces_kinds(graph, &initializers, {kAtenAdd}, {kOnnxAdd});
}
TEST_CASE("test_lower_bool_bitwise_and_emits_onnx_and", "[model][rfdetr][onnx_lowering]") {
 lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return at::bitwise_and(input > 0.0, input < 1.0); }, {kAtenBitwiseAnd}, {kOnnxAnd});
}
TEST_CASE("test_lower_integer_bitwise_and_fails_loudly", "[model][rfdetr][onnx_lowering]") {
 auto graph = trace_unary_graph(torch::randint(0, 8, {2, 3}, torch::TensorOptions().dtype(torch::kInt64)), [](const torch::Tensor& input) { return at::bitwise_and(input, 1); });
 REQUIRE((block_contains_kind(graph->block(), kAtenBitwiseAnd)));
 mmltk::testsupport::expect_runtime_error_contains([&graph]() { lower_test_graph(graph); }, "integer bitwise_and is not supported");
}
TEST_CASE("test_cuda_constants_stage_once_and_retain_nested_graph_readers", "[model][rfdetr][onnx_lowering]") {
 if (!torch::cuda::is_available()) { SKIP("CUDA unavailable"); }
 auto source = torch::arange(8, torch::TensorOptions().device(torch::kCUDA)).narrow(0, 2, 3);
 auto* original_storage = source.const_data_ptr();
 auto graph = std::make_shared<torch::jit::Graph>();
 // CLEANUP-IGNORE: Different nested graphs own constants with distinct tensor and IValue payloads; only node allocation matches.
 auto nested = std::make_shared<torch::jit::Graph>();
 auto* first = nested->create(c10::Symbol::fromQualString("onnx::Constant"), 1);
 first->t_(c10::attr::value, source);
 nested->appendNode(first);
 nested->registerOutput(first->output());
 auto* second = nested->create(c10::Symbol::fromQualString("onnx::Constant"), 1);
 second->t_(c10::attr::value, source);
 nested->appendNode(second);
 nested->registerOutput(second->output());
 auto* container = graph->create(c10::Symbol::fromQualString("onnx::If"), 1);
 container->g_(c10::Symbol::attr("then_branch"), nested);
 graph->appendNode(container);
 graph->registerOutput(container->output());
 const auto payload = c10::Symbol::attr("tensor_payload");
 c10::List<at::Tensor> tensor_list;
 tensor_list.push_back(source);
 c10::impl::GenericDict dictionary(c10::StringType::get(), c10::TensorType::get());
 dictionary.insert("same", source);
 first->ival_(payload, c10::ivalue::Tuple::create({c10::IValue(tensor_list), c10::IValue(dictionary)}));
 second->ts_(c10::Symbol::attr("tensor_list"), {source, source});
 const auto large_dead = torch::empty({65536}, source.options());
 const auto append_dead = [&](torch::jit::Graph& owner) {
  auto* dead = owner.create(c10::Symbol::fromQualString("onnx::Constant"), 1);
  dead->t_(c10::attr::value, large_dead);
  owner.appendNode(dead);
 };
 append_dead(*graph);
 append_dead(*nested);
 // CLEANUP-IGNORE: Different nested graphs own constants with distinct tensor and IValue payloads; only node allocation matches.
 auto graph_list = std::make_shared<torch::jit::Graph>();
 auto* list_constant = graph_list->create(c10::Symbol::fromQualString("onnx::Constant"), 1);
 list_constant->ival_(payload, first->ival(payload));
 graph_list->appendNode(list_constant);
 graph_list->registerOutput(list_constant->output());
 append_dead(*graph_list);
 container->gs_(c10::Symbol::attr("other_branches"), {graph_list});
 auto* block = container->addBlock();
 auto* block_constant = graph->create(c10::Symbol::fromQualString("onnx::Constant"), 1);
 block_constant->ival_(payload, first->ival(payload));
 block->appendNode(block_constant);
 block->registerOutput(block_constant->output());
 auto* dead_block_constant = graph->create(c10::Symbol::fromQualString("onnx::Constant"), 1);
 dead_block_constant->t_(c10::attr::value, large_dead);
 block->appendNode(dead_block_constant);
 OnnxInitializerMap initializers{{"unused", large_dead}};
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
 mmltk::backend::models::rfdetr::lower_graph_for_onnx_export({.graph = graph.get(), .initializer_context = &initializers, .find_initializer = &find_test_initializer, .readback = &readback});
 REQUIRE((first->t(c10::attr::value).is_cpu()));
 REQUIRE((first->t(c10::attr::value).data_ptr() == second->t(c10::attr::value).data_ptr()));
 REQUIRE((torch::equal(first->t(c10::attr::value), torch::tensor({2, 3, 4}, source.options().device(torch::kCPU)))));
 REQUIRE((source.const_data_ptr() == original_storage));
 REQUIRE((source.is_cuda()));
 // Every live reference names the same TensorImpl. A dead attribute or
 // unused initializer would add at least another registered page/slot.
 REQUIRE(readback.capacity_bytes() == mmltk::common::system::page_rounded_bytes(source.nbytes()));
 REQUIRE(std::distance(graph->nodes().begin(), graph->nodes().end()) == 1);
 REQUIRE(std::distance(nested->nodes().begin(), nested->nodes().end()) == 2);
 REQUIRE(std::distance(graph_list->nodes().begin(), graph_list->nodes().end()) == 1);
 REQUIRE(std::distance(block->nodes().begin(), block->nodes().end()) == 1);
 for (auto* node : {first, list_constant, block_constant}) {
  const auto tuple = node->ival(payload).toTuple();
  const auto from_list = tuple->elements()[0].toTensorList().get(0);
  const auto from_dictionary = tuple->elements()[1].toGenericDict().at(c10::IValue("same")).toTensor();
  REQUIRE(from_list.is_cpu());
  REQUIRE(from_dictionary.is_cpu());
  REQUIRE(from_list.sizes() == source.sizes());
  REQUIRE(from_list.scalar_type() == source.scalar_type());
  REQUIRE(torch::equal(from_list, first->t(c10::attr::value)));
  REQUIRE(from_list.unsafeGetTensorImpl() == first->t(c10::attr::value).unsafeGetTensorImpl());
  REQUIRE(from_dictionary.unsafeGetTensorImpl() == from_list.unsafeGetTensorImpl());
 }
 REQUIRE(second->ts(c10::Symbol::attr("tensor_list"))[0].unsafeGetTensorImpl() == first->t(c10::attr::value).unsafeGetTensorImpl());
 REQUIRE_THROWS(readback.Release());
 graph.reset();
 nested.reset();
 graph_list.reset();
 readback.Release();
}
TEST_CASE("test_pre_staging_cleanup_preserves_unused_mutating_results", "[model][rfdetr][onnx_lowering]") {
 auto graph = trace_unary_graph(torch::randn({2, 3}), [](const torch::Tensor& input) {
  auto out = input + 1.0;
  return out.add_(input);
 });
 auto* mutation = find_first_node_kind(graph->block(), kAtenAddInplace);
 REQUIRE(mutation != nullptr);
 mutation->output()->replaceAllUsesWith(mutation->input(0));
 REQUIRE_FALSE(mutation->output()->hasUses());
 lower_test_graph(graph);
 REQUIRE(graph->outputs()[0]->node()->kind() == kOnnxAdd);
 // Both the ordinary producer and the mutating add remain necessary.
 std::size_t additions = 0;
 for (const auto* node : graph->nodes())
  if (node->kind() == kOnnxAdd) ++additions;
 REQUIRE(additions == 2);
}
TEST_CASE("ONNX lowering orders ready nodes by original ordinal in each block", "[model][rfdetr][onnx_lowering]") {
 auto graph = std::make_shared<torch::jit::Graph>();
 auto* input = graph->addInput();
 const auto populate = [&](torch::jit::Block* block) {
  auto* first = graph->create(kOnnxAdd, {input, input}, 1);
  auto* second = graph->create(kOnnxAdd, {input, input}, 1);
  auto* producer = graph->create(kOnnxAdd, {input, input}, 1);
  auto* independent = graph->create(kOnnxAdd, {input, input}, 1);
  first->replaceInput(0, producer->output());
  first->replaceInput(1, producer->output());
  second->replaceInput(0, first->output());
  for (auto* node : {first, second, producer, independent}) {
   block->appendNode(node);
   block->registerOutput(node->output());
  }
  return std::vector{producer, first, second, independent};
 };
 auto expected = populate(graph->block());
 auto* container = graph->create(c10::Symbol::fromQualString("onnx::If"), 1);
 graph->appendNode(container);
 graph->registerOutput(container->output());
 expected.push_back(container);
 auto* nested = container->addBlock();
 const auto nested_expected = populate(nested);
 lower_test_graph(graph);
 CHECK(std::vector<torch::jit::Node*>(graph->nodes().begin(), graph->nodes().end()) == expected);
 CHECK(std::vector<torch::jit::Node*>(nested->nodes().begin(), nested->nodes().end()) == nested_expected);
}
TEST_CASE("ONNX lowering rejects cyclic dependencies before reordering the block", "[model][rfdetr][onnx_lowering]") {
 auto graph = std::make_shared<torch::jit::Graph>();
 auto* input = graph->addInput();
 auto* first = graph->create(kOnnxAdd, {input, input}, 1);
 auto* second = graph->create(kOnnxAdd, {first->output(), input}, 1);
 graph->appendNode(first);
 graph->appendNode(second);
 first->replaceInput(0, second->output());
 graph->registerOutput(second->output());
 mmltk::testsupport::expect_runtime_error_contains([&] { lower_test_graph(graph); }, "RF-DETR ONNX export produced a cyclic or unsortable lowered graph");
 CHECK(std::vector<torch::jit::Node*>(graph->nodes().begin(), graph->nodes().end()) == std::vector{first, second});
}
TEST_CASE("Registered segmentation depthwise lowering preserves convolution values", "[model][rfdetr][onnx_lowering]") {
 const auto input = torch::sin(torch::arange(126, torch::kFloat)).reshape({1, 2, 7, 9});
 const auto weight = torch::cos(torch::arange(18, torch::kFloat)).reshape({2, 1, 3, 3});
 for (const bool biased : {false, true}) {
  const auto bias = biased ? torch::tensor({.3F, -.2F}) : torch::Tensor{};
  const auto operation = [&](const torch::Tensor& value) { return mmltk::backend::models::rfdetr::segmentation_depthwise(value, weight, bias); };
  auto graph = trace_unary_graph(input, operation);
  REQUIRE(block_contains_kind(graph->block(), c10::Symbol::fromQualString("mmltk::segmentation_depthwise")));
  lower_test_graph(graph);
  const auto* convolution = find_first_node_kind(graph->block(), kOnnxConv);
  REQUIRE(convolution);
  CHECK(convolution->i(kAttrGroup) == 2);
  CHECK(torch::allclose(execute_unary_onnx(graph, input), operation(input), 1e-5, 1e-6));
 }
}
TEST_CASE("Linear and fused ReLU lowering preserve batched projections", "[model][rfdetr][onnx_lowering][cuda]") {
 namespace rf = mmltk::backend::models::rfdetr;
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 c10::cuda::CUDAGuard guard(0);
 const mmltk::backend::ml::testsupport::FullMatrixPrecision precision;
 torch::NoGradGuard no_grad;
 torch::nn::Linear layer(4, 5);
 layer->to(torch::kCUDA);
 layer->weight.set_requires_grad(false);
 layer->bias.set_requires_grad(false);
 layer->weight.copy_(torch::cos(torch::arange(20, layer->weight.options())).reshape_as(layer->weight));
 layer->bias.copy_(torch::tensor({-.2F, .3F, -.1F, .4F, -.5F}, layer->bias.options()));
 for (const bool batched : {false, true}) {
  const auto input = torch::sin(torch::arange(24, layer->weight.options())).reshape(batched ? std::vector<int64_t>{2, 3, 4} : std::vector<int64_t>{6, 4});
  for (const bool fused : {false, true}) {
   CAPTURE(batched, fused);
   const auto operation = [&](const torch::Tensor& value) { return fused ? rf::detail::linear_relu_forward(*layer, value) : rf::detail::linear_forward(*layer, value); };
   auto graph = trace_unary_graph(input, operation);
   const auto kind = c10::Symbol::fromQualString(fused ? "mmltk::linear_relu" : "aten::linear");
   assert_lowering_replaces_kinds(graph, nullptr, {kind}, {kOnnxMatMul});
   CHECK(block_contains_kind(graph->block(), c10::Symbol::fromQualString("onnx::Relu")) == fused);
   CHECK(torch::allclose(execute_unary_onnx(graph, input.cpu()), operation(input).cpu(), 1e-5, 1e-6));
  }
 }
}
TEST_CASE("SDPA lowering preserves public scaling and both math outputs", "[model][rfdetr][onnx_lowering]") {
 for (const bool math : {false, true}) {
  for (const double scale : {.125, .25, -0.25, 0.0, 1e-40, -1e-40}) {
   if (!math && scale != 0.0 && std::abs(scale) < 1e-30) continue;
   CAPTURE(math, scale);
   const auto input = (torch::sin(torch::arange(24, torch::kFloat)) * (std::abs(scale) == 1e-40 ? 1e20 : 2.0)).reshape({1, 2, 3, 4});
   const auto key = input.flip({2});
   const auto value = torch::cos(torch::arange(24, torch::kFloat)).reshape_as(input);
   const auto operation = [&](const torch::Tensor& query) {
    if (!math) return at::scaled_dot_product_attention(query, key, value, std::nullopt, 0.0, false, scale).flatten();
    const auto result = at::_scaled_dot_product_attention_math(query, key, value, std::nullopt, 0.0, false, std::nullopt, scale, false);
    return torch::cat({std::get<0>(result).flatten(), std::get<1>(result).flatten()});
   };
   auto graph = trace_unary_graph(input, operation);
   REQUIRE(block_contains_kind(graph->block(), c10::Symbol::fromQualString(math ? "aten::_scaled_dot_product_attention_math" : "aten::scaled_dot_product_attention")));
   lower_test_graph(graph);
   const auto expected = operation(input), actual = execute_unary_onnx(graph, input);
   REQUIRE(torch::isfinite(expected).all().item<bool>());
   REQUIRE(torch::equal(torch::isfinite(actual), torch::isfinite(expected)));
   CHECK(torch::allclose(actual, expected, 1e-5, 1e-6));
  }
 }
 for (const auto dtype : {torch::kFloat16, torch::kBFloat16}) {
  CAPTURE(dtype);
  const auto input = torch::sin(torch::arange(24, torch::kFloat)).reshape({1, 2, 3, 4});
  const auto operation = [&](const torch::Tensor& query) {
   const auto low = query.to(dtype);
   return std::get<0>(at::_scaled_dot_product_attention_math(low, low, low, std::nullopt, 0.0, false, std::nullopt, std::nullopt, false)).to(torch::kFloat32);
  };
  auto graph = trace_unary_graph(input, operation);
  lower_test_graph(graph);
  const auto expected = operation(input), actual = execute_unary_onnx(graph, input);
  CHECK(torch::allclose(actual, expected, 1e-3, dtype == torch::kBFloat16 ? .004 : .0005));
 }
}
TEST_CASE("Math SDPA lowering preserves safe empty rows and exceptional operands", "[model][rfdetr][onnx_lowering]") {
 for (const auto scalar : {-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
  const auto input = torch::full({1, 1, 2, 4}, scalar);
  const auto key = torch::ones_like(input);
  const auto value = torch::arange(8, torch::kFloat).reshape_as(input);
  const auto operation = [&](const torch::Tensor& query) { return std::get<0>(at::_scaled_dot_product_attention_math(query, key, value)); };
  auto graph = trace_unary_graph(input, operation);
  lower_test_graph(graph);
  const auto expected = operation(input), actual = execute_unary_onnx(graph, input);
  mmltk::backend::ml::testsupport::require_nonfinite_pattern(actual, expected);
  CHECK(torch::allclose(actual, expected, 1e-5, 1e-6, true));
 }
}
TEST_CASE("Math SDPA lowering refuses training dropout explicitly", "[model][rfdetr][onnx_lowering]") {
 const auto input = torch::rand({1, 2, 3, 4});
 auto graph = trace_unary_graph(input, [&](const torch::Tensor& value) { return std::get<0>(at::_scaled_dot_product_attention_math(value, value, value, std::nullopt, .1)); });
 mmltk::testsupport::expect_runtime_error_contains([&] { lower_test_graph(graph); }, "dropout_p must be 0");
}
TEST_CASE("GroupPose attention lowering preserves class masks and fully excluded rows", "[model][rfdetr][onnx_lowering]") {
 const auto input = torch::sin(torch::arange(24, torch::kFloat)).reshape({1, 2, 3, 4});
 const auto reversed = input.flip({2});
 const auto admitted = torch::tensor({{true, false, true}, {false, false, false}, {false, true, true}}, torch::kBool);
 const auto additive = torch::zeros({3, 3}).masked_fill(~admitted, -std::numeric_limits<float>::infinity());
 for (const bool math : {false, true}) {
  CAPTURE(math);
  const auto operation = [&](const torch::Tensor& value) {
   if (math) {
    const auto result = at::_scaled_dot_product_attention_math(value, value, reversed, additive);
    return torch::cat({std::get<0>(result).flatten(), std::get<1>(result).flatten()});
   }
   return at::scaled_dot_product_attention(value, value, reversed, admitted).flatten();
  };
  auto graph = trace_unary_graph(input, operation);
  lower_test_graph(graph);
  const auto expected = operation(input), actual = execute_unary_onnx(graph, input);
  CHECK(torch::isfinite(actual).all().item<bool>());
  CHECK(torch::allclose(actual, expected, 1e-5, 1e-6));
 }
}
TEST_CASE("PE rotary pairs and GroupPose finite refinement survive ONNX lowering", "[model][rfdetr][onnx_lowering]") {
 const auto input = torch::arange(16, torch::kFloat).reshape({1, 2, 2, 4}) / 10;
 const auto operation = [](const torch::Tensor& value) {
  auto angles = value.narrow(-1, 0, 2).repeat_interleave(2, -1);
  auto pairs = torch::stack({-value.slice(-1, 1, 4, 2), value.slice(-1, 0, 4, 2)}, -1).reshape_as(value);
  auto rotary = value * angles.cos() + pairs * angles.sin();
  return rotary + value.narrow(-1, 0, 1).expand_as(value);
 };
 auto graph = trace_unary_graph(input, operation);
 lower_test_graph(graph);
 CHECK(torch::allclose(execute_unary_onnx(graph, input), operation(input), 1e-5, 1e-6));
 const auto exceptional = torch::tensor({0.F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()});
 const auto finite = [](const torch::Tensor& value) { return torch::nan_to_num(value, 0., 0., 0.); };
 graph = trace_unary_graph(exceptional, finite);
 lower_test_graph(graph);
 CHECK(torch::equal(execute_unary_onnx(graph, exceptional), finite(exceptional)));
}
}  // namespace
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
TEST_CASE("Every official model and trained task artifact round trips through ONNX and TensorRT", "[model][rfdetr][export][upstream_oracle][cuda]") {
 namespace rf = mmltk::backend::models::rfdetr;
 namespace support = rf::testsupport;
 namespace runtime = mmltk::backend::ml::runtime;
 const auto requested = support::UpstreamOracle::requested();
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 const support::UpstreamOracle oracle(requested);
 const auto* trace_export = std::getenv("MMLTK_RFDETR_EXPORT_TRACE");
 const bool portable_reference = trace_export && std::string_view(trace_export) == "portable";
 const bool diagnostic = portable_reference || (trace_export && std::string_view(trace_export) == "1");
 const auto* source_runtime_option = std::getenv("MMLTK_RFDETR_EXPORT_SOURCE_RUNTIME");
 const bool source_runtime = source_runtime_option && std::string_view(source_runtime_option) == "1";
 const auto* backend_option = std::getenv("MMLTK_RFDETR_EXPORT_BACKEND");
 const std::string_view backend_filter = backend_option ? backend_option : "all";
 REQUIRE((backend_filter == "all" || backend_filter == "onnx" || backend_filter == "tensorrt"));
 const mmltk::backend::ml::testsupport::FullMatrixPrecision precision;
 support::MatcherExecutionFixture matcher;
 rf::ScopedRuntimeContext runtime_context(nullptr, 0, &matcher.workspace);
 const BackendTestStream stream;
 const auto command = stream.command();
 for (const auto& entry : oracle.cases()) {
  if (entry.protocol != "controlled-group2" || entry.precision != "fp32" || entry.batch != 1) continue;
  DYNAMIC_SECTION(entry.preset << (entry.training ? " trained" : " official")) {
   const mmltk::testsupport::ScopedTempDir root("catalog-export-roundtrip");
   auto model = oracle.model(entry);
   std::vector<std::string> export_tensors;
   if (!entry.training) {
    export_tensors.emplace_back("input");
    if (diagnostic) export_tensors.emplace_back("input_mask");
    if (portable_reference)
     for (const auto* prefix : {"export/", "export-cuda/"}) export_tensors.push_back(std::string(prefix) + "encoder/0");
    rf::RfdetrOutputSchema::visit([&]<rf::RfdetrOutputRole>(rf::RfdetrOutputPolicy facts) {
     if (!facts.supports(model.config().task)) return;
     export_tensors.push_back("export-math/output/main/" + std::string(facts.name));
     if (diagnostic) export_tensors.push_back("output/main/" + std::string(facts.name));
     if (portable_reference)
      for (const auto* prefix : {"export/", "export-cuda/"}) export_tensors.push_back(std::string(prefix) + "output/main/" + std::string(facts.name));
    });
   }
   const auto reference = oracle.archive(entry, 0, export_tensors);
   rf::ExportOnnxRequest request;
   request.preset_name = entry.preset;
   request.resolution = entry.resolution;
   request.output_path = root.path() / "model.onnx";
   const std::string expected_prefix = "export-math/output/main/";
   std::string native_prefix = "output/main/", input_key = "input";
   if (entry.training) {
    auto optimizer = support::unfused_recipe_optimizer(model, entry.recipe);
    const auto targets = support::oracle_targets(reference, entry, torch::kCUDA);
    const auto output = model.forward({reference.tensor("input", torch::kCUDA), reference.tensor("input_mask", torch::kCUDA)}, model.config().has_masks());
    const auto criterion = support::oracle_detection(model.config());
    const auto samples = support::oracle_samples(reference, 1 + output.aux_outputs.size() + static_cast<std::size_t>(output.enc_outputs.has_value()), torch::kCUDA);
    const auto count = std::max<int64_t>(targets.all_labels.numel() * entry.groups, 1);
    const auto terms = rf::detection_loss_terms(output, targets, criterion, true, torch::tensor(static_cast<float>(count), output.main.pred_logits.options().dtype(torch::kFloat32)), samples);
    rf::weighted_detection_loss(terms, criterion, torch::kCUDA).backward();
    optimizer.optimizer.clip_grad_norm_(entry.clip_max_norm);
    optimizer.optimizer.step();
    request.weights_path = root.path() / "trained.pt";
    rf::DecodedNativeModelState state;
    state.metadata = support::model_checkpoint_metadata(model, "trained-oracle-roundtrip", request.weights_path.string());
    state.replace_entries(support::clone_normalized_model_state(model, true));
    rf::save_native_checkpoint(request.weights_path, state);
    native_prefix = "held/output/main/";
    input_key = "held/input";
   } else
    request.weights_path = oracle.official_checkpoint(entry.preset);
   std::map<std::string, torch::Tensor> export_permutations;
   const auto compare_export_reference = [&](support::OracleComparison& comparison, const std::string& role, const torch::Tensor& actual) {
    for (const auto* prefix : {"export/", "export-cuda/"}) {
     const auto expected = reference.tensor(std::string(prefix) + "output/main/" + role);
     comparison.compare(std::string(prefix) + "output/" + role, actual, expected, support::OracleComparisonPolicy::Value);
     if (const auto found = export_permutations.find(prefix); found != export_permutations.end()) {
      auto shape = expected.sizes().vec();
      std::fill(shape.begin() + 2, shape.end(), 1);
      const auto aligned = expected.gather(1, found->second.view(shape).expand_as(expected));
      comparison.compare(std::string(prefix) + "aligned/output/" + role, actual, aligned, support::OracleComparisonPolicy::Value);
     }
    }
   };
   if (diagnostic) {
    torch::NoGradGuard no_grad;
    model.eval();
    const auto input = reference.tensor(input_key, torch::kCUDA), mask = reference.tensor("input_mask", torch::kCUDA);
    std::vector<torch::Tensor> admitted_indices;
    const auto report = [&](std::string_view variant, const rf::ModelOutputs& output, const std::vector<torch::Tensor>& indices) {
     const auto tensors = support::oracle_output_tensors(output);
     const auto name = "export-native-" + entry.id + (variant.empty() ? "" : "-" + std::string(variant));
     support::OracleComparison comparison(requested, name, torch::kFloat32, "gated isolation of native evaluation and the exporter's existing execution policies");
     comparison.observe_values([&] {
      std::optional<torch::Tensor> native_alignment;
      comparison.fact("encoder groups", {{"actual", indices.size()}, {"production", admitted_indices.size()}});
      for (std::size_t index = 0; index < std::min(indices.size(), admitted_indices.size()); ++index)
       comparison.compare("encoder/" + std::to_string(index), indices[index], admitted_indices[index], support::OracleComparisonPolicy::Exact);
      if (indices.size() == 1 && admitted_indices.size() == 1) {
       const auto [actual_sorted, actual_order] = indices[0].detach().cpu().sort(-1);
       const auto [expected_sorted, expected_order] = admitted_indices[0].detach().cpu().sort(-1);
       comparison.compare("encoder identity set", actual_sorted, expected_sorted, support::OracleComparisonPolicy::Exact);
       if (torch::equal(actual_sorted, expected_sorted)) native_alignment = actual_order.gather(-1, expected_order.argsort(false, -1, false));
      }
      if (portable_reference) {
       REQUIRE(indices.size() == 1);
       const auto actual = indices[0].detach().cpu();
       const auto [actual_sorted, actual_order] = actual.sort(-1);
       for (const auto* prefix : {"export/", "export-cuda/"}) {
        const auto expected = reference.tensor(std::string(prefix) + "encoder/0");
        const auto [expected_sorted, expected_order] = expected.sort(-1);
        comparison.compare(std::string(prefix) + "encoder/0", actual, expected, support::OracleComparisonPolicy::Exact);
        comparison.compare(std::string(prefix) + "encoder identities", actual_sorted, expected_sorted, support::OracleComparisonPolicy::Exact);
        if (torch::equal(actual_sorted, expected_sorted))
         export_permutations[prefix] = expected_order.gather(-1, actual_order.argsort(false, -1, false));
        else
         export_permutations.erase(prefix);
       }
      }
      rf::RfdetrOutputSchema::visit([&]<rf::RfdetrOutputRole>(rf::RfdetrOutputPolicy facts) {
       if (!facts.supports(model.config().task)) return;
       const std::string role(facts.name);
       comparison.compare("output/" + role, tensors.at("main/" + role), reference.tensor(native_prefix + role), support::OracleComparisonPolicy::Value);
       comparison.compare("export-math/output/" + role, tensors.at("main/" + role), reference.tensor(expected_prefix + role), support::OracleComparisonPolicy::Value);
       if (native_alignment) {
        const auto& actual = tensors.at("main/" + role);
        auto shape = actual.sizes().vec();
        std::fill(shape.begin() + 2, shape.end(), 1);
        const auto aligned = actual.gather(1, native_alignment->to(actual.device()).view(shape).expand_as(actual));
        comparison.compare("native-aligned/output/" + role, aligned, reference.tensor(native_prefix + role), support::OracleComparisonPolicy::Value);
       }
       if (portable_reference) compare_export_reference(comparison, role, tensors.at("main/" + role));
      });
     });
    };
    const auto admitted = model.forward_observed({input, mask}, model.config().has_masks(), admitted_indices);
    report({}, admitted, admitted_indices);
    for (const auto& [name, math, deformable] : {std::tuple{"math", true, false}, std::tuple{"deformable", false, true}, std::tuple{"export-policy", true, true}}) {
     auto snapshot = model.make_inference_clone(entry.batch, rf::CompilationMode::kNone);
     snapshot->set_force_math_attention(math);
     snapshot->set_force_pytorch_deformable_attn(deformable);
     std::vector<torch::Tensor> indices;
     const auto projected = snapshot->forward_observed({input, mask}, model.config().has_masks(), indices);
     report(name, projected, indices);
    }
   }
   // Both official and trained paths re-admit a persisted artifact through
   // the production exporter, including named class/joint contract metadata.
   rf::export_onnx(request);
   const auto metadata = rf::load_onnx_model_info(request.output_path);
   REQUIRE(metadata.output_contract);
   CHECK(metadata.output_contract->task == model.config().task);
   CHECK(metadata.output_contract->keypoints_per_class == model.config().keypoints_per_class);
   REQUIRE(metadata.class_layout);
   CHECK(rf::ResolvedClassLayout(*metadata.class_layout).catalog()->ordered_equal(*model.class_layout()->catalog()));
   const auto engine_path = root.path() / "model.engine";
   if (backend_filter != "onnx") {
    runtime::TensorRtEngineOptions engine_options{};
    engine_options.allow_fp16 = false;
    engine_options.allow_tf32 = false;
    runtime::TensorRtEngine engine(request.output_path, engine_options);
    REQUIRE_FALSE(engine.cancelled());
    engine.Save(engine_path);
   }
   std::optional<std::filesystem::path> source_onnx, source_engine;
   if (source_runtime) {
    REQUIRE(entry.upstream_export.has_value());
    source_onnx = oracle.export_onnx(entry);
    if (backend_filter != "onnx") {
     source_engine = root.path() / "upstream.engine";
     runtime::TensorRtEngineOptions options{};
     options.allow_fp16 = false;
     options.allow_tf32 = false;
     runtime::TensorRtEngine engine(*source_onnx, options);
     REQUIRE_FALSE(engine.cancelled());
     engine.Save(*source_engine);
    }
   }
   const auto input = reference.tensor(input_key, torch::kCUDA).contiguous();
   std::exception_ptr difference;
   for (const auto kind : {runtime::RuntimeBackendKind::Onnx, runtime::RuntimeBackendKind::TensorRt}) {
    if ((backend_filter == "onnx" && kind != runtime::RuntimeBackendKind::Onnx) || (backend_filter == "tensorrt" && kind != runtime::RuntimeBackendKind::TensorRt)) continue;
    runtime::RuntimeBackendOptions backend_options{};
    backend_options.kind = kind;
    backend_options.model_path = kind == runtime::RuntimeBackendKind::Onnx ? request.output_path : engine_path;
    backend_options.device = 0;
    backend_options.command_stream = command;
    backend_options.allow_fp16 = false;
    backend_options.allow_tf32 = false;
    auto backend = runtime::make_runtime_backend(backend_options);
    const auto& info = backend->model_info();
    REQUIRE(info.output_count == rf::RfdetrOutputSchema::count(model.config().task));
    std::vector<torch::Tensor> values;
    std::vector<runtime::RuntimeTensorBuffer> outputs;
    for (std::size_t index = 0; index < info.output_count; ++index) {
     const auto& name = info.outputs[index].name;
     REQUIRE(rf::RfdetrOutputSchema::canonical_role(name) != rf::RfdetrOutputRole::Unspecified);
     const auto expected = reference.tensor(expected_prefix + name);
     values.push_back(torch::empty(expected.sizes(), input.options()));
     outputs.push_back(runtime_buffer(values.back()));
    }
    support::OracleComparison comparison(
     requested, "export-" + entry.id + "-" + std::to_string(static_cast<int>(kind)), torch::kFloat32, "persisted artifact runtime against the pinned upstream prepared CUDA math-export graph");
    std::shared_ptr<runtime::RuntimeBackend> source_backend;
    std::map<std::string, torch::Tensor> source_values;
    std::vector<runtime::RuntimeTensorBuffer> source_outputs;
    if (source_runtime) {
     auto options = backend_options;
     options.model_path = kind == runtime::RuntimeBackendKind::Onnx ? *source_onnx : *source_engine;
     source_backend = runtime::make_runtime_backend(options);
     const auto& source_info = source_backend->model_info();
     REQUIRE(source_info.output_count == info.output_count);
     for (std::size_t index = 0; index < source_info.output_count; ++index) {
      const auto& descriptor = source_info.outputs[index];
      const auto role = std::ranges::find_if(entry.upstream_export->outputs, [&](const auto& item) { return item.second == descriptor.name; });
      REQUIRE(role != entry.upstream_export->outputs.end());
      REQUIRE(descriptor.element_type == runtime::RuntimeElementType::Float32);
      const auto expected = reference.tensor(expected_prefix + role->first);
      auto [value, inserted] = source_values.emplace(role->first, torch::empty(expected.sizes(), input.options()));
      REQUIRE(inserted);
      source_outputs.push_back(runtime_buffer(value->second));
     }
    }
    try {
     comparison.compare_all([&] {
      for (int pass = 0; pass < 2; ++pass) {
       auto receipt = backend->Run(runtime_buffer(input), outputs, {}, {}, {});
       backend->ReleaseAfterCompletion(std::move(receipt));
       REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
       if (source_backend) {
        auto source_receipt = source_backend->Run(runtime_buffer(input), source_outputs, {}, {}, {});
        source_backend->ReleaseAfterCompletion(std::move(source_receipt));
        REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
       }
       for (std::size_t index = 0; index < info.output_count; ++index) {
        comparison.compare("output/" + info.outputs[index].name, values[index], reference.tensor(expected_prefix + info.outputs[index].name), support::OracleComparisonPolicy::Value);
        if (portable_reference) comparison.observe_values([&] { compare_export_reference(comparison, info.outputs[index].name, values[index]); });
        if (source_backend)
         comparison.observe_values([&] {
          const auto& role = info.outputs[index].name;
          comparison.compare("source-runtime/output/" + role, values[index], source_values.at(role), support::OracleComparisonPolicy::Value);
          comparison.compare("source-runtime/math-reference/" + role, source_values.at(role), reference.tensor(expected_prefix + role), support::OracleComparisonPolicy::Value);
         });
       }
      }
     });
    } catch (const std::runtime_error&) {
     if (!difference) difference = std::current_exception();
    }
    if (source_backend) {
     comparison.fact("source-runtime/close begin", {});
     const auto closed = source_backend->Close();
     comparison.fact("source-runtime/close returned", closed);
     REQUIRE(closed == runtime::kRuntimeSuccess);
    }
    if (diagnostic) comparison.fact("runtime/close begin", {});
    const auto closed = backend->Close();
    if (diagnostic) comparison.fact("runtime/close returned", closed);
    REQUIRE(closed == runtime::kRuntimeSuccess);
   }
   if (difference) std::rethrow_exception(difference);
  }
 }
}
#endif
TEST_CASE("scratch task outputs export to executable ONNX and TensorRT without base sources", "[model][rfdetr][export][encoder_pretraining][cuda]") {
 namespace rf = mmltk::backend::models::rfdetr;
 namespace runtime = mmltk::backend::ml::runtime;
 const auto evidence = rf::testsupport::encoder_training_evidence();
 if (!evidence) SKIP("real scratch export requires explicit MMLTK_RFDETR_ENCODER_PRETRAINING_DIR");
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 for (const auto* preset : {"rf-detr-atto", "rf-detr-seg-nano", "rf-detr-keypoint-preview"}) DYNAMIC_SECTION(preset) {
   const mmltk::testsupport::ScopedTempDir root("scratch-export");
   const auto training = rf::testsupport::encoder_training_request(root.path(), *evidence, preset);
   const auto trained = rf::run_training(training);
   REQUIRE(trained.selected);
   std::filesystem::remove(training.encoder_weights_path);
   REQUIRE_FALSE(std::filesystem::exists(training.encoder_weights_path));
   rf::ExportOnnxRequest request;
   request.weights_path = trained.selected->artifact.path;
   request.preset_name = preset;
   request.resolution = trained.artifacts.config.resolution;
   request.output_path = root.path() / "model.onnx";
   rf::export_onnx(request);
   const auto metadata = rf::load_onnx_model_info(request.output_path);
   REQUIRE(metadata.output_contract);
   CHECK(metadata.output_contract->task == trained.artifacts.config.task);
   CHECK(metadata.output_contract->keypoints_per_class == trained.artifacts.config.keypoints_per_class);
   CHECK(metadata.class_layout == trained.artifacts.class_layout);
   const auto engine_path = root.path() / "model.engine";
   {
    runtime::TensorRtEngineOptions options{};
    options.allow_fp16 = options.allow_tf32 = false;
    runtime::TensorRtEngine engine(request.output_path, options);
    REQUIRE_FALSE(engine.cancelled());
    engine.Save(engine_path);
   }
   const BackendTestStream stream;
   const auto command = stream.command();
   const auto input = torch::zeros({1, 3, request.resolution, request.resolution}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA));
   for (const auto kind : {runtime::RuntimeBackendKind::Onnx, runtime::RuntimeBackendKind::TensorRt}) {
    runtime::RuntimeBackendOptions options{};
    options.kind = kind;
    options.model_path = kind == runtime::RuntimeBackendKind::Onnx ? request.output_path : engine_path;
    options.device = 0;
    options.command_stream = command;
    options.allow_fp16 = options.allow_tf32 = false;
    // CLEANUP-OFF: Existing backend factory and borrowed descriptor APIs serve distinct output-verification cases.
    auto backend = runtime::make_runtime_backend(options);
    const auto& info = backend->model_info();
    // CLEANUP-ON
    REQUIRE(info.output_count == rf::RfdetrOutputSchema::count(trained.artifacts.config.task));
    std::vector<torch::Tensor> values;
    std::vector<runtime::RuntimeTensorBuffer> outputs;
    for (std::size_t index = 0; index < info.output_count; ++index) {
     const auto& descriptor = info.outputs[index];
     REQUIRE(descriptor.element_type == runtime::RuntimeElementType::Float32);
     std::vector<int64_t> shape(descriptor.shape.extents.begin(), descriptor.shape.extents.begin() + descriptor.shape.rank);
     REQUIRE(std::ranges::all_of(shape, [](auto dimension) { return dimension > 0; }));
     values.push_back(torch::empty(shape, input.options()));
     outputs.push_back(runtime_buffer(values.back()));
    }
    auto receipt = backend->Run(runtime_buffer(input), outputs, {}, {}, {});
    backend->ReleaseAfterCompletion(std::move(receipt));
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    for (const auto& output : values) CHECK(torch::isfinite(output).all().item<bool>());
    CHECK(backend->Close() == runtime::kRuntimeSuccess);
   }
  }
}
