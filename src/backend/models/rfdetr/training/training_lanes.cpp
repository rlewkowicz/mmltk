#include "detail/training_step.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_lanes.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include <torch/csrc/autograd/autograd.h>
#include <ATen/ops/_foreach_copy.h>
#include <algorithm>
#include "src/common/math/checked_arithmetic.h"
#include <stdexcept>
#include <sstream>
#include <cmath>
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
using mmltk::common::math::checked_cast;
struct TrainLaneContext {
 TrainLaneContext(torch_cuda::TorchCudaStream lane_stream, const std::size_t staging_depth) : stream(std::move(lane_stream)), target_scratch(staging_depth) {}
 torch_cuda::TorchCudaStream stream;
 at::cuda::CUDAEvent ready;
 std::unique_ptr<GpuBatchAugmenter> augmenter;
 TargetScratch target_scratch;
 std::shared_ptr<NativeRfDetrModel> model;
 std::vector<torch::Tensor> grad_params;
 std::vector<torch::Tensor> copy_sources, copy_destinations;
 std::vector<TrainingDonorDescriptor> donors;
 size_t synced_parameter_version = std::numeric_limits<size_t>::max();
};
std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> record_current_stream_event(mmltk::backend::ml::cuda::CudaEventPool& event_pool, const int device_id, const char* context) {
 return event_pool.record(reinterpret_cast<std::uintptr_t>(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)).stream()), context);
}
void wait_for_lane_result(TrainLaneResult& lane_result, int device_id) {
 if (!lane_result.ready_event) { throw std::runtime_error("parallel RF-DETR train result is missing a completion event"); }
 lane_result.ready_event->block(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)));
 lane_result.ready_event = nullptr;
}
void record_lane_result_on_current_stream(const TrainLaneResult& lane_result, int device_id) {
 const auto stream = torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id));
 const auto record_tensor = [&stream](const torch::Tensor& value) {
  if (value.defined() && value.device().is_cuda()) { value.record_stream(stream); }
 };
 record_tensor(lane_result.loss);
 record_tensor(lane_result.class_loss);
 record_tensor(lane_result.box_loss);
 for (const auto& scalar : lane_result.scalars) record_tensor(scalar);
 for (const auto& value : lane_result.diagnostics) record_tensor(value);
 for (const auto& [name, value] : lane_result.loss_terms) record_tensor(value);
}
void TrainingLossReport::begin_attempt() { values_ = {}; }
void TrainingLossReport::accumulate(TensorMap terms) {
 // One scalar per criterion term, independent of logical K; never keep a
 // graph. Batch the device selection instead of launching kernels per term.
 torch::NoGradGuard no_grad;
 if (names_.empty()) {
  names_.reserve(terms.size()); incoming_.reserve(terms.size());
  for (const auto& [name, value] : terms) names_.push_back(name);
 }
 if (terms.size() != names_.size()) throw std::logic_error("training criterion report schema changed within an attempt");
 incoming_.clear();
 std::size_t index = 0;
 for (const auto& [name, value] : terms) {
  if (name != names_[index++]) throw std::logic_error("training criterion report term changed within an attempt");
  incoming_.push_back(value);
 }
 if (incoming_.empty()) return;
 auto incoming = torch::stack(incoming_);
 incoming_.clear();
 // The first nonfinite value survives later healthy contributions. Nothing
 // crosses to the host until the globally agreed failure/overflow path.
 values_ = values_.defined() ? torch::where(torch::isfinite(values_), incoming, values_) : std::move(incoming);
}
std::string TrainingLossReport::details(const std::vector<torch::Tensor>& parameters, const std::vector<std::string>& parameter_names) const {
 std::ostringstream report;
 bool wrote_loss = false;
 if (values_.defined()) {
  const auto host = values_.to(torch::kCPU, torch::kFloat64).contiguous();
  const auto* values = host.const_data_ptr<double>();
  for (std::size_t index = 0; index < names_.size(); ++index) {
   if (std::isfinite(values[index])) continue;
   report << (wrote_loss ? ", " : " nonfinite_losses=[") << names_[index] << "=" << values[index];
   wrote_loss = true;
  }
 }
 if (wrote_loss) report << "]";
 for (size_t index = 0; index < parameters.size(); ++index) {
  const auto& param = parameters[index];
  if (param.defined() && !torch::isfinite(param).all().item<bool>()) {
   report << " nonfinite_param=" << parameter_names[index];
   break;
  }
  if (param.grad().defined() && !torch::isfinite(param.grad()).all().item<bool>()) {
   report << " nonfinite_grad=" << parameter_names[index];
   break;
  }
 }
 return report.str();
}
std::runtime_error TrainingLossReport::failure(const std::vector<torch::Tensor>& parameters, const std::vector<std::string>& names) const {
 return std::runtime_error("non-finite RF-DETR loss or gradients encountered during native training" + details(parameters, names));
}
void copy_module_state(NativeRfDetrModel& destination, const NativeRfDetrModel& source) {
 torch::NoGradGuard no_grad;
 auto destination_parameters = destination.named_parameters(true);
 for (const auto& item : source.named_parameters(true)) {
  auto* target = destination_parameters.find(item.key());
  if (target == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing parameter: " + item.key()); }
  target->copy_(item.value());
  target->requires_grad_(item.value().requires_grad());
 }
 auto destination_buffers = destination.named_buffers(true);
 for (const auto& item : source.named_buffers(true)) {
  auto* target = destination_buffers.find(item.key());
  if (target == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing buffer: " + item.key()); }
  target->copy_(item.value());
 }
}
std::shared_ptr<NativeRfDetrModel> make_train_lane_model(NativeRfDetrModel& model, int device_id) {
 auto lane_model = std::make_shared<NativeRfDetrModel>(model.config(), model.class_layout()->record());
 auto& lane_module = (*lane_model);
 lane_module.to(mmltk::backend::ml::cuda::cuda_device(device_id));
 lane_module.train();
 copy_module_state(lane_module, (model));
 (*lane_model).replicate_training_supervision_runtime_from((model));
 return lane_model;
}
std::vector<torch::Tensor> lane_grad_parameters(const NativeRfDetrModel& lane_model, const std::vector<std::string>& parameter_names) {
 const auto named_parameters = lane_model.named_parameters(true);
 std::vector<torch::Tensor> parameters;
 parameters.reserve(parameter_names.size());
 for (const auto& name : parameter_names) {
  auto* tensor = named_parameters.find(name);
  if (tensor == nullptr) { throw std::runtime_error("parallel RF-DETR lane is missing trainable parameter: " + name); }
  parameters.push_back(*tensor);
 }
 return parameters;
}
std::optional<std::string> first_running_stats_buffer_name(NativeRfDetrModel& model) {
 for (const auto& item : model.named_buffers(true)) {
  if (item.key().find("running_mean") != std::string::npos || item.key().find("running_var") != std::string::npos || item.key().find("num_batches_tracked") != std::string::npos) { return item.key(); }
 }
 return std::nullopt;
}
void ensure_train_lane_model_supported(NativeRfDetrModel& model, int train_lane_count) {
 if (train_lane_count <= 1) { return; }
 const auto running_stats = first_running_stats_buffer_name(model);
 if (running_stats.has_value()) { throw std::runtime_error("parallel RF-DETR train --lanes requires a model without running-stat buffers; found " + *running_stats); }
}
struct TrainingLanes::Impl {
 at::cuda::CUDAEvent source_ready;
 std::deque<TrainLaneContext> lanes;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
};
TrainingLanes::TrainingLanes(const TrainRequest& options, RuntimeContext& train_runtime, mmltk::backend::data::DatasetLoader& train_loader, std::shared_ptr<NativeRfDetrModel> model_owner,
 const std::vector<std::string>& all_param_names, int train_lane_count, std::size_t local_batch, const mmltk::frameworks::gpu::DeviceContext& augmentation_context)
    : impl_(std::make_shared<Impl>()) {
 auto& model = *model_owner;
 auto& train_lane_pool = impl_->pool;
 auto& train_lanes = impl_->lanes;
 try {
  if (!local_batch) {
   train_lanes.emplace_back(torch_cuda::get_priority_cuda_stream(options.device_id, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()), 1);
   train_lanes.back().model = model_owner;
   train_lanes.back().grad_params = lane_grad_parameters(model, all_param_names);
   return;
  }
  train_lane_pool =
   std::make_unique<mmltk::common::concurrency::WorkerPool>(static_cast<size_t>(train_lane_count), train_runtime.lane_cpus(), "rfdtrtlane", 0U, &train_runtime.execution().placement, false);
  for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
   train_lanes.emplace_back(
    torch_cuda::get_priority_cuda_stream(options.device_id, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()), static_cast<std::size_t>(std::max(1, options.grad_accum_steps)));
  }
  for (auto& lane : train_lanes) {
   lane.donors.reserve(local_batch);
   lane.augmenter = std::make_unique<GpuBatchAugmenter>(
    options.gpu_augmentation, static_cast<std::int64_t>(local_batch), static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()), augmentation_context);
   lane.model = train_lane_count == 1 ? model_owner : make_train_lane_model(model, options.device_id);
   if (train_lane_count > 1) {
    const auto append = [&](const auto& source, const auto& destination) {
     for (const auto& item : source) { lane.copy_sources.push_back(item.value()); lane.copy_destinations.push_back(destination[item.key()]); }
    };
    append(model.named_parameters(true), lane.model->named_parameters(true));
    append(model.named_buffers(true), lane.model->named_buffers(true));
   }
   (*lane.model)
    .configure_supervision_timing(
     SupervisionTimingSetup{mmltk::backend::ml::cuda::cuda_device(options.device_id), static_cast<std::size_t>(derive_execution_facts(options, 0).microbatches_per_attempt), mmltk::common::logging::profile_enabled()});
   lane.model->optimize_for_inference(checked_cast<int>(local_batch, "batch_size exceeds supported inference compilation range"), true, options.compilation_mode);
   lane.grad_params = lane_grad_parameters(*lane.model, all_param_names);
  }
 } catch (...) { retire(); throw; }
}
TrainingLanes::~TrainingLanes() { retire(); }
void TrainingLanes::retire() noexcept {
 if (!impl_) return;
 impl_->pool.reset();
 cudaError_t failure = cudaSuccess;
 for (const auto& lane : impl_->lanes) {
  try {
   torch_cuda::TorchCudaDeviceGuard device(lane.stream.device_index());
   const auto status = cudaStreamSynchronize(lane.stream.stream());
   if (failure == cudaSuccess) failure = status;
  } catch (...) { if (failure == cudaSuccess) failure = cudaErrorUnknown; }
 }
 if (failure != cudaSuccess) std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), failure);
}
void TrainingLanes::reconfigure(NativeRfDetrModel& source, const std::vector<std::string>& names, const GpuAugmentationConfig& augmentation, int batch_size, CompilationMode mode) {
 // Called only at a drained epoch boundary. Replicas and allocations are retained.
 settle_targets();
 impl_->source_ready.record(torch_cuda::getCurrentCUDAStream(impl_->lanes.front().stream.device_index()));
 for (auto& lane : impl_->lanes) {
  impl_->source_ready.block(lane.stream);
  torch_cuda::TorchCudaStreamGuard stream_guard(lane.stream);
  copy_module_state(*lane.model, source);
  lane.grad_params = lane_grad_parameters(*lane.model, names);
  lane.model->invalidate_compilation();
  lane.model->optimize_for_inference(batch_size, true, mode);
  if (lane.augmenter) lane.augmenter->reconfigure(augmentation);
  lane.synced_parameter_version = std::numeric_limits<size_t>::max();
 }
}
std::future<TrainLaneResult> TrainingLanes::enqueue(RuntimeContext* runtime, mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch,
 const mmltk::backend::ml::cuda::CudaEventPool::Lease* params_ready, std::size_t admitted_microbatches, double gradient_scale,
 size_t parameter_version, const DetectionConfig& detection_config, const NativeRfDetrModel& model, int device_id, int image_height, int image_width, std::uint64_t seed, int epoch, int rank,
 std::uint64_t augmentation_sequence, bool amp_enabled, at::ScalarType autocast_dtype, TrainingSupervisionRoute route, std::shared_ptr<TrainingTargetCounts> normalizer, TrainingGradientReducer& reducer, std::size_t lane_index, std::span<const TrainingDonorDescriptor> donors) {
 auto& lane = impl_->lanes.at(lane_index);
 lane.donors.assign(donors.begin(), donors.end());
 auto& lane_pool = *impl_->pool;
 return lane_pool.enqueue([runtime, &loader, &lane, batch, params_ready, admitted_microbatches, gradient_scale, parameter_version, &detection_config, &model, device_id, image_height,
                           image_width, seed, epoch, rank, augmentation_sequence, amp_enabled, autocast_dtype, route, normalizer = std::move(normalizer), &reducer, lane_index]() mutable {
  try {
   ScopedRuntimeContext worker_scope(runtime, lane_index + 1);
   torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(device_id));
   torch_cuda::TorchCudaStreamGuard stream_guard(lane.stream);
   LoaderBatchGuard batch_guard(loader, batch, device_id);
   torch::Tensor normalized;
   {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_augment{"rfdetr.train.parallel.augment"};
    mmltk::common::logging::ScopedProfile profile_benchmark_rfdetr_train_augmentation{"benchmark.rfdetr.train.augmentation"};
    if (!lane.augmenter) { throw std::runtime_error("parallel RF-DETR train lane is missing its GPU augmenter"); }
    normalized = lane.augmenter->run(batch, seed, epoch, rank, augmentation_sequence, &loader, lane.donors);
   }
   PreparedTargets prepared;
   {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets{"rfdetr.train.parallel.targets"};
    prepared = build_targets(batch, image_height, image_width, detection_config.include_masks, detection_config.include_masks, device_id, lane.target_scratch, "train", model.config().num_queries,
     model.config().training_supervision, static_cast<int>(model.class_layout()->catalog()->size()), &lane.augmenter->batch_plan());
   }
   const auto target_count = prepared_target_count(prepared);
   normalizer->publish(lane_index, target_count);
   batch_guard.set_consumer_stream(lane.augmenter->prepare_batch_consumer());
   static_cast<void>(lane.augmenter->finish_batch(batch));
   batch_guard.release();
   if (!lane.model) { throw std::runtime_error("parallel RF-DETR train lane is missing its model replica"); }
   if (lane.synced_parameter_version != parameter_version) {
    if (params_ready) { params_ready->wait(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "wait for parallel train parameter readiness"); }
    torch::NoGradGuard no_grad;
    if (!lane.copy_sources.empty()) at::_foreach_copy_(lane.copy_destinations, lane.copy_sources);
    lane.synced_parameter_version = parameter_version;
   }
   (*lane.model).train();
   torch::Tensor detached_loss;
   torch::Tensor detached_class_loss;
   torch::Tensor detached_box_loss;
   scalar_packet::Tensors scalar_values;
   TrainingDiagnosticTensors diagnostics;
   TensorMap loss_terms;
   {
    RoutedTrainingLoss loss_result;
    TargetConsumerLease target_consumer(lane.target_scratch, prepared, device_id);
    auto& lane_owner = (*lane.model);
    SupervisionTimingLease step_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Step);
    const TrainingStep step(admitted_microbatches, gradient_scale, amp_enabled, autocast_dtype);
    step.forward([&] {
     ModelOutputs outputs;
     if (route_uses_denoising(route)) {
      mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{"rfdetr.train.parallel.targets_handoff"};
      target_consumer.handoff();
      outputs = (*lane.model)
                 .forward_with_denoising(
                  NestedTensor{normalized, prepared.nested_mask}, prepared, TrainingStepIdentity{seed, static_cast<std::uint64_t>(epoch), static_cast<std::uint32_t>(rank), augmentation_sequence});
     } else {
      mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_forward{"rfdetr.train.parallel.forward"};
      outputs = route_uses_match_free(route) ? (*lane.model).forward_for_match_free(NestedTensor{normalized, prepared.nested_mask})
                                             : (*lane.model).forward(NestedTensor{normalized, prepared.nested_mask}, true);
     }
     if (!route_uses_denoising(route)) {
      mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{"rfdetr.train.parallel.targets_handoff"};
      target_consumer.handoff();
     }
     auto global_count = normalizer->consume(lane_index, lane.stream.stream());
     SupervisionTimingLease criterion_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Criterion);
     loss_result = compute_routed_training_loss(*lane.model, route, outputs, prepared, global_count, detection_config);
     criterion_timing.finish();
    });
    {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_grad{"rfdetr.train.parallel.grad"};
     reducer.arm(lane_index);
     // Hooks retain each produced tensor until its bucket accumulation has been
     // enqueued. The returned autograd inventory is never transported to main.
     static_cast<void>(step.gradients(loss_result.total, lane.grad_params));
     reducer.collect(lane_index);
     runtime->matcher_workspace().complete_assignments(lane.stream.stream());
    }
    target_consumer.retire();
    step_timing.finish();
    detached_loss = loss_result.total.detach();
    detached_class_loss = loss_result.classification.detach();
    detached_box_loss = loss_result.box.detach();
    scalar_values = std::move(loss_result.scalars);
    diagnostics = std::move(loss_result.diagnostics);
    loss_terms = std::move(loss_result.ordinary_terms);
    for (auto& [name, value] : loss_terms) value = value.detach();
   }
   lane.ready.record(lane.stream);
   return TrainLaneResult{
    std::move(detached_loss),
    std::move(detached_class_loss),
    std::move(detached_box_loss),
    std::move(scalar_values),
    std::move(diagnostics),
    std::move(loss_terms),
    &lane.ready,
   };
  } catch (...) {
   normalizer->fail(std::current_exception());
   reducer.abort(std::current_exception());
   throw;
  }
 });
}
void TrainingLanes::settle(TrainLaneResult& result, int device_id) { wait_for_lane_result(result, device_id); record_lane_result_on_current_stream(result, device_id); }
std::vector<std::vector<torch::Tensor>> TrainingLanes::gradient_leaves() const {
 std::vector<std::vector<torch::Tensor>> leaves; leaves.reserve(impl_->lanes.size());
 for (const auto& lane : impl_->lanes) leaves.push_back(lane.grad_params);
 return leaves;
}
void TrainingLanes::harvest_timing() {
 for (auto& lane : impl_->lanes) static_cast<void>(lane.model->harvest_supervision_timing());
}
void TrainingLanes::settle_targets() {
 for (auto& lane : impl_->lanes) lane.target_scratch.wait_for_pending_copy();
}
}  // namespace mmltk::backend::models::rfdetr
