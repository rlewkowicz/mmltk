#include "rfdetr/onnx_lowering.h"

#include <ATen/core/jit_type.h>
#include <ATen/ops/scalar_tensor.h>
#include <ATen/ops/tensor.h>
#include <c10/core/SymInt.h>
#include <torch/csrc/jit/ir/constants.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mmltk::rfdetr {

namespace {

struct LoweringContext {
    const std::shared_ptr<torch::jit::Graph>& graph;
    const OnnxInitializerMap* initializers = nullptr;
};

thread_local const LoweringContext* g_current_lowering_context = nullptr;

bool has_prefix(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

class LoweringContextScope {
public:
    explicit LoweringContextScope(const LoweringContext& context)
        : previous_(g_current_lowering_context) {
        g_current_lowering_context = &context;
    }

    ~LoweringContextScope() {
        g_current_lowering_context = previous_;
    }

private:
    const LoweringContext* previous_ = nullptr;
};

constexpr int kOnnxTensorProtoFloat = 1;
constexpr int kOnnxTensorProtoUint8 = 2;
constexpr int kOnnxTensorProtoInt8 = 3;
constexpr int kOnnxTensorProtoInt16 = 5;
constexpr int kOnnxTensorProtoInt32 = 6;
constexpr int kOnnxTensorProtoInt64 = 7;
constexpr int kOnnxTensorProtoBool = 9;
constexpr int kOnnxTensorProtoFloat16 = 10;
constexpr int kOnnxTensorProtoDouble = 11;
constexpr int kOnnxTensorProtoBfloat16 = 16;

const c10::Symbol kAtenTo = c10::Symbol::fromQualString("aten::to");
const c10::Symbol kAtenTypeAs = c10::Symbol::fromQualString("aten::type_as");
const c10::Symbol kAtenConvolution = c10::Symbol::fromQualString("aten::_convolution");
const c10::Symbol kAtenFlatten = c10::Symbol::fromQualString("aten::flatten");
const c10::Symbol kAtenT = c10::Symbol::fromQualString("aten::t");
const c10::Symbol kAtenTranspose = c10::Symbol::fromQualString("aten::transpose");
const c10::Symbol kAtenPermute = c10::Symbol::fromQualString("aten::permute");
const c10::Symbol kAtenView = c10::Symbol::fromQualString("aten::view");
const c10::Symbol kAtenReshape = c10::Symbol::fromQualString("aten::reshape");
const c10::Symbol kAtenUnsqueeze = c10::Symbol::fromQualString("aten::unsqueeze");
const c10::Symbol kAtenSqueeze = c10::Symbol::fromQualString("aten::squeeze");
const c10::Symbol kAtenContiguous = c10::Symbol::fromQualString("aten::contiguous");
const c10::Symbol kAtenAlias = c10::Symbol::fromQualString("aten::alias");
const c10::Symbol kAtenCat = c10::Symbol::fromQualString("aten::cat");
const c10::Symbol kAtenStack = c10::Symbol::fromQualString("aten::stack");
const c10::Symbol kAtenExpand = c10::Symbol::fromQualString("aten::expand");
const c10::Symbol kAtenRepeat = c10::Symbol::fromQualString("aten::repeat");
const c10::Symbol kAtenSoftmax = c10::Symbol::fromQualString("aten::softmax");
const c10::Symbol kAtenSigmoid = c10::Symbol::fromQualString("aten::sigmoid");
const c10::Symbol kAtenRelu = c10::Symbol::fromQualString("aten::relu");
const c10::Symbol kAtenTanh = c10::Symbol::fromQualString("aten::tanh");
const c10::Symbol kAtenSin = c10::Symbol::fromQualString("aten::sin");
const c10::Symbol kAtenCos = c10::Symbol::fromQualString("aten::cos");
const c10::Symbol kAtenExp = c10::Symbol::fromQualString("aten::exp");
const c10::Symbol kAtenLog = c10::Symbol::fromQualString("aten::log");
const c10::Symbol kAtenAbs = c10::Symbol::fromQualString("aten::abs");
const c10::Symbol kAtenFloor = c10::Symbol::fromQualString("aten::floor");
const c10::Symbol kAtenGelu = c10::Symbol::fromQualString("aten::gelu");
const c10::Symbol kAtenClampMin = c10::Symbol::fromQualString("aten::clamp_min");
const c10::Symbol kAtenClamp = c10::Symbol::fromQualString("aten::clamp");
const c10::Symbol kAtenDropout = c10::Symbol::fromQualString("aten::dropout");
const c10::Symbol kAtenLayerNorm = c10::Symbol::fromQualString("aten::layer_norm");
const c10::Symbol kAtenNativeLayerNorm = c10::Symbol::fromQualString("aten::native_layer_norm");
const c10::Symbol kAtenSize = c10::Symbol::fromQualString("aten::size");
const c10::Symbol kAtenDetach = c10::Symbol::fromQualString("aten::detach");
const c10::Symbol kAtenInt = c10::Symbol::fromQualString("aten::Int");
const c10::Symbol kAtenFloat = c10::Symbol::fromQualString("aten::Float");
const c10::Symbol kAtenScalarImplicit = c10::Symbol::fromQualString("aten::ScalarImplicit");
const c10::Symbol kAtenNeg = c10::Symbol::fromQualString("aten::neg");
const c10::Symbol kAtenAdd = c10::Symbol::fromQualString("aten::add");
const c10::Symbol kAtenAddInplace = c10::Symbol::fromQualString("aten::add_");
const c10::Symbol kAtenSub = c10::Symbol::fromQualString("aten::sub");
const c10::Symbol kAtenRsub = c10::Symbol::fromQualString("aten::rsub");
const c10::Symbol kAtenMul = c10::Symbol::fromQualString("aten::mul");
const c10::Symbol kAtenDiv = c10::Symbol::fromQualString("aten::div");
const c10::Symbol kAtenPow = c10::Symbol::fromQualString("aten::pow");
const c10::Symbol kAtenGt = c10::Symbol::fromQualString("aten::gt");
const c10::Symbol kAtenLt = c10::Symbol::fromQualString("aten::lt");
const c10::Symbol kAtenGe = c10::Symbol::fromQualString("aten::ge");
const c10::Symbol kAtenLe = c10::Symbol::fromQualString("aten::le");
const c10::Symbol kAtenEq = c10::Symbol::fromQualString("aten::eq");
const c10::Symbol kAtenNe = c10::Symbol::fromQualString("aten::ne");
const c10::Symbol kAtenAnd = c10::Symbol::fromQualString("aten::__and__");
const c10::Symbol kAtenBitwiseAnd = c10::Symbol::fromQualString("aten::bitwise_and");
const c10::Symbol kAtenBitwiseAndInplace = c10::Symbol::fromQualString("aten::bitwise_and_");
const c10::Symbol kAtenLogicalNot = c10::Symbol::fromQualString("aten::logical_not");
const c10::Symbol kAtenBitwiseNot = c10::Symbol::fromQualString("aten::bitwise_not");
const c10::Symbol kAtenSlice = c10::Symbol::fromQualString("aten::slice");
const c10::Symbol kAtenSelect = c10::Symbol::fromQualString("aten::select");
const c10::Symbol kAtenNarrow = c10::Symbol::fromQualString("aten::narrow");
const c10::Symbol kAtenGather = c10::Symbol::fromQualString("aten::gather");
const c10::Symbol kAtenIndexSelect = c10::Symbol::fromQualString("aten::index_select");
const c10::Symbol kAtenMaskedFill = c10::Symbol::fromQualString("aten::masked_fill");
const c10::Symbol kAtenArange = c10::Symbol::fromQualString("aten::arange");
const c10::Symbol kAtenSum = c10::Symbol::fromQualString("aten::sum");
const c10::Symbol kAtenMean = c10::Symbol::fromQualString("aten::mean");
const c10::Symbol kAtenProd = c10::Symbol::fromQualString("aten::prod");
const c10::Symbol kAtenAll = c10::Symbol::fromQualString("aten::all");
const c10::Symbol kAtenCumsum = c10::Symbol::fromQualString("aten::cumsum");
const c10::Symbol kAtenArgmax = c10::Symbol::fromQualString("aten::argmax");
const c10::Symbol kAtenTopk = c10::Symbol::fromQualString("aten::topk");
const c10::Symbol kAtenMax = c10::Symbol::fromQualString("aten::max");
const c10::Symbol kAtenMatmul = c10::Symbol::fromQualString("aten::matmul");
const c10::Symbol kAtenBmm = c10::Symbol::fromQualString("aten::bmm");
const c10::Symbol kAtenEinsum = c10::Symbol::fromQualString("aten::einsum");
const c10::Symbol kAtenSplit = c10::Symbol::fromQualString("aten::split");
const c10::Symbol kAtenChunk = c10::Symbol::fromQualString("aten::chunk");
const c10::Symbol kAtenUnbind = c10::Symbol::fromQualString("aten::unbind");
const c10::Symbol kAtenOnesLike = c10::Symbol::fromQualString("aten::ones_like");
const c10::Symbol kAtenZerosLike = c10::Symbol::fromQualString("aten::zeros_like");
const c10::Symbol kAtenFullLike = c10::Symbol::fromQualString("aten::full_like");
const c10::Symbol kAtenZeros = c10::Symbol::fromQualString("aten::zeros");
const c10::Symbol kAtenOnes = c10::Symbol::fromQualString("aten::ones");
const c10::Symbol kAtenFull = c10::Symbol::fromQualString("aten::full");
const c10::Symbol kAtenNewZeros = c10::Symbol::fromQualString("aten::new_zeros");
const c10::Symbol kAtenNewOnes = c10::Symbol::fromQualString("aten::new_ones");
const c10::Symbol kAtenUpsampleNearest2d = c10::Symbol::fromQualString("aten::upsample_nearest2d");
const c10::Symbol kAtenUpsampleBilinear2d = c10::Symbol::fromQualString("aten::upsample_bilinear2d");
const c10::Symbol kAtenGridSampler = c10::Symbol::fromQualString("aten::grid_sampler");
const c10::Symbol kAtenScaledDotProductAttention =
    c10::Symbol::fromQualString("aten::scaled_dot_product_attention");
const c10::Symbol kPrimNumToTensor = c10::Symbol::fromQualString("prim::NumToTensor");

const c10::Symbol kOnnxConstant = c10::Symbol::fromQualString("onnx::Constant");
const c10::Symbol kOnnxConstantOfShape = c10::Symbol::fromQualString("onnx::ConstantOfShape");
const c10::Symbol kOnnxCast = c10::Symbol::fromQualString("onnx::Cast");
const c10::Symbol kOnnxConv = c10::Symbol::fromQualString("onnx::Conv");
const c10::Symbol kOnnxAdd = c10::Symbol::fromQualString("onnx::Add");
const c10::Symbol kOnnxSub = c10::Symbol::fromQualString("onnx::Sub");
const c10::Symbol kOnnxMul = c10::Symbol::fromQualString("onnx::Mul");
const c10::Symbol kOnnxDiv = c10::Symbol::fromQualString("onnx::Div");
const c10::Symbol kOnnxPow = c10::Symbol::fromQualString("onnx::Pow");
const c10::Symbol kOnnxReshape = c10::Symbol::fromQualString("onnx::Reshape");
const c10::Symbol kOnnxTranspose = c10::Symbol::fromQualString("onnx::Transpose");
const c10::Symbol kOnnxConcat = c10::Symbol::fromQualString("onnx::Concat");
const c10::Symbol kOnnxExpand = c10::Symbol::fromQualString("onnx::Expand");
const c10::Symbol kOnnxTile = c10::Symbol::fromQualString("onnx::Tile");
const c10::Symbol kOnnxSigmoid = c10::Symbol::fromQualString("onnx::Sigmoid");
const c10::Symbol kOnnxRelu = c10::Symbol::fromQualString("onnx::Relu");
const c10::Symbol kOnnxTanh = c10::Symbol::fromQualString("onnx::Tanh");
const c10::Symbol kOnnxSin = c10::Symbol::fromQualString("onnx::Sin");
const c10::Symbol kOnnxCos = c10::Symbol::fromQualString("onnx::Cos");
const c10::Symbol kOnnxExp = c10::Symbol::fromQualString("onnx::Exp");
const c10::Symbol kOnnxLog = c10::Symbol::fromQualString("onnx::Log");
const c10::Symbol kOnnxAbs = c10::Symbol::fromQualString("onnx::Abs");
const c10::Symbol kOnnxFloor = c10::Symbol::fromQualString("onnx::Floor");
const c10::Symbol kOnnxErf = c10::Symbol::fromQualString("onnx::Erf");
const c10::Symbol kOnnxClip = c10::Symbol::fromQualString("onnx::Clip");
const c10::Symbol kOnnxSoftmax = c10::Symbol::fromQualString("onnx::Softmax");
const c10::Symbol kOnnxLayerNormalization = c10::Symbol::fromQualString("onnx::LayerNormalization");
const c10::Symbol kOnnxRange = c10::Symbol::fromQualString("onnx::Range");
const c10::Symbol kOnnxMatMul = c10::Symbol::fromQualString("onnx::MatMul");
const c10::Symbol kOnnxSlice = c10::Symbol::fromQualString("onnx::Slice");
const c10::Symbol kOnnxGather = c10::Symbol::fromQualString("onnx::Gather");
const c10::Symbol kOnnxGatherElements = c10::Symbol::fromQualString("onnx::GatherElements");
const c10::Symbol kOnnxWhere = c10::Symbol::fromQualString("onnx::Where");
const c10::Symbol kOnnxNot = c10::Symbol::fromQualString("onnx::Not");
const c10::Symbol kOnnxAnd = c10::Symbol::fromQualString("onnx::And");
const c10::Symbol kOnnxGreater = c10::Symbol::fromQualString("onnx::Greater");
const c10::Symbol kOnnxLess = c10::Symbol::fromQualString("onnx::Less");
const c10::Symbol kOnnxGreaterOrEqual = c10::Symbol::fromQualString("onnx::GreaterOrEqual");
const c10::Symbol kOnnxLessOrEqual = c10::Symbol::fromQualString("onnx::LessOrEqual");
const c10::Symbol kOnnxEqual = c10::Symbol::fromQualString("onnx::Equal");
const c10::Symbol kOnnxReduceSum = c10::Symbol::fromQualString("onnx::ReduceSum");
const c10::Symbol kOnnxReduceProd = c10::Symbol::fromQualString("onnx::ReduceProd");
const c10::Symbol kOnnxReduceMean = c10::Symbol::fromQualString("onnx::ReduceMean");
const c10::Symbol kOnnxReduceMin = c10::Symbol::fromQualString("onnx::ReduceMin");
const c10::Symbol kOnnxCumSum = c10::Symbol::fromQualString("onnx::CumSum");
const c10::Symbol kOnnxArgMax = c10::Symbol::fromQualString("onnx::ArgMax");
const c10::Symbol kOnnxTopK = c10::Symbol::fromQualString("onnx::TopK");
const c10::Symbol kOnnxSplit = c10::Symbol::fromQualString("onnx::Split");
const c10::Symbol kOnnxMax = c10::Symbol::fromQualString("onnx::Max");
const c10::Symbol kOnnxResize = c10::Symbol::fromQualString("onnx::Resize");
const c10::Symbol kOnnxGridSample = c10::Symbol::fromQualString("onnx::GridSample");

const c10::Symbol kAttrTo = c10::Symbol::attr("to");
const c10::Symbol kAttrKernelShape = c10::Symbol::attr("kernel_shape");
const c10::Symbol kAttrStrides = c10::Symbol::attr("strides");
const c10::Symbol kAttrPads = c10::Symbol::attr("pads");
const c10::Symbol kAttrDilations = c10::Symbol::attr("dilations");
const c10::Symbol kAttrGroup = c10::Symbol::attr("group");
const c10::Symbol kAttrPerm = c10::Symbol::attr("perm");
const c10::Symbol kAttrAxis = c10::Symbol::attr("axis");
const c10::Symbol kAttrAxes = c10::Symbol::attr("axes");
const c10::Symbol kAttrKeepdims = c10::Symbol::attr("keepdims");
const c10::Symbol kAttrLargest = c10::Symbol::attr("largest");
const c10::Symbol kAttrSorted = c10::Symbol::attr("sorted");
const c10::Symbol kAttrValue = c10::attr::value;
const c10::Symbol kAttrMode = c10::Symbol::attr("mode");
const c10::Symbol kAttrPaddingMode = c10::Symbol::attr("padding_mode");
const c10::Symbol kAttrCoordinateTransformationMode =
    c10::Symbol::attr("coordinate_transformation_mode");
const c10::Symbol kAttrNearestMode = c10::Symbol::attr("nearest_mode");
const c10::Symbol kAttrAlignCorners = c10::Symbol::attr("align_corners");
const c10::Symbol kAttrEpsilon = c10::Symbol::attr("epsilon");

std::string scalar_type_name(at::ScalarType scalar_type) {
    switch (scalar_type) {
    case at::kFloat:
        return "float32";
    case at::kHalf:
        return "float16";
    case at::kBFloat16:
        return "bfloat16";
    case at::kDouble:
        return "float64";
    case at::kBool:
        return "bool";
    case at::kByte:
        return "uint8";
    case at::kChar:
        return "int8";
    case at::kShort:
        return "int16";
    case at::kInt:
        return "int32";
    case at::kLong:
        return "int64";
    default:
        return "unsupported";
    }
}

[[noreturn]] void throw_lowering_error(const torch::jit::Node* node, const std::string& message) {
    throw std::runtime_error(
        std::string("RF-DETR ONNX export cannot lower ") + node->kind().toQualString() + ": " + message);
}

std::optional<at::Tensor> lookup_initializer_tensor(
    const torch::jit::Value* value,
    const LoweringContext& context);

std::optional<at::ScalarType> value_scalar_type(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto tensor_type = value->type()->cast<c10::TensorType>();
    if (!tensor_type) {
        return std::nullopt;
    }
    return tensor_type->scalarType();
}

std::optional<at::ScalarType> resolved_scalar_type(const torch::jit::Value* value) {
    if (const auto scalar_type = value_scalar_type(value)) {
        return scalar_type;
    }
    if (g_current_lowering_context != nullptr) {
        if (const auto initializer = lookup_initializer_tensor(value, *g_current_lowering_context)) {
            return initializer->scalar_type();
        }
    }
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value() && ivalue->isTensor()) {
        return ivalue->toTensor().scalar_type();
    }
    return std::nullopt;
}

bool is_tensor_value(const torch::jit::Value* value) {
    if (value == nullptr) {
        return false;
    }
    return static_cast<bool>(value->type()->cast<c10::TensorType>());
}

std::optional<size_t> value_tensor_rank(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto tensor_type = value->type()->cast<c10::TensorType>();
    if (!tensor_type) {
        return std::nullopt;
    }
    return tensor_type->dim();
}

std::optional<std::vector<int64_t>> value_tensor_sizes(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto tensor_type = value->type()->cast<c10::TensorType>();
    if (!tensor_type) {
        return std::nullopt;
    }
    return tensor_type->sizes().concrete_sizes();
}

int64_t normalize_axis(int64_t axis, size_t rank, const torch::jit::Node* node, const char* what = "axis") {
    const auto rank_i = static_cast<int64_t>(rank);
    if (axis < 0) {
        axis += rank_i;
    }
    if (axis < 0 || axis >= rank_i) {
        throw_lowering_error(node, std::string(what) + " " + std::to_string(axis) +
                                       " is out of range for rank " + std::to_string(rank_i));
    }
    return axis;
}

std::vector<int64_t> normalize_axes(
    const std::vector<int64_t>& axes,
    size_t rank,
    const torch::jit::Node* node,
    const char* what = "axes") {
    std::vector<int64_t> normalized;
    normalized.reserve(axes.size());
    for (const int64_t axis : axes) {
        normalized.push_back(normalize_axis(axis, rank, node, what));
    }
    return normalized;
}

bool is_none_value(const torch::jit::Value* value) {
    return value == nullptr || value->mustBeNone();
}

std::optional<int64_t> tensor_item_int64(const at::Tensor& tensor) {
    if (!tensor.defined() || tensor.numel() != 1) {
        return std::nullopt;
    }
    return tensor.item<int64_t>();
}

std::optional<double> tensor_item_double(const at::Tensor& tensor) {
    if (!tensor.defined() || tensor.numel() != 1) {
        return std::nullopt;
    }
    return tensor.item<double>();
}

std::optional<bool> tensor_item_bool(const at::Tensor& tensor) {
    if (!tensor.defined() || tensor.numel() != 1) {
        return std::nullopt;
    }
    return tensor.item<bool>();
}

std::optional<std::string> full_attribute_name(
    const torch::jit::Value* value,
    const std::shared_ptr<torch::jit::Graph>& graph) {
    if (value == nullptr || graph == nullptr || graph->inputs().empty()) {
        return std::nullopt;
    }

    std::vector<std::string> names;
    const torch::jit::Value* current = value;
    while (current != nullptr && current != graph->inputs().at(0)) {
        const auto* node = current->node();
        if (node->kind() != c10::prim::GetAttr || !node->hasAttributeS("name")) {
            return std::nullopt;
        }
        names.push_back(node->s(c10::attr::name));
        current = node->inputs().empty() ? nullptr : node->input(0);
    }
    if (names.empty() || current != graph->inputs().at(0)) {
        return std::nullopt;
    }

    std::string full_name;
    for (size_t index = names.size(); index-- > 0;) {
        if (!full_name.empty()) {
            full_name.push_back('.');
        }
        full_name += names[index];
    }
    return full_name;
}

std::optional<at::Tensor> lookup_initializer_tensor(
    const torch::jit::Value* value,
    const LoweringContext& context) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value() && ivalue->isTensor()) {
        return ivalue->toTensor();
    }
    if (context.initializers == nullptr) {
        return std::nullopt;
    }

    if (const auto by_debug_name = context.initializers->find(value->debugName());
        by_debug_name != context.initializers->end()) {
        return by_debug_name->second;
    }

    const auto attr_name = full_attribute_name(value, context.graph);
    if (!attr_name.has_value()) {
        return std::nullopt;
    }
    if (const auto it = context.initializers->find(*attr_name); it != context.initializers->end()) {
        return it->second;
    }

    return std::nullopt;
}

