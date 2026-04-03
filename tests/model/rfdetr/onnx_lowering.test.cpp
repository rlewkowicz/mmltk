#include "rfdetr/onnx_lowering.h"
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

#include "support/catch2_compat.hpp"
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Avoid Torch/JIT symbol registration before main in release/LTO test bundles.
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

std::shared_ptr<torch::jit::Graph> trace_unary_graph(
    const torch::Tensor& input,
    const std::function<torch::Tensor(const torch::Tensor&)>& fn) {
    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLowering", cu, true);
    torch::jit::Module module(cu, cls);

    auto trace_res = torch::jit::tracer::trace(
        {input},
        [&](torch::jit::Stack args) -> torch::jit::Stack {
            return {fn(args[0].toTensor())};
        },
        [](const torch::autograd::Variable&) { return ""; },
        false,
        false,
        &module);

    return trace_res.first->graph;
}

std::pair<std::shared_ptr<torch::jit::Graph>, mmltk::rfdetr::OnnxInitializerMap>
trace_unary_graph_with_parameters(
    const torch::Tensor& input,
    const std::vector<std::pair<std::string, torch::Tensor>>& parameters,
    const std::function<torch::Tensor(torch::jit::Module&, const torch::Tensor&)>& fn) {
    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLoweringWithParameters", cu, true);
    torch::jit::Module module(cu, cls);
    mmltk::rfdetr::OnnxInitializerMap initializers;
    for (const auto& [name, value] : parameters) {
        module.register_parameter(name, value, false);
        initializers.emplace(name, value);
    }

    auto trace_res = torch::jit::tracer::trace(
        {input},
        [&](torch::jit::Stack args) -> torch::jit::Stack {
            return {fn(module, args[0].toTensor())};
        },
        [](const torch::autograd::Variable&) { return ""; },
        false,
        false,
        &module);

    return {trace_res.first->graph, std::move(initializers)};
}

std::pair<std::shared_ptr<torch::jit::Graph>, mmltk::rfdetr::OnnxInitializerMap>
trace_unary_graph_with_bias_attr(const torch::Tensor& input, const torch::Tensor& bias) {
    return trace_unary_graph_with_parameters(
        input,
        {{"bias", bias}},
        [](torch::jit::Module& module, const torch::Tensor& traced_input) {
            return traced_input + module.attr("bias").toTensor();
        });
}

bool block_contains_kind(const torch::jit::Block* block, c10::Symbol kind) {
    for (const auto* node : block->nodes()) {
        if (node->kind() == kind) {
            return true;
        }
        for (const auto* child : node->blocks()) {
            if (block_contains_kind(child, kind)) {
                return true;
            }
        }
    }
    return false;
}

torch::jit::Node* find_first_node_kind(torch::jit::Block* block, c10::Symbol kind) {
    for (auto* node : block->nodes()) {
        if (node->kind() == kind) {
            return node;
        }
        for (auto* child : node->blocks()) {
            if (auto* found = find_first_node_kind(child, kind)) {
                return found;
            }
        }
    }
    return nullptr;
}

void test_supported_opset_validation() {
    mmltk::rfdetr::validate_supported_onnx_export_opset(19);

    bool threw = false;
    try {
        mmltk::rfdetr::validate_supported_onnx_export_opset(17);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find("supports opset 19") != std::string::npos);
    }
    assert(threw);
}

void test_dtype_mapping() {
    using mmltk::rfdetr::onnx_tensor_data_type;
    assert(onnx_tensor_data_type(torch::kFloat16) == 10);
    assert(onnx_tensor_data_type(torch::kFloat32) == 1);
    assert(onnx_tensor_data_type(torch::kBool) == 9);
    assert(onnx_tensor_data_type(torch::kInt32) == 6);
    assert(onnx_tensor_data_type(torch::kInt64) == 7);
    assert(onnx_tensor_data_type(torch::kBFloat16) == 16);
}

void test_lower_to_emits_onnx_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            return input.to(torch::kBool);
        });
    assert(block_contains_kind(graph->block(), kAtenTo));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTo));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == mmltk::rfdetr::onnx_tensor_data_type(torch::kBool));
}

