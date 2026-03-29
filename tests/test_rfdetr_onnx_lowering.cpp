#include "rfdetr/onnx_lowering.h"
#include "rfdetr/onnx_simplify.h"

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

#include <cassert>
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

const c10::Symbol kAtenTo = c10::Symbol::fromQualString("aten::to");
const c10::Symbol kAtenTypeAs = c10::Symbol::fromQualString("aten::type_as");
const c10::Symbol kAtenConvolution = c10::Symbol::fromQualString("aten::_convolution");
const c10::Symbol kAtenFlatten = c10::Symbol::fromQualString("aten::flatten");
const c10::Symbol kAtenT = c10::Symbol::fromQualString("aten::t");
const c10::Symbol kAtenTranspose = c10::Symbol::fromQualString("aten::transpose");
const c10::Symbol kAtenView = c10::Symbol::fromQualString("aten::view");
const c10::Symbol kAtenCat = c10::Symbol::fromQualString("aten::cat");
const c10::Symbol kAtenStack = c10::Symbol::fromQualString("aten::stack");
const c10::Symbol kAtenRepeat = c10::Symbol::fromQualString("aten::repeat");
const c10::Symbol kAtenSlice = c10::Symbol::fromQualString("aten::slice");
const c10::Symbol kAtenSelect = c10::Symbol::fromQualString("aten::select");
const c10::Symbol kAtenNarrow = c10::Symbol::fromQualString("aten::narrow");
const c10::Symbol kAtenDetach = c10::Symbol::fromQualString("aten::detach");
const c10::Symbol kAtenAlias = c10::Symbol::fromQualString("aten::alias");
const c10::Symbol kAtenDropout = c10::Symbol::fromQualString("aten::dropout");
const c10::Symbol kAtenLayerNorm = c10::Symbol::fromQualString("aten::layer_norm");
const c10::Symbol kAtenAddInplace = c10::Symbol::fromQualString("aten::add_");
const c10::Symbol kAtenArange = c10::Symbol::fromQualString("aten::arange");
const c10::Symbol kAtenSoftmax = c10::Symbol::fromQualString("aten::softmax");
const c10::Symbol kAtenTopk = c10::Symbol::fromQualString("aten::topk");
const c10::Symbol kAtenOnesLike = c10::Symbol::fromQualString("aten::ones_like");
const c10::Symbol kAtenEinsum = c10::Symbol::fromQualString("aten::einsum");
const c10::Symbol kAtenSplit = c10::Symbol::fromQualString("aten::split");
const c10::Symbol kAtenUnbind = c10::Symbol::fromQualString("aten::unbind");
const c10::Symbol kAtenAdd = c10::Symbol::fromQualString("aten::add");
const c10::Symbol kAtenBitwiseAnd = c10::Symbol::fromQualString("aten::bitwise_and");
const c10::Symbol kPrimGetAttr = c10::prim::GetAttr;
const c10::Symbol kOnnxAdd = c10::Symbol::fromQualString("onnx::Add");
const c10::Symbol kOnnxAnd = c10::Symbol::fromQualString("onnx::And");
const c10::Symbol kOnnxCast = c10::Symbol::fromQualString("onnx::Cast");
const c10::Symbol kOnnxConv = c10::Symbol::fromQualString("onnx::Conv");
const c10::Symbol kOnnxLayerNormalization = c10::Symbol::fromQualString("onnx::LayerNormalization");
const c10::Symbol kOnnxRange = c10::Symbol::fromQualString("onnx::Range");
const c10::Symbol kOnnxMatMul = c10::Symbol::fromQualString("onnx::MatMul");
const c10::Symbol kOnnxReshape = c10::Symbol::fromQualString("onnx::Reshape");
const c10::Symbol kOnnxTranspose = c10::Symbol::fromQualString("onnx::Transpose");
const c10::Symbol kOnnxConcat = c10::Symbol::fromQualString("onnx::Concat");
const c10::Symbol kOnnxTile = c10::Symbol::fromQualString("onnx::Tile");
const c10::Symbol kOnnxSlice = c10::Symbol::fromQualString("onnx::Slice");
const c10::Symbol kOnnxSoftmax = c10::Symbol::fromQualString("onnx::Softmax");
const c10::Symbol kOnnxTopK = c10::Symbol::fromQualString("onnx::TopK");
const c10::Symbol kOnnxConstantOfShape = c10::Symbol::fromQualString("onnx::ConstantOfShape");
const c10::Symbol kOnnxSplit = c10::Symbol::fromQualString("onnx::Split");
const c10::Symbol kAttrTo = c10::Symbol::attr("to");
const c10::Symbol kAttrKernelShape = c10::Symbol::attr("kernel_shape");
const c10::Symbol kAttrStrides = c10::Symbol::attr("strides");
const c10::Symbol kAttrPads = c10::Symbol::attr("pads");
const c10::Symbol kAttrDilations = c10::Symbol::attr("dilations");
const c10::Symbol kAttrGroup = c10::Symbol::attr("group");
const c10::Symbol kAttrAxis = c10::Symbol::attr("axis");

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