torch::jit::Node* create_onnx_constant_tensor(
    torch::jit::Graph* graph,
    const at::Tensor& value,
    torch::jit::Node* insert_before) {
    auto* constant = graph->create(kOnnxConstant, {}, 1);
    constant->t_(kAttrValue, value);
    constant->insertBefore(insert_before);
    constant->output()->inferTypeFrom(value);
    return constant;
}

at::Tensor make_int64_tensor(const std::vector<int64_t>& values) {
    return at::tensor(values, at::TensorOptions().dtype(at::kLong));
}

at::Tensor make_float_tensor(const std::vector<float>& values) {
    return at::tensor(values, at::TensorOptions().dtype(at::kFloat));
}

std::optional<int64_t> constant_int_value(const torch::jit::Value* value);
std::optional<double> constant_double_value(const torch::jit::Value* value);
std::optional<bool> constant_bool_value(const torch::jit::Value* value);

std::optional<int64_t> exact_int_from_double(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    const double rounded = std::nearbyint(value);
    if (std::fabs(value - rounded) > 1e-9) {
        return std::nullopt;
    }
    return static_cast<int64_t>(rounded);
}

at::Tensor make_scalar_tensor(double value, at::ScalarType scalar_type) {
    const auto options = at::TensorOptions().dtype(scalar_type);
    if (scalar_type == at::kBool) {
        return at::scalar_tensor(value != 0.0, options);
    }
    if (scalar_type == at::kByte || scalar_type == at::kChar ||
        scalar_type == at::kShort || scalar_type == at::kInt || scalar_type == at::kLong) {
        return at::scalar_tensor(static_cast<int64_t>(value), options);
    }
    return at::scalar_tensor(value, options);
}

std::optional<int64_t> static_size_value(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (node->kind() != kAtenSize || node->inputs().size() != 2) {
        return std::nullopt;
    }
    const auto sizes = value_tensor_sizes(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    if (!sizes.has_value() || !dim.has_value()) {
        return std::nullopt;
    }
    const int64_t axis = normalize_axis(*dim, sizes->size(), node, "size dim");
    return sizes->at(static_cast<size_t>(axis));
}

std::optional<int64_t> constant_int_value(const torch::jit::Value* value) {
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value()) {
        if (ivalue->isInt() || ivalue->isSymInt()) {
            return ivalue->toInt();
        }
        if (ivalue->isBool() || ivalue->isSymBool()) {
            return ivalue->toBool() ? 1 : 0;
        }
        if (ivalue->isTensor()) {
            return tensor_item_int64(ivalue->toTensor());
        }
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (const auto size_value = static_size_value(value)) {
        return size_value;
    }
    if ((node->kind() == kPrimNumToTensor || node->kind() == kAtenInt ||
         node->kind() == kAtenDetach || node->kind() == kAtenScalarImplicit) &&
        node->inputs().size() == 1) {
        return constant_int_value(node->input(0));
    }
    if (node->kind() == kAtenTo && !node->inputs().empty()) {
        return constant_int_value(node->input(0));
    }
    if (node->kind() == kAtenFloat && node->inputs().size() == 1) {
        if (const auto double_value = constant_double_value(node->input(0))) {
            return exact_int_from_double(*double_value);
        }
    }
    if (node->kind() == kAtenNeg && node->inputs().size() == 1) {
        if (const auto inner = constant_int_value(node->input(0))) {
            return -*inner;
        }
    }
    if (node->kind() == kAtenAdd && node->inputs().size() >= 2) {
        const auto lhs = constant_int_value(node->input(0));
        const auto rhs = constant_int_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            int64_t scaled_rhs = *rhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_int_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_rhs *= *alpha;
            }
            return *lhs + scaled_rhs;
        }
    }
    if (node->kind() == kAtenSub && node->inputs().size() >= 2) {
        const auto lhs = constant_int_value(node->input(0));
        const auto rhs = constant_int_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            int64_t scaled_rhs = *rhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_int_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_rhs *= *alpha;
            }
            return *lhs - scaled_rhs;
        }
    }
    if (node->kind() == kAtenRsub && node->inputs().size() >= 2) {
        const auto lhs = constant_int_value(node->input(0));
        const auto rhs = constant_int_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            int64_t scaled_lhs = *lhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_int_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_lhs *= *alpha;
            }
            return *rhs - scaled_lhs;
        }
    }
    if (node->kind() == kAtenMul && node->inputs().size() == 2) {
        const auto lhs = constant_int_value(node->input(0));
        const auto rhs = constant_int_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            return (*lhs) * (*rhs);
        }
    }
    if (node->kind() == kAtenDiv && node->inputs().size() >= 2 &&
        (node->inputs().size() == 2 || is_none_value(node->input(2)))) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value() && *rhs != 0.0) {
            return exact_int_from_double(*lhs / *rhs);
        }
    }
    if (node->kind() == kAtenPow && node->inputs().size() == 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            return exact_int_from_double(std::pow(*lhs, *rhs));
        }
    }
    if (node->kind() == c10::prim::Constant && node->hasAttribute(kAttrValue)) {
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::i) {
            return node->i(kAttrValue);
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
            const auto& attr_value = node->ival(kAttrValue);
            if (attr_value.isInt() || attr_value.isSymInt()) {
                return attr_value.toInt();
            }
            if (attr_value.isBool() || attr_value.isSymBool()) {
                return attr_value.toBool() ? 1 : 0;
            }
            if (attr_value.isTensor()) {
                return tensor_item_int64(attr_value.toTensor());
            }
        }
    }
    return std::nullopt;
}

std::optional<double> constant_double_value(const torch::jit::Value* value) {
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value()) {
        if (ivalue->isDouble()) {
            return ivalue->toDouble();
        }
        if (ivalue->isInt() || ivalue->isSymInt()) {
            return static_cast<double>(ivalue->toInt());
        }
        if (ivalue->isBool() || ivalue->isSymBool()) {
            return ivalue->toBool() ? 1.0 : 0.0;
        }
        if (ivalue->isTensor()) {
            return tensor_item_double(ivalue->toTensor());
        }
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (const auto size_value = static_size_value(value)) {
        return static_cast<double>(*size_value);
    }
    if ((node->kind() == kPrimNumToTensor || node->kind() == kAtenFloat ||
         node->kind() == kAtenDetach || node->kind() == kAtenScalarImplicit) &&
        node->inputs().size() == 1) {
        return constant_double_value(node->input(0));
    }
    if (node->kind() == kAtenTo && !node->inputs().empty()) {
        return constant_double_value(node->input(0));
    }
    if (node->kind() == kAtenInt && node->inputs().size() == 1) {
        if (const auto int_value = constant_int_value(node->input(0))) {
            return static_cast<double>(*int_value);
        }
    }
    if (node->kind() == kAtenNeg && node->inputs().size() == 1) {
        if (const auto inner = constant_double_value(node->input(0))) {
            return -*inner;
        }
    }
    if (node->kind() == kAtenAdd && node->inputs().size() >= 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            double scaled_rhs = *rhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_double_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_rhs *= *alpha;
            }
            return *lhs + scaled_rhs;
        }
    }
    if (node->kind() == kAtenSub && node->inputs().size() >= 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            double scaled_rhs = *rhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_double_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_rhs *= *alpha;
            }
            return *lhs - scaled_rhs;
        }
    }
    if (node->kind() == kAtenRsub && node->inputs().size() >= 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            double scaled_lhs = *lhs;
            if (node->inputs().size() == 3) {
                const auto alpha = constant_double_value(node->input(2));
                if (!alpha.has_value()) {
                    return std::nullopt;
                }
                scaled_lhs *= *alpha;
            }
            return *rhs - scaled_lhs;
        }
    }
    if (node->kind() == kAtenMul && node->inputs().size() == 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            return (*lhs) * (*rhs);
        }
    }
    if (node->kind() == kAtenDiv && node->inputs().size() >= 2 &&
        (node->inputs().size() == 2 || is_none_value(node->input(2)))) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value() && *rhs != 0.0) {
            return *lhs / *rhs;
        }
    }
    if (node->kind() == kAtenPow && node->inputs().size() == 2) {
        const auto lhs = constant_double_value(node->input(0));
        const auto rhs = constant_double_value(node->input(1));
        if (lhs.has_value() && rhs.has_value()) {
            return std::pow(*lhs, *rhs);
        }
    }
    if (node->kind() == c10::prim::Constant && node->hasAttribute(kAttrValue)) {
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::f) {
            return node->f(kAttrValue);
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::i) {
            return static_cast<double>(node->i(kAttrValue));
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
            const auto& attr_value = node->ival(kAttrValue);
            if (attr_value.isDouble()) {
                return attr_value.toDouble();
            }
            if (attr_value.isInt() || attr_value.isSymInt()) {
                return static_cast<double>(attr_value.toInt());
            }
            if (attr_value.isBool() || attr_value.isSymBool()) {
                return attr_value.toBool() ? 1.0 : 0.0;
            }
            if (attr_value.isTensor()) {
                return tensor_item_double(attr_value.toTensor());
            }
        }
    }
    return std::nullopt;
}

