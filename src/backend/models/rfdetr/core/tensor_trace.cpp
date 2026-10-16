#include "detail/tensor_trace.h"
#include "detail/traced_graph.h"
#include <ATen/Context.h>
#include <ATen/autocast_mode.h>
#include <torch/script.h>
#include <torch/version.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/csrc/jit/codegen/fuser/interface.h>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>
import mmltk.common.logging.mmltk_logging;
namespace mmltk::backend::models::rfdetr::detail {
namespace {
struct TensorSignature {
 c10::Device device{torch::kCPU};
 c10::ScalarType dtype = c10::ScalarType::Undefined;
 std::array<int64_t, 4> sizes{};
 int64_t rank = 0;
 bool requires_grad = false;
 bool operator==(const TensorSignature&) const = default;
};
struct Signature {
 std::array<TensorSignature, 2> tensors{};
 size_t count = 0;
 bool grad = torch::GradMode::is_enabled();
 bool amp = false;
 bool amp_cache = false;
 c10::ScalarType amp_dtype = c10::ScalarType::Undefined;
 std::uint64_t specialization = 0;
 std::array<bool, 10> attention_policy{};
 decltype(at::globalContext().sDPPriorityOrder()) attention_priority{};
 bool operator==(const Signature&) const = default;
};
Signature signature_of(std::span<const torch::Tensor> inputs, bool dynamic, std::uint64_t specialization) {
 Signature result;
 result.specialization = specialization;
 auto& context = at::globalContext();
 result.attention_policy = {
  context.userEnabledFlashSDP(), context.userEnabledMemEfficientSDP(), context.userEnabledMathSDP(), context.userEnabledCuDNNSDP(), context.allowFP16BF16ReductionMathSDP(), context.allowTF32CuBLAS(),
  context.allowTF32CuDNN(), context.userEnabledFA3SDP(), context.userEnabledOverrideableSDP(), context.deterministicAlgorithms()
 };
 result.attention_priority = context.sDPPriorityOrder();
 for (const auto& input : inputs) {
  TORCH_CHECK(input.defined() && input.dim() <= 4, "tensor trace requires defined tensors of rank at most four");
  std::array<int64_t, 4> sizes{};
  std::copy(input.sizes().begin(), input.sizes().end(), sizes.begin());
  if (dynamic) {
   TORCH_CHECK(input.dim() == 3, "dynamic query region requires rank three");
   sizes[0] = -1;
   sizes[1] = -1;
  }
  result.tensors[result.count++] = {input.device(), input.scalar_type(), sizes, input.dim(), input.requires_grad()};
 }
 const auto device_type = inputs.front().device().type();
 result.amp = at::autocast::is_autocast_enabled(device_type);
 result.amp_cache = result.amp && at::autocast::is_autocast_cache_enabled();
 if (result.amp) result.amp_dtype = at::autocast::get_autocast_dtype(device_type);
 return result;
}
void trace_cached_cast(const torch::Tensor& tensor, c10::DeviceType device, at::ScalarType dtype) {
 if (tensor.device().type() != device || tensor.scalar_type() != torch::kFloat32 || !tensor.requires_grad() || !tensor.is_leaf()) return;
 const auto converted = at::autocast::cached_cast(dtype, tensor, device);
 if (torch::jit::tracer::getTracingState()->hasValue(converted)) return;
 // A cast cached by the enclosing AMP scope predates this graph. Give its
 // alias a recorded conversion while retaining the ordinary shared autograd
 // tensor. Disabling the cache instead changes low-precision accumulation.
 const auto recorded = tensor.to(dtype, false, true);
 torch::jit::tracer::setValueTrace(converted, torch::jit::tracer::getValueTrace(recorded));
}
TensorSignature state_signature(const torch::Tensor& tensor) {
 TORCH_CHECK(tensor.dim() <= 4, "traced owner tensor rank exceeds four");
 TensorSignature result{tensor.device(), tensor.scalar_type(), {}, tensor.dim(), tensor.requires_grad()};
 std::copy(tensor.sizes().begin(), tensor.sizes().end(), result.sizes.begin());
 return result;
}
bool has_low_precision_values(const torch::jit::Block& block) {
 const auto low_precision = [](const torch::jit::Value* value) {
  const auto type = value->type()->cast<torch::jit::TensorType>();
  return type && (type->scalarType() == torch::kFloat16 || type->scalarType() == torch::kBFloat16);
 };
 if (std::ranges::any_of(block.inputs(), low_precision)) return true;
 for (const auto* node : block.nodes()) {
  if (std::ranges::any_of(node->outputs(), low_precision)) return true;
  for (const auto* nested : node->blocks())
   if (has_low_precision_values(*nested)) return true;
 }
 return false;
}
}  // namespace
void install_recorded_forward(torch::jit::Module& module, torch::jit::CompilationUnit& compilation_unit, const std::shared_ptr<torch::jit::Graph>& graph) {
 auto* function = compilation_unit.create_function("forward", graph, true);
 // Eager dispatch already recorded the selected autocast conversions. A second
 // JIT autocast pass changes that arithmetic (including CUDA GELU's dtype).
 // Keep this function-local; runtime traces admit the recorded AMP signature.
 auto& recorded = torch::jit::toGraphFunction(*function);
 recorded._set_ignore_amp(true);
 // The pinned profiling path changes low-precision rounding boundaries and
 // its symbolic Linear backward can mix FP32 cotangents with FP16 weights.
 // The simple executor preserves recorded ATen operations and their native
 // autograd kernels. FP32 graphs retain the ordinary profiling executor.
 if (has_low_precision_values(*graph->block())) recorded._set_initial_executor_execution_mode(torch::jit::SIMPLE);
 module.type()->addMethod(function);
}
c10::IValue run_recorded_forward(torch::jit::Module& module, std::vector<c10::IValue> inputs, c10::DeviceType device) {
 // Recorded conversions already encode both AMP and protected FP32 regions.
 // Ambient dispatcher autocast would cast their FP32 intermediates again,
 // even with the graph's separate JIT autocast rewrite disabled.
 c10::impl::ExcludeDispatchKeyGuard precision(at::autocast::get_autocast_dispatch_key_from_device_type(device));
 return module.forward(std::move(inputs));
}
struct TensorTrace::State {
 torch::jit::Module module;
 Signature signature;
 struct LiveTensor {
  torch::Tensor tensor;
  TensorSignature signature;
 };
 std::vector<LiveTensor> members;
 std::int64_t preparation_batch = 0;
};
TensorTrace::TensorTrace(std::string name, bool dynamic) : name_(std::move(name)), dynamic_batch_queries_(dynamic) {}
TensorTrace::~TensorTrace() = default;
void TensorTrace::prepare(bool training, bool enabled, std::int64_t batch_size) {
 TORCH_CHECK(batch_size > 0, "tensor trace preparation requires a positive batch size");
 auto& slot = slots_[training];
 if (slot.enabled == enabled && slot.batch_size == batch_size) {
  mmltk::common::logging::debug([&](auto& logger) { logger.debug("rfdetr compilation region={} training={} preparation=identical", name_, training); });
  return;
 }
 mmltk::common::logging::debug([&](auto& logger) { logger.debug("rfdetr compilation region={} training={} preparation={} batch={}", name_, training, enabled ? "armed" : "disabled", batch_size); });
 slot.enabled = enabled;
 slot.batch_size = batch_size;
 slot.armed = enabled;
}
void TensorTrace::invalidate() {
 for (auto& slot : slots_) {
  slot.state.reset();
  slot.armed = slot.enabled;
 }
}
const void* TensorTrace::identity(bool training) const noexcept { return slots_[training].state.get(); }
bool TensorTrace::enabled(bool training) const noexcept { return slots_[training].enabled; }
std::shared_ptr<torch::jit::Graph> TensorTrace::graph(bool training) const {
 const auto& state = slots_[training].state;
 return state ? state->module.get_method("forward").graph() : nullptr;
}
TensorTrace::Result TensorTrace::invoke(bool training, std::span<const torch::Tensor> inputs, std::initializer_list<torch::nn::Module*> owners, Operation ordinary, std::uint64_t specialization) {
 auto& slot = slots_[training];
 if (!slot.enabled) return {ordinary(inputs), false};
 TORCH_CHECK(!inputs.empty() && inputs.size() <= 2, "tensor trace requires one or two inputs");
 bool compatible_extents = true;
 if (dynamic_batch_queries_) {
  for (const auto& input : inputs) compatible_extents &= input.dim() == 3 && input.sizes() == inputs.front().sizes() && input.device() == inputs.front().device();
 }
 const auto signature = signature_of(inputs, dynamic_batch_queries_ && compatible_extents, specialization);
 auto diagnostic = [&](const char* reason, std::shared_ptr<torch::jit::Graph> graph = {}, bool optimized = false) {
  mmltk::common::logging::debug([&](auto& logger) {
   if (optimized) graph = torch::jit::lastExecutedOptimizedGraph();
   std::ostringstream metadata;
   for (const auto& input : inputs) metadata << input.device() << ':' << input.scalar_type() << ':' << input.sizes() << ';';
   std::size_t operators = 0;
   std::size_t fusion_groups = 0;
   std::size_t tensor_expr_groups = 0;
   if (graph) {
    const auto count = [&](const auto& self, const torch::jit::Block* block) -> void {
     for (const auto* node : block->nodes()) {
      ++operators;
      const std::string_view kind = node->kind().toQualString();
      if (kind == "prim::TensorExprGroup") ++tensor_expr_groups;
      if (kind == "prim::FusionGroup" || kind == "prim::CudaFusionGroup" || kind == "prim::TensorExprGroup") ++fusion_groups;
      for (const auto* nested : node->blocks()) self(self, nested);
      if (node->hasAttribute(torch::jit::attr::Subgraph)) self(self, node->g(torch::jit::attr::Subgraph)->block());
     }
    };
    count(count, graph->block());
   }
   logger.debug(
    "rfdetr compilation region={} owner={} reason={} training={} signature={} grad={} amp={} amp_cache={} amp_dtype={} torch={} executor_optimize={} fuse_cpu={} fuse_gpu={} tf32_matmul={} "
    "tf32_cudnn={} "
    "graph_available={} operators={} fusion_groups={} tensor_expr_groups={}",
    name_, static_cast<const void*>(this), reason, training, metadata.str(), signature.grad, signature.amp, signature.amp_cache, static_cast<int>(signature.amp_dtype), TORCH_VERSION,
    torch::jit::getGraphExecutorOptimize(), torch::jit::canFuseOnCPU(), torch::jit::canFuseOnGPU(), at::globalContext().allowTF32CuBLAS(), at::globalContext().allowTF32CuDNN(),
    static_cast<bool>(graph), operators, fusion_groups, tensor_expr_groups);
  });
 };
 if (!compatible_extents) {
  diagnostic("incompatible tensor extents; ordinary fallback");
  return {ordinary(inputs), false};
 }
 if (!dynamic_batch_queries_) {
  for (const auto& input : inputs) {
   if (input.dim() == 0 || input.size(0) != slot.batch_size) {
    diagnostic("prepared batch mismatch; ordinary fallback");
    return {ordinary(inputs), false};
   }
  }
 }
 if (slot.state && (slot.state->signature.attention_policy != signature.attention_policy || slot.state->signature.attention_priority != signature.attention_priority ||
                    std::ranges::any_of(slot.state->members, [](const auto& member) { return state_signature(member.tensor) != member.signature; }))) {
  diagnostic("registered owner or attention policy changed; invalidated");
  slot.state.reset();
  slot.armed = true;
 }
 if (!slot.armed && slot.state) {
  if (slot.state->signature != signature) {
   diagnostic("signature mismatch; ordinary fallback");
   return {ordinary(inputs), false};
  }
  torch::jit::Stack arguments;
  for (const auto& input : inputs) arguments.emplace_back(input);
  auto output = run_recorded_forward(slot.state->module, std::move(arguments), inputs.front().device().type());
  diagnostic("reuse; optimized graph when available", {}, true);
  return {std::move(output), false};
 }
 try {
  auto cu = std::make_shared<torch::jit::CompilationUnit>();
  auto type = torch::jit::ClassType::create("__torch__.TensorTrace", cu, true);
  auto candidate = std::make_unique<State>(torch::jit::Module(cu, type), signature);
  std::size_t member = 0;
  for (auto* owner : owners) {
   if (!owner) continue;
   for (const auto& parameter : owner->named_parameters(true)) {
    candidate->module.register_parameter("parameter_" + std::to_string(member++), parameter.value(), false);
    candidate->members.push_back({parameter.value(), state_signature(parameter.value())});
   }
   for (const auto& buffer : owner->named_buffers(true)) {
    candidate->module.register_buffer("buffer_" + std::to_string(member++), buffer.value());
    candidate->members.push_back({buffer.value(), state_signature(buffer.value())});
   }
  }
  torch::jit::Stack arguments;
  for (const auto& input : inputs) arguments.emplace_back(input);
  const auto recording = [&] {
   return torch::jit::tracer::trace(std::move(arguments), [&](torch::jit::Stack values) {
    if (signature.amp_cache) {
     const auto device = inputs.front().device().type();
     for (const auto& input : inputs) trace_cached_cast(input, device, signature.amp_dtype);
     for (const auto& owned : candidate->members) trace_cached_cast(owned.tensor, device, signature.amp_dtype);
    }
    std::array<torch::Tensor, 2> tensors;
    for (std::size_t index = 0; index < values.size(); ++index) tensors[index] = values[index].toTensor();
    auto output = ordinary(std::span(tensors.data(), values.size()));
    values.clear();
    values.emplace_back(std::move(output));
    return values;
   }, [](const torch::autograd::Variable&) { return ""; }, false, false, &candidate->module);
  }();
  install_recorded_forward(candidate->module, *cu, recording.first->graph);
  auto output = recording.second.front();
  diagnostic("prepared; recorded graph", recording.first->graph);
  candidate->preparation_batch = slot.batch_size;
  slot.state = std::move(candidate);
  slot.armed = false;
  return {std::move(output), true};
 } catch (...) {
  // A failed replacement leaves the previous admitted request and graph usable.
  // The failing operation is never replayed as ordinary GPU work.
  if (slot.state) {
   slot.batch_size = slot.state->preparation_batch;
   slot.armed = false;
  }
  throw;
 }
}
}  // namespace mmltk::backend::models::rfdetr::detail