void test_lower_type_as_emits_onnx_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            return input.type_as(torch::ones({1}, torch::TensorOptions().dtype(torch::kFloat16)));
        });
    assert(block_contains_kind(graph->block(), kAtenTypeAs));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTypeAs));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == mmltk::rfdetr::onnx_tensor_data_type(torch::kFloat16));
}

void test_lower_noop_to_removes_redundant_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            return input.to(torch::kFloat32);
        });
    assert(block_contains_kind(graph->block(), kAtenTo));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTo));
    assert(!block_contains_kind(graph->block(), kOnnxCast));
}

void test_lower_six_input_to_emits_onnx_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            auto source = torch::ones(
                {2, 3},
                torch::TensorOptions().dtype(torch::kInt64).device(input.device()));
            return source.to(input.device(), input.dtype(), false, false);
        });
    assert(block_contains_kind(graph->block(), kAtenTo));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTo));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == mmltk::rfdetr::onnx_tensor_data_type(torch::kFloat32));
}

void test_lower_convolution_emits_onnx_conv() {
    auto weight = torch::randn({8, 3, 3, 3});
    auto bias = torch::randn({8});
    auto graph = trace_unary_graph(
        torch::randn({1, 3, 16, 16}),
        [weight, bias](const torch::Tensor& input) {
            const std::vector<int64_t> stride{2, 2};
            const std::vector<int64_t> padding{1, 1};
            const std::vector<int64_t> dilation{1, 1};
            return torch::conv2d(input, weight, bias, stride, padding, dilation, 1);
        });
    assert(block_contains_kind(graph->block(), kAtenConvolution));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenConvolution));
    auto* conv = find_first_node_kind(graph->block(), kOnnxConv);
    assert(conv != nullptr);
    assert(conv->is(kAttrKernelShape) == std::vector<int64_t>({3, 3}));
    assert(conv->is(kAttrStrides) == std::vector<int64_t>({2, 2}));
    assert(conv->is(kAttrPads) == std::vector<int64_t>({1, 1, 1, 1}));
    assert(conv->is(kAttrDilations) == std::vector<int64_t>({1, 1}));
    assert(conv->i(kAttrGroup) == 1);
}

void test_lower_flatten_emits_onnx_reshape() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4, 5}),
        [](const torch::Tensor& input) {
            return input.flatten(2);
        });
    assert(block_contains_kind(graph->block(), kAtenFlatten));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenFlatten));
    auto* reshape = find_first_node_kind(graph->block(), kOnnxReshape);
    assert(reshape != nullptr);
}

void test_lower_t_emits_onnx_transpose() {
    auto graph = trace_unary_graph(
        torch::randn({3, 4}),
        [](const torch::Tensor& input) {
            return at::t(input);
        });
    assert(block_contains_kind(graph->block(), kAtenT));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenT));
    assert(block_contains_kind(graph->block(), kOnnxTranspose));
}

void test_lower_transpose_emits_onnx_transpose() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return input.transpose(1, 2);
        });
    assert(block_contains_kind(graph->block(), kAtenTranspose));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTranspose));
    assert(block_contains_kind(graph->block(), kOnnxTranspose));
}

void test_lower_view_emits_onnx_reshape() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return input.view({2, 12});
        });
    assert(block_contains_kind(graph->block(), kAtenView));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenView));
    assert(block_contains_kind(graph->block(), kOnnxReshape));
}

void test_lower_cat_emits_onnx_concat() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return torch::cat({input, input}, 1);
        });
    assert(block_contains_kind(graph->block(), kAtenCat));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenCat));
    auto* concat = find_first_node_kind(graph->block(), kOnnxConcat);
    assert(concat != nullptr);
    assert(concat->i(kAttrAxis) == 1);
}

void test_lower_stack_emits_concat_path() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return torch::stack({input, input}, 1);
        });
    assert(block_contains_kind(graph->block(), kAtenStack));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenStack));
    assert(block_contains_kind(graph->block(), kOnnxConcat));
}

void test_lower_repeat_emits_onnx_tile() {
    auto graph = trace_unary_graph(
        torch::randn({1, 3, 4}),
        [](const torch::Tensor& input) {
            return input.repeat({2, 1, 1});
        });
    assert(block_contains_kind(graph->block(), kAtenRepeat));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenRepeat));
    assert(block_contains_kind(graph->block(), kOnnxTile));
}