std::optional<bool> constant_bool_value(const torch::jit::Value* value) {
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value()) {
        if (ivalue->isBool() || ivalue->isSymBool()) {
            return ivalue->toBool();
        }
        if (ivalue->isInt() || ivalue->isSymInt()) {
            return ivalue->toInt() != 0;
        }
        if (ivalue->isTensor()) {
            return tensor_item_bool(ivalue->toTensor());
        }
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if ((node->kind() == kAtenDetach || node->kind() == kAtenScalarImplicit) && node->inputs().size() == 1) {
        return constant_bool_value(node->input(0));
    }
    if (node->kind() == kAtenTo && !node->inputs().empty()) {
        return constant_bool_value(node->input(0));
    }
    if (node->kind() == c10::prim::Constant && node->hasAttribute(kAttrValue)) {
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::i) {
            return node->i(kAttrValue) != 0;
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
            const auto& attr_value = node->ival(kAttrValue);
            if (attr_value.isBool() || attr_value.isSymBool()) {
                return attr_value.toBool();
            }
            if (attr_value.isInt() || attr_value.isSymInt()) {
                return attr_value.toInt() != 0;
            }
            if (attr_value.isTensor()) {
                return tensor_item_bool(attr_value.toTensor());
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> constant_string_value(const torch::jit::Value* value) {
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value() && ivalue->isString()) {
        return ivalue->toStringRef();
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (node->kind() == c10::prim::Constant && node->hasAttribute(kAttrValue)) {
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::s) {
            return node->s(kAttrValue);
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
            const auto& attr_value = node->ival(kAttrValue);
            if (attr_value.isString()) {
                return attr_value.toStringRef();
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<int64_t>> constant_int_list_value(const torch::jit::Value* value) {
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value()) {
        if (ivalue->isIntList()) {
            return ivalue->toIntVector();
        }
        if (ivalue->isSymIntList()) {
            std::vector<int64_t> ints;
            for (const auto& sym_int : ivalue->toSymIntVector()) {
                ints.push_back(sym_int.expect_int());
            }
            return ints;
        }
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (node->kind() == c10::prim::ListConstruct) {
        std::vector<int64_t> ints;
        ints.reserve(node->inputs().size());
        for (const auto* input : node->inputs()) {
            const auto elem = constant_int_value(input);
            if (!elem.has_value()) {
                return std::nullopt;
            }
            ints.push_back(*elem);
        }
        return ints;
    }
    if (node->kind() == c10::prim::Constant && node->hasAttribute(kAttrValue)) {
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::is) {
            return node->is(kAttrValue);
        }
        if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
            const auto& attr_value = node->ival(kAttrValue);
            if (attr_value.isIntList()) {
                return attr_value.toIntVector();
            }
            if (attr_value.isSymIntList()) {
                std::vector<int64_t> ints;
                for (const auto& sym_int : attr_value.toSymIntVector()) {
                    ints.push_back(sym_int.expect_int());
                }
                return ints;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<torch::jit::Value*>> tensor_list_inputs(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* node = value->node();
    if (node->kind() != c10::prim::ListConstruct) {
        return std::nullopt;
    }
    std::vector<torch::jit::Value*> values;
    values.reserve(node->inputs().size());
    for (auto* input : node->inputs()) {
        values.push_back(const_cast<torch::jit::Value*>(input));
    }
    return values;
}

std::optional<at::ScalarType> extract_target_scalar_type(const torch::jit::Node* node) {
    if (node->outputs().size() == 1) {
        if (const auto output_type = value_scalar_type(node->output())) {
            return output_type;
        }
    }

    if (node->kind() == kAtenTo) {
        size_t dtype_index = 0;
        if (node->inputs().size() == 5) {
            dtype_index = 1;
        } else if (node->inputs().size() == 6) {
            dtype_index = 2;
        } else {
            return std::nullopt;
        }

        const auto dtype_value = constant_int_value(node->input(dtype_index));
        if (!dtype_value.has_value()) {
            return std::nullopt;
        }
        if (*dtype_value < 0 || *dtype_value >= static_cast<int64_t>(at::ScalarType::NumOptions)) {
            return std::nullopt;
        }
        return static_cast<at::ScalarType>(*dtype_value);
    }

    if (node->kind() == kAtenTypeAs && node->inputs().size() == 2) {
        return value_scalar_type(node->input(1));
    }

    return std::nullopt;
}

torch::jit::Value* create_constant_value(
    torch::jit::Node* insert_before,
    const at::Tensor& value) {
    return create_onnx_constant_tensor(insert_before->owningGraph(), value, insert_before)->output();
}

torch::jit::Value* create_int64s_constant(
    torch::jit::Node* insert_before,
    const std::vector<int64_t>& values) {
    return create_constant_value(insert_before, make_int64_tensor(values));
}

torch::jit::Value* create_scalar_constant(
    torch::jit::Node* insert_before,
    double value,
    at::ScalarType scalar_type) {
    return create_constant_value(insert_before, make_scalar_tensor(value, scalar_type));
}

torch::jit::Value* create_output_shape_constant(torch::jit::Node* node, const torch::jit::Value* value) {
    const auto output_sizes = value_tensor_sizes(value);
    if (!output_sizes.has_value()) {
        throw_lowering_error(node, "output tensor shape must be statically known");
    }
    return create_int64s_constant(node, *output_sizes);
}

torch::jit::Value* create_onnx_cast(
    torch::jit::Node* insert_before,
    torch::jit::Value* input,
    at::ScalarType scalar_type);

torch::jit::Value* materialize_tensor_input(
    torch::jit::Node* node,
    torch::jit::Value* value,
    at::ScalarType target_scalar_type) {
    if (value == nullptr) {
        throw_lowering_error(node, "encountered a null tensor input");
    }
    if (const auto scalar_type = value_scalar_type(value)) {
        if (*scalar_type == target_scalar_type) {
            return value;
        }
        return create_onnx_cast(node, value, target_scalar_type);
    }
    if (g_current_lowering_context != nullptr) {
        if (const auto initializer = lookup_initializer_tensor(value, *g_current_lowering_context)) {
            if (initializer->scalar_type() == target_scalar_type) {
                return value;
            }
            return create_onnx_cast(node, value, target_scalar_type);
        }
    }
    if (is_tensor_value(value)) {
        return value;
    }
    if (const auto int_value = constant_int_value(value)) {
        return create_scalar_constant(node, static_cast<double>(*int_value), target_scalar_type);
    }
    if (const auto double_value = constant_double_value(value)) {
        return create_scalar_constant(node, *double_value, target_scalar_type);
    }
    if (const auto bool_value = constant_bool_value(value)) {
        return create_scalar_constant(node, *bool_value ? 1.0 : 0.0, target_scalar_type);
    }
    if ((value->node()->kind() == kPrimNumToTensor ||
         value->node()->kind() == kAtenDetach ||
         value->node()->kind() == kAtenInt ||
         value->node()->kind() == kAtenFloat) &&
        value->node()->inputs().size() == 1) {
        return materialize_tensor_input(node, value->node()->input(0), target_scalar_type);
    }
    if (value->node()->kind() == kAtenTo && !value->node()->inputs().empty()) {
        return materialize_tensor_input(node, value->node()->input(0), target_scalar_type);
    }
    std::ostringstream message;
    message << "expected a tensor or scalar constant input";
    if (value != nullptr && value->node() != nullptr) {
        message << " (input producer: " << value->node()->kind().toQualString();
        if (value->type() != nullptr) {
            message << ", type: " << value->type()->str();
        }
        message << ")";
    }
    throw_lowering_error(node, message.str());
}

torch::jit::Value* create_onnx_cast(
    torch::jit::Node* insert_before,
    torch::jit::Value* input,
    at::ScalarType scalar_type) {
    auto* cast = insert_before->owningGraph()->create(kOnnxCast, {input}, 1);
    cast->copyMetadata(insert_before);
    cast->i_(kAttrTo, onnx_tensor_data_type(scalar_type));
    cast->insertBefore(insert_before);
    return cast->output();
}

torch::jit::Value* reshape_value_to_output(
    torch::jit::Node* node,
    torch::jit::Value* input,
    torch::jit::Value* output_template) {
    auto* reshape = node->owningGraph()->create(kOnnxReshape, {input, create_output_shape_constant(node, output_template)}, 1);
    reshape->copyMetadata(node);
    reshape->insertBefore(node);
    reshape->output()->copyMetadata(output_template);
    return reshape->output();
}

torch::jit::Value* create_constant_of_shape(
    torch::jit::Node* node,
    const at::Tensor& fill_value) {
    const auto output_sizes = value_tensor_sizes(node->output());
    if (!output_sizes.has_value()) {
        throw_lowering_error(node, "output tensor shape must be statically known");
    }
    auto* constant_of_shape = node->owningGraph()->create(
        kOnnxConstantOfShape,
        {create_int64s_constant(node, *output_sizes)},
        1);
    constant_of_shape->copyMetadata(node);
    constant_of_shape->t_(kAttrValue, fill_value);
    constant_of_shape->insertBefore(node);
    constant_of_shape->output()->copyMetadata(node->output());
    return constant_of_shape->output();
}

void lower_passthrough_node(torch::jit::Node* node);

bool is_onnx_node(const torch::jit::Node* node) {
    if (node == nullptr) {
        return false;
    }
    const std::string qual_name = node->kind().toQualString();
    return has_prefix(qual_name, "onnx::");
}

bool is_trivially_removable_prim_node(const torch::jit::Node* node) {
    if (node == nullptr) {
        return false;
    }
    switch (node->kind()) {
    case c10::prim::Constant:
    case c10::prim::ListConstruct:
    case c10::prim::ListUnpack:
    case c10::prim::TupleConstruct:
    case c10::prim::TupleUnpack:
    case c10::prim::DictConstruct:
    case c10::prim::GetAttr:
    case c10::prim::NumToTensor:
        return true;
    default:
        return false;
    }
}

bool is_safe_to_erase_if_unused(const torch::jit::Node* node) {
    if (node == nullptr || !node->blocks().empty() || node->kind() == c10::prim::Return) {
        return false;
    }
    if (is_onnx_node(node) || is_trivially_removable_prim_node(node)) {
        return true;
    }
    const auto* op = node->maybeOperator();
    if (op == nullptr) {
        return false;
    }
    switch (op->aliasAnalysisKind()) {
    case c10::AliasAnalysisKind::PURE_FUNCTION:
    case c10::AliasAnalysisKind::FROM_SCHEMA:
    case c10::AliasAnalysisKind::INTERNAL_SPECIAL_CASE:
        return true;
    case c10::AliasAnalysisKind::CONSERVATIVE:
        return false;
    }
    return false;
}

void materialize_initializer_getattrs(torch::jit::Block* block, const LoweringContext& context) {
    if (block == nullptr) {
        return;
    }
    for (auto it = block->nodes().begin(); it != block->nodes().end();) {
        auto* node = *it;
        ++it;
        for (auto* child : node->blocks()) {
            materialize_initializer_getattrs(child, context);
        }
        if (node->kind() != c10::prim::GetAttr || node->outputs().size() != 1) {
            continue;
        }
        const auto initializer = lookup_initializer_tensor(node->output(), context);
        if (!initializer.has_value()) {
            continue;
        }
        auto* constant = create_onnx_constant_tensor(node->owningGraph(), *initializer, node);
        constant->copyMetadata(node);
        constant->output()->copyMetadata(node->output());
        node->output()->replaceAllUsesWith(constant->output());
        node->destroy();
    }
}

std::optional<at::Tensor> tensor_from_supported_constant_value(const torch::jit::Value* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto ivalue = torch::jit::toIValue(value);
    if (!ivalue.has_value()) {
        return std::nullopt;
    }
    if (ivalue->isTensor()) {
        return ivalue->toTensor();
    }
    if (ivalue->isBool() || ivalue->isSymBool()) {
        return make_scalar_tensor(ivalue->toBool() ? 1.0 : 0.0, at::kBool);
    }
    if (ivalue->isInt() || ivalue->isSymInt()) {
        return make_scalar_tensor(static_cast<double>(ivalue->toInt()), at::kLong);
    }
    if (ivalue->isDouble()) {
        return make_scalar_tensor(ivalue->toDouble(), at::kDouble);
    }
    if (ivalue->isIntList()) {
        return make_int64_tensor(ivalue->toIntVector());
    }
    if (ivalue->isSymIntList()) {
        std::vector<int64_t> ints;
        for (const auto& sym_int : ivalue->toSymIntVector()) {
            ints.push_back(sym_int.expect_int());
        }
        return make_int64_tensor(ints);
    }
    return std::nullopt;
}

void materialize_remaining_prim_constants(torch::jit::Block* block) {
    if (block == nullptr) {
        return;
    }
    for (auto it = block->nodes().begin(); it != block->nodes().end();) {
        auto* node = *it;
        ++it;
        for (auto* child : node->blocks()) {
            materialize_remaining_prim_constants(child);
        }
        if (node->kind() != c10::prim::Constant || node->mustBeNone() || node->outputs().size() != 1) {
            continue;
        }
        const auto tensor = tensor_from_supported_constant_value(node->output());
        if (!tensor.has_value()) {
            continue;
        }
        auto* constant = create_onnx_constant_tensor(node->owningGraph(), *tensor, node);
        constant->copyMetadata(node);
        constant->output()->copyMetadata(node->output());
        node->output()->replaceAllUsesWith(constant->output());
        node->destroy();
    }
}

void erase_trivially_dead_nodes(torch::jit::Block* block) {
    if (block == nullptr) {
        return;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = block->nodes().rbegin(); it != block->nodes().rend();) {
            auto* node = *it;
            ++it;
            for (auto* child : node->blocks()) {
                erase_trivially_dead_nodes(child);
            }
            bool has_uses = false;
            for (const auto* output : node->outputs()) {
                if (output->hasUses()) {
                    has_uses = true;
                    break;
                }
            }
            if (!has_uses && is_safe_to_erase_if_unused(node)) {
                node->destroy();
                changed = true;
            }
        }
    }
}

void topologically_sort_block(torch::jit::Block* block) {
    if (block == nullptr) {
        return;
    }

    for (auto* node : block->nodes()) {
        for (auto* child : node->blocks()) {
            topologically_sort_block(child);
        }
    }

    std::vector<torch::jit::Node*> nodes;
    std::unordered_map<torch::jit::Node*, size_t> original_index;
    size_t index = 0;
    for (auto* node : block->nodes()) {
        nodes.push_back(node);
        original_index.emplace(node, index++);
    }
    if (nodes.size() < 2) {
        return;
    }

    std::unordered_map<torch::jit::Node*, size_t> indegree;
    std::unordered_map<torch::jit::Node*, std::vector<torch::jit::Node*>> edges;
    indegree.reserve(nodes.size());
    edges.reserve(nodes.size());
    for (auto* node : nodes) {
        indegree.emplace(node, 0);
    }

    for (auto* node : nodes) {
        std::unordered_set<torch::jit::Node*> dependencies;
        for (auto* input : node->inputs()) {
            auto* producer = input->node();
            if (producer == nullptr || producer == node || producer->kind() == c10::prim::Param ||
                producer->owningBlock() != block) {
                continue;
            }
            if (!dependencies.insert(producer).second) {
                continue;
            }
            edges[producer].push_back(node);
            ++indegree[node];
        }
    }

    std::deque<torch::jit::Node*> ready;
    for (auto* node : nodes) {
        if (indegree.at(node) == 0) {
            ready.push_back(node);
        }
    }
    std::sort(
        ready.begin(),
        ready.end(),
        [&](const torch::jit::Node* lhs, const torch::jit::Node* rhs) {
            return original_index.at(const_cast<torch::jit::Node*>(lhs)) <
                   original_index.at(const_cast<torch::jit::Node*>(rhs));
        });

    std::vector<torch::jit::Node*> sorted;
    sorted.reserve(nodes.size());
    while (!ready.empty()) {
        auto* node = ready.front();
        ready.pop_front();
        sorted.push_back(node);

        const auto edge_it = edges.find(node);
        if (edge_it == edges.end()) {
            continue;
        }
        for (auto* user : edge_it->second) {
            auto& degree = indegree.at(user);
            TORCH_INTERNAL_ASSERT(degree > 0);
            --degree;
            if (degree == 0) {
                auto insert_it = std::upper_bound(
                    ready.begin(),
                    ready.end(),
                    user,
                    [&](const torch::jit::Node* lhs, const torch::jit::Node* rhs) {
                        return original_index.at(const_cast<torch::jit::Node*>(lhs)) <
                               original_index.at(const_cast<torch::jit::Node*>(rhs));
                    });
                ready.insert(insert_it, user);
            }
        }
    }

    if (sorted.size() != nodes.size()) {
        throw std::runtime_error("RF-DETR ONNX export produced a cyclic or unsortable lowered graph");
    }

    for (auto* node : sorted) {
        node->moveBefore(block->return_node());
    }
}

void collect_non_onnx_node_kinds(
    const torch::jit::Block* block,
    std::map<std::string, size_t>* counts) {
    if (block == nullptr || counts == nullptr) {
        return;
    }
    for (const auto* node : block->nodes()) {
        for (const auto* child : node->blocks()) {
            collect_non_onnx_node_kinds(child, counts);
        }
        if (node->mustBeNone() || node->kind() == c10::Symbol::onnx("LocalFunctionDef")) {
            continue;
        }
        if (node->kind().is_onnx()) {
            continue;
        }
        ++(*counts)[node->kind().toQualString()];
    }
}

void validate_graph_is_onnx_only(const std::shared_ptr<torch::jit::Graph>& graph) {
    std::map<std::string, size_t> counts;
    collect_non_onnx_node_kinds(graph->block(), &counts);
    if (counts.empty()) {
        return;
    }
    std::ostringstream message;
    message << "native ONNX lowering left non-ONNX nodes:";
    bool first = true;
    for (const auto& [kind, count] : counts) {
        message << (first ? " " : ", ") << kind << " x" << count;
        first = false;
    }
    throw std::runtime_error(message.str());
}

void lower_cast_like_node(torch::jit::Node* node) {
    const auto target_scalar_type = extract_target_scalar_type(node);
    if (!target_scalar_type.has_value()) {
        throw std::runtime_error(
            std::string("RF-DETR ONNX export requires a statically known target dtype for ") +
            node->kind().toQualString());
    }

    const auto input_scalar_type = value_scalar_type(node->input(0));
    if (input_scalar_type.has_value() && *input_scalar_type == *target_scalar_type) {
        node->output()->replaceAllUsesWith(node->input(0));
        node->destroy();
        return;
    }

    auto* cast = node->owningGraph()->create(kOnnxCast, {node->input(0)}, 1);
    cast->copyMetadata(node);
    cast->i_(kAttrTo, onnx_tensor_data_type(*target_scalar_type));
    cast->insertBefore(node);
    cast->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(cast->output());
    node->destroy();
}

void lower_convolution_node(torch::jit::Node* node, const LoweringContext& context) {
    if (node->inputs().size() < 9) {
        throw_lowering_error(node, "expected at least 9 inputs, got " + std::to_string(node->inputs().size()));
    }

    auto weight_sizes = value_tensor_sizes(node->input(1));
    if (!weight_sizes.has_value()) {
        if (const auto weight_tensor = lookup_initializer_tensor(node->input(1), context)) {
            weight_sizes = weight_tensor->sizes().vec();
        }
    }
    if (!weight_sizes.has_value() || weight_sizes->size() < 3) {
        throw_lowering_error(node, "weight tensor shape must be statically known");
    }

    const auto stride = constant_int_list_value(node->input(3));
    const auto padding = constant_int_list_value(node->input(4));
    const auto dilation = constant_int_list_value(node->input(5));
    const auto transposed = constant_bool_value(node->input(6));
    const auto output_padding = constant_int_list_value(node->input(7));
    const auto groups = constant_int_value(node->input(8));
    if (!stride.has_value() || !padding.has_value() || !dilation.has_value() ||
        !transposed.has_value() || !output_padding.has_value() || !groups.has_value()) {
        std::vector<std::string> missing;
        if (!stride.has_value()) {
            missing.emplace_back("stride");
        }
        if (!padding.has_value()) {
            missing.emplace_back("padding");
        }
        if (!dilation.has_value()) {
            missing.emplace_back("dilation");
        }
        if (!transposed.has_value()) {
            missing.emplace_back("transposed");
        }
        if (!output_padding.has_value()) {
            missing.emplace_back("output_padding");
        }
        if (!groups.has_value()) {
            missing.emplace_back("groups");
        }
        std::ostringstream message;
        message << "convolution parameters must be compile-time constants";
        if (!missing.empty()) {
            message << " (missing: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i != 0) {
                    message << ", ";
                }
                message << missing[i];
            }
            message << ")";
        }
        throw_lowering_error(node, message.str());
    }
    if (*transposed) {
        throw_lowering_error(node, "transposed convolutions are not supported by the native exporter");
    }
    for (const int64_t value : *output_padding) {
        if (value != 0) {
            throw_lowering_error(node, "non-zero output_padding requires ConvTranspose lowering");
        }
    }

    std::vector<int64_t> kernel_shape(weight_sizes->begin() + 2, weight_sizes->end());
    if (kernel_shape.empty()) {
        throw_lowering_error(node, "kernel shape must have at least one spatial dimension");
    }
    if (stride->size() != kernel_shape.size() || padding->size() != kernel_shape.size() ||
        dilation->size() != kernel_shape.size()) {
        throw_lowering_error(node, "convolution parameter rank does not match kernel rank");
    }

    std::vector<torch::jit::Value*> conv_inputs{node->input(0), node->input(1)};
    const bool has_bias = !node->input(2)->mustBeNone();
    auto bias_rank = value_tensor_rank(node->input(2));
    if (!bias_rank.has_value()) {
        if (const auto bias_tensor = lookup_initializer_tensor(node->input(2), context)) {
            bias_rank = static_cast<size_t>(bias_tensor->dim());
        }
    }
    const bool bias_is_onnx_compatible = has_bias && bias_rank.has_value() && *bias_rank == 1;
    if (bias_is_onnx_compatible) {
        conv_inputs.push_back(node->input(2));
    }

    auto* conv = node->owningGraph()->create(kOnnxConv, conv_inputs, 1);
    conv->copyMetadata(node);
    conv->is_(kAttrKernelShape, kernel_shape);
    conv->is_(kAttrStrides, *stride);
    conv->is_(kAttrDilations, *dilation);
    conv->i_(kAttrGroup, *groups);

    std::vector<int64_t> pads;
    pads.reserve(padding->size() * 2);
    pads.insert(pads.end(), padding->begin(), padding->end());
    pads.insert(pads.end(), padding->begin(), padding->end());
    conv->is_(kAttrPads, pads);
    conv->insertBefore(node);
    conv->output()->copyMetadata(node->output());

    torch::jit::Value* replacement = conv->output();
    if (has_bias && !bias_is_onnx_compatible) {
        auto* add = node->owningGraph()->create(kOnnxAdd, {conv->output(), node->input(2)}, 1);
        add->copyMetadata(node);
        add->insertAfter(conv);
        add->output()->copyMetadata(node->output());
        replacement = add->output();
    }

    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_static_reshape_node(torch::jit::Node* node) {
    if (node->outputs().size() != 1) {
        throw_lowering_error(node, "expected a single output");
    }
    auto* reshape = node->owningGraph()->create(
        kOnnxReshape,
        {node->input(0), create_output_shape_constant(node, node->output())},
        1);
    reshape->copyMetadata(node);
    reshape->insertBefore(node);
    reshape->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(reshape->output());
    node->destroy();
}

void lower_transpose_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    if (!input_rank.has_value()) {
        throw_lowering_error(node, "input tensor rank must be statically known");
    }
    const auto dim0 = constant_int_value(node->input(1));
    const auto dim1 = constant_int_value(node->input(2));
    if (!dim0.has_value() || !dim1.has_value()) {
        throw_lowering_error(node, "transpose dims must be compile-time constants");
    }

    std::vector<int64_t> perm;
    perm.reserve(*input_rank);
    for (size_t i = 0; i < *input_rank; ++i) {
        perm.push_back(static_cast<int64_t>(i));
    }
    const int64_t axis0 = normalize_axis(*dim0, *input_rank, node, "dim0");
    const int64_t axis1 = normalize_axis(*dim1, *input_rank, node, "dim1");
    std::swap(perm[static_cast<size_t>(axis0)], perm[static_cast<size_t>(axis1)]);

    auto* transpose = node->owningGraph()->create(kOnnxTranspose, {node->input(0)}, 1);
    transpose->copyMetadata(node);
    transpose->is_(kAttrPerm, perm);
    transpose->insertBefore(node);
    transpose->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(transpose->output());
    node->destroy();
}

void lower_t_node(torch::jit::Node* node) {
    if (node->inputs().size() != 1 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected t(self) -> Tensor");
    }
    auto input_rank = value_tensor_rank(node->input(0));
    if (!input_rank.has_value()) {
        if (const auto output_sizes = value_tensor_sizes(node->output())) {
            input_rank = output_sizes->size();
        }
    }
    if (!input_rank.has_value()) {
        throw_lowering_error(node, "input tensor rank must be statically known");
    }
    if (*input_rank <= 1) {
        lower_passthrough_node(node);
        return;
    }
    if (*input_rank != 2) {
        throw_lowering_error(node, "t only supports tensors with rank <= 2");
    }

    auto* transpose = node->owningGraph()->create(kOnnxTranspose, {node->input(0)}, 1);
    transpose->copyMetadata(node);
    transpose->is_(kAttrPerm, std::vector<int64_t>{1, 0});
    transpose->insertBefore(node);
    transpose->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(transpose->output());
    node->destroy();
}

void lower_permute_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2) {
        throw_lowering_error(node, "expected 2 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dims = constant_int_list_value(node->input(1));
    if (!input_rank.has_value() || !dims.has_value()) {
        throw_lowering_error(node, "permute dims and input rank must be statically known");
    }
    if (dims->size() != *input_rank) {
        throw_lowering_error(node, "permute rank does not match input rank");
    }

    auto perm = normalize_axes(*dims, *input_rank, node, "permute dim");
    auto* transpose = node->owningGraph()->create(kOnnxTranspose, {node->input(0)}, 1);
    transpose->copyMetadata(node);
    transpose->is_(kAttrPerm, perm);
    transpose->insertBefore(node);
    transpose->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(transpose->output());
    node->destroy();
}

void lower_passthrough_node(torch::jit::Node* node) {
    if (node->inputs().empty() || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected a single-input passthrough node");
    }
    node->output()->replaceAllUsesWith(node->input(0));
    node->destroy();
}

void lower_dropout_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected dropout(input, p, train) -> Tensor");
    }
    const auto train = constant_bool_value(node->input(2));
    const auto p = constant_double_value(node->input(1));
    if (!train.has_value()) {
        throw_lowering_error(node, "dropout train flag must be a compile-time constant");
    }
    if (!p.has_value()) {
        throw_lowering_error(node, "dropout probability must be a compile-time constant");
    }
    if (*train && *p != 0.0) {
        throw_lowering_error(node, "training dropout is not supported by the native exporter");
    }
    lower_passthrough_node(node);
}

torch::jit::Value* materialize_layer_norm_affine_input(
    torch::jit::Node* node,
    torch::jit::Value* value,
    const std::vector<int64_t>& normalized_shape,
    at::ScalarType scalar_type,
    double fill_value) {
    if (!is_none_value(value)) {
        return materialize_tensor_input(node, value, scalar_type);
    }
    int64_t numel = 1;
    for (const int64_t dim : normalized_shape) {
        if (dim < 0) {
            throw_lowering_error(node, "layer_norm affine shape must be statically known");
        }
        numel *= dim;
    }

    at::Tensor tensor;
    if (scalar_type == at::kBool || scalar_type == at::kByte || scalar_type == at::kChar ||
        scalar_type == at::kShort || scalar_type == at::kInt || scalar_type == at::kLong) {
        tensor = at::tensor(
            std::vector<int64_t>(static_cast<size_t>(numel), static_cast<int64_t>(fill_value)),
            at::TensorOptions().dtype(scalar_type));
    } else {
        tensor = at::tensor(
            std::vector<double>(static_cast<size_t>(numel), fill_value),
            at::TensorOptions().dtype(scalar_type));
    }
    return create_constant_value(node, tensor.reshape(normalized_shape));
}

void lower_layer_norm_node(torch::jit::Node* node) {
    if (node->inputs().size() != 6 || (node->outputs().size() != 1 && node->outputs().size() != 3)) {
        throw_lowering_error(node, "expected layer_norm/native_layer_norm inputs and outputs");
    }

    const auto normalized_shape = constant_int_list_value(node->input(1));
    if (!normalized_shape.has_value() || normalized_shape->empty()) {
        throw_lowering_error(node, "normalized_shape must be a compile-time constant list");
    }

    auto input_rank = value_tensor_rank(node->input(0));
    const auto output_sizes = value_tensor_sizes(node->output(0));
    if (!input_rank.has_value() && output_sizes.has_value()) {
        input_rank = output_sizes->size();
    }
    if (!input_rank.has_value()) {
        throw_lowering_error(node, "input rank must be statically known");
    }
    if (normalized_shape->size() > *input_rank) {
        throw_lowering_error(node, "normalized_shape rank exceeds input rank");
    }

    const auto epsilon = constant_double_value(node->input(4));
    if (!epsilon.has_value()) {
        throw_lowering_error(node, "epsilon must be a compile-time constant");
    }

    const auto scalar_type =
        value_scalar_type(node->input(0))
            .value_or(value_scalar_type(node->output(0)).value_or(at::kFloat));
    const auto axis = static_cast<int64_t>(*input_rank - normalized_shape->size());

    std::vector<torch::jit::Value*> inputs{
        node->input(0),
        materialize_layer_norm_affine_input(node, node->input(2), *normalized_shape, scalar_type, 1.0),
        materialize_layer_norm_affine_input(node, node->input(3), *normalized_shape, scalar_type, 0.0)};

    auto* layer_norm = node->owningGraph()->create(kOnnxLayerNormalization, inputs, node->outputs().size());
    layer_norm->copyMetadata(node);
    layer_norm->i_(kAttrAxis, axis);
    layer_norm->f_(kAttrEpsilon, static_cast<float>(*epsilon));
    layer_norm->insertBefore(node);

    for (size_t i = 0; i < node->outputs().size(); ++i) {
        layer_norm->output(i)->copyMetadata(node->output(i));
        node->output(i)->replaceAllUsesWith(layer_norm->output(i));
    }
    node->destroy();
}

void lower_cat_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2) {
        throw_lowering_error(node, "expected 2 inputs");
    }
    const auto tensors = tensor_list_inputs(node->input(0));
    if (!tensors.has_value() || tensors->empty()) {
        throw_lowering_error(node, "cat requires a statically known tensor list");
    }
    const auto input_rank = value_tensor_rank(tensors->front());
    const auto dim = constant_int_value(node->input(1));
    if (!input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "cat axis and input rank must be statically known");
    }

    auto* concat = node->owningGraph()->create(kOnnxConcat, *tensors, 1);
    concat->copyMetadata(node);
    concat->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    concat->insertBefore(node);
    concat->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(concat->output());
    node->destroy();
}

