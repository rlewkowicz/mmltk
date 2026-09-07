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
#include <torch/csrc/jit/serialization/export.h>
#include <torch/script.h>
#include <torch/torch.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "catch2_compat.hpp"
#include "error_expectation_test_utils.hpp"

import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.model_export.onnx_lowering;

namespace {

// Every case in this suite registers under the same tag.
#define MMLTK_ONNX_LOWERING_TEST_CASE(test_function) MMLTK_TEST_CASE("[model][rfdetr][onnx_lowering]", test_function)

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
    mmltk::backend::models::rfdetr::lower_graph_for_onnx_export({
        .graph = graph.get(),
        .initializer_context = initializers,
        .find_initializer = initializers == nullptr ? nullptr : &find_test_initializer,
    });
}

std::shared_ptr<torch::jit::Graph> trace_unary_graph(const torch::Tensor& input,
                                                     const std::function<torch::Tensor(const torch::Tensor&)>& fn) {
    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLowering", cu, true);
    torch::jit::Module module(cu, cls);

    auto trace_res = torch::jit::tracer::trace(
        {input}, [&](torch::jit::Stack args) -> torch::jit::Stack { return {fn(args[0].toTensor())}; },
        [](const torch::autograd::Variable&) { return ""; }, false, false, &module);

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

std::pair<std::shared_ptr<torch::jit::Graph>, OnnxInitializerMap> trace_unary_graph_with_bias_attr(const torch::Tensor& input,
                                                                                                   const torch::Tensor& bias) {
    return trace_unary_graph_with_parameters(input, {{"bias", bias}}, [](torch::jit::Module& module, const torch::Tensor& traced_input) {
        return traced_input + module.attr("bias").toTensor();
    });
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
                                    const std::initializer_list<c10::Symbol> removed_kinds,
                                    const std::initializer_list<c10::Symbol> emitted_kinds) {
    for (const c10::Symbol kind : removed_kinds) {
        MMLTK_ASSERT(block_contains_kind(graph->block(), kind));
    }

    lower_test_graph(graph, initializers);

    for (const c10::Symbol kind : removed_kinds) {
        MMLTK_ASSERT(!block_contains_kind(graph->block(), kind));
    }
    for (const c10::Symbol kind : emitted_kinds) {
        MMLTK_ASSERT(block_contains_kind(graph->block(), kind));
    }
}

std::shared_ptr<torch::jit::Graph> lower_unary_graph_and_check(const torch::Tensor& input,
                                                               const std::function<torch::Tensor(const torch::Tensor&)>& fn,
                                                               const std::initializer_list<c10::Symbol> removed_kinds,
                                                               const std::initializer_list<c10::Symbol> emitted_kinds = {}) {
    auto graph = trace_unary_graph(input, fn);
    assert_lowering_replaces_kinds(graph, nullptr, removed_kinds, emitted_kinds);
    return graph;
}

void check_lowered_cast_target(const torch::Tensor& input, const std::function<torch::Tensor(const torch::Tensor&)>& fn,
                               const c10::Symbol traced_kind, const mmltk::backend::models::rfdetr::OnnxTensorElementType expected_dtype) {
    const auto graph = lower_unary_graph_and_check(input, fn, {traced_kind});
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    MMLTK_ASSERT(cast != nullptr);
    MMLTK_ASSERT(cast->i(kAttrTo) == mmltk::backend::models::rfdetr::onnx_tensor_data_type(expected_dtype));
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_supported_opset_validation) {
    mmltk::backend::models::rfdetr::validate_supported_onnx_export_opset(19);
    mmltk::testsupport::expect_runtime_error_contains([]() { mmltk::backend::models::rfdetr::validate_supported_onnx_export_opset(17); },
                                                      "supports opset 19");
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_dtype_mapping) {
    using mmltk::backend::models::rfdetr::onnx_tensor_data_type;
    using mmltk::backend::models::rfdetr::OnnxTensorElementType;
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::Float16) == 10);
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::Float32) == 1);
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::Bool) == 9);
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::Int32) == 6);
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::Int64) == 7);
    MMLTK_ASSERT(onnx_tensor_data_type(OnnxTensorElementType::BFloat16) == 16);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_to_emits_onnx_cast) {
    check_lowered_cast_target(
        torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kBool); }, kAtenTo,
        mmltk::backend::models::rfdetr::OnnxTensorElementType::Bool);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_type_as_emits_onnx_cast) {
    check_lowered_cast_target(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) { return input.type_as(torch::ones({1}, torch::TensorOptions().dtype(torch::kFloat16))); },
        kAtenTypeAs, mmltk::backend::models::rfdetr::OnnxTensorElementType::Float16);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_noop_to_removes_redundant_cast) {
    const auto graph =
        lower_unary_graph_and_check(torch::randn({2, 3}), [](const torch::Tensor& input) { return input.to(torch::kFloat32); }, {kAtenTo});
    MMLTK_ASSERT(!block_contains_kind(graph->block(), kOnnxCast));
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_six_input_to_emits_onnx_cast) {
    check_lowered_cast_target(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            auto source = torch::ones({2, 3}, torch::TensorOptions().dtype(torch::kInt64).device(input.device()));
            return source.to(input.device(), input.dtype(), false, false);
        },
        kAtenTo, mmltk::backend::models::rfdetr::OnnxTensorElementType::Float32);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_convolution_emits_onnx_conv) {
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
    MMLTK_ASSERT(conv != nullptr);
    MMLTK_ASSERT(conv->is(kAttrKernelShape) == std::vector<int64_t>({3, 3}));
    MMLTK_ASSERT(conv->is(kAttrStrides) == std::vector<int64_t>({2, 2}));
    MMLTK_ASSERT(conv->is(kAttrPads) == std::vector<int64_t>({1, 1, 1, 1}));
    MMLTK_ASSERT(conv->is(kAttrDilations) == std::vector<int64_t>({1, 1}));
    MMLTK_ASSERT(conv->i(kAttrGroup) == 1);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_flatten_emits_onnx_reshape) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4, 5}), [](const torch::Tensor& input) { return input.flatten(2); }, {kAtenFlatten},
                                {kOnnxReshape});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_t_emits_onnx_transpose) {
    lower_unary_graph_and_check(torch::randn({3, 4}), [](const torch::Tensor& input) { return at::t(input); }, {kAtenT}, {kOnnxTranspose});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_transpose_emits_onnx_transpose) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.transpose(1, 2); }, {kAtenTranspose},
                                {kOnnxTranspose});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_view_emits_onnx_reshape) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.view({2, 12}); }, {kAtenView},
                                {kOnnxReshape});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_cat_emits_onnx_concat) {
    const auto graph = lower_unary_graph_and_check(torch::randn({2, 3, 4}),
                                                   [](const torch::Tensor& input) { return torch::cat({input, input}, 1); }, {kAtenCat});
    auto* concat = find_first_node_kind(graph->block(), kOnnxConcat);
    MMLTK_ASSERT(concat != nullptr);
    MMLTK_ASSERT(concat->i(kAttrAxis) == 1);
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_stack_emits_concat_path) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::stack({input, input}, 1); },
                                {kAtenStack}, {kOnnxConcat});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_repeat_emits_onnx_tile) {
    lower_unary_graph_and_check(torch::randn({1, 3, 4}), [](const torch::Tensor& input) { return input.repeat({2, 1, 1}); }, {kAtenRepeat},
                                {kOnnxTile});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_slice_and_select_emit_onnx_slice_path) {
    auto graph = trace_unary_graph(torch::randn({2, 3, 4}), [](const torch::Tensor& input) {
        return input.index({torch::indexing::Slice(), 0, torch::indexing::Slice()});
    });
    MMLTK_ASSERT(block_contains_kind(graph->block(), kAtenSlice) || block_contains_kind(graph->block(), kAtenSelect));

    lower_test_graph(graph);

    MMLTK_ASSERT(!block_contains_kind(graph->block(), kAtenSlice));
    MMLTK_ASSERT(!block_contains_kind(graph->block(), kAtenSelect));
    MMLTK_ASSERT(block_contains_kind(graph->block(), kOnnxSlice) || block_contains_kind(graph->block(), kOnnxReshape));
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_slice_with_negative_axis_and_step_emits_onnx_slice) {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return input.slice(-1, 0, c10::nullopt, 2); },
                                {kAtenSlice}, {kOnnxSlice});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_inference_dropout_removes_dropout_node) {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return torch::dropout(input, 0.1, false); },
                                {kAtenDropout});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_detach_removes_detach_node) {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return input.detach(); }, {kAtenDetach});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_alias_removes_alias_node) {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}), [](const torch::Tensor& input) { return at::alias(input); }, {kAtenAlias});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_layer_norm_emits_onnx_layer_normalization) {
    const auto weight = torch::randn({8});
    const auto bias = torch::randn({8});
    auto [graph, initializers] = trace_unary_graph_with_parameters(
        torch::randn({2, 3, 8}),
        {
            {"weight", weight},
            {"bias", bias},
        },
        [](torch::jit::Module& module, const torch::Tensor& input) {
            return torch::layer_norm(input, {8}, module.attr("weight").toTensor(), module.attr("bias").toTensor(), 1.0e-5, false);
        });
    assert_lowering_replaces_kinds(graph, &initializers, {kAtenLayerNorm}, {kOnnxLayerNormalization});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_add_inplace_emits_onnx_add) {
    lower_unary_graph_and_check(torch::randn({2, 3, 8}),
                                [](const torch::Tensor& input) {
                                    auto out = input + 1.0;
                                    return out.add_(input);
                                },
                                {kAtenAddInplace}, {kOnnxAdd});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_arange_emits_onnx_range) {
    lower_unary_graph_and_check(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) { return at::arange(input.size(2), input.options().dtype(torch::kFloat32)); }, {kAtenArange},
        {kOnnxRange});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice) {
    lower_unary_graph_and_check(torch::randn({2, 5, 4}), [](const torch::Tensor& input) { return input.narrow(1, 1, input.size(1) - 1); },
                                {kAtenNarrow}, {kOnnxSlice});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_softmax_emits_onnx_softmax) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return input.softmax(-1, c10::nullopt); },
                                {kAtenSoftmax}, {kOnnxSoftmax});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_topk_emits_onnx_topk) {
    lower_unary_graph_and_check(torch::randn({2, 4, 4}),
                                [](const torch::Tensor& input) { return std::get<1>(input.topk(2, 1, true, true)); }, {kAtenTopk},
                                {kOnnxTopK});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_ones_like_emits_constant_of_shape) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}), [](const torch::Tensor& input) { return torch::ones_like(input); },
                                {kAtenOnesLike}, {kOnnxConstantOfShape});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_einsum_emits_onnx_einsum) {
    const auto query = torch::randn({2, 7, 3});
    lower_unary_graph_and_check(torch::randn({2, 3, 4, 5}),
                                [query](const torch::Tensor& input) { return at::einsum("bchw,bnc->bnhw", {input, query}); }, {kAtenEinsum},
                                {kOnnxMatMul, kOnnxReshape});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_split_list_unpack_emits_onnx_split) {
    lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return input.split(1, 1)[0]; }, {kAtenSplit},
                                {kOnnxSplit});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_unbind_list_unpack_emits_onnx_split) {
    lower_unary_graph_and_check(torch::randn({2, 4, 4}), [](const torch::Tensor& input) { return at::unbind(input, 1)[0]; }, {kAtenUnbind},
                                {kOnnxSplit});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_add_with_bias_attr_emits_onnx_add) {
    auto bias = torch::randn({1, 3, 1, 1});
    auto [graph, initializers] = trace_unary_graph_with_bias_attr(torch::randn({1, 3, 8, 8}), bias);
    MMLTK_ASSERT(block_contains_kind(graph->block(), kPrimGetAttr));
    assert_lowering_replaces_kinds(graph, &initializers, {kAtenAdd}, {kOnnxAdd});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_bool_bitwise_and_emits_onnx_and) {
    lower_unary_graph_and_check(torch::randn({2, 3, 4}),
                                [](const torch::Tensor& input) { return at::bitwise_and(input > 0.0, input < 1.0); }, {kAtenBitwiseAnd},
                                {kOnnxAnd});
}

MMLTK_ONNX_LOWERING_TEST_CASE(test_lower_integer_bitwise_and_fails_loudly) {
    auto graph = trace_unary_graph(torch::randint(0, 8, {2, 3}, torch::TensorOptions().dtype(torch::kInt64)),
                                   [](const torch::Tensor& input) { return at::bitwise_and(input, 1); });
    MMLTK_ASSERT(block_contains_kind(graph->block(), kAtenBitwiseAnd));

    mmltk::testsupport::expect_runtime_error_contains([&graph]() { lower_test_graph(graph); }, "integer bitwise_and is not supported");
}

}  // namespace
