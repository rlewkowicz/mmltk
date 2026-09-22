#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_lanes.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include <torch/csrc/autograd/autograd.h>
#include <algorithm>
#include "src/common/math/checked_arithmetic.h"
#include <stdexcept>
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
using mmltk::common::math::checked_cast;
struct TrainLaneContext {
 TrainLaneContext(torch_cuda::TorchCudaStream lane_stream, const std::size_t staging_depth) : stream(std::move(lane_stream)), target_scratch(staging_depth) {}
 torch_cuda::TorchCudaStream stream;
 std::unique_ptr<GpuBatchAugmenter> augmenter;
 TargetScratch target_scratch;
 std::shared_ptr<NativeRfDetrModel> model;
 std::vector<torch::Tensor> grad_params;
 size_t synced_parameter_version = std::numeric_limits<size_t>::max();
};
std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> record_current_stream_event(mmltk::backend::ml::cuda::CudaEventPool& event_pool, const int device_id, const char* context) {
 return event_pool.record(reinterpret_cast<std::uintptr_t>(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)).stream()), context);
}
void wait_for_lane_result(TrainLaneResult& lane_result, int device_id) {
 if (!lane_result.ready_event) { throw std::runtime_error("parallel RF-DETR train result is missing a completion event"); }
 lane_result.ready_event->wait(
  reinterpret_cast<std::uintptr_t>(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id)).stream()), "wait for parallel train lane result");
 lane_result.ready_event->retire();
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
 for (const auto& gradient : lane_result.gradients) { record_tensor(gradient); }
}
void merge_lane_gradients(TrainLaneResult& lane_result, std::vector<torch::Tensor>& parameters, int device_id) {
 wait_for_lane_result(lane_result, device_id);
 record_lane_result_on_current_stream(lane_result, device_id);
 torch::NoGradGuard no_grad;
 const size_t limit = std::min(parameters.size(), lane_result.gradients.size());
 for (size_t index = 0; index < limit; ++index) {
  const auto& gradient = lane_result.gradients[index];
  if (!gradient.defined()) { continue; }
  if (!parameters[index].grad().defined()) {
   parameters[index].mutable_grad() = gradient.detach().clone();
  } else {
   parameters[index].mutable_grad().add_(gradient);
  }
 }
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
 std::deque<TrainLaneContext> lanes;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
};
TrainingLanes::TrainingLanes(const TrainRequest& options, RuntimeContext& train_runtime, mmltk::backend::data::DatasetLoader& train_loader, NativeRfDetrModel& model,
 const std::vector<std::string>& all_param_names, int train_lane_count, const mmltk::frameworks::gpu::DeviceContext& augmentation_context)
    : impl_(std::make_unique<Impl>()) {
 auto& train_lane_pool = impl_->pool;
 auto& train_lanes = impl_->lanes;
 if (train_lane_count > 1) {
  train_lane_pool =
   std::make_unique<mmltk::common::concurrency::WorkerPool>(static_cast<size_t>(train_lane_count), train_runtime.lane_cpus(), "rfdtrtlane", 0U, &train_runtime.execution().placement, false);
  for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
   train_lanes.emplace_back(
    torch_cuda::get_priority_cuda_stream(options.device_id, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()), static_cast<std::size_t>(std::max(1, options.grad_accum_steps)));
  }
  for (auto& lane : train_lanes) {
   lane.augmenter = std::make_unique<GpuBatchAugmenter>(
    options.gpu_augmentation, static_cast<std::int64_t>(options.batch_size), static_cast<int>(train_loader.image_height()), static_cast<int>(train_loader.image_width()), augmentation_context);
   lane.model = make_train_lane_model(model, options.device_id);
   (*lane.model)
    .configure_supervision_timing(
     SupervisionTimingSetup{mmltk::backend::ml::cuda::cuda_device(options.device_id), static_cast<std::size_t>(std::max(1, options.grad_accum_steps)), mmltk::common::logging::profile_enabled()});
   lane.model->optimize_for_inference(checked_cast<int>(std::max<std::size_t>(1, options.batch_size), "batch_size exceeds supported inference compilation range"), true, options.compilation_mode);
   lane.grad_params = lane_grad_parameters(*lane.model, all_param_names);
  }
 }
}
TrainingLanes::~TrainingLanes() = default;
std::future<TrainLaneResult> TrainingLanes::enqueue(RuntimeContext* runtime, mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch,
 const mmltk::backend::ml::cuda::CudaEventPool::Lease* params_ready, mmltk::backend::ml::cuda::CudaEventPool& event_pool, double scaled_loss_factor, size_t parameter_version,
 const DetectionConfig& detection_config, const NativeRfDetrModel& model, int device_id, int image_height, int image_width, std::uint64_t seed, int epoch, int rank,
 std::uint64_t augmentation_sequence, bool amp_enabled, at::ScalarType autocast_dtype, TrainingSupervisionRoute route, std::shared_ptr<WaveTargetNormalizer> wave_normalizer, std::size_t lane_index) {
 auto& lane = impl_->lanes.at(lane_index);
 auto& lane_pool = *impl_->pool;
 return lane_pool.enqueue([runtime, &loader, &lane, batch, params_ready, &event_pool, scaled_loss_factor, parameter_version, &detection_config, &model, device_id, image_height, image_width, seed,
                           epoch, rank, augmentation_sequence, amp_enabled, autocast_dtype, route, wave_normalizer = std::move(wave_normalizer), lane_index]() mutable {
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
    normalized = lane.augmenter->run(batch, seed, epoch, rank, augmentation_sequence);
   }
   PreparedTargets prepared;
   {
    mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets{"rfdetr.train.parallel.targets"};
    prepared = build_targets(batch, image_height, image_width, detection_config.include_masks, detection_config.include_masks, device_id, lane.target_scratch, "train", model.config().num_queries,
     model.config().training_supervision, static_cast<int>(model.class_layout()->catalog()->size()), &lane.augmenter->batch_plan());
   }
   const auto target_count = prepared_target_count(prepared);
   if (wave_normalizer) { wave_normalizer->publish(lane_index, target_count); }
   batch_guard.set_consumer_stream(lane.augmenter->prepare_batch_consumer());
   static_cast<void>(lane.augmenter->finish_batch(batch));
   batch_guard.release();
   if (!lane.model) { throw std::runtime_error("parallel RF-DETR train lane is missing its model replica"); }
   if (lane.synced_parameter_version != parameter_version) {
    if (params_ready) { params_ready->wait(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "wait for parallel train parameter readiness"); }
    copy_module_state((*lane.model), (model));
    lane.synced_parameter_version = parameter_version;
   }
   (*lane.model).train();
   torch::Tensor detached_loss;
   torch::Tensor detached_class_loss;
   torch::Tensor detached_box_loss;
   scalar_packet::Tensors scalar_values;
   std::vector<torch::Tensor> gradients;
   {
    TensorMap loss_dict;
    torch::Tensor loss;
    torch::Tensor class_loss;
    torch::Tensor box_loss;
    mmltk::backend::ml::cuda::TorchAutocastScope autocast_guard(amp_enabled, autocast_dtype);
    TargetConsumerLease target_consumer(lane.target_scratch, prepared, device_id);
    ModelOutputs outputs;
    auto& lane_owner = (*lane.model);
    SupervisionTimingLease step_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Step);
    if (route_uses_denoising(route)) {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{"rfdetr.train.parallel.targets_handoff"};
     target_consumer.handoff();
     outputs = (*lane.model)
                .forward_with_denoising(
                 NestedTensor{normalized, prepared.nested_mask}, prepared, TrainingStepIdentity{seed, static_cast<std::uint64_t>(epoch), static_cast<std::uint32_t>(rank), augmentation_sequence});
    } else {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_forward{"rfdetr.train.parallel.forward"};
     outputs =
      route_uses_match_free(route) ? (*lane.model).forward_for_match_free(NestedTensor{normalized, prepared.nested_mask}) : (*lane.model).forward(NestedTensor{normalized, prepared.nested_mask}, true);
    }
    if (!route_uses_denoising(route)) {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_targets_handoff{"rfdetr.train.parallel.targets_handoff"};
     target_consumer.handoff();
    }
    if (route_is_active(route) || wave_normalizer) {
     if (!wave_normalizer) { throw std::runtime_error("active RF-DETR lane is missing its wave target normalizer"); }
     auto normalizer = wave_normalizer->consume(lane_index, lane.stream.stream());
     SupervisionTimingLease criterion_timing(lane_owner, route_is_active(route), SupervisionTimingLease::Kind::Criterion);
     // CLEANUP-IGNORE: The parallel lane unpacks into detached gradient work owned by this lane.
     auto routed = compute_routed_training_loss(*lane.model, route, outputs, prepared, normalizer, detection_config);
     criterion_timing.finish();
     loss = std::move(routed.total);
     class_loss = std::move(routed.classification);
     box_loss = std::move(routed.box);
     loss_dict = std::move(routed.ordinary_terms);
     scalar_values = std::move(routed.scalars);
    } else {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_loss_dict{"rfdetr.train.parallel.loss_dict"};
     const double group_divisor = detection_config.sum_group_losses ? 1.0 : static_cast<double>(detection_config.group_detr);
     const double num_boxes_value = std::max(static_cast<double>(target_count) * group_divisor / static_cast<double>(std::max<int64_t>(1, detection_config.world_size)), 1.0);
     loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, num_boxes_value);
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_loss_total{"rfdetr.train.parallel.loss_total"};
     torch::Tensor auxiliary;
     loss = weighted_detection_loss(loss_dict, detection_config, normalized.device(), &auxiliary);
     scalar_values = ordinary_scalar_tensors(loss_dict, loss, auxiliary);
     class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
     box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
    }
    {
     mmltk::common::logging::ScopedProfile profile_rfdetr_train_parallel_grad{"rfdetr.train.parallel.grad"};
     gradients = torch::autograd::grad({loss * scaled_loss_factor}, lane.grad_params, {}, std::nullopt, false, true);
     runtime->matcher_workspace().complete_assignments(lane.stream.stream());
    }
    target_consumer.retire();
    step_timing.finish();
    detached_loss = loss.detach();
    detached_class_loss = class_loss.detach();
    detached_box_loss = box_loss.detach();
   }
   return TrainLaneResult{
    std::move(detached_loss),
    std::move(detached_class_loss),
    std::move(detached_box_loss),
    std::move(scalar_values),
    std::move(gradients),
    event_pool.record(reinterpret_cast<std::uintptr_t>(lane.stream.stream()), "record parallel train lane completion event"),
   };
  } catch (...) {
   if (wave_normalizer) { wave_normalizer->fail(std::current_exception()); }
   throw;
  }
 });
}
void TrainingLanes::merge(TrainLaneResult& result, std::vector<torch::Tensor>& parameters, int device_id) { merge_lane_gradients(result, parameters, device_id); }
void TrainingLanes::harvest_timing() {
 for (auto& lane : impl_->lanes) static_cast<void>(lane.model->harvest_supervision_timing());
}
void TrainingLanes::settle_targets() {
 for (auto& lane : impl_->lanes) lane.target_scratch.wait_for_pending_copy();
}
}  // namespace mmltk::backend::models::rfdetr