void lower_stack_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2) {
        throw_lowering_error(node, "expected 2 inputs");
    }
    const auto tensors = tensor_list_inputs(node->input(0));
    if (!tensors.has_value() || tensors->empty()) {
        throw_lowering_error(node, "stack requires a statically known tensor list");
    }
    const auto output_sizes = value_tensor_sizes(node->output());
    const auto output_rank = value_tensor_rank(node->output());
    const auto dim = constant_int_value(node->input(1));
    if (!output_sizes.has_value() || !output_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "stack output shape and axis must be statically known");
    }
    const int64_t axis = normalize_axis(*dim, *output_rank, node);

    std::vector<torch::jit::Value*> unsqueezed;
    unsqueezed.reserve(tensors->size());
    std::vector<int64_t> unsqueezed_shape = *output_sizes;
    unsqueezed_shape[static_cast<size_t>(axis)] = 1;
    for (auto* tensor : *tensors) {
        auto* reshape = node->owningGraph()->create(
            kOnnxReshape,
            {tensor, create_int64s_constant(node, unsqueezed_shape)},
            1);
        reshape->copyMetadata(node);
        reshape->insertBefore(node);
        unsqueezed.push_back(reshape->output());
    }

    auto* concat = node->owningGraph()->create(kOnnxConcat, unsqueezed, 1);
    concat->copyMetadata(node);
    concat->i_(kAttrAxis, axis);
    concat->insertBefore(node);
    concat->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(concat->output());
    node->destroy();
}