std::pair<std::shared_ptr<torch::jit::Graph>, fastloader::rfdetr::OnnxInitializerMap>
trace_unary_graph_with_bias_attr(const torch::Tensor& input, const torch::Tensor& bias) {
    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.TestOnnxLoweringWithBias", cu, true);
    torch::jit::Module module(cu, cls);
    module.register_parameter("bias", bias, false);

    auto trace_res = torch::jit::tracer::trace(
        {input},
        [&](torch::jit::Stack args) -> torch::jit::Stack {
            return {args[0].toTensor() + module.attr("bias").toTensor()};
        },
        [](const torch::autograd::Variable&) { return ""; },
        false,
        false,
        &module);

    fastloader::rfdetr::OnnxInitializerMap initializers;
    initializers.emplace("bias", bias);
    return {trace_res.first->graph, std::move(initializers)};
}

std::shared_ptr<ONNX_NAMESPACE::ModelProto> export_onnx_model(
    const std::shared_ptr<torch::jit::Graph>& graph,
    const std::map<std::string, at::Tensor>& initializers = {}) {
    std::unordered_map<std::string, std::unordered_map<int64_t, std::string>> dynamic_axes;
    auto [model_proto, raw_data, sym_dim_map, use_external_data_format, node_names] =
        torch::jit::export_onnx(
            graph,
            initializers,
            fastloader::rfdetr::kSupportedOnnxExportOpsetVersion,
            dynamic_axes,
            false,
            ::torch::onnx::OperatorExportTypes::ONNX,
            true,
            false,
            {},
            true,
            false,
            "test.onnx");
    return model_proto;
}

void erase_unused_module_self_input(const std::shared_ptr<torch::jit::Graph>& graph) {
    if (!graph || graph->inputs().empty()) {
        return;
    }
    auto* self_input = graph->inputs().front();
    const auto class_type = self_input->type()->cast<c10::ClassType>();
    if (!class_type) {
        return;
    }
    if (self_input->hasUses()) {
        throw std::runtime_error(
            "native ONNX export left the module self input live after lowering: " +
            self_input->debugName());
    }
    graph->eraseInput(0);
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
    fastloader::rfdetr::validate_supported_onnx_export_opset(19);

    bool threw = false;
    try {
        fastloader::rfdetr::validate_supported_onnx_export_opset(17);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find("supports opset 19") != std::string::npos);
    }
    assert(threw);
}

void test_dtype_mapping() {
    using fastloader::rfdetr::onnx_tensor_data_type;
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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTo));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == fastloader::rfdetr::onnx_tensor_data_type(torch::kBool));
}

void test_lower_type_as_emits_onnx_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            return input.type_as(torch::ones({1}, torch::TensorOptions().dtype(torch::kFloat16)));
        });
    assert(block_contains_kind(graph->block(), kAtenTypeAs));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTypeAs));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == fastloader::rfdetr::onnx_tensor_data_type(torch::kFloat16));
}

void test_lower_noop_to_removes_redundant_cast() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3}),
        [](const torch::Tensor& input) {
            return input.to(torch::kFloat32);
        });
    assert(block_contains_kind(graph->block(), kAtenTo));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenTo));
    auto* cast = find_first_node_kind(graph->block(), kOnnxCast);
    assert(cast != nullptr);
    assert(cast->i(kAttrTo) == fastloader::rfdetr::onnx_tensor_data_type(torch::kFloat32));
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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenDropout));
}

void test_lower_detach_removes_detach_node() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return input.detach();
        });
    assert(block_contains_kind(graph->block(), kAtenDetach));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenDetach));
}

