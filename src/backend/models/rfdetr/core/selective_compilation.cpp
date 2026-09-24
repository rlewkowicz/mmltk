#include "detail/selective_compilation.h"
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
 std::array<TensorSignature, SelectiveTensorRegion::Tensors::capacity()> tensors{};
 size_t count = 0;
 bool grad = torch::GradMode::is_enabled();
 bool amp = false;
 c10::ScalarType amp_dtype = c10::ScalarType::Undefined;
 bool operator==(const Signature&) const = default;
};
Signature signature_of(const SelectiveTensorRegion::Tensors& inputs, bool dynamic) {
 Signature result;
 for (const auto& input : inputs) {
  TORCH_CHECK(input.defined() && input.dim() <= 4, "selective tensor region requires defined tensors of rank at most four");
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
 if (result.amp) result.amp_dtype = at::autocast::get_autocast_dtype(device_type);
 return result;
}
class RecordingAutocastCache final {
public:
 RecordingAutocastCache() : previous_(at::autocast::is_autocast_cache_enabled()) { at::autocast::set_autocast_cache_enabled(false); }
 ~RecordingAutocastCache() { at::autocast::set_autocast_cache_enabled(previous_); }

private:
 bool previous_;
};
SelectiveTensorRegion::Tensors unpack(const c10::IValue& value) {
 SelectiveTensorRegion::Tensors result;
 const auto& elements = value.toTupleRef().elements();
 TORCH_CHECK(!elements.empty() && elements.size() <= SelectiveTensorRegion::Tensors::capacity(), "selective tensor region requires one or two outputs");
 for (const auto& item : elements) result.push_back(item.toTensor());
 return result;
}
}  // namespace
struct SelectiveTensorRegion::State {
 torch::jit::Module module;
 Signature signature;
 torch::Tensor live_reference;
 c10::Device parameter_device{torch::kCPU};
 std::int64_t preparation_batch = 0;
};
SelectiveTensorRegion::SelectiveTensorRegion(std::string name, bool dynamic) : name_(std::move(name)), dynamic_batch_queries_(dynamic) {}
SelectiveTensorRegion::~SelectiveTensorRegion() = default;
void SelectiveTensorRegion::prepare(bool training, bool enabled, std::int64_t batch_size) {
 TORCH_CHECK(batch_size > 0, "selective preparation requires a positive batch size");
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
void SelectiveTensorRegion::invalidate() {
 for (auto& slot : slots_) {
  slot.state.reset();
  slot.armed = slot.enabled;
 }
}
const void* SelectiveTensorRegion::identity(bool training) const noexcept { return slots_[training].state.get(); }
SelectiveTensorRegion::Tensors SelectiveTensorRegion::invoke(bool training, const Tensors& inputs, std::initializer_list<torch::nn::Module*> owners, Operation ordinary) {
 auto& slot = slots_[training];
 if (!slot.enabled) return ordinary(inputs);
 TORCH_CHECK(!inputs.empty() && inputs.size() <= Tensors::capacity(), "selective tensor region requires one or two inputs");
 bool compatible_extents = true;
 if (dynamic_batch_queries_) {
  for (const auto& input : inputs) compatible_extents &= input.dim() == 3 && input.sizes() == inputs.front().sizes() && input.device() == inputs.front().device();
 }
 const auto signature = signature_of(inputs, dynamic_batch_queries_ && compatible_extents);
 auto diagnostic = [&](const char* reason, std::shared_ptr<torch::jit::Graph> graph = {}, bool optimized = false) {
  mmltk::common::logging::debug([&](auto& logger) {
   if (optimized) graph = torch::jit::lastExecutedOptimizedGraph();
   std::ostringstream metadata;
   for (const auto& input : inputs) metadata << input.device() << ':' << input.scalar_type() << ':' << input.sizes() << ';';
   std::size_t operators = 0;
   std::size_t fusion_groups = 0;
   if (graph) {
    const auto count = [&](const auto& self, const torch::jit::Block* block) -> void {
     for (const auto* node : block->nodes()) {
      ++operators;
      if (node->kind().toQualString() == std::string_view("prim::FusionGroup") || node->kind().toQualString() == std::string_view("prim::CudaFusionGroup")) ++fusion_groups;
      for (const auto* nested : node->blocks()) self(self, nested);
      if (node->hasAttribute(torch::jit::attr::Subgraph)) self(self, node->g(torch::jit::attr::Subgraph)->block());
     }
    };
    count(count, graph->block());
   }
   logger.debug(
    "rfdetr compilation region={} owner={} reason={} training={} signature={} grad={} amp={} amp_dtype={} torch={} executor_optimize={} fuse_cpu={} fuse_gpu={} tf32_matmul={} tf32_cudnn={} "
    "graph_available={} operators={} fusion_groups={}",
    name_, static_cast<const void*>(this), reason, training, metadata.str(), signature.grad, signature.amp, static_cast<int>(signature.amp_dtype), TORCH_VERSION,
    torch::jit::getGraphExecutorOptimize(), torch::jit::canFuseOnCPU(), torch::jit::canFuseOnGPU(), at::globalContext().allowTF32CuBLAS(), at::globalContext().allowTF32CuDNN(),
    static_cast<bool>(graph), operators, fusion_groups);
  });
 };
 if (!compatible_extents) {
  diagnostic("incompatible tensor extents; ordinary fallback");
  return ordinary(inputs);
 }
 if (!dynamic_batch_queries_) {
  for (const auto& input : inputs) {
   if (input.dim() == 0 || input.size(0) != slot.batch_size) {
    diagnostic("prepared batch mismatch; ordinary fallback");
    return ordinary(inputs);
   }
  }
 }
 if (slot.state && slot.state->live_reference.defined() && slot.state->live_reference.device() != slot.state->parameter_device) {
  diagnostic("registered owner moved device; invalidated");
  slot.state.reset();
  slot.armed = true;
 }
 if (!slot.armed && slot.state) {
  if (slot.state->signature != signature) {
   diagnostic("signature mismatch; ordinary fallback");
   return ordinary(inputs);
  }
  torch::jit::Stack arguments;
  for (const auto& input : inputs) arguments.emplace_back(input);
  auto output = unpack(slot.state->module.forward(std::move(arguments)));
  diagnostic("reuse; optimized graph when available", {}, true);
  return output;
 }
 try {
  auto cu = std::make_shared<torch::jit::CompilationUnit>();
  auto type = torch::jit::ClassType::create("__torch__.SelectiveTensorRegion", cu, true);
  auto candidate = std::make_unique<State>(torch::jit::Module(cu, type), signature);
  std::size_t member = 0;
  for (auto* owner : owners) {
   if (!owner) continue;
   for (const auto& parameter : owner->named_parameters(true)) {
    candidate->module.register_parameter("parameter_" + std::to_string(member++), parameter.value(), false);
    if (!candidate->live_reference.defined()) candidate->live_reference = parameter.value();
   }
   for (const auto& buffer : owner->named_buffers(true)) {
    candidate->module.register_buffer("buffer_" + std::to_string(member++), buffer.value());
    if (!candidate->live_reference.defined()) candidate->live_reference = buffer.value();
   }
  }
  if (candidate->live_reference.defined()) candidate->parameter_device = candidate->live_reference.device();
  torch::jit::Stack arguments;
  for (const auto& input : inputs) arguments.emplace_back(input);
  const auto recording = [&] {
   RecordingAutocastCache cache;
   return torch::jit::tracer::trace(
    std::move(arguments),
    [&](torch::jit::Stack values) {
     Tensors tensors;
     for (const auto& value : values) tensors.push_back(value.toTensor());
     std::vector<c10::IValue> outputs;
     for (auto& output : ordinary(tensors)) outputs.emplace_back(std::move(output));
     values.clear();
     values.emplace_back(c10::ivalue::Tuple::create(std::move(outputs)));
     return values;
    },
    [](const torch::autograd::Variable&) { return ""; }, false, false, &candidate->module);
  }();
  candidate->module.type()->addMethod(cu->create_function("forward", recording.first->graph, true));
  auto output = unpack(recording.second.front());
  diagnostic("prepared; recorded graph", recording.first->graph);
  candidate->preparation_batch = slot.batch_size;
  slot.state = std::move(candidate);
  slot.armed = false;
  return output;
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