void lower_expand_node(torch::jit::Node* node) {
    auto* expand = node->owningGraph()->create(
        kOnnxExpand,
        {node->input(0), create_output_shape_constant(node, node->output())},
        1);
    expand->copyMetadata(node);
    expand->insertBefore(node);
    expand->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(expand->output());
    node->destroy();
}

void lower_repeat_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2) {
        throw_lowering_error(node, "expected 2 inputs");
    }
    const auto repeats = constant_int_list_value(node->input(1));
    if (!repeats.has_value()) {
        throw_lowering_error(node, "repeat factors must be compile-time constants");
    }
    const auto input_sizes = value_tensor_sizes(node->input(0));
    if (!input_sizes.has_value()) {
        throw_lowering_error(node, "repeat input shape must be statically known");
    }

    torch::jit::Value* repeated_input = node->input(0);
    const std::vector<int64_t>& normalized_repeats = *repeats;
    if (normalized_repeats.size() < input_sizes->size()) {
        throw_lowering_error(node, "repeat rank smaller than input rank is not supported");
    }
    if (normalized_repeats.size() > input_sizes->size()) {
        std::vector<int64_t> reshape_sizes(normalized_repeats.size() - input_sizes->size(), 1);
        reshape_sizes.insert(reshape_sizes.end(), input_sizes->begin(), input_sizes->end());
        auto* reshape = node->owningGraph()->create(
            kOnnxReshape,
            {node->input(0), create_int64s_constant(node, reshape_sizes)},
            1);
        reshape->copyMetadata(node);
        reshape->insertBefore(node);
        repeated_input = reshape->output();
    }

    auto* tile = node->owningGraph()->create(
        kOnnxTile,
        {repeated_input, create_int64s_constant(node, normalized_repeats)},
        1);
    tile->copyMetadata(node);
    tile->insertBefore(node);
    tile->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(tile->output());
    node->destroy();
}

void lower_softmax_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    if (!input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "softmax axis and input rank must be statically known");
    }
    auto* softmax = node->owningGraph()->create(kOnnxSoftmax, {node->input(0)}, 1);
    softmax->copyMetadata(node);
    softmax->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    softmax->insertBefore(node);

    torch::jit::Value* replacement = softmax->output();
    const auto output_scalar_type = value_scalar_type(node->output());
    const auto input_scalar_type = value_scalar_type(node->input(0));
    if (output_scalar_type.has_value() && input_scalar_type.has_value() &&
        *output_scalar_type != *input_scalar_type) {
        replacement = create_onnx_cast(node, softmax->output(), *output_scalar_type);
    }
    replacement->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_unary_node(torch::jit::Node* node, c10::Symbol onnx_kind) {
    auto* unary = node->owningGraph()->create(onnx_kind, {node->input(0)}, 1);
    unary->copyMetadata(node);
    unary->insertBefore(node);
    unary->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(unary->output());
    node->destroy();
}

void lower_gelu_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2) {
        throw_lowering_error(node, "expected 2 inputs");
    }
    const auto approximate = constant_string_value(node->input(1));
    if (!approximate.has_value() || *approximate != "none") {
        throw_lowering_error(node, "only approximate='none' is supported");
    }
    const auto scalar_type = value_scalar_type(node->input(0)).value_or(at::kFloat);
    auto* div = node->owningGraph()->create(
        kOnnxDiv,
        {node->input(0), create_scalar_constant(node, std::numbers::sqrt2, scalar_type)},
        1);
    div->copyMetadata(node);
    div->insertBefore(node);

    auto* erf = node->owningGraph()->create(kOnnxErf, {div->output()}, 1);
    erf->copyMetadata(node);
    erf->insertAfter(div);

    auto* add = node->owningGraph()->create(
        kOnnxAdd,
        {erf->output(), create_scalar_constant(node, 1.0, scalar_type)},
        1);
    add->copyMetadata(node);
    add->insertAfter(erf);

    auto* half_mul = node->owningGraph()->create(
        kOnnxMul,
        {node->input(0), create_scalar_constant(node, 0.5, scalar_type)},
        1);
    half_mul->copyMetadata(node);
    half_mul->insertAfter(add);

    auto* output = node->owningGraph()->create(kOnnxMul, {half_mul->output(), add->output()}, 1);
    output->copyMetadata(node);
    output->insertAfter(half_mul);
    output->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(output->output());
    node->destroy();
}

void lower_clamp_min_node(torch::jit::Node* node) {
    const auto scalar_type = value_scalar_type(node->input(0)).value_or(at::kFloat);
    auto* max = node->owningGraph()->create(
        kOnnxMax,
        {node->input(0), materialize_tensor_input(node, node->input(1), scalar_type)},
        1);
    max->copyMetadata(node);
    max->insertBefore(node);
    max->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(max->output());
    node->destroy();
}

void lower_clamp_node(torch::jit::Node* node) {
    const auto scalar_type = value_scalar_type(node->input(0)).value_or(at::kFloat);
    std::vector<torch::jit::Value*> inputs{node->input(0)};
    if (!is_none_value(node->input(1))) {
        inputs.push_back(materialize_tensor_input(node, node->input(1), scalar_type));
    }
    if (!is_none_value(node->input(2))) {
        if (inputs.size() == 1) {
            inputs.push_back(create_constant_value(
                node,
                at::tensor(std::vector<float>{}, at::TensorOptions().dtype(at::kFloat))));
        }
        inputs.push_back(materialize_tensor_input(node, node->input(2), scalar_type));
    }
    auto* clip = node->owningGraph()->create(kOnnxClip, inputs, 1);
    clip->copyMetadata(node);
    clip->insertBefore(node);
    clip->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(clip->output());
    node->destroy();
}

void lower_binary_arithmetic_node(torch::jit::Node* node, c10::Symbol onnx_kind) {
    const auto preferred_scalar_type =
        value_scalar_type(node->input(0))
            .value_or(value_scalar_type(node->output()).value_or(at::kFloat));
    torch::jit::Value* lhs = materialize_tensor_input(node, node->input(0), preferred_scalar_type);
    torch::jit::Value* rhs = materialize_tensor_input(node, node->input(1), preferred_scalar_type);

    if ((node->kind() == kAtenAdd || node->kind() == kAtenSub) &&
        node->inputs().size() == 3) {
        const auto alpha = constant_double_value(node->input(2));
        if (!alpha.has_value()) {
            throw_lowering_error(node, "alpha must be a compile-time constant");
        }
        if (*alpha != 1.0) {
            auto* scaled_rhs = node->owningGraph()->create(
                kOnnxMul,
                {rhs, create_scalar_constant(node, *alpha, preferred_scalar_type)},
                1);
            scaled_rhs->copyMetadata(node);
            scaled_rhs->insertBefore(node);
            rhs = scaled_rhs->output();
        }
    } else if (node->kind() == kAtenRsub && node->inputs().size() == 3) {
        const auto alpha = constant_double_value(node->input(2));
        if (!alpha.has_value()) {
            throw_lowering_error(node, "alpha must be a compile-time constant");
        }
        if (*alpha != 1.0) {
            auto* scaled_lhs = node->owningGraph()->create(
                kOnnxMul,
                {lhs, create_scalar_constant(node, *alpha, preferred_scalar_type)},
                1);
            scaled_lhs->copyMetadata(node);
            scaled_lhs->insertBefore(node);
            lhs = scaled_lhs->output();
        }
    } else if (node->kind() == kAtenDiv && node->inputs().size() == 3) {
        if (!is_none_value(node->input(2))) {
            throw_lowering_error(node, "rounding_mode is not supported");
        }
    }

    if (node->kind() == kAtenRsub) {
        std::swap(lhs, rhs);
    }

    auto* binary = node->owningGraph()->create(onnx_kind, {lhs, rhs}, 1);
    binary->copyMetadata(node);
    binary->insertBefore(node);
    binary->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(binary->output());
    node->destroy();
}

void lower_inplace_add_node(torch::jit::Node* node) {
    if (node->inputs().size() < 2 || node->inputs().size() > 3 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected add_(self, other[, alpha]) -> Tensor");
    }
    const auto preferred_scalar_type =
        value_scalar_type(node->input(0))
            .value_or(value_scalar_type(node->output()).value_or(at::kFloat));
    torch::jit::Value* lhs = materialize_tensor_input(node, node->input(0), preferred_scalar_type);
    torch::jit::Value* rhs = materialize_tensor_input(node, node->input(1), preferred_scalar_type);

    if (node->inputs().size() == 3) {
        const auto alpha = constant_double_value(node->input(2));
        if (!alpha.has_value()) {
            throw_lowering_error(node, "alpha must be a compile-time constant");
        }
        if (*alpha != 1.0) {
            auto* scaled_rhs = node->owningGraph()->create(
                kOnnxMul,
                {rhs, create_scalar_constant(node, *alpha, preferred_scalar_type)},
                1);
            scaled_rhs->copyMetadata(node);
            scaled_rhs->insertBefore(node);
            rhs = scaled_rhs->output();
        }
    }

    auto* add = node->owningGraph()->create(kOnnxAdd, {lhs, rhs}, 1);
    add->copyMetadata(node);
    add->insertBefore(node);
    add->output()->copyMetadata(node->output());

    node->input(0)->replaceAllUsesAfterNodeWith(node, add->output());
    node->output()->replaceAllUsesWith(add->output());
    node->destroy();
}

void lower_arange_node(torch::jit::Node* node) {
    if (node->outputs().size() != 1) {
        throw_lowering_error(node, "expected a single arange output");
    }

    torch::jit::Value* start_value = nullptr;
    torch::jit::Value* end_value = nullptr;
    torch::jit::Value* step_value = nullptr;
    if (node->inputs().size() == 5) {
        end_value = node->input(0);
    } else if (node->inputs().size() == 6) {
        start_value = node->input(0);
        end_value = node->input(1);
    } else if (node->inputs().size() == 7) {
        start_value = node->input(0);
        end_value = node->input(1);
        step_value = node->input(2);
    } else {
        throw_lowering_error(node, "unsupported arange overload");
    }

    const auto scalar_type = value_scalar_type(node->output()).value_or(at::kFloat);
    torch::jit::Value* start = start_value != nullptr
        ? materialize_tensor_input(node, start_value, scalar_type)
        : create_scalar_constant(node, 0.0, scalar_type);
    torch::jit::Value* limit = materialize_tensor_input(node, end_value, scalar_type);
    torch::jit::Value* delta = step_value != nullptr
        ? materialize_tensor_input(node, step_value, scalar_type)
        : create_scalar_constant(node, 1.0, scalar_type);

    if (step_value != nullptr) {
        const auto step_const = constant_double_value(step_value);
        if (step_const.has_value() && *step_const == 0.0) {
            throw_lowering_error(node, "arange step must be non-zero");
        }
    }

    auto* range = node->owningGraph()->create(kOnnxRange, {start, limit, delta}, 1);
    range->copyMetadata(node);
    range->insertBefore(node);
    range->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(range->output());
    node->destroy();
}