void test_lower_slice_and_select_emit_onnx_slice_path() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return input.index({torch::indexing::Slice(), 0, torch::indexing::Slice()});
        });
    assert(block_contains_kind(graph->block(), kAtenSlice) || block_contains_kind(graph->block(), kAtenSelect));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenSlice));
    assert(!block_contains_kind(graph->block(), kAtenSelect));
    assert(block_contains_kind(graph->block(), kOnnxSlice) || block_contains_kind(graph->block(), kOnnxReshape));
}

void test_lower_slice_with_negative_axis_and_step_emits_onnx_slice() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return input.slice(-1, 0, c10::nullopt, 2);
        });
    assert(block_contains_kind(graph->block(), kAtenSlice));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenSlice));
    assert(block_contains_kind(graph->block(), kOnnxSlice));
}

void test_lower_inference_dropout_removes_dropout_node() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return torch::dropout(input, 0.1, false);
        });
    assert(block_contains_kind(graph->block(), kAtenDropout));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenDropout));
}

void test_lower_detach_removes_detach_node() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return input.detach();
        });
    assert(block_contains_kind(graph->block(), kAtenDetach));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenDetach));
}

void test_lower_alias_removes_alias_node() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return at::alias(input);
        });
    assert(block_contains_kind(graph->block(), kAtenAlias));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenAlias));
}

void test_lower_layer_norm_emits_onnx_layer_normalization() {
    const auto weight = torch::randn({8});
    const auto bias = torch::randn({8});
    auto [graph, initializers] = trace_unary_graph_with_parameters(
        torch::randn({2, 3, 8}),
        {
            {"weight", weight},
            {"bias", bias},
        },
        [](torch::jit::Module& module, const torch::Tensor& input) {
            return torch::layer_norm(input,
                                     {8},
                                     module.attr("weight").toTensor(),
                                     module.attr("bias").toTensor(),
                                     1.0e-5,
                                     false);
        });
    assert(block_contains_kind(graph->block(), kAtenLayerNorm));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph, &initializers);

    assert(!block_contains_kind(graph->block(), kAtenLayerNorm));
    assert(block_contains_kind(graph->block(), kOnnxLayerNormalization));
}

void test_lower_add_inplace_emits_onnx_add() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            auto out = input + 1.0;
            return out.add_(input);
        });
    assert(block_contains_kind(graph->block(), kAtenAddInplace));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenAddInplace));
    assert(block_contains_kind(graph->block(), kOnnxAdd));
}

void test_lower_arange_emits_onnx_range() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return at::arange(input.size(2), input.options().dtype(torch::kFloat32));
        });
    assert(block_contains_kind(graph->block(), kAtenArange));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenArange));
    assert(block_contains_kind(graph->block(), kOnnxRange));
}

void test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice() {
    auto graph = trace_unary_graph(
        torch::randn({2, 5, 4}),
        [](const torch::Tensor& input) {
            return input.narrow(1, 1, input.size(1) - 1);
        });
    assert(block_contains_kind(graph->block(), kAtenNarrow));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenNarrow));
    assert(block_contains_kind(graph->block(), kOnnxSlice));
}

void test_lower_softmax_emits_onnx_softmax() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return input.softmax(-1, c10::nullopt);
        });
    assert(block_contains_kind(graph->block(), kAtenSoftmax));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenSoftmax));
    assert(block_contains_kind(graph->block(), kOnnxSoftmax));
}

void test_lower_topk_emits_onnx_topk() {
    auto graph = trace_unary_graph(
        torch::randn({2, 4, 4}),
        [](const torch::Tensor& input) {
            return std::get<1>(input.topk(2, 1, true, true));
        });
    assert(block_contains_kind(graph->block(), kAtenTopk));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTopk));
    assert(block_contains_kind(graph->block(), kOnnxTopK));
}

void test_lower_ones_like_emits_constant_of_shape() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return torch::ones_like(input);
        });
    assert(block_contains_kind(graph->block(), kAtenOnesLike));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenOnesLike));
    assert(block_contains_kind(graph->block(), kOnnxConstantOfShape));
}

