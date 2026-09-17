#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "src/backend/ml/runtime/onnx_environment.h"
#include "src/common/system/numa_memory.h"
#include <ATen/TensorIndexing.h>
#include <ATen/ops/alias.h>
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
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "src/test_support/error_expectation_test_utils.hpp"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.model_export.onnx_lowering;
namespace {
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
    auto trace_res = torch::jit::tracer::trace(
        {input}, [&](torch::jit::Stack args) -> torch::jit::Stack { return {fn(args[0].toTensor())}; }, [](const torch::autograd::Variable&) { return ""; },
        false, false, &module);
    return trace_res.first->graph;
}
std::pair<std::shared_ptr<torch::jit::Graph>, OnnxInitializerMap> trace_unary_graph_with_parameters(
    const torch::Tensor& input, const std::vector<std::pair<std::string, torch::Tensor>>& parameters,
    const std::function<torch::Tensor(torch::jit::Module&, const torch::Tensor&)>& fn) {
    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLoweringWithParameters", cu, true);
    torch::jit::Module module(cu, cls);
    OnnxInitializerMap initializers;
    for (const auto& [name, value] : parameters) {
        module.register_parameter(name, value, false);
        initializers.emplace(name, value);
    }
    auto trace_res = torch::jit::tracer::trace(
        {input}, [&](torch::jit::Stack args) -> torch::jit::Stack { return {fn(module, args[0].toTensor())}; },
        [](const torch::autograd::Variable&) { return ""; }, false, false, &module);
    return {trace_res.first->graph, std::move(initializers)};
}
std::pair<std::shared_ptr<torch::jit::Graph>, OnnxInitializerMap> trace_unary_graph_with_bias_attr(const torch::Tensor& input, const torch::Tensor& bias) {
    return trace_unary_graph_with_parameters(
        input, {{"bias", bias}}, [](torch::jit::Module& module, const torch::Tensor& traced_input) { return traced_input + module.attr("bias").toTensor(); });
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
void assert_lowering_replaces_kinds(const std::shared_ptr<torch::jit::Graph>& graph, const OnnxInitializerMap* initializers,
                                    const std::initializer_list<c10::Symbol> removed_kinds, const std::initializer_list<c10::Symbol> emitted_kinds) {
    for (const c10::Symbol kind : removed_kinds) { REQUIRE((block_contains_kind(graph->block(), kind))); }
    lower_test_graph(graph, initializers);
    for (const c10::Symbol kind : removed_kinds) { REQUIRE((!block_contains_kind(graph->block(), kind))); }
    for (const c10::Symbol kind : emitted_kinds) { REQUIRE((block_contains_kind(graph->block(), kind))); }
}
std::shared_ptr<torch::jit::Graph> lower_unary_graph_and_check(const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn,
                                                               const std::initializer_list<c10::Symbol> removed_kinds,
                                                               const std::initializer_list<c10::Symbol> emitted_kinds = {}) {
    auto graph = trace_unary_graph(input, fn);
    assert_lowering_replaces_kinds(graph, nullptr, removed_kinds, emitted_kinds);
    return graph;
}
void check_lowered_cast_target(const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn, const c10::Symbol traced_kind,
                               const mmltk::backend::models::rfdetr::OnnxTensorElementType expected_dtype) {
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
    check_lowered_cast_target(
        torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kBool); }, kAtenTo,
        mmltk::backend::models::rfdetr::OnnxTensorElementType::Bool);
}
TEST_CASE("test_lower_type_as_emits_onnx_cast", "[model][rfdetr][onnx_lowering]") {
    check_lowered_cast_target(
        torch::randn({2, 3}), [](const torch::Tensor& input) { return input.type_as(torch::ones({1}, torch::TensorOptions().dtype(torch::kFloat16))); },
        kAtenTypeAs, mmltk::backend::models::rfdetr::OnnxTensorElementType::Float16);
}
TEST_CASE("test_lower_noop_to_removes_redundant_cast", "[model][rfdetr][onnx_lowering]") {
    const auto graph = lower_unary_graph_and_check(torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kFloat32); }, {kAtenTo});
    REQUIRE((!block_contains_kind(graph->block(), kOnnxCast)));
}
TEST_CASE("test_lower_six_input_to_emits_onnx_cast", "[model][rfdetr][onnx_lowering]") {
    check_lowered_cast_target(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            auto source = torch::ones({2, 3}, torch::TensorOptions().dtype(torch::kInt64).device(input.device()));
            return source.to(input.device(), input.dtype(), false, false);
        },
        kAtenTo, mmltk::backend::models::rfdetr::OnnxTensorElementType::Float32);
}
TEST_CASE("test_lower_convolution_emits_onnx_conv", "[model][rfdetr][onnx_lowering]") {
    auto weight = torch::randn({8, 3, 3, 3});
    auto bias = torch::randn({8});
    const auto graph = lower_unary_graph_and_check(torch::randn({1, 3, 16, 16}),
                                                   [weight, bias](const torch::Tensor& input) {
                                                       const std::vector<int64_t> stride{2, 2};
                                                       const std::vector<int64_t> padding{1, 1};
                                                       const std::vector<int64_t> dilation{1, 1};
                                                       return torch::conv2d(input, weight, bias, stride, padding, dilation, 1);
                                                   },
                                                   {kAtenConvolution});
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
    const auto graph =
        lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::cat({input, input}, 1); }, {kAtenCat});
    auto* concat = find_first_node_kind(graph->block(), kOnnxConcat);
    REQUIRE((concat != nullptr));
    REQUIRE((concat->i(kAttrAxis) == 1));
}
TEST_CASE("test_lower_stack_emits_concat_path", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::stack({input, input}, 1); }, {kAtenStack},
                                {kOnnxConcat});
}
TEST_CASE("test_lower_repeat_emits_onnx_tile", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({1, 3, 4}), [](const torch::Tensor& input) { return input.repeat({2, 1, 1}); }, {kAtenRepeat}, {kOnnxTile});
}
TEST_CASE("test_lower_slice_and_select_emit_onnx_slice_path", "[model][rfdetr][onnx_lowering]") {
    auto graph = trace_unary_graph(torch::randn({2, 3, 4}),
                                   [](const torch::Tensor& input) { return input.index({torch::indexing::Slice(), 0, torch::indexing::Slice()}); });
    REQUIRE((block_contains_kind(graph->block(), kAtenSlice) || block_contains_kind(graph->block(), kAtenSelect)));
    lower_test_graph(graph);
    REQUIRE((!block_contains_kind(graph->block(), kAtenSlice)));
    REQUIRE((!block_contains_kind(graph->block(), kAtenSelect)));
    REQUIRE((block_contains_kind(graph->block(), kOnnxSlice) || block_contains_kind(graph->block(), kOnnxReshape)));
    for (const auto axis : {0, -3}) {
        auto [parameter_graph, initializers] = trace_unary_graph_with_parameters(
            torch::zeros({3, 4}), {{"weight", torch::arange(24, torch::kFloat).reshape({2, 3, 4})}},
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
    lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return input.slice(-1, 0, c10::nullopt, 2); }, {kAtenSlice},
                                {kOnnxSlice});
}
TEST_CASE("test_antialiased_bicubic_resize_preserves_torch_values", "[model][rfdetr][onnx_lowering]") {
    namespace F = torch::nn::functional;
    mmltk::backend::ml::runtime::OnnxEnvironment environment(ORT_LOGGING_LEVEL_ERROR, "resize-lowering");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input = torch::sin(torch::arange(126, torch::kFloat)).reshape({1, 2, 7, 9});
    for (const auto& size : {std::vector<int64_t>{3, 5}, std::vector<int64_t>{11, 13}, std::vector<int64_t>{1, 1}}) {
        for (const bool align_corners : {false, true}) {
            CAPTURE(size, align_corners);
            const auto interpolate = [&](const torch::Tensor& value) {
                return F::interpolate(value, F::InterpolateFuncOptions().size(size).mode(torch::kBicubic).align_corners(align_corners).antialias(true));
            };
            auto graph = trace_unary_graph(input, interpolate);
            if (align_corners) {
                mmltk::testsupport::expect_runtime_error_contains([&] { lower_test_graph(graph); }, "requires align_corners=false");
                continue;
            }
            lower_test_graph(graph);
            if (graph->inputs().front()->type()->kind() == c10::TypeKind::ClassType) graph->eraseInput(0);
            graph->inputs().front()->setDebugName("image");
            graph->outputs().front()->setDebugName("resized");
            auto exported = torch::jit::export_onnx(graph, {}, 19, {}, false, ::torch::onnx::OperatorExportTypes::ONNX, true, false, {}, true, false, "");
            const auto bytes = torch::jit::serialize_model_proto_to_string(std::get<0>(exported));
            Ort::Session session(environment.get(), bytes.data(), bytes.size(), options);
            auto value = Ort::Value::CreateTensor<float>(memory, input.data_ptr<float>(), input.numel(), input.sizes().data(), input.dim());
            const char* input_name = "image";
            const char* output_name = "resized";
            auto output = session.Run(Ort::RunOptions{}, &input_name, &value, 1, &output_name, 1);
            const auto expected = interpolate(input);
            REQUIRE(output.front().GetTensorTypeAndShapeInfo().GetShape() == expected.sizes().vec());
            const auto actual = torch::from_blob(output.front().GetTensorMutableData<float>(), expected.sizes(), torch::kFloat);
            CHECK(torch::allclose(actual, expected, 1.0e-5, 1.0e-6));
        }
    }
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
                                                                   [](torch::jit::Module& module, const torch::Tensor& input) {
                                                                       return torch::layer_norm(input, {8}, module.attr("weight").toTensor(),
                                                                                                module.attr("bias").toTensor(), 1.0e-5, false);
                                                                   });
    assert_lowering_replaces_kinds(graph, &initializers, {kAtenLayerNorm}, {kOnnxLayerNormalization});
}
TEST_CASE("test_lower_add_inplace_emits_onnx_add", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}),
                                [](const torch::Tensor& input) {
                                    auto out = input + 1.0;
                                    return out.add_(input);
                                },
                                {kAtenAddInplace}, {kOnnxAdd});
}
TEST_CASE("test_lower_arange_emits_onnx_range", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}),
                                [](const torch::Tensor& input) { return at::arange(input.size(2), input.options().dtype(torch::kFloat32)); }, {kAtenArange},
                                {kOnnxRange});
}
TEST_CASE("test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 5, 4}), [](const torch::Tensor& input) { return input.narrow(1, 1, input.size(1) - 1); }, {kAtenNarrow},
                                {kOnnxSlice});
}
TEST_CASE("test_lower_softmax_emits_onnx_softmax", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.softmax(-1, c10::nullopt); }, {kAtenSoftmax},
                                {kOnnxSoftmax});
}
TEST_CASE("test_lower_topk_emits_onnx_topk", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return std::get<1>(input.topk(2, 1, true, true)); }, {kAtenTopk},
                                {kOnnxTopK});
}
TEST_CASE("test_lower_ones_like_emits_constant_of_shape", "[model][rfdetr][onnx_lowering]") {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::ones_like(input); }, {kAtenOnesLike},
                                {kOnnxConstantOfShape});
}
TEST_CASE("test_lower_einsum_emits_onnx_einsum", "[model][rfdetr][onnx_lowering]") {
    const auto query = torch::randn({2, 7, 3});
    lower_unary_graph_and_check(torch::randn({2, 3, 4, 5}), [query](const torch::Tensor& input) { return at::einsum("bchw,bnc->bnhw", {input, query}); },
                                {kAtenEinsum}, {kOnnxMatMul, kOnnxReshape});
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
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return at::bitwise_and(input > 0.0, input < 1.0); },
                                {kAtenBitwiseAnd}, {kOnnxAnd});
}
TEST_CASE("test_lower_integer_bitwise_and_fails_loudly", "[model][rfdetr][onnx_lowering]") {
    auto graph = trace_unary_graph(torch::randint(0, 8, {2, 3}, torch::TensorOptions().dtype(torch::kInt64)),
                                   [](const torch::Tensor& input) { return at::bitwise_and(input, 1); });
    REQUIRE((block_contains_kind(graph->block(), kAtenBitwiseAnd)));
    mmltk::testsupport::expect_runtime_error_contains([&graph]() { lower_test_graph(graph); }, "integer bitwise_and is not supported");
}
TEST_CASE("test_cuda_constants_stage_once_and_retain_nested_graph_readers", "[model][rfdetr][onnx_lowering]") {
    if (!torch::cuda::is_available()) { SKIP("CUDA unavailable"); }
    auto source = torch::arange(8, torch::TensorOptions().device(torch::kCUDA)).narrow(0, 2, 3);
    auto* original_storage = source.const_data_ptr();
    auto graph = std::make_shared<torch::jit::Graph>();
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
    mmltk::backend::models::rfdetr::lower_graph_for_onnx_export(
        {.graph = graph.get(), .initializer_context = &initializers, .find_initializer = &find_test_initializer, .readback = &readback});
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
}  // namespace