void lower_einsum_bchw_bnc_to_bnhw_node(
    torch::jit::Node* node,
    torch::jit::Value* spatial_features,
    torch::jit::Value* query_features) {
    const auto spatial_sizes = value_tensor_sizes(spatial_features);
    const auto query_sizes = value_tensor_sizes(query_features);
    const auto output_sizes = value_tensor_sizes(node->output());
    if (!spatial_sizes.has_value() || !query_sizes.has_value() || !output_sizes.has_value() ||
        spatial_sizes->size() != 4 || query_sizes->size() != 3 || output_sizes->size() != 4) {
        throw_lowering_error(node, "einsum bchw,bnc->bnhw requires statically known 4D/3D/4D tensor shapes");
    }

    const int64_t batch = spatial_sizes->at(0);
    const int64_t channels = spatial_sizes->at(1);
    const int64_t height = spatial_sizes->at(2);
    const int64_t width = spatial_sizes->at(3);
    const int64_t query_batch = query_sizes->at(0);
    const int64_t num_queries = query_sizes->at(1);
    const int64_t query_channels = query_sizes->at(2);
    if (batch != query_batch || channels != query_channels) {
        throw_lowering_error(node, "einsum bchw,bnc->bnhw requires matching batch and channel dimensions");
    }
    if (output_sizes->at(0) != batch || output_sizes->at(1) != num_queries ||
        output_sizes->at(2) != height || output_sizes->at(3) != width) {
        throw_lowering_error(node, "einsum output shape does not match statically inferred bnhw dimensions");
    }

    auto* spatial_transpose = node->owningGraph()->create(kOnnxTranspose, {spatial_features}, 1);
    spatial_transpose->copyMetadata(node);
    spatial_transpose->is_(kAttrPerm, std::vector<int64_t>{0, 2, 3, 1});
    spatial_transpose->insertBefore(node);

    auto* spatial_flatten = node->owningGraph()->create(
        kOnnxReshape,
        {spatial_transpose->output(), create_int64s_constant(node, {batch, height * width, channels})},
        1);
    spatial_flatten->copyMetadata(node);
    spatial_flatten->insertAfter(spatial_transpose);

    auto* query_transpose = node->owningGraph()->create(kOnnxTranspose, {query_features}, 1);
    query_transpose->copyMetadata(node);
    query_transpose->is_(kAttrPerm, std::vector<int64_t>{0, 2, 1});
    query_transpose->insertAfter(spatial_flatten);

    auto* matmul = node->owningGraph()->create(
        kOnnxMatMul,
        {spatial_flatten->output(), query_transpose->output()},
        1);
    matmul->copyMetadata(node);
    matmul->insertAfter(query_transpose);

    auto* logits_transpose = node->owningGraph()->create(kOnnxTranspose, {matmul->output()}, 1);
    logits_transpose->copyMetadata(node);
    logits_transpose->is_(kAttrPerm, std::vector<int64_t>{0, 2, 1});
    logits_transpose->insertAfter(matmul);

    auto* reshape = node->owningGraph()->create(
        kOnnxReshape,
        {logits_transpose->output(), create_output_shape_constant(node, node->output())},
        1);
    reshape->copyMetadata(node);
    reshape->insertAfter(logits_transpose);
    reshape->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(reshape->output());
    node->destroy();
}

void lower_einsum_node(torch::jit::Node* node) {
    if (node->inputs().size() < 2 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected einsum(equation, tensors[, path]) -> Tensor");
    }
    const auto equation = constant_string_value(node->input(0));
    if (!equation.has_value() || equation->empty()) {
        throw_lowering_error(node, "einsum equation must be a compile-time constant string");
    }
    const auto tensors = tensor_list_inputs(node->input(1));
    if (!tensors.has_value() || tensors->empty()) {
        throw_lowering_error(node, "einsum tensor list must be statically known");
    }
    if (tensors->size() > 2) {
        throw_lowering_error(node, "einsum with more than two inputs is not supported by the native exporter");
    }
    if (equation->find("...") != std::string::npos) {
        throw_lowering_error(node, "einsum with ellipsis is not supported by the native exporter");
    }
    if (*equation == "bchw,bnc->bnhw") {
        lower_einsum_bchw_bnc_to_bnhw_node(node, tensors->at(0), tensors->at(1));
        return;
    }
    throw_lowering_error(node, "unsupported einsum equation " + *equation);
}

void lower_binary_comparison_node(torch::jit::Node* node, c10::Symbol onnx_kind) {
    const auto preferred_scalar_type =
        value_scalar_type(node->input(0))
            .value_or(value_scalar_type(node->input(1)).value_or(at::kFloat));
    auto* compare = node->owningGraph()->create(
        onnx_kind,
        {materialize_tensor_input(node, node->input(0), preferred_scalar_type),
         materialize_tensor_input(node, node->input(1), preferred_scalar_type)},
        1);
    compare->copyMetadata(node);
    compare->insertBefore(node);
    compare->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(compare->output());
    node->destroy();
}

void lower_not_equal_node(torch::jit::Node* node) {
    const auto preferred_scalar_type =
        value_scalar_type(node->input(0))
            .value_or(value_scalar_type(node->input(1)).value_or(at::kFloat));
    auto* equal = node->owningGraph()->create(
        kOnnxEqual,
        {materialize_tensor_input(node, node->input(0), preferred_scalar_type),
         materialize_tensor_input(node, node->input(1), preferred_scalar_type)},
        1);
    equal->copyMetadata(node);
    equal->insertBefore(node);

    auto* not_node = node->owningGraph()->create(kOnnxNot, {equal->output()}, 1);
    not_node->copyMetadata(node);
    not_node->insertAfter(equal);
    not_node->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(not_node->output());
    node->destroy();
}

bool is_explicit_bool_literal(const torch::jit::Value* value) {
    if (value != nullptr && value->type() != nullptr && value->type()->kind() == c10::TypeKind::BoolType) {
        return true;
    }
    const auto ivalue = torch::jit::toIValue(value);
    if (ivalue.has_value()) {
        if (ivalue->isBool() || ivalue->isSymBool()) {
            return true;
        }
        if (ivalue->isTensor()) {
            const auto tensor = ivalue->toTensor();
            return tensor.defined() && tensor.numel() == 1 && tensor.scalar_type() == at::kBool;
        }
        return false;
    }
    if (value == nullptr) {
        return false;
    }
    const auto* node = value->node();
    if (node->kind() != c10::prim::Constant || !node->hasAttribute(kAttrValue)) {
        return false;
    }
    if (node->kindOf(kAttrValue) == torch::jit::AttributeKind::ival) {
        const auto& attr_value = node->ival(kAttrValue);
        if (attr_value.isBool() || attr_value.isSymBool()) {
            return true;
        }
        if (attr_value.isTensor()) {
            const auto tensor = attr_value.toTensor();
            return tensor.defined() && tensor.numel() == 1 && tensor.scalar_type() == at::kBool;
        }
    }
    return false;
}

void ensure_bool_bitwise_operand(torch::jit::Node* node, torch::jit::Value* value, bool output_is_bool) {
    if (const auto scalar_type = resolved_scalar_type(value)) {
        if (*scalar_type != at::kBool) {
            throw_lowering_error(node, "integer bitwise_and is not supported by the native exporter");
        }
        return;
    }
    if (is_explicit_bool_literal(value)) {
        return;
    }
    if (constant_int_value(value).has_value() || constant_double_value(value).has_value()) {
        throw_lowering_error(node, "integer bitwise_and is not supported by the native exporter");
    }
    if ((is_tensor_value(value) || constant_bool_value(value).has_value()) && output_is_bool) {
        return;
    }
    throw_lowering_error(node, "bitwise_and operand types must resolve to bool");
}

void lower_bool_and_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected bitwise/logical and with 2 inputs and 1 output");
    }
    const auto output_scalar_type = resolved_scalar_type(node->output());
    if (output_scalar_type.has_value() && *output_scalar_type != at::kBool) {
        throw_lowering_error(node, "integer bitwise_and is not supported by the native exporter");
    }
    const bool output_is_bool = !output_scalar_type.has_value() || *output_scalar_type == at::kBool;
    ensure_bool_bitwise_operand(node, node->input(0), output_is_bool);
    ensure_bool_bitwise_operand(node, node->input(1), output_is_bool);

    auto* and_node = node->owningGraph()->create(
        kOnnxAnd,
        {materialize_tensor_input(node, node->input(0), at::kBool),
         materialize_tensor_input(node, node->input(1), at::kBool)},
        1);
    and_node->copyMetadata(node);
    and_node->insertBefore(node);
    and_node->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(and_node->output());
    node->destroy();
}

void lower_inplace_bool_and_node(torch::jit::Node* node) {
    if (node->inputs().size() != 2 || node->outputs().size() != 1) {
        throw_lowering_error(node, "expected bitwise_and_(self, other) -> Tensor");
    }
    const auto output_scalar_type = resolved_scalar_type(node->output());
    if (output_scalar_type.has_value() && *output_scalar_type != at::kBool) {
        throw_lowering_error(node, "integer bitwise_and is not supported by the native exporter");
    }
    const bool output_is_bool = !output_scalar_type.has_value() || *output_scalar_type == at::kBool;
    ensure_bool_bitwise_operand(node, node->input(0), output_is_bool);
    ensure_bool_bitwise_operand(node, node->input(1), output_is_bool);

    auto* and_node = node->owningGraph()->create(
        kOnnxAnd,
        {materialize_tensor_input(node, node->input(0), at::kBool),
         materialize_tensor_input(node, node->input(1), at::kBool)},
        1);
    and_node->copyMetadata(node);
    and_node->insertBefore(node);
    and_node->output()->copyMetadata(node->output());
    node->input(0)->replaceAllUsesAfterNodeWith(node, and_node->output());
    node->output()->replaceAllUsesWith(and_node->output());
    node->destroy();
}

void lower_select_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    const auto index = constant_int_value(node->input(2));
    if (!input_rank.has_value() || !dim.has_value() || !index.has_value()) {
        throw_lowering_error(node, "select axis and index must be compile-time constants");
    }

    auto* gather = node->owningGraph()->create(
        kOnnxGather,
        {node->input(0), create_constant_value(node, make_scalar_tensor(static_cast<double>(*index), at::kLong))},
        1);
    gather->copyMetadata(node);
    gather->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    gather->insertBefore(node);

    torch::jit::Value* replacement = gather->output();
    if (value_tensor_sizes(node->output()).has_value()) {
        replacement = reshape_value_to_output(node, gather->output(), node->output());
    }
    replacement->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_slice_like_node(
    torch::jit::Node* node,
    int64_t axis,
    int64_t start,
    int64_t end,
    int64_t step) {
    auto* slice = node->owningGraph()->create(
        kOnnxSlice,
        {node->input(0),
         create_int64s_constant(node, {start}),
         create_int64s_constant(node, {end}),
         create_int64s_constant(node, {axis}),
         create_int64s_constant(node, {step})},
        1);
    slice->copyMetadata(node);
    slice->insertBefore(node);
    slice->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(slice->output());
    node->destroy();
}

void lower_slice_node(torch::jit::Node* node) {
    if (node->inputs().size() != 5) {
        throw_lowering_error(node, "expected 5 inputs");
    }
    const auto dim = constant_int_value(node->input(1));
    auto step = is_none_value(node->input(4))
        ? std::optional<int64_t>(1)
        : constant_int_value(node->input(4));

    auto input_rank = value_tensor_rank(node->input(0));
    const auto output_sizes = value_tensor_sizes(node->output());
    if (!input_rank.has_value() && output_sizes.has_value()) {
        input_rank = output_sizes->size();
    }

    std::optional<int64_t> axis;
    if (dim.has_value()) {
        if (input_rank.has_value()) {
            axis = normalize_axis(*dim, *input_rank, node);
        } else if (*dim >= 0) {
            axis = dim;
        }
    }

    if (!axis.has_value() || !step.has_value()) {
        std::vector<std::string> missing;
        if (!axis.has_value()) {
            missing.emplace_back("dim");
        }
        if (!step.has_value()) {
            missing.emplace_back("step");
        }
        std::ostringstream message;
        message << "slice axis and step must be compile-time constants";
        if (!missing.empty()) {
            message << " (missing: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i != 0) {
                    message << ", ";
                }
                message << missing[i];
            }
            message << ")";
        }
        throw_lowering_error(node, message.str());
    }
    const auto start_value = is_none_value(node->input(2))
        ? std::optional<int64_t>(0)
        : constant_int_value(node->input(2));
    const auto end_value = is_none_value(node->input(3))
        ? std::optional<int64_t>(std::numeric_limits<int64_t>::max())
        : constant_int_value(node->input(3));
    if (!start_value.has_value() || !end_value.has_value()) {
        throw_lowering_error(node, "slice start and end must be compile-time constants");
    }
    lower_slice_like_node(
        node,
        *axis,
        *start_value,
        *end_value,
        *step);
}

void lower_narrow_node(torch::jit::Node* node) {
    if (node->inputs().size() != 4) {
        throw_lowering_error(node, "expected 4 inputs");
    }
    const auto dim = constant_int_value(node->input(1));
    const auto start = constant_int_value(node->input(2));
    auto length = constant_int_value(node->input(3));

    const auto output_sizes = value_tensor_sizes(node->output());
    auto input_rank = value_tensor_rank(node->input(0));
    if (!input_rank.has_value() && output_sizes.has_value()) {
        input_rank = output_sizes->size();
    }

    std::optional<int64_t> axis;
    if (dim.has_value()) {
        if (input_rank.has_value()) {
            axis = normalize_axis(*dim, *input_rank, node);
        } else if (*dim >= 0) {
            axis = dim;
        }
    }

    if (!length.has_value() && axis.has_value() && output_sizes.has_value() &&
        static_cast<size_t>(*axis) < output_sizes->size()) {
        length = output_sizes->at(static_cast<size_t>(*axis));
    }

    if (!axis.has_value() || !start.has_value() || !length.has_value()) {
        std::vector<std::string> missing;
        if (!axis.has_value()) {
            missing.emplace_back("dim");
        }
        if (!start.has_value()) {
            missing.emplace_back("start");
        }
        if (!length.has_value()) {
            missing.emplace_back("length");
        }
        std::ostringstream message;
        message << "narrow parameters must be compile-time constants";
        if (!missing.empty()) {
            message << " (missing: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i != 0) {
                    message << ", ";
                }
                message << missing[i];
            }
            message << ")";
        }
        throw_lowering_error(node, message.str());
    }
    lower_slice_like_node(
        node,
        *axis,
        *start,
        *start + *length,
        1);
}

void lower_gather_node(torch::jit::Node* node) {
    if (node->inputs().size() != 4) {
        throw_lowering_error(node, "expected 4 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    if (!input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "gather axis and input rank must be statically known");
    }
    auto* gather = node->owningGraph()->create(kOnnxGatherElements, {node->input(0), node->input(2)}, 1);
    gather->copyMetadata(node);
    gather->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    gather->insertBefore(node);
    gather->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(gather->output());
    node->destroy();
}

void lower_index_select_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    if (!input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "index_select axis and input rank must be statically known");
    }
    auto* gather = node->owningGraph()->create(kOnnxGather, {node->input(0), node->input(2)}, 1);
    gather->copyMetadata(node);
    gather->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    gather->insertBefore(node);
    gather->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(gather->output());
    node->destroy();
}

void lower_masked_fill_node(torch::jit::Node* node) {
    const auto data_scalar_type = value_scalar_type(node->input(0)).value_or(at::kFloat);
    torch::jit::Value* mask = node->input(1);
    const auto mask_scalar_type = value_scalar_type(mask);
    if (!mask_scalar_type.has_value() || *mask_scalar_type != at::kBool) {
        mask = create_onnx_cast(node, mask, at::kBool);
    }

    auto* where = node->owningGraph()->create(
        kOnnxWhere,
        {mask, materialize_tensor_input(node, node->input(2), data_scalar_type), node->input(0)},
        1);
    where->copyMetadata(node);
    where->insertBefore(node);
    where->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(where->output());
    node->destroy();
}