void test_lower_alias_removes_alias_node() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            return at::alias(input);
        });
    assert(block_contains_kind(graph->block(), kAtenAlias));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenAlias));
}

void test_lower_layer_norm_emits_onnx_layer_normalization() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 8}),
        [](const torch::Tensor& input) {
            auto weight = torch::randn({8}, input.options());
            auto bias = torch::randn({8}, input.options());
            return torch::layer_norm(input, {8}, weight, bias, 1.0e-5, false);
        });
    assert(block_contains_kind(graph->block(), kAtenLayerNorm));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenOnesLike));
    assert(block_contains_kind(graph->block(), kOnnxConstantOfShape));
}

void test_lower_einsum_emits_onnx_einsum() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4, 5}),
        [](const torch::Tensor& input) {
            auto query = torch::randn({2, 7, 3}, input.options());
            return at::einsum("bchw,bnc->bnhw", {input, query});
        });
    assert(block_contains_kind(graph->block(), kAtenEinsum));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

    assert(!block_contains_kind(graph->block(), kAtenUnbind));
    assert(block_contains_kind(graph->block(), kOnnxSplit));
}

void test_lower_add_with_bias_attr_emits_onnx_add() {
    auto bias = torch::randn({1, 3, 1, 1});
    auto [graph, initializers] = trace_unary_graph_with_bias_attr(torch::randn({1, 3, 8, 8}), bias);
    assert(block_contains_kind(graph->block(), kAtenAdd));
    assert(block_contains_kind(graph->block(), kPrimGetAttr));

    fastloader::rfdetr::lower_graph_for_onnx_export(graph, &initializers);

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

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);

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
        fastloader::rfdetr::lower_graph_for_onnx_export(graph);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find("integer bitwise_and is not supported") != std::string::npos);
    }
    assert(threw);
}

void test_simplify_checks_and_infers_exported_model() {
    auto graph = trace_unary_graph(
        torch::randn({2, 3, 4}),
        [](const torch::Tensor& input) {
            return input.transpose(1, 2).contiguous();
        });

    fastloader::rfdetr::lower_graph_for_onnx_export(graph);
    erase_unused_module_self_input(graph);
    graph->lint();
    for (auto* input : graph->inputs()) {
        assert(input->debugName() != "self");
        assert(!input->type()->cast<c10::ClassType>());
    }
    auto model_proto = export_onnx_model(graph);
    fastloader::rfdetr::run_onnx_simplify(*model_proto);

    const std::string serialized = torch::jit::serialize_model_proto_to_string(model_proto);
    assert(!serialized.empty());
}

} // namespace

int main() {
    test_supported_opset_validation();
    test_dtype_mapping();
    test_lower_to_emits_onnx_cast();
    test_lower_type_as_emits_onnx_cast();
    test_lower_noop_to_removes_redundant_cast();
    test_lower_six_input_to_emits_onnx_cast();
    test_lower_convolution_emits_onnx_conv();
    test_lower_flatten_emits_onnx_reshape();
    test_lower_t_emits_onnx_transpose();
    test_lower_transpose_emits_onnx_transpose();
    test_lower_view_emits_onnx_reshape();
    test_lower_cat_emits_onnx_concat();
    test_lower_stack_emits_concat_path();
    test_lower_repeat_emits_onnx_tile();
    test_lower_slice_and_select_emit_onnx_slice_path();
    test_lower_slice_with_negative_axis_and_step_emits_onnx_slice();
    test_lower_inference_dropout_removes_dropout_node();
    test_lower_detach_removes_detach_node();
    test_lower_alias_removes_alias_node();
    test_lower_layer_norm_emits_onnx_layer_normalization();
    test_lower_add_inplace_emits_onnx_add();
    test_lower_arange_emits_onnx_range();
    test_lower_narrow_with_static_size_arithmetic_emits_onnx_slice();
    test_lower_softmax_emits_onnx_softmax();
    test_lower_topk_emits_onnx_topk();
    test_lower_ones_like_emits_constant_of_shape();
    test_lower_einsum_emits_onnx_einsum();
    test_lower_split_list_unpack_emits_onnx_split();
    test_lower_unbind_list_unpack_emits_onnx_split();
    test_lower_add_with_bias_attr_emits_onnx_add();
    test_lower_bool_bitwise_and_emits_onnx_and();
    test_lower_integer_bitwise_and_fails_loudly();
    test_simplify_checks_and_infers_exported_model();
    return 0;
}