void test_lower_einsum_emits_onnx_einsum() {
    const auto query = torch::randn({2, 7, 3});
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4, 5}),
        [query](const torch::Tensor& input) {
            return at::einsum("bchw,bnc->bnhw", {input, query});
        });
    assert(block_contains_kind(graph->block(), kAtenEinsum));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenEinsum));
    assert(block_contains_kind(graph->block(), kOnnxMatMul));
    assert(block_contains_kind(graph->block(), kOnnxReshape));
}

void test_lower_split_list_unpack_emits_onnx_split() {
    auto graph = trace_unary_graph(
        torch::randn({2, 4, 4}),
        [](const torch::Tensor& input) {
            return input.split(1, 1)[0];
        });
    assert(block_contains_kind(graph->block(), kAtenSplit));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenSplit));
    assert(block_contains_kind(graph->block(), kOnnxSplit));
}

void test_lower_unbind_list_unpack_emits_onnx_split() {
    auto graph = trace_unary_graph(
        torch::randn({2, 4, 4}),
        [](const torch::Tensor& input) {
            return at::unbind(input, 1)[0];
        });
    assert(block_contains_kind(graph->block(), kAtenUnbind));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenUnbind));
    assert(block_contains_kind(graph->block(), kOnnxSplit));
}

void test_lower_add_with_bias_attr_emits_onnx_add() {
    auto bias = torch::randn({1, 3, 1, 1});
    auto [graph, initializers] = trace_unary_graph_with_bias_attr(torch::randn({1, 3, 8, 8}), bias);
    assert(block_contains_kind(graph->block(), kAtenAdd));
    assert(block_contains_kind(graph->block(), kPrimGetAttr));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph, &initializers);

    assert(!block_contains_kind(graph->block(), kAtenAdd));
    assert(block_contains_kind(graph->block(), kOnnxAdd));
}

void test_lower_bool_bitwise_and_emits_onnx_and() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return at::bitwise_and(input > 0.0, input < 1.0);
        });
    assert(block_contains_kind(graph->block(), kAtenBitwiseAnd));

    mmltk::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenBitwiseAnd));
    assert(block_contains_kind(graph->block(), kOnnxAnd));
}

void test_lower_integer_bitwise_and_fails_loudly() {
    auto graph = trace_unary_graph(
        torch::randint(0, 8, {2, 3}, torch::TensorOptions().dtype(torch::kInt64)),
        [](const torch::Tensor& input) {
            return at::bitwise_and(input, 1);
        });
    assert(block_contains_kind(graph->block(), kAtenBitwiseAnd));

    bool threw = false;
    try {
        mmltk::rfdetr::lower_graph_for_onnx_export(graph);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find("integer bitwise_and is not supported") != std::string::npos);
    }
    assert(threw);
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_supported_opset_validation);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_dtype_mapping);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_to_emits_onnx_cast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_type_as_emits_onnx_cast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_noop_to_removes_redundant_cast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_six_input_to_emits_onnx_cast);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_convolution_emits_onnx_conv);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_flatten_emits_onnx_reshape);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_t_emits_onnx_transpose);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_transpose_emits_onnx_transpose);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_view_emits_onnx_reshape);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_cat_emits_onnx_concat);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_stack_emits_concat_path);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_repeat_emits_onnx_tile);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_slice_and_select_emit_onnx_slice_path);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_slice_with_negative_axis_and_step_emits_onnx_slice);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_inference_dropout_removes_dropout_node);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_detach_removes_detach_node);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_alias_removes_alias_node);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_layer_norm_emits_onnx_layer_normalization);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_add_inplace_emits_onnx_add);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_arange_emits_onnx_range);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_softmax_emits_onnx_softmax);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_topk_emits_onnx_topk);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_ones_like_emits_constant_of_shape);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_einsum_emits_onnx_einsum);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_split_list_unpack_emits_onnx_split);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_unbind_list_unpack_emits_onnx_split);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_add_with_bias_attr_emits_onnx_add);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_bool_bitwise_and_emits_onnx_and);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_lowering]", test_lower_integer_bitwise_and_fails_loudly);