std::optional<std::vector<int64_t>> extract_reduce_axes(const torch::jit::Node* node) {
    if (node->kind() == kAtenProd) {
        if (node->inputs().size() < 2 || is_none_value(node->input(1))) {
            return std::nullopt;
        }
        const auto dim = constant_int_value(node->input(1));
        if (!dim.has_value()) {
            return std::nullopt;
        }
        return std::vector<int64_t>{*dim};
    }
    if (node->inputs().size() < 2 || is_none_value(node->input(1))) {
        return std::nullopt;
    }
    if (const auto dims = constant_int_list_value(node->input(1))) {
        return dims;
    }
    if (const auto dim = constant_int_value(node->input(1))) {
        return std::vector<int64_t>{*dim};
    }
    return std::nullopt;
}

bool extract_keepdim(const torch::jit::Node* node, bool default_value = false) {
    if (node->inputs().size() < 3 || is_none_value(node->input(2))) {
        return default_value;
    }
    return constant_bool_value(node->input(2)).value_or(default_value);
}

void lower_reduce_node(torch::jit::Node* node, c10::Symbol onnx_kind) {
    const auto input_rank = value_tensor_rank(node->input(0));
    if (!input_rank.has_value()) {
        throw_lowering_error(node, "reduce input rank must be statically known");
    }
    const auto output_scalar_type = value_scalar_type(node->output());
    const auto input_scalar_type = value_scalar_type(node->input(0));
    torch::jit::Value* reduce_input = node->input(0);
    if (output_scalar_type.has_value() && input_scalar_type.has_value() &&
        *output_scalar_type != *input_scalar_type) {
        reduce_input = create_onnx_cast(node, reduce_input, *output_scalar_type);
    }

    std::vector<torch::jit::Value*> inputs{reduce_input};
    const auto axes = extract_reduce_axes(node);
    if (axes.has_value()) {
        inputs.push_back(create_int64s_constant(node, normalize_axes(*axes, *input_rank, node, "reduce axis")));
    }

    auto* reduce = node->owningGraph()->create(onnx_kind, inputs, 1);
    reduce->copyMetadata(node);
    reduce->i_(kAttrKeepdims, extract_keepdim(node) ? 1 : 0);
    reduce->insertBefore(node);
    reduce->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(reduce->output());
    node->destroy();
}

void lower_all_node(torch::jit::Node* node) {
    const auto input_rank = value_tensor_rank(node->input(0));
    if (!input_rank.has_value()) {
        throw_lowering_error(node, "all input rank must be statically known");
    }
    std::vector<int64_t> axes;
    if (node->inputs().size() >= 2 && !is_none_value(node->input(1))) {
        const auto dim = constant_int_value(node->input(1));
        if (!dim.has_value()) {
            throw_lowering_error(node, "all axis must be a compile-time constant");
        }
        axes.push_back(normalize_axis(*dim, *input_rank, node));
    }

    auto* cast = node->owningGraph()->create(kOnnxCast, {node->input(0)}, 1);
    cast->copyMetadata(node);
    cast->i_(kAttrTo, onnx_tensor_data_type(at::kLong));
    cast->insertBefore(node);

    std::vector<torch::jit::Value*> reduce_inputs{cast->output()};
    if (!axes.empty()) {
        reduce_inputs.push_back(create_int64s_constant(node, axes));
    }
    auto* reduce = node->owningGraph()->create(kOnnxReduceMin, reduce_inputs, 1);
    reduce->copyMetadata(node);
    reduce->i_(kAttrKeepdims, extract_keepdim(node) ? 1 : 0);
    reduce->insertAfter(cast);

    auto* to_bool = node->owningGraph()->create(kOnnxCast, {reduce->output()}, 1);
    to_bool->copyMetadata(node);
    to_bool->i_(kAttrTo, onnx_tensor_data_type(at::kBool));
    to_bool->insertAfter(reduce);
    to_bool->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(to_bool->output());
    node->destroy();
}

void lower_cumsum_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    if (!input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(node, "cumsum axis and input rank must be statically known");
    }
    const auto output_scalar_type = value_scalar_type(node->output());
    const auto input_scalar_type = value_scalar_type(node->input(0));
    torch::jit::Value* input = node->input(0);
    if (output_scalar_type.has_value() && input_scalar_type.has_value() &&
        *output_scalar_type != *input_scalar_type) {
        input = create_onnx_cast(node, input, *output_scalar_type);
    }

    auto* cumsum = node->owningGraph()->create(
        kOnnxCumSum,
        {input, create_constant_value(node, make_scalar_tensor(
            static_cast<double>(normalize_axis(*dim, *input_rank, node)),
            at::kLong))},
        1);
    cumsum->copyMetadata(node);
    cumsum->insertBefore(node);
    cumsum->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(cumsum->output());
    node->destroy();
}

void lower_argmax_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3) {
        throw_lowering_error(node, "expected 3 inputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    const auto keepdim = constant_bool_value(node->input(2));
    if (!input_rank.has_value() || !dim.has_value() || !keepdim.has_value()) {
        throw_lowering_error(node, "argmax parameters must be compile-time constants");
    }
    auto* argmax = node->owningGraph()->create(kOnnxArgMax, {node->input(0)}, 1);
    argmax->copyMetadata(node);
    argmax->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    argmax->i_(kAttrKeepdims, *keepdim ? 1 : 0);
    argmax->insertBefore(node);
    argmax->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(argmax->output());
    node->destroy();
}

void lower_topk_node(torch::jit::Node* node) {
    if (node->inputs().size() != 5 || node->outputs().size() != 2) {
        throw_lowering_error(node, "expected 5 inputs and 2 outputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto k = constant_int_value(node->input(1));
    const auto dim = constant_int_value(node->input(2));
    const auto largest = constant_bool_value(node->input(3));
    const auto sorted = constant_bool_value(node->input(4));
    if (!input_rank.has_value() || !k.has_value() || !dim.has_value() ||
        !largest.has_value() || !sorted.has_value()) {
        throw_lowering_error(node, "topk parameters must be compile-time constants");
    }
    auto* topk = node->owningGraph()->create(
        kOnnxTopK,
        {node->input(0), create_int64s_constant(node, {*k})},
        2);
    topk->copyMetadata(node);
    topk->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    topk->i_(kAttrLargest, *largest ? 1 : 0);
    topk->i_(kAttrSorted, *sorted ? 1 : 0);
    topk->insertBefore(node);
    topk->output(0)->copyMetadata(node->output(0));
    topk->output(1)->copyMetadata(node->output(1));
    node->output(0)->replaceAllUsesWith(topk->output(0));
    node->output(1)->replaceAllUsesWith(topk->output(1));
    node->destroy();
}

void lower_max_reduce_node(torch::jit::Node* node) {
    if (node->inputs().size() != 3 || node->outputs().size() != 2) {
        throw_lowering_error(node, "expected 3 inputs and 2 outputs");
    }
    const auto input_rank = value_tensor_rank(node->input(0));
    const auto dim = constant_int_value(node->input(1));
    const auto keepdim = constant_bool_value(node->input(2));
    if (!input_rank.has_value() || !dim.has_value() || !keepdim.has_value()) {
        throw_lowering_error(node, "max reduction parameters must be compile-time constants");
    }
    auto* topk = node->owningGraph()->create(
        kOnnxTopK,
        {node->input(0), create_int64s_constant(node, {1})},
        2);
    topk->copyMetadata(node);
    topk->i_(kAttrAxis, normalize_axis(*dim, *input_rank, node));
    topk->i_(kAttrLargest, 1);
    topk->i_(kAttrSorted, 1);
    topk->insertBefore(node);

    torch::jit::Value* values = topk->output(0);
    torch::jit::Value* indices = topk->output(1);
    if (!*keepdim) {
        values = reshape_value_to_output(node, values, node->output(0));
        indices = reshape_value_to_output(node, indices, node->output(1));
    } else {
        values->copyMetadata(node->output(0));
        indices->copyMetadata(node->output(1));
    }
    node->output(0)->replaceAllUsesWith(values);
    node->output(1)->replaceAllUsesWith(indices);
    node->destroy();
}

void lower_matmul_node(torch::jit::Node* node) {
    auto* matmul = node->owningGraph()->create(kOnnxMatMul, {node->input(0), node->input(1)}, 1);
    matmul->copyMetadata(node);
    matmul->insertBefore(node);
    matmul->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(matmul->output());
    node->destroy();
}

void lower_creation_node(torch::jit::Node* node, double fill_value) {
    const auto output_scalar_type = value_scalar_type(node->output()).value_or(at::kFloat);
    auto replacement = create_constant_of_shape(node, make_scalar_tensor(fill_value, output_scalar_type));
    replacement->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_full_like_node(torch::jit::Node* node) {
    const auto output_scalar_type = value_scalar_type(node->output()).value_or(at::kFloat);
    const auto fill = constant_double_value(node->input(1));
    if (!fill.has_value()) {
        throw_lowering_error(node, "full_like fill value must be a compile-time scalar constant");
    }
    auto replacement = create_constant_of_shape(node, make_scalar_tensor(*fill, output_scalar_type));
    replacement->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_full_node(torch::jit::Node* node) {
    const auto output_scalar_type = value_scalar_type(node->output()).value_or(at::kFloat);
    const auto fill = constant_double_value(node->input(1));
    if (!fill.has_value()) {
        throw_lowering_error(node, "full fill value must be a compile-time scalar constant");
    }
    auto replacement = create_constant_of_shape(node, make_scalar_tensor(*fill, output_scalar_type));
    replacement->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(replacement);
    node->destroy();
}

void lower_resize_node(torch::jit::Node* node, std::string_view mode) {
    const auto output_sizes = value_tensor_sizes(node->output());
    if (!output_sizes.has_value()) {
        throw_lowering_error(node, "resize output tensor shape must be statically known");
    }

    auto* resize = node->owningGraph()->create(
        kOnnxResize,
        {node->input(0),
         create_constant_value(node, make_float_tensor({})),
         create_constant_value(node, make_float_tensor({})),
         create_int64s_constant(node, *output_sizes)},
        1);
    resize->copyMetadata(node);
    resize->s_(kAttrMode, std::string(mode));
    resize->s_(kAttrNearestMode, "floor");

    if (mode == "nearest") {
        resize->s_(kAttrCoordinateTransformationMode, "asymmetric");
    } else {
        bool align_corners = false;
        if (node->kind() == kAtenUpsampleBilinear2d && node->inputs().size() >= 3 && !is_none_value(node->input(2))) {
            const auto align_corners_value = constant_bool_value(node->input(2));
            if (!align_corners_value.has_value()) {
                throw_lowering_error(node, "align_corners must be a compile-time constant");
            }
            align_corners = *align_corners_value;
        }
        resize->s_(
            kAttrCoordinateTransformationMode,
            align_corners ? "align_corners" : "half_pixel");
    }

    resize->insertBefore(node);
    resize->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(resize->output());
    node->destroy();
}

void lower_grid_sampler_node(torch::jit::Node* node) {
    if (node->inputs().size() != 5) {
        throw_lowering_error(node, "expected 5 inputs");
    }
    const auto mode_enum = constant_int_value(node->input(2));
    const auto padding_mode_enum = constant_int_value(node->input(3));
    const auto align_corners = constant_bool_value(node->input(4));
    if (!mode_enum.has_value() || !padding_mode_enum.has_value() || !align_corners.has_value()) {
        throw_lowering_error(node, "grid_sampler attributes must be compile-time constants");
    }
    static const std::vector<std::string> kModeNames = {"bilinear", "nearest", "bicubic"};
    static const std::vector<std::string> kPaddingModeNames = {"zeros", "border", "reflection"};
    if (*mode_enum < 0 || static_cast<size_t>(*mode_enum) >= kModeNames.size()) {
        throw_lowering_error(node, "unsupported grid_sampler mode enum " + std::to_string(*mode_enum));
    }
    if (*padding_mode_enum < 0 || static_cast<size_t>(*padding_mode_enum) >= kPaddingModeNames.size()) {
        throw_lowering_error(
            node,
            "unsupported grid_sampler padding mode enum " + std::to_string(*padding_mode_enum));
    }

    auto* grid_sample = node->owningGraph()->create(kOnnxGridSample, {node->input(0), node->input(1)}, 1);
    grid_sample->copyMetadata(node);
    grid_sample->s_(kAttrMode, kModeNames[static_cast<size_t>(*mode_enum)]);
    grid_sample->s_(kAttrPaddingMode, kPaddingModeNames[static_cast<size_t>(*padding_mode_enum)]);
    grid_sample->i_(kAttrAlignCorners, *align_corners ? 1 : 0);
    grid_sample->insertBefore(node);
    grid_sample->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(grid_sample->output());
    node->destroy();
}

void lower_scaled_dot_product_attention_node(torch::jit::Node* node) {
    if (node->inputs().size() != 8) {
        throw_lowering_error(node, "expected 8 inputs");
    }
    if (!is_none_value(node->input(3))) {
        throw_lowering_error(node, "attention masks are not supported yet");
    }
    const auto dropout_p = constant_double_value(node->input(4));
    const auto is_causal = constant_bool_value(node->input(5));
    const auto enable_gqa = constant_bool_value(node->input(7));
    if (!dropout_p.has_value() || !is_causal.has_value() || !enable_gqa.has_value()) {
        throw_lowering_error(node, "dropout, is_causal, and enable_gqa must be compile-time constants");
    }
    if (*dropout_p != 0.0) {
        throw_lowering_error(node, "dropout_p must be 0 for export");
    }
    if (*is_causal) {
        throw_lowering_error(node, "causal scaled dot product attention is not supported yet");
    }
    if (*enable_gqa) {
        throw_lowering_error(node, "enable_gqa is not supported yet");
    }

    const auto query_sizes = value_tensor_sizes(node->input(0));
    const auto key_rank = value_tensor_rank(node->input(1));
    if (!query_sizes.has_value() || !key_rank.has_value() || query_sizes->size() < 2) {
        throw_lowering_error(node, "attention input shapes must be statically known");
    }

    std::vector<int64_t> key_perm;
    key_perm.reserve(*key_rank);
    for (size_t i = 0; i < *key_rank; ++i) {
        key_perm.push_back(static_cast<int64_t>(i));
    }
    std::swap(key_perm[key_perm.size() - 2], key_perm[key_perm.size() - 1]);
    auto* key_transpose = node->owningGraph()->create(kOnnxTranspose, {node->input(1)}, 1);
    key_transpose->copyMetadata(node);
    key_transpose->is_(kAttrPerm, key_perm);
    key_transpose->insertBefore(node);

    auto* scores = node->owningGraph()->create(kOnnxMatMul, {node->input(0), key_transpose->output()}, 1);
    scores->copyMetadata(node);
    scores->insertAfter(key_transpose);

    double scale = 0.0;
    if (!is_none_value(node->input(6))) {
        const auto scale_value = constant_double_value(node->input(6));
        if (!scale_value.has_value()) {
            throw_lowering_error(node, "attention scale must be a compile-time constant");
        }
        scale = *scale_value;
    } else {
        const int64_t head_dim = query_sizes->back();
        if (head_dim <= 0) {
            throw_lowering_error(node, "attention head dimension must be positive");
        }
        scale = 1.0 / std::sqrt(static_cast<double>(head_dim));
    }
    const auto scalar_type = value_scalar_type(node->input(0)).value_or(at::kFloat);
    auto* scaled_scores = node->owningGraph()->create(
        kOnnxMul,
        {scores->output(), create_scalar_constant(node, scale, scalar_type)},
        1);
    scaled_scores->copyMetadata(node);
    scaled_scores->insertAfter(scores);

    auto* softmax = node->owningGraph()->create(kOnnxSoftmax, {scaled_scores->output()}, 1);
    softmax->copyMetadata(node);
    softmax->i_(kAttrAxis, -1);
    softmax->insertAfter(scaled_scores);

    auto* output = node->owningGraph()->create(kOnnxMatMul, {softmax->output(), node->input(2)}, 1);
    output->copyMetadata(node);
    output->insertAfter(softmax);
    output->output()->copyMetadata(node->output());
    node->output()->replaceAllUsesWith(output->output());
    node->destroy();
}

std::vector<int64_t> compute_split_sizes(
    torch::jit::Node* producer,
    size_t num_outputs,
    int64_t dim_size) {
    if (producer->kind() == kAtenSplit) {
        if (const auto explicit_sizes = constant_int_list_value(producer->input(1))) {
            if (explicit_sizes->size() != num_outputs) {
                throw_lowering_error(producer, "split sizes do not match list unpack arity");
            }
            return *explicit_sizes;
        }
        const auto split_size = constant_int_value(producer->input(1));
        if (!split_size.has_value() || *split_size <= 0) {
            throw_lowering_error(producer, "split size must be a positive compile-time constant");
        }
        std::vector<int64_t> split_sizes;
        split_sizes.reserve(num_outputs);
        int64_t remaining = dim_size;
        while (remaining > 0) {
            const int64_t size = std::min(*split_size, remaining);
            split_sizes.push_back(size);
            remaining -= size;
        }
        if (split_sizes.size() != num_outputs) {
            throw_lowering_error(producer, "split arity does not match statically inferred output count");
        }
        return split_sizes;
    }

    if (producer->kind() == kAtenUnbind) {
        if (dim_size < 0) {
            throw_lowering_error(producer, "unbind axis size must be statically known");
        }
        if (static_cast<size_t>(dim_size) != num_outputs) {
            throw_lowering_error(producer, "unbind arity does not match statically inferred axis size");
        }
        return std::vector<int64_t>(num_outputs, 1);
    }

    const auto chunks = constant_int_value(producer->input(1));
    if (!chunks.has_value() || *chunks <= 0) {
        throw_lowering_error(producer, "chunk count must be a positive compile-time constant");
    }
    if (static_cast<size_t>(*chunks) != num_outputs) {
        throw_lowering_error(producer, "chunk count does not match list unpack arity");
    }
    const int64_t split_size = (dim_size + *chunks - 1) / *chunks;
    std::vector<int64_t> split_sizes;
    split_sizes.reserve(num_outputs);
    int64_t remaining = dim_size;
    for (size_t i = 0; i < num_outputs; ++i) {
        const int64_t current = std::min(split_size, remaining);
        split_sizes.push_back(current);
        remaining = std::max<int64_t>(0, remaining - current);
    }
    return split_sizes;
}

void lower_split_list_unpack_node(torch::jit::Node* unpack_node) {
    if (unpack_node->inputs().size() != 1 || unpack_node->outputs().empty()) {
        return;
    }
    auto* producer = unpack_node->input(0)->node();
    if (producer->kind() != kAtenSplit && producer->kind() != kAtenChunk &&
        producer->kind() != kAtenUnbind) {
        return;
    }
    if (producer->output()->uses().size() != 1) {
        throw_lowering_error(producer, "list-producing tensor op must only feed a single prim::ListUnpack");
    }

    const size_t num_outputs = unpack_node->outputs().size();
    const auto input_sizes = value_tensor_sizes(producer->input(0));
    const auto input_rank = value_tensor_rank(producer->input(0));
    const size_t dim_input_index = producer->kind() == kAtenUnbind ? 1 : 2;
    const auto dim = constant_int_value(producer->input(dim_input_index));
    if (!input_sizes.has_value() || !input_rank.has_value() || !dim.has_value()) {
        throw_lowering_error(producer, "list-producing tensor op input shape and axis must be statically known");
    }

    const int64_t axis = normalize_axis(*dim, *input_rank, producer);
    const int64_t dim_size = input_sizes->at(static_cast<size_t>(axis));
    const auto split_sizes = compute_split_sizes(producer, num_outputs, dim_size);

    auto* split = unpack_node->owningGraph()->create(
        kOnnxSplit,
        {producer->input(0), create_int64s_constant(unpack_node, split_sizes)},
        static_cast<size_t>(num_outputs));
    split->copyMetadata(unpack_node);
    split->i_(kAttrAxis, axis);
    split->insertBefore(producer);
    for (size_t i = 0; i < num_outputs; ++i) {
        torch::jit::Value* replacement = split->output(static_cast<size_t>(i));
        if (producer->kind() == kAtenUnbind) {
            replacement = reshape_value_to_output(
                unpack_node,
                split->output(static_cast<size_t>(i)),
                unpack_node->output(static_cast<size_t>(i)));
        }
        replacement->copyMetadata(unpack_node->output(static_cast<size_t>(i)));
        unpack_node->output(static_cast<size_t>(i))->replaceAllUsesWith(replacement);
    }
    unpack_node->destroy();
    producer->destroy();
}

void lower_block_for_onnx_export(torch::jit::Block* block, const LoweringContext& context) {
    for (auto it = block->nodes().begin(); it != block->nodes().end();) {
        torch::jit::Node* node = *it;
        ++it;

        for (torch::jit::Block* child : node->blocks()) {
            lower_block_for_onnx_export(child, context);
        }

        if (node->kind() == c10::prim::ListUnpack) {
            lower_split_list_unpack_node(node);
            continue;
        }

        if (node->kind() == kAtenTo || node->kind() == kAtenTypeAs) {
            lower_cast_like_node(node);
        } else if (node->kind() == kAtenConvolution) {
            lower_convolution_node(node, context);
        } else if (node->kind() == kAtenFlatten || node->kind() == kAtenView ||
                   node->kind() == kAtenReshape || node->kind() == kAtenUnsqueeze ||
                   node->kind() == kAtenSqueeze) {
            lower_static_reshape_node(node);
        } else if (node->kind() == kAtenT) {
            lower_t_node(node);
        } else if (node->kind() == kAtenTranspose) {
            lower_transpose_node(node);
        } else if (node->kind() == kAtenPermute) {
            lower_permute_node(node);
        } else if (node->kind() == kAtenContiguous || node->kind() == kAtenDetach ||
                   node->kind() == kAtenAlias) {
            lower_passthrough_node(node);
        } else if (node->kind() == kAtenCat) {
            lower_cat_node(node);
        } else if (node->kind() == kAtenStack) {
            lower_stack_node(node);
        } else if (node->kind() == kAtenExpand) {
            lower_expand_node(node);
        } else if (node->kind() == kAtenRepeat) {
            lower_repeat_node(node);
        } else if (node->kind() == kAtenSoftmax) {
            lower_softmax_node(node);
        } else if (node->kind() == kAtenSigmoid) {
            lower_unary_node(node, kOnnxSigmoid);
        } else if (node->kind() == kAtenRelu) {
            lower_unary_node(node, kOnnxRelu);
        } else if (node->kind() == kAtenTanh) {
            lower_unary_node(node, kOnnxTanh);
        } else if (node->kind() == kAtenDropout) {
            lower_dropout_node(node);
        } else if (node->kind() == kAtenLayerNorm || node->kind() == kAtenNativeLayerNorm) {
            lower_layer_norm_node(node);
        } else if (node->kind() == kAtenSin) {
            lower_unary_node(node, kOnnxSin);
        } else if (node->kind() == kAtenCos) {
            lower_unary_node(node, kOnnxCos);
        } else if (node->kind() == kAtenExp) {
            lower_unary_node(node, kOnnxExp);
        } else if (node->kind() == kAtenLog) {
            lower_unary_node(node, kOnnxLog);
        } else if (node->kind() == kAtenAbs) {
            lower_unary_node(node, kOnnxAbs);
        } else if (node->kind() == kAtenFloor) {
            lower_unary_node(node, kOnnxFloor);
        } else if (node->kind() == kAtenGelu) {
            lower_gelu_node(node);
        } else if (node->kind() == kAtenClampMin) {
            lower_clamp_min_node(node);
        } else if (node->kind() == kAtenClamp) {
            lower_clamp_node(node);
        } else if (node->kind() == kAtenAdd) {
            lower_binary_arithmetic_node(node, kOnnxAdd);
        } else if (node->kind() == kAtenAddInplace) {
            lower_inplace_add_node(node);
        } else if (node->kind() == kAtenSub || node->kind() == kAtenRsub) {
            lower_binary_arithmetic_node(node, kOnnxSub);
        } else if (node->kind() == kAtenMul) {
            lower_binary_arithmetic_node(node, kOnnxMul);
        } else if (node->kind() == kAtenDiv) {
            lower_binary_arithmetic_node(node, kOnnxDiv);
        } else if (node->kind() == kAtenPow) {
            lower_binary_arithmetic_node(node, kOnnxPow);
        } else if (node->kind() == kAtenGt) {
            lower_binary_comparison_node(node, kOnnxGreater);
        } else if (node->kind() == kAtenLt) {
            lower_binary_comparison_node(node, kOnnxLess);
        } else if (node->kind() == kAtenGe) {
            lower_binary_comparison_node(node, kOnnxGreaterOrEqual);
        } else if (node->kind() == kAtenLe) {
            lower_binary_comparison_node(node, kOnnxLessOrEqual);
        } else if (node->kind() == kAtenEq) {
            lower_binary_comparison_node(node, kOnnxEqual);
        } else if (node->kind() == kAtenNe) {
            lower_not_equal_node(node);
        } else if (node->kind() == kAtenAnd || node->kind() == kAtenBitwiseAnd) {
            lower_bool_and_node(node);
        } else if (node->kind() == kAtenBitwiseAndInplace) {
            lower_inplace_bool_and_node(node);
        } else if (node->kind() == kAtenLogicalNot || node->kind() == kAtenBitwiseNot) {
            lower_unary_node(node, kOnnxNot);
        } else if (node->kind() == kAtenSlice) {
            lower_slice_node(node);
        } else if (node->kind() == kAtenSelect) {
            lower_select_node(node);
        } else if (node->kind() == kAtenNarrow) {
            lower_narrow_node(node);
        } else if (node->kind() == kAtenGather) {
            lower_gather_node(node);
        } else if (node->kind() == kAtenIndexSelect) {
            lower_index_select_node(node);
        } else if (node->kind() == kAtenMaskedFill) {
            lower_masked_fill_node(node);
        } else if (node->kind() == kAtenArange) {
            lower_arange_node(node);
        } else if (node->kind() == kAtenEinsum) {
            lower_einsum_node(node);
        } else if (node->kind() == kAtenSum) {
            lower_reduce_node(node, kOnnxReduceSum);
        } else if (node->kind() == kAtenMean) {
            lower_reduce_node(node, kOnnxReduceMean);
        } else if (node->kind() == kAtenProd) {
            lower_reduce_node(node, kOnnxReduceProd);
        } else if (node->kind() == kAtenAll) {
            lower_all_node(node);
        } else if (node->kind() == kAtenCumsum) {
            lower_cumsum_node(node);
        } else if (node->kind() == kAtenArgmax) {
            lower_argmax_node(node);
        } else if (node->kind() == kAtenTopk) {
            lower_topk_node(node);
        } else if (node->kind() == kAtenMax && node->outputs().size() == 2) {
            lower_max_reduce_node(node);
        } else if (node->kind() == kAtenMatmul || node->kind() == kAtenBmm) {
            lower_matmul_node(node);
        } else if (node->kind() == kAtenOnesLike || node->kind() == kAtenNewOnes || node->kind() == kAtenOnes) {
            lower_creation_node(node, 1.0);
        } else if (node->kind() == kAtenZerosLike || node->kind() == kAtenNewZeros || node->kind() == kAtenZeros) {
            lower_creation_node(node, 0.0);
        } else if (node->kind() == kAtenFullLike) {
            lower_full_like_node(node);
        } else if (node->kind() == kAtenFull) {
            lower_full_node(node);
        } else if (node->kind() == kAtenUpsampleNearest2d) {
            lower_resize_node(node, "nearest");
        } else if (node->kind() == kAtenUpsampleBilinear2d) {
            lower_resize_node(node, "linear");
        } else if (node->kind() == kAtenGridSampler) {
            lower_grid_sampler_node(node);
        } else if (node->kind() == kAtenScaledDotProductAttention) {
            lower_scaled_dot_product_attention_node(node);
        }
    }
}

} // namespace

int onnx_tensor_data_type(at::ScalarType scalar_type) {
    switch (scalar_type) {
    case at::kFloat:
        return kOnnxTensorProtoFloat;
    case at::kHalf:
        return kOnnxTensorProtoFloat16;
    case at::kBFloat16:
        return kOnnxTensorProtoBfloat16;
    case at::kDouble:
        return kOnnxTensorProtoDouble;
    case at::kBool:
        return kOnnxTensorProtoBool;
    case at::kByte:
        return kOnnxTensorProtoUint8;
    case at::kChar:
        return kOnnxTensorProtoInt8;
    case at::kShort:
        return kOnnxTensorProtoInt16;
    case at::kInt:
        return kOnnxTensorProtoInt32;
    case at::kLong:
        return kOnnxTensorProtoInt64;
    default:
        throw std::runtime_error(
            std::string("RF-DETR ONNX export does not support dtype ") + scalar_type_name(scalar_type));
    }
}

void validate_supported_onnx_export_opset(int opset_version) {
    if (opset_version != kSupportedOnnxExportOpsetVersion) {
        throw std::runtime_error(
            std::string("RF-DETR native ONNX export only supports opset ") +
            std::to_string(kSupportedOnnxExportOpsetVersion) +
            ", got " + std::to_string(opset_version));
    }
}

void lower_graph_for_onnx_export(
    const std::shared_ptr<torch::jit::Graph>& graph,
    const OnnxInitializerMap* initializers) {
    if (graph == nullptr) {
        throw std::runtime_error("RF-DETR ONNX export requires a valid TorchScript graph");
    }
    const LoweringContext context{graph, initializers};
    const LoweringContextScope scope(context);
    lower_block_for_onnx_export(graph->block(), context);
    materialize_initializer_getattrs(graph->block(), context);
    materialize_remaining_prim_constants(graph->block());
    erase_trivially_dead_nodes(graph->block());
    topologically_sort_block(graph->block());
    validate_graph_is_onnx_only(graph);
}

} // namespace mmltk::rfdetr
