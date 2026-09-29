#include "src/test_support/cuda_test_gate.h"
#include "src/test_support/async_test_utils.hpp"
#include "training_gradient_fixture.h"
#include "src/backend/models/rfdetr/training/detail/training_ops_private.h"
#include <torch/csrc/distributed/c10d/Backend.hpp>
#include <torch/csrc/distributed/c10d/Store.hpp>
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/backend/models/rfdetr/training/detail/training_gradient_reducer.h"
#include "src/backend/models/rfdetr/training/detail/training_lanes.h"
#include "src/backend/models/rfdetr/training/detail/training_data_plan.h"
#include "src/backend/models/rfdetr/training/detail/training_metrics.h"
#include "src/backend/models/rfdetr/training/detail/training_model.h"
#include "src/backend/models/rfdetr/training/detail/training_snapshot.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/training/detail/training_step.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/backend/models/rfdetr/training/detail/model_ema.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include <ATen/cuda/CUDAEvent.h>
#include <ATen/Context.h>
#include <torch/csrc/autograd/custom_function.h>
#include <chrono>
#include <array>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <condition_variable>
#include <future>
#include <functional>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <set>
#include <atomic>
#include <semaphore>
#include <cuda.h>
namespace mmltk::backend::models::rfdetr::testsupport {
namespace tc = mmltk::backend::ml::cuda;
struct TrainingPreparationTestAccess final {
 static TrainingLanes& lanes(TrainingModel& model) { return model.test_lanes(); }
 static void observe(TrainingLanes& lanes, TrainingLanes::PreparationObserver observer) { lanes.preparation_completed_ = std::move(observer); }
};
DistributedContext TrainingDistributedTestAccess::backend(c10::intrusive_ptr<c10d::Backend> value, int rank, int device) { return DistributedContext::from_backend(rank, 2, device, std::move(value)); }
c10::intrusive_ptr<c10d::Backend> TrainingDistributedTestAccess::backend(const DistributedContext& group) { return group.backend(); }
c10::intrusive_ptr<c10d::Store> TrainingDistributedTestAccess::store(const DistributedContext& group) { return group.store(); }
std::weak_ptr<const void> TrainingDistributedTestAccess::custody(const DistributedContext& group) { return group.transport_; }
std::weak_ptr<const void> TrainingDistributedTestAccess::custody(const TrainingTargetCounts& counts) { return counts.state_; }
std::weak_ptr<const void> TrainingDistributedTestAccess::custody(const TrainingMetricHandoff& metrics) { return metrics.impl_; }
void TrainingDistributedTestAccess::retire(TrainingTargetCounts& counts) { counts.retire(); }
void TrainingDistributedTestAccess::retire(TrainingMetricHandoff& metrics) { metrics.retire(); }
bool TrainingDistributedTestAccess::terminal(const TrainingTargetCounts& counts) { return counts.retirement_.fact().terminal; }
bool TrainingDistributedTestAccess::terminal(const TrainingMetricHandoff& metrics) { return metrics.retirement_.fact().terminal; }
struct TrainingGradientReducerTestAccess {
 static std::weak_ptr<const void> custody(const TrainingGradientReducer& reducer) { return reducer.impl_; }
 static void retire(TrainingGradientReducer& reducer) { reducer.retire(); }
 static auto fact(const TrainingGradientReducer& reducer) { return reducer.retirement_.fact(); }
};
namespace {
void require(bool condition, const char* message) {
 if (!condition) throw std::runtime_error(message);
}
void finish_gradients(TrainingGradientReducer& reducer, int device) {
 static_cast<void>(reducer.finish_attempt());
 // Standalone gradient fixtures have no metric consumer; settle their caller
 // stream explicitly before exercising the same CPU usage projection.
 tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize();
 reducer.finalize_attempt();
}
void equal(const torch::Tensor& actual, const torch::Tensor& expected, const char* message) {
 require(actual.defined() == expected.defined(), message);
 if (actual.defined()) require(torch::allclose(actual, expected, 2e-5, 2e-6), message);
}
torch::OrderedDict<std::string, torch::Tensor> inventory(int device, double initial) {
 auto options = torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kFloat32);
 torch::OrderedDict<std::string, torch::Tensor> result;
 result.insert("transformer.decoder.linear.weight", torch::full({2, 2}, initial, options).set_requires_grad(true));
 result.insert("sometimes.bias", torch::full({2}, initial + .1, options).set_requires_grad(true));
 result.insert("unused.weight", torch::full({2}, initial - .1, options).set_requires_grad(true));
 result.insert("backbone.0.encoder.weight", torch::full({2, 2}, initial + .2, options));
 return result;
}
std::vector<std::vector<torch::Tensor>> lane_leaves(const NativeOptimizer& optimizer, std::size_t count) {
 std::vector<std::vector<torch::Tensor>> result(count);
 for (auto& lane : result)
  for (const auto& value : optimizer.parameters()) lane.push_back(value.detach().clone().set_requires_grad(true));
 return result;
}
std::int64_t targets(std::size_t microbatch, std::size_t image) { return microbatch == 0 ? 0 : (microbatch + image) % 3; }
torch::Tensor objective(const std::vector<torch::Tensor>& leaves, std::size_t micro, TrainingRankSlice slice, const torch::Tensor& count, int criterion) {
 // Includes background-only and rank-local unused leaves. The three distinct
 // criterion denominators are ordinary, MatchFree, and denoising.
 const auto denominator = criterion == 0 ? (count * 3).clamp_min(1) : criterion == 1 ? count.clamp_min(1) * 3 : (count * 2).clamp_min(1);
 auto loss = leaves[0].sum() * 0;
 for (std::size_t image = slice.begin; image < slice.begin + slice.count; ++image) {
  const auto coefficient = .2 + .03 * static_cast<double>(micro + image);
  loss = loss + (leaves[0] * coefficient).square().sum() / denominator;
  if (image % 2) loss = loss + leaves[1].square().sum() / denominator;
  if (leaves.size() > 3) loss = loss + leaves[3].square().sum() * .07 / denominator;
 }
 return loss;
}
void restore_optimizer(NativeOptimizer& optimizer) {
 tc::TensorReadbackBuffers readback;
 readback.Begin();
 optimizer.reserve_checkpoint(readback, 0);
 torch::serialize::OutputArchive archive;
 optimizer.save(archive, readback, 0);
 readback.Complete();
 std::stringstream stream;
 archive.save_to(stream);
 torch::serialize::InputArchive loaded;
 loaded.load_from(stream);
 optimizer.load(loaded);
}
struct BackwardGate {
 explicit BackwardGate(int device) : stream(tc::getStreamFromPool(false, tc::checked_device_index(device))) {}
 tc::TorchCudaStream stream;
 at::cuda::CUDAEvent event;
 std::mutex mutex;
 std::condition_variable changed;
 bool entered = false, released = false;
 std::exception_ptr failure;
 void enter() {
  // Queue an actual CUDA completion event behind a held host callback, only
  // after the early backward branch has run. The late autograd derivative
  // cannot proceed until the test releases this device event.
  const auto status = cudaLaunchHostFunc(stream.stream(), [](void* context) {
   auto& gate = *static_cast<BackwardGate*>(context);
   std::unique_lock lock(gate.mutex);
   gate.changed.wait(lock, [&] { return gate.released; });
  }, this);
  require(status == cudaSuccess, "could not hold late backward CUDA event");
  event.record(stream);
  {
   std::lock_guard lock(mutex);
   entered = true;
   changed.notify_all();
  }
  event.synchronize();
  std::lock_guard lock(mutex);
  if (failure) std::rethrow_exception(failure);
 }
 void release(std::exception_ptr error = {}) {
  std::lock_guard lock(mutex);
  if (error && !failure) failure = error;
  released = true;
  changed.notify_all();
 }
 bool await() {
  std::unique_lock lock(mutex);
  return changed.wait_for(lock, std::chrono::seconds(20), [&] { return entered; });
 }
};
class LateBackward final : public torch::autograd::Function<LateBackward> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* context, torch::Tensor input, std::int64_t gate) {
  context->saved_data["gate"] = gate;
  return input.clone();
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* context, torch::autograd::variable_list gradients) {
  // The device event holds the actual late autograd node. There is no
  // future-completion surrogate; the early bucket must launch meanwhile.
  reinterpret_cast<BackwardGate*>(context->saved_data["gate"].toInt())->enter();
  return {gradients[0], torch::Tensor{}};
 }
};
// Own the readiness event and worker through the deliberately held derivative,
// including assertion/exception unwinding before the fixture releases its gate.
class HeldGradientBackward final {
public:
 HeldGradientBackward(
  int device, tc::TorchCudaStream launch, TrainingGradientReducer& reducer, const std::vector<torch::Tensor>& parameters, std::function<torch::Tensor()> late_input, torch::Tensor early)
     : gate(device), worker_stream(tc::getStreamFromPool(false, tc::checked_device_index(device))) {
  ready_.record(launch);
  future = std::async(std::launch::async, [this, &reducer, &parameters, late_input = std::move(late_input), early = std::move(early)] {
   tc::TorchCudaStreamGuard stream(worker_stream);
   ready_.block(worker_stream);
   // Build the late branch first; autograd prioritizes the later independent early branch.
   auto delayed = LateBackward::apply(late_input(), reinterpret_cast<std::int64_t>(&gate));
   auto loss = (delayed + early.square()).sum();
   reducer.arm(0);
   static_cast<void>(TrainingStep(1, 1, false, at::kFloat).gradients(loss, parameters));
   reducer.collect(0);
  });
 }
 ~HeldGradientBackward() {
  gate.release();
  if (future.valid()) try {
    future.get();
   } catch (...) {}
 }
 HeldGradientBackward(const HeldGradientBackward&) = delete;
 HeldGradientBackward& operator=(const HeldGradientBackward&) = delete;
 BackwardGate gate;
 tc::TorchCudaStream worker_stream;
 std::future<void> future;

private:
 at::cuda::CUDAEvent ready_;
};
void mixed_early_bucket_overlap(const DistributedContext& distributed, int device) {
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 const auto options = torch::TensorOptions().device(tc::cuda_device(device));
 auto late = torch::ones({3, 2}, options).transpose(0, 1).detach().set_requires_grad(true);
 auto wide = torch::ones({2, 3}, options.dtype(torch::kFloat64)).set_requires_grad(true);
 auto early = torch::ones_like(late).set_requires_grad(true);
 auto unused = torch::ones({2}, options).set_requires_grad(true);
 std::vector<torch::Tensor> parameters{unused, late, wide, early};
 TrainingGradientReducer reducer(distributed, device, launch, {"unused", "late", "wide", "early"}, parameters, {parameters}, 128);
 reducer.begin_attempt(2);
 reducer.contribute_empty();
 reducer.enable_gradient_launch_after_counts();
 HeldGradientBackward backward(device, launch, reducer, parameters, [&] { return late + wide; }, early);
 auto& gate = backward.gate;
 auto& future = backward.future;
 const bool entered = gate.await();
 const auto launched_while_held = reducer.launched_buckets();
 gate.release();
 future.get();
 finish_gradients(reducer, device);
 require(entered && launched_while_held == 1, "interleaved dtype early bucket did not launch during held late backward");
 require(reducer.bucket_count() == 3, "interleaved dtype regions lost contiguous backward ordering");
 require(late.grad().strides() == late.strides(), "interleaved bucket lost dense parameter strides");
 equal(early.grad(), torch::full_like(early, 2 * distributed.world_size), "interleaved early SUM differs");
 equal(late.grad(), torch::full_like(late, distributed.world_size), "interleaved late SUM differs");
 equal(wide.grad(), torch::full_like(wide, distributed.world_size), "interleaved FP64 SUM differs");
 require(!unused.grad().defined(), "interleaved unused parameter acquired a gradient");
}
}  // namespace
void exercise_training_initialization(const DistributedContext& distributed, int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 torch::manual_seed(713);
 auto config = native_config_from_preset(model_presets().front());
 config.resolution = 64;
 config.num_classes = 3;
 config.num_queries = 3;
 config.num_select = 3;
 config.dec_layers = 1;
 config.group_detr = 1;
 config.training_supervision.assignment = TrainAssignmentKind::MatchFree;
 config.training_supervision.denoising.enabled = true;
 NativeRfDetrModel model(config, synthetic_training_layout(2));
 model.initialize_training_supervision(713);
 agree_model_inventory(distributed, model, "fixture-full-model");
 model.to(tc::cuda_device(device));
 std::vector<torch::Tensor> before;
 const auto prepare = [&](const auto& inventory) {
  torch::NoGradGuard no_grad;
  for (const auto& item : inventory) {
   before.push_back(item.value().detach().clone());
   if (distributed.rank) item.value().fill_(17);
  }
 };
 prepare(model.named_parameters(true));
 prepare(model.named_buffers(true));
 broadcast_training_model(distributed, model);
 std::size_t index = 0;
 auto valid = torch::ones({}, torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kBool));
 const auto verify = [&](const auto& inventory) {
  for (const auto& item : inventory) valid.logical_and_(item.value().eq(before[index++]).all());
 };
 verify(model.named_parameters(true));
 verify(model.named_buffers(true));
 require(valid.item<bool>(), "full named model initialization broadcast differs");
 ModelEma ema(model.parameters(), .9, 3);
 for (std::size_t parameter = 0; parameter < ema.shadow_params().size(); ++parameter) equal(ema.shadow_params()[parameter], before[parameter], "EMA did not capture admitted broadcast initialization");
}
void exercise_early_bucket_overlap(const DistributedContext& distributed, int device, bool abort_after_launch) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 auto late = torch::ones({4}, torch::TensorOptions().device(tc::cuda_device(device)).requires_grad(true));
 auto early = late.detach().clone().set_requires_grad(true);
 std::vector<torch::Tensor> parameters{late, early};
 TrainingGradientReducer reducer(distributed, device, launch, {"late", "early"}, parameters, {parameters}, 16);
 reducer.begin_attempt(2);
 reducer.contribute_empty();
 auto count = torch::zeros({1}, late.options().requires_grad(false));
 distributed_all_reduce_tensor(distributed, count);
 reducer.enable_gradient_launch_after_counts();
 HeldGradientBackward backward(device, launch, reducer, parameters, [&] { return late; }, early);
 auto& gate = backward.gate;
 auto& future = backward.future;
 const auto& worker_stream = backward.worker_stream;
 const bool entered = gate.await();
 const auto launched_while_held = reducer.launched_buckets();
 if (abort_after_launch && entered && launched_while_held == 1) {
  const auto custody = TrainingGradientReducerTestAccess::custody(reducer);
  std::exception_ptr failure;
  try {
#if defined(USE_C10D_NCCL)
   if (distributed.enabled && distributed.rank != 0) {
    gate.release();
    future.get();
    // The late bucket has no rank-zero counterpart. Tell the initiating
    // rank that this peer is now waiting for real collective completion.
    TrainingDistributedTestAccess::store(distributed)->set("failure-peer-waiting", std::vector<std::uint8_t>{1});
    finish_gradients(reducer, device);
    throw std::runtime_error("cancelled peer unexpectedly completed its collective");
   }
   if (distributed.enabled) static_cast<void>(TrainingDistributedTestAccess::store(distributed)->get("failure-peer-waiting"));
#endif
   throw std::runtime_error("injected failure after early gradient bucket");
  } catch (...) {
   failure = std::current_exception();
   // Release the fixture's CUDA host callback before NCCL abort can wait for
   // device cleanup. The injected failure already occurred with only the early
   // bucket launched. Fail the held derivative before it can submit another
   // bucket, even if its worker wakes before abort closes reducer admission.
   gate.release(failure);
   reducer.abort(failure);
  }
  gate.release();
  if (future.valid()) try {
    future.get();
   } catch (...) {}
  worker_stream.synchronize();
  TrainingGradientReducerTestAccess::retire(reducer);
  if (distributed.enabled) {
   require(TrainingGradientReducerTestAccess::fact(reducer).terminal && !custody.expired(), "aborted NCCL bucket/model custody was reclaimed without physical completion");
   require(late.use_count() >= 4 && early.use_count() >= 4, "terminal reducer lost its master and lane leaf references");
  } else {
   require(custody.expired(), "settled local reducer custody was unnecessarily retained");
  }
  std::fprintf(stderr, "early bucket failure custody verified on rank %d\n", distributed.rank);
  std::rethrow_exception(failure);
 }
 gate.release();
 future.get();
 finish_gradients(reducer, device);
 require(entered, "late backward did not reach its hold event");
 require(launched_while_held == 1, "early bucket did not launch while late backward was held");
 equal(early.grad(), torch::full_like(early, 2 * distributed.world_size), "early NCCL SUM differs");
 equal(late.grad(), torch::full_like(late, distributed.world_size), "late NCCL SUM differs");
 mixed_early_bucket_overlap(distributed, device);
}
void exercise_bounded_gradient_buckets(int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const DistributedContext distributed;
 const auto options = torch::TensorOptions().device(tc::cuda_device(device));
 auto first = torch::ones({3, 2}, options).transpose(0, 1).detach().set_requires_grad(true);
 auto second = torch::ones_like(first).set_requires_grad(true);
 std::vector<torch::Tensor> parameters{first, second};
 constexpr std::size_t limit = 32;  // Each tensor is 24 bytes, together 48.
 TrainingGradientReducer reducer(distributed, device, tc::getCurrentCUDAStream(tc::checked_device_index(device)), {"first", "second"}, parameters, {parameters}, limit);
 require(reducer.bucket_count() == 2, "individually fitting tensors exceeded the combined byte limit");
 const void* first_storage = nullptr;
 const void* second_storage = nullptr;
 for (int attempt = 1; attempt <= 2; ++attempt) {
  reducer.begin_attempt(1);
  reducer.enable_gradient_launch_after_counts();
  reducer.arm(0);
  static_cast<void>(TrainingStep(1, 1, false, at::kFloat).gradients((first.square().sum() + second.sum()) * attempt, parameters));
  reducer.collect(0);
  finish_gradients(reducer, device);
  equal(first.grad(), torch::full_like(first, 2 * attempt), "bounded bucket reused stale gradient values");
  equal(second.grad(), torch::full_like(second, attempt), "bounded second gradient differs");
  for (const auto& parameter : parameters) {
   require(parameter.grad().storage().nbytes() <= limit, "bounded gradient storage exceeded configured bytes");
   require(parameter.grad().strides() == parameter.strides(), "bounded gradient lost parameter strides");
  }
  if (attempt == 1) {
   first_storage = first.grad().data_ptr();
   second_storage = second.grad().data_ptr();
  } else
   require(first_storage == first.grad().data_ptr() && second_storage == second.grad().data_ptr(), "settled bucket storage was not reused");
 }
}
void exercise_direct_gradients(int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 const auto producer = tc::getStreamFromPool(false, tc::checked_device_index(device));
 const auto options = torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kFloat32);
 auto exclusive = torch::full({4}, 3, options).set_requires_grad(true);
 auto expanded = torch::full({4}, 5, options).set_requires_grad(true);
 auto unused = torch::full({4}, 7, options).set_requires_grad(true);
 std::vector<torch::Tensor> parameters{exclusive, expanded, unused};
 TrainingGradientReducer reducer({}, device, launch, {"exclusive", "expanded", "unused"}, parameters, {parameters});
 TrainingMetricHandoff metrics(device);
 GradScaler scaler(true, 8);
 const void* produced = nullptr;
 const auto hook = exclusive.register_hook([&](const torch::Tensor& gradient) {
  produced = gradient.const_data_ptr();
  return gradient;
 });
 for (int attempt = 0; attempt < 3; ++attempt) {
  reducer.begin_attempt(1);
  metrics.begin_attempt(1);
  reducer.enable_gradient_launch_after_counts();
  at::cuda::CUDAEvent ready;
  ready.record(launch);
  std::vector<torch::Tensor> returned;
  {
   tc::TorchCudaStreamGuard stream(producer);
   ready.block(producer);
   const auto loss = exclusive.square().sum() + expanded.sum();
   reducer.arm(0);
   // Keeping a borrowed autograd result alive must prohibit direct mutation.
   returned = TrainingStep(1, scaler.current_scale(), false, at::kFloat).gradients(loss, parameters);
   if (attempt != 1) returned.clear();
   reducer.collect(0);
  }
  const auto& gradients = reducer.finish_attempt();
  require(gradients.size() == 2, "direct finite-check inventory includes an unused parameter");
  require((gradients.front().const_data_ptr() == produced) == (attempt != 1), "direct storage admission ignored exclusive derivative ownership");
  require(reducer.launched_buckets() == 0, "single-producer local attempt launched gradient buckets");
  for (const auto& parameter : parameters) require(!parameter.grad().defined(), "direct handoff published optimizer gradients too early");
  const auto found_inf = scaler.check_and_unscale_gradients_(gradients, exclusive.device());
  metrics.accumulate_empty();
  const auto snapshot = metrics.complete_step(found_inf, 1, attempt + 1);
  reducer.finalize_attempt();
  require(snapshot.gradients_finite && !unused.grad().defined(), "direct gradients lost finite/unused semantics");
  if (!returned.empty()) equal(returned.front(), torch::full_like(exclusive, 48), "unscale mutated an externally retained derivative");
  equal(exclusive.grad(), torch::full_like(exclusive, 6), "direct unscale differs");
  equal(expanded.grad(), torch::ones_like(expanded), "expanded derivative was not safely materialized");
  require(exclusive.eq(3).all().item<bool>() && expanded.eq(5).all().item<bool>(), "gradient preparation changed model parameters");
 }
 exclusive.remove_hook(hook);
 // Zero-image single-rank admission still closes without creating a gradient.
 reducer.begin_attempt(1);
 reducer.contribute_empty();
 reducer.enable_gradient_launch_after_counts();
 require(reducer.finish_attempt().empty(), "empty direct contribution acquired a gradient");
 reducer.finalize_attempt();
 for (const auto& parameter : parameters) require(!parameter.grad().defined(), "empty direct attempt retained a preceding gradient");
}
void exercise_gradient_trajectory(const DistributedContext& distributed, int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 {
  const auto options = torch::TensorOptions().device(tc::cuda_device(device));
  auto strided = torch::arange(6, options.dtype(torch::kFloat32)).reshape({2, 3}).transpose(0, 1).detach().set_requires_grad(true);
  auto wide = torch::full({4}, .7, options.dtype(torch::kFloat64)).set_requires_grad(true);
  auto unused = torch::ones({2}, options.dtype(torch::kFloat32)).set_requires_grad(true);
  std::vector<torch::Tensor> parameters{strided, wide, unused};
  TrainingGradientReducer reducer(distributed, device, launch, {"strided", "wide", "unused"}, parameters, {parameters}, 16);
  reducer.begin_attempt(2);
  reducer.enable_gradient_launch_after_counts();
  for (int contribution = 0; contribution < 2; ++contribution) {
   reducer.arm(0);
   static_cast<void>(TrainingStep(2, 1, false, at::kFloat).gradients(strided.square().sum() + wide.square().sum(), parameters));
   reducer.collect(0);
  }
  finish_gradients(reducer, device);
  require(strided.grad().strides() == strided.strides(), "reducer lost a dense noncontiguous parameter stride");
  require(wide.grad().scalar_type() == torch::kFloat64 && !unused.grad().defined(), "mixed buckets lost dtype or global unused semantics");
  equal(strided.grad(), strided.detach() * distributed.world_size, "strided c10d bucket SUM differs");
  equal(wide.grad(), wide.detach() * distributed.world_size, "mixed-dtype c10d bucket SUM differs");
 }
 constexpr std::size_t k = 3 * 2;
 for (const auto kind : {TrainOptimizerKind::AdamW, TrainOptimizerKind::Muon, TrainOptimizerKind::SGD})
  for (const std::size_t batch : {1U, 3U})
   for (const std::size_t capacity : {1U, 2U}) {
    TrainRequest request;
    request.lanes = 3;
    request.grad_accum_steps = 2;
    request.batch_size = batch;
    request.recipe.optimizer = kind;
    request.recipe.lr = .003;
    request.recipe.lr_encoder = .0003;
    request.recipe.lr_scheduler = TrainLrSchedulerKind::Step;
    request.recipe.warmup_epochs = 0;
    request.fused_optimizer = false;
    auto working = inventory(device, distributed.rank == 0 ? .3 : .9);
    std::vector<torch::Tensor> initial;
    for (const auto& item : working) initial.push_back(item.value());
    broadcast_training_tensors(distributed, initial);
    auto integer_buffer = torch::full({2}, distributed.rank == 0 ? 7 : 11, torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kInt64));
    broadcast_training_tensors(distributed, {integer_buffer});
    require(integer_buffer.eq(7).all().item<bool>(), "initial nonfloating buffer broadcast differs");
    auto reference = inventory(device, .3);
    auto built = build_optimizer(working, request), reference_built = build_optimizer(reference, request);
    auto& optimizer = built.optimizer;
    auto& expected_optimizer = reference_built.optimizer;
    const auto physical = distributed.rank == 0 ? capacity : 1U;  // Deliberately different P across ranks.
    auto facts = derive_execution_facts(request, 0);
    facts.admitted_capacity = physical;
    require(facts.microbatches_per_attempt == k && facts.effective_batch_per_model == batch * k, "physical capacity changed logical K or batch");
    auto leaves = lane_leaves(optimizer, physical);
    TrainingGradientReducer reducer(distributed, device, launch, optimizer.parameter_names(), optimizer.parameters(), leaves, 16);
    TrainingTargetCounts counts(physical, k, device, distributed);
    TrainingMetricHandoff metrics(device);
    metrics.reset_epoch();
    require(metrics.epoch_average() == 0 && metrics.state().microbatches == 0, "empty settled epoch mean differs");
    GradScaler scaler(true, 128, 2, .5, 2), expected_scaler(true, 128, 2, .5, 2);
    ModelEma ema(optimizer.eligible_parameters(), .9, 3), expected_ema(expected_optimizer.eligible_parameters(), .9, 3);
    TrainingSchedule schedule(request.recipe, built.base_lrs, built.roles, 2, 4 * k, k);
    schedule.begin_epoch(0);
    double epoch_loss = 0, epoch_class_error = 0, epoch_cardinality = 0;
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
     if (attempt == 2) {
      restore_optimizer(optimizer);
      const auto saved_metrics = metrics.state();
      const auto saved_mean = metrics.epoch_average();
      metrics.restore_epoch(saved_metrics);
      require(metrics.state().microbatches == attempt * k && metrics.epoch_average() == saved_mean, "restored epoch prefix changed its settled temporal mean");
      const auto saved = schedule.state();
      schedule.restore(saved);
      scaler.load_state(scaler.current_scale(), scaler.growth_tracker());
      std::vector<torch::Tensor> cpu_shadow;
      for (const auto& tensor : ema.shadow_params()) cpu_shadow.push_back(tensor.cpu());
      ema = ModelEma::from_cpu_shadow(optimizer.eligible_parameters(), cpu_shadow, .9, 3, ema.completed_updates());
     }
     if (attempt == 3) {
      working["backbone.0.encoder.weight"].set_requires_grad(true);
      reference["backbone.0.encoder.weight"].set_requires_grad(true);
      optimizer.zero_grad(true);
      expected_optimizer.zero_grad(true);
      optimizer.activate();
      expected_optimizer.activate();
      leaves = lane_leaves(optimizer, physical);
      reducer.rebuild(optimizer.parameter_names(), optimizer.parameters(), leaves);
     }
     {
      torch::NoGradGuard no_grad;
      for (auto& lane : leaves)
       for (std::size_t p = 0; p < lane.size(); ++p) lane[p].copy_(optimizer.parameters()[p]);
     }
     at::cuda::CUDAEvent parameters_ready;
     parameters_ready.record(launch);
     reducer.begin_attempt(k);
     counts.begin_attempt();
     metrics.begin_attempt(k);
     expected_optimizer.zero_grad(true);
     double attempt_loss = 0;
     const auto slice = training_rank_slice(batch, distributed.rank, distributed.world_size);
     for (std::size_t start = 0; start < k; start += physical) {
      const auto slots = std::min(physical, k - start);
      counts.begin_wave(slots);
      std::vector<std::future<torch::Tensor>> futures(slots);
      std::vector<std::promise<void>> completed(slots);
      std::vector<std::shared_future<void>> completion;
      for (auto& promise : completed) completion.push_back(promise.get_future().share());
      for (std::size_t reverse = slots; reverse > 0; --reverse) {
       const auto lane = reverse - 1, micro = start + lane;
       std::int64_t local_count = 0;
       for (std::size_t image = slice.begin; image < slice.begin + slice.count; ++image) local_count += targets(micro, image);
       counts.publish(lane, local_count);
       if (!slice.count) {
        reducer.contribute_empty();
        continue;
       }
       futures[lane] = std::async(std::launch::async, [&, lane, micro] {
        try {
         tc::TorchCudaStreamGuard stream(tc::getStreamFromPool(false, tc::checked_device_index(device)));
         parameters_ready.block(tc::getCurrentCUDAStream(tc::checked_device_index(device)));
         const auto normalizer = counts.consume(lane, tc::getCurrentCUDAStream(tc::checked_device_index(device)).stream());
         auto loss = objective(leaves[lane], micro, slice, normalizer.target_count, static_cast<int>(attempt % 3));
         reducer.arm(lane);
         static_cast<void>(TrainingStep(k, scaler.current_scale(), false, at::kFloat).gradients(loss, leaves[lane]));
         reducer.collect(lane);
         // Physically settle this test-only return before its scalar is borrowed.
         tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize();
         if (lane + 1 < slots) completion[lane + 1].get();
         completed[lane].set_value();
         return loss.detach();
        } catch (...) {
         completed[lane].set_exception(std::current_exception());
         counts.fail(std::current_exception());
         reducer.abort(std::current_exception());
         throw;
        }
       });
      }
      counts.resolve();
      if (start + slots == k) reducer.enable_gradient_launch_after_counts();
      for (std::size_t lane = 0; lane < slots; ++lane) {
       const auto micro = start + lane;
       if (futures[lane].valid()) {
        const auto loss = futures[lane].get();
        double matched = 0, errors = 0, cardinality = 0;
        for (std::size_t image = slice.begin; image < slice.begin + slice.count; ++image) {
         matched += static_cast<double>(targets(micro, image));
         errors += static_cast<double>(targets(micro, image)) * static_cast<double>(10 + image);
         cardinality += static_cast<double>(image + 1);
        }
        const auto scalar = [&](double value) { return torch::full({}, value, loss.options()); };
        DetectionStatisticsPacket::Tensors statistics;
        DetectionStatisticsPacket::set<^^DetectionSufficientStatistics::class_error_sum>(statistics, scalar(errors));
        DetectionStatisticsPacket::set<^^DetectionSufficientStatistics::matched_count>(statistics, scalar(matched));
        DetectionStatisticsPacket::set<^^DetectionSufficientStatistics::cardinality_error_sum>(statistics, scalar(cardinality));
        DetectionStatisticsPacket::set<^^DetectionSufficientStatistics::image_count>(statistics, scalar(static_cast<double>(slice.count)));
        TrainingScalarPacket::Tensors scalars;
        TrainingScalarPacket::set<^^TrainingScalars::class_error>(scalars, scalar(matched ? errors / matched : 100));
        TrainingScalarPacket::set<^^TrainingScalars::cardinality_error>(scalars, scalar(cardinality / static_cast<double>(slice.count)));
        metrics.accumulate(loss, loss, loss, scalars, statistics);
       } else
        metrics.accumulate_empty();
       std::int64_t global_count = 0;
       for (std::size_t image = 0; image < batch; ++image) global_count += targets(micro, image);
       const auto loss =
        objective(expected_optimizer.parameters(), micro, {0, batch}, torch::full({}, global_count, working.begin()->value().options().requires_grad(false)), static_cast<int>(attempt % 3));
       TrainingStep(k, expected_scaler.current_scale(), false, at::kFloat).backward(loss);
       attempt_loss += loss.detach().item<double>();
       schedule.consume_microbatch();
       double errors = 0, cardinality = 0;
       for (std::size_t image = 0; image < batch; ++image) {
        errors += static_cast<double>(targets(micro, image)) * static_cast<double>(10 + image);
        cardinality += static_cast<double>(image + 1);
       }
       epoch_class_error += global_count ? errors / static_cast<double>(global_count) : 100;
       epoch_cardinality += cardinality / static_cast<double>(batch);
      }
      counts.end_wave();
     }
     counts.finish_attempt();
     const auto& gradients = reducer.finish_attempt();
     std::size_t checked = 0;
     for (std::size_t p = 0; p < optimizer.parameters().size(); ++p) {
      require(!optimizer.parameters()[p].grad().defined(), "gradient handoff projected usage before the numerical boundary");
      const auto& expected = expected_optimizer.parameters()[p].grad();
      if (distributed.enabled || expected.defined()) {
       if (expected.defined())
        equal(gradients.at(checked), expected, "fixed-K global gradients differ across capacity/rank slicing");
       else
        require(gradients.at(checked).eq(0).all().item<bool>(), "globally unused finite-check view is not zero");
       ++checked;
      }
     }
     require(checked == gradients.size(), "reducer finite-check inventory differs");
     // Inject an overflow on one rank only after SUM, so the finite control
     // itself must agree the skipped update and scaler/EMA clocks.
     if (attempt == 1 && distributed.rank == distributed.world_size - 1) gradients.front().fill_(std::numeric_limits<float>::infinity());
     const auto found_inf = scaler.check_and_unscale_gradients_(gradients, optimizer.parameters().front().device());
     const auto snapshot = metrics.complete_step(found_inf, k, (attempt + 1) * k, distributed);
     counts.finalize_attempt();
     reducer.finalize_attempt();
     static_cast<void>(expected_scaler.check_and_unscale_(expected_optimizer));
     for (std::size_t p = 0; p < optimizer.parameters().size(); ++p)
      require(optimizer.parameters()[p].grad().defined() == expected_optimizer.parameters()[p].grad().defined(), "global unused-gradient projection differs");
     const bool skipped = attempt == 1;
     require(snapshot.loss_finite && snapshot.gradients_finite == !skipped, "global overflow decision differs");
     epoch_loss += attempt_loss;
     require(std::abs(snapshot.loss_sum - epoch_loss) < 2e-5, "global live epoch numerator differs");
     require(std::abs(snapshot.step_loss - attempt_loss / static_cast<double>(k)) < 2e-5, "global live attempt mean differs");
     require(metrics.epoch_average() == snapshot.loss_sum / static_cast<double>((attempt + 1) * k), "settled epoch mean differs from live state");
     require(metrics.state().scalar_means() == snapshot.scalars, "settled epoch scalar means differ from the live snapshot");
     require(snapshot.scalars.class_error && std::abs(*snapshot.scalars.class_error - epoch_class_error / static_cast<double>((attempt + 1) * k)) < 2e-5, "matched-class sufficient statistics differ");
     require(snapshot.scalars.cardinality_error && std::abs(*snapshot.scalars.cardinality_error - epoch_cardinality / static_cast<double>((attempt + 1) * k)) < 2e-5,
      "global image/cardinality statistics differ");
     if (!skipped) {
      optimizer.clip_grad_norm_(.1);
      expected_optimizer.clip_grad_norm_(.1);
     }
     scaler.step(optimizer, skipped);
     expected_scaler.step(expected_optimizer, skipped);
     scaler.update(skipped);
     expected_scaler.update(skipped);
     ema.update();
     expected_ema.update();
     optimizer.zero_grad(false);
     optimizer.zero_grad(true);
     expected_optimizer.zero_grad(true);
     schedule.prepare_attempt();
     schedule.finish_attempt(!skipped);
     require(scaler.current_scale() == expected_scaler.current_scale() && scaler.growth_tracker() == expected_scaler.growth_tracker(), "rank scaler clocks differ");
     require(ema.completed_updates() == static_cast<std::int64_t>(attempt + 1), "overflow did not consume EMA attempt");
     require(schedule.state().consumed_attempts == attempt + 1 && schedule.state().successful_updates == attempt + 1 - (attempt >= 1), "attempt/success clocks differ");
     require(checked_training_product(schedule.state().consumed_microbatches, batch) == (attempt + 1) * k * batch, "global processed images were counted per rank");
     require(
      checked_training_product(schedule.state().successful_updates, facts.effective_batch_per_model) == (attempt + 1 - (attempt >= 1)) * k * batch, "overflow changed successful-update image count");
     for (std::size_t p = 0; p < optimizer.eligible_parameters().size(); ++p) {
      equal(optimizer.eligible_parameters()[p], expected_optimizer.eligible_parameters()[p], "optimizer trajectory differs from global reference");
      equal(ema.shadow_params()[p], expected_ema.shadow_params()[p], "EMA trajectory differs from global reference");
     }
    }
    require(std::abs(metrics.epoch_average() - epoch_loss / (4 * k)) < 2e-5, "final temporal mean differs");
    const auto settled_mean = metrics.epoch_average();
    const auto options = working.begin()->value().options().requires_grad(false);
    metrics.begin_validation();
    metrics.accumulate_validation(torch::full({}, 7.0, options));
    require(metrics.epoch_average() == settled_mean, "validation work changed the authoritative settled epoch mean");
    require(metrics.validation_average(1) == 7 && metrics.epoch_average() == settled_mean, "validation handoff changed the settled epoch mean");
    // MatchFree has no Hungarian statistics. Empty rank slices still take part
    // in every collective without manufacturing class/cardinality availability.
    metrics.reset_epoch();
    metrics.begin_attempt(k);
    const auto slice = training_rank_slice(batch, distributed.rank, distributed.world_size);
    for (std::size_t micro = 0; micro < k; ++micro) {
     if (!slice.count) {
      metrics.accumulate_empty();
      continue;
     }
     const auto loss = torch::full({}, static_cast<double>(slice.count), options);
     TrainingScalarPacket::Tensors scalars;
     TrainingScalarPacket::set<^^TrainingScalars::total>(scalars, loss);
     metrics.accumulate(loss, loss, loss, scalars);
    }
    const auto absent = metrics.complete_step(torch::zeros({}, options), k, k, distributed);
    require(!absent.scalars.class_error && !absent.scalars.cardinality_error && !absent.scalars.mask_ce, "absent MatchFree statistics acquired fabricated values");
    require(absent.scalars.total == static_cast<double>(batch) && metrics.epoch_average() == static_cast<double>(batch), "absent statistics changed available loss means");
    const auto absent_prefix = metrics.state();
    metrics.restore_epoch(absent_prefix);
    require(metrics.state().scalar_means() == absent.scalars && metrics.epoch_average() == static_cast<double>(batch), "Resume changed optional statistics availability");
    // A nonfinite constant leaves its gradients finite, and must still fail
    // every rank rather than becoming a recoverable overflow skip.
    metrics.begin_attempt(1);
    const auto loss = working["transformer.decoder.linear.weight"].sum() + (distributed.rank == distributed.world_size - 1 ? std::numeric_limits<double>::quiet_NaN() : 0.0);
    const auto finite_gradient = TrainingStep(1, 1, false, at::kFloat).gradients(loss, {working["transformer.decoder.linear.weight"]});
    require(torch::isfinite(finite_gradient[0]).all().item<bool>(), "constant nonfinite loss fixture changed derivatives");
    metrics.accumulate(loss, loss, loss, {});
    const auto control = metrics.complete_step(torch::zeros({}, loss.options()), 1, 1, distributed);
    require(!control.loss_finite && control.gradients_finite, "nonfinite constant loss was not globally rejected");
    TrainLaneResult contribution;
    contribution.loss_terms.emplace("loss_ce", loss.detach());
    TrainingLossReport failure_report;
    failure_report.accumulate(std::move(contribution.loss_terms));
    // A later healthy contribution must not erase the earlier cause, and
    // neither contribution may keep its autograd graph alive in the report.
    failure_report.accumulate({{"loss_ce", torch::zeros({}, loss.options().requires_grad(false))}});
    const auto message = std::string(failure_report.failure(optimizer.parameters(), optimizer.parameter_names()).what());
    require(message.find("non-finite RF-DETR loss or gradients encountered during native training") != std::string::npos, "training loss failure message changed");
    if (distributed.rank == distributed.world_size - 1) {
     require(message.find("nonfinite_losses=[loss_ce=nan]") != std::string::npos, "training failure lost its nonfinite criterion identity/value");
    }
   }
}
void exercise_target_count_handoff(const DistributedContext& distributed, int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto count_stream = tc::getStreamFromPool(false, tc::checked_device_index(device));
 const auto metric_stream = tc::getStreamFromPool(false, tc::checked_device_index(device));
 const auto consumer = tc::getStreamFromPool(false, tc::checked_device_index(device));
 constexpr std::size_t k = 5;
 const std::size_t capacity = distributed.rank == 0 ? 2 : 1;
 tc::TorchCudaStreamGuard count_guard(count_stream);
 TrainingTargetCounts counts(capacity, k, device, distributed);
 tc::TorchCudaStreamGuard metric_guard(metric_stream);
 TrainingMetricHandoff metrics(device);
 const auto finite = torch::zeros({}, torch::TensorOptions().device(tc::cuda_device(device)));
 for (std::size_t attempt = 0; attempt < 3; ++attempt) {
  counts.begin_attempt();
  metrics.begin_attempt(k);
  std::array<torch::Tensor, k> values;
  for (std::size_t start = 0; start < k; start += capacity) {
   const auto slots = std::min(capacity, k - start);
   counts.begin_wave(slots);
   for (std::size_t lane = 0; lane < slots; ++lane) counts.publish(lane, distributed.rank == 0 ? 16777217 + 2 * (attempt * k + start + lane) : 0);
   counts.resolve();
   for (std::size_t lane = 0; lane < slots; ++lane) {
    // Rank one contributes counts without a loss, conversion or gradient path.
    if (distributed.rank == 0) values[start + lane] = counts.consume(lane, consumer.stream()).target_count;
    metrics.accumulate_empty();
   }
   counts.end_wave();
  }
  counts.finish_attempt();
  const auto snapshot = metrics.complete_step(finite, k, (attempt + 1) * k, distributed);
  counts.finalize_attempt();
  require(snapshot.loss_finite && snapshot.gradients_finite && snapshot.step_loss == 0, "empty count handoff changed numerical controls");
  if (distributed.rank == 0)
   for (std::size_t micro = 0; micro < k; ++micro)
    require(values[micro].item<float>() == static_cast<float>(16777217 + 2 * (attempt * k + micro)), "cross-stream count handoff changed scalar SUM or conversion");
 }
}
namespace {
enum class CollectiveOutcome { Success, SubmitThrow, WaitFalse, WaitThrow };
struct CollectiveGate final {
 std::mutex mutex;
 std::condition_variable changed;
 bool entered = false, released = false;
 static void hold(void* value) {
  auto& gate = *static_cast<CollectiveGate*>(value);
  std::unique_lock lock(gate.mutex);
  gate.entered = true;
  gate.changed.notify_all();
  gate.changed.wait(lock, [&] { return gate.released; });
 }
 void release() {
  std::lock_guard lock(mutex);
  released = true;
  changed.notify_all();
 }
 bool await() {
  std::unique_lock lock(mutex);
  return changed.wait_for(lock, std::chrono::seconds(20), [&] { return entered; });
 }
};
struct CollectiveFacts final {
 int reductions = 0, broadcasts = 0, waits = 0, live_work = 0, aborts = 0;
 std::set<const void*> broadcast_storage;
 std::set<const void*> reduction_storage;
 std::vector<std::pair<at::ScalarType, std::int64_t>> reduction_shapes;
};
class FixtureCollectiveWork final : public c10d::Work {
public:
 FixtureCollectiveWork(std::shared_ptr<CollectiveFacts> facts, CollectiveOutcome outcome, tc::TorchCudaStream stream)
     : c10d::Work(0, c10d::OpType::ALLREDUCE), facts_(std::move(facts)), outcome_(outcome) {
  ++facts_->live_work;
  completed_.record(stream);
 }
 ~FixtureCollectiveWork() override { --facts_->live_work; }
 bool wait(std::chrono::milliseconds = kNoTimeout) override {
  ++facts_->waits;
  if (outcome_ == CollectiveOutcome::WaitThrow) throw std::runtime_error("injected collective wait exception");
  if (outcome_ == CollectiveOutcome::WaitFalse) return false;
  completed_.block(tc::getCurrentCUDAStream(completed_.device_index()));
  return true;
 }

private:
 std::shared_ptr<CollectiveFacts> facts_;
 CollectiveOutcome outcome_;
 at::cuda::CUDAEvent completed_;
};
class FixtureCollectiveBackend final : public c10d::Backend {
public:
 FixtureCollectiveBackend(int device, CollectiveOutcome outcome, std::shared_ptr<CollectiveFacts> facts, std::shared_ptr<CollectiveGate> gate = {}, int successful_submissions = 0)
     : c10d::Backend(0, 2),
       stream_(tc::getStreamFromPool(false, tc::checked_device_index(device))),
       outcome_(outcome),
       facts_(std::move(facts)),
       gate_(std::move(gate)),
       successful_submissions_(successful_submissions) {}
 const std::string getBackendName() const override { return "training-custody-fixture"; }
 c10::intrusive_ptr<c10d::Work> allreduce(std::vector<at::Tensor>& tensors, const c10d::AllreduceOptions& = {}) override {
  ++facts_->reductions;
  facts_->reduction_storage.insert(tensors.front().const_data_ptr());
  facts_->reduction_shapes.emplace_back(tensors.front().scalar_type(), tensors.front().numel());
  return queue(tensors, 1);
 }
 c10::intrusive_ptr<c10d::Work> broadcast(std::vector<at::Tensor>& tensors, const c10d::BroadcastOptions& = {}) override {
  ++facts_->broadcasts;
  facts_->broadcast_storage.insert(tensors.front().const_data_ptr());
  return queue(tensors, 0);
 }
 void abort() override { ++facts_->aborts; }
 void release() {
  if (gate_) gate_->release();
  stream_.synchronize();
 }

private:
 c10::intrusive_ptr<c10d::Work> queue(std::vector<at::Tensor>& tensors, int increment) {
  at::cuda::CUDAEvent produced;
  produced.record(tc::getCurrentCUDAStream(stream_.device_index()));
  produced.block(stream_);
  tc::TorchCudaStreamGuard guard(stream_);
  torch::NoGradGuard no_grad;
  if (gate_ && !prepared_) {
   // CUDA may synchronize the context while loading an unseen Torch kernel.
   // Prepare this exact operation before holding the stream; the tested write
   // still remains behind the callback and retains its real device custody.
   auto scratch = torch::zeros_like(tensors.front());
   scratch.add_(increment);
   stream_.synchronize();
   prepared_ = true;
  }
  if (gate_) require(cudaLaunchHostFunc(stream_.stream(), &CollectiveGate::hold, gate_.get()) == cudaSuccess, "could not hold collective device work");
  tensors.front().add_(increment);
  const auto outcome = facts_->reductions + facts_->broadcasts <= successful_submissions_ ? CollectiveOutcome::Success : outcome_;
  if (outcome == CollectiveOutcome::SubmitThrow) throw std::runtime_error("injected collective exception after queue");
  return c10::make_intrusive<FixtureCollectiveWork>(facts_, outcome, stream_);
 }
 tc::TorchCudaStream stream_;
 CollectiveOutcome outcome_;
 std::shared_ptr<CollectiveFacts> facts_;
 std::shared_ptr<CollectiveGate> gate_;
 int successful_submissions_;
 bool prepared_ = false;
};
struct CollectiveFixture final {
 CollectiveFixture(int device, CollectiveOutcome outcome, bool held = false, int successful_submissions = 0)
     : gate(held ? std::make_shared<CollectiveGate>() : nullptr),
       backend(c10::make_intrusive<FixtureCollectiveBackend>(device, outcome, facts, gate, successful_submissions)),
       group(TrainingDistributedTestAccess::backend(backend, 0, device)) {}
 std::shared_ptr<CollectiveFacts> facts = std::make_shared<CollectiveFacts>();
 std::shared_ptr<CollectiveGate> gate;
 c10::intrusive_ptr<FixtureCollectiveBackend> backend;
 DistributedContext group;
};
class CollectiveRelease final {
public:
 explicit CollectiveRelease(FixtureCollectiveBackend& backend) : backend_(backend) {}
 ~CollectiveRelease() {
  try {
   finish();
  } catch (...) {}
 }
 void finish() {
  if (!finished_) {
   backend_.release();
   finished_ = true;
  }
 }

private:
 FixtureCollectiveBackend& backend_;
 bool finished_ = false;
};
class HeldCudaStream final {
public:
 explicit HeldCudaStream(tc::TorchCudaStream stream) : stream_(stream) {
  require(cudaLaunchHostFunc(stream_.stream(), &CollectiveGate::hold, &gate_) == cudaSuccess, "could not delay CUDA stream consumption");
 }
 ~HeldCudaStream() {
  release();
  try {
   stream_.synchronize();
  } catch (...) {}
 }
 void release() { gate_.release(); }
 bool await() { return gate_.await(); }

private:
 tc::TorchCudaStream stream_;
 CollectiveGate gate_;
};
class CancelCollectiveWork final : public c10d::Work {
public:
 CancelCollectiveWork(c10::intrusive_ptr<c10d::Work> work, c10::intrusive_ptr<c10d::Store> store) : c10d::Work(1, c10d::OpType::ALLREDUCE), work_(std::move(work)), store_(std::move(store)) {}
 bool wait(std::chrono::milliseconds = kNoTimeout) override {
  // The real NCCL Work exists but has no rank-zero counterpart. Coordinate
  // cancellation while that exact submission remains physically in flight.
  store_->set("custody-submitted", std::vector<std::uint8_t>{1});
  static_cast<void>(store_->get("custody-cancel"));
  throw std::runtime_error("peer training cancellation");
 }

private:
 c10::intrusive_ptr<c10d::Work> work_;
 c10::intrusive_ptr<c10d::Store> store_;
};
class CancelCollectiveBackend final : public c10d::Backend {
public:
 CancelCollectiveBackend(c10::intrusive_ptr<c10d::Backend> backend, c10::intrusive_ptr<c10d::Store> store) : c10d::Backend(1, 2), backend_(std::move(backend)), store_(std::move(store)) {}
 const std::string getBackendName() const override { return "cancel-real-training-collective"; }
 c10::intrusive_ptr<c10d::Work> allreduce(std::vector<at::Tensor>& tensors, const c10d::AllreduceOptions& options = {}) override {
  return c10::make_intrusive<CancelCollectiveWork>(backend_->allreduce(tensors, options), store_);
 }
 c10::intrusive_ptr<c10d::Work> broadcast(std::vector<at::Tensor>& tensors, const c10d::BroadcastOptions& options = {}) override {
  return c10::make_intrusive<CancelCollectiveWork>(backend_->broadcast(tensors, options), store_);
 }
 void abort() override { backend_->abort(); }

private:
 c10::intrusive_ptr<c10d::Backend> backend_;
 c10::intrusive_ptr<c10d::Store> store_;
};
}  // namespace
void exercise_collective_custody(int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto options = torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kFloat32);
 // CLEANUP-IGNORE: All-reduce and broadcast share the fixture and weak custody probe; their device operations and retirement assertions are independent.
 for (const auto outcome : {CollectiveOutcome::SubmitThrow, CollectiveOutcome::WaitFalse, CollectiveOutcome::WaitThrow}) {
  auto [facts, gate, backend, group] = CollectiveFixture(device, outcome, true);
  const auto transport = TrainingDistributedTestAccess::custody(group);
  auto tensor = torch::ones({4}, options);
  std::string failure;
  {
   TrainingCollectiveWork work(device);
   CollectiveRelease release(*backend);
   try {
    work.join(work.all_reduce(group, tensor));
   } catch (const std::exception& error) { failure = error.what(); }
   require(gate->await(), "collective fixture did not reach real in-flight device work");
   require(!failure.empty() && work.uncertain() && work.retire() != cudaSuccess, "failed collective falsely proved physical completion");
   require(tensor.use_count() > 1, "failed collective lost its tensor before retirement");
   require(facts->aborts == 1, "collective failure did not abort exactly once");
   distributed_abort(group);
   require(facts->aborts == 1, "peer cancellation repeated the initiating abort");
   release.finish();
  }
  group = {};
  require(!transport.expired() && tensor.use_count() > 1, "unproved collective reclaimed transport or tensor custody");
  require(facts->live_work == (outcome == CollectiveOutcome::SubmitThrow ? 0 : 1), "unproved collective released its returned Work");
 }
 // A checked join is asynchronous: retained tensors stay live until the
 // explicit drained boundary, then slots, Work and events can be reused.
 {
  auto [facts, gate, backend, group] = CollectiveFixture(device, CollectiveOutcome::Success, true);
  auto tensor = torch::ones({4}, options);
  TrainingCollectiveWork work(device);
  CollectiveRelease release(*backend);
  for (int attempt = 0; attempt < 4; ++attempt) {
   work.join(work.all_reduce(group, tensor));
   work.record_completion();
   require(gate->await(), "joined collective fixture never entered its device hold");
   require(tensor.use_count() > 1 && facts->live_work == 1, "stream join prematurely released collective storage");
   if (attempt == 0) require(!work.release_completed(), "in-flight collective was reclaimed by a nonblocking completion check");
   release.finish();
   tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize();
   require(work.release_completed(), "later stream completion did not reclaim the collective without another wait");
   require(tensor.use_count() == 1 && facts->live_work == 0, "settled collective failed to reclaim its bounded slot");
  }
  require(tensor.eq(5).all().item<bool>(), "delayed device writes did not settle before collective reuse");
 }
 // The reducer returns while the launch stream is deliberately held. Its two
 // Work handles survive the metric's single collective and are reclaimed only
 // after the shared physical handoff. The timeout is a deadlock watchdog.
 {
  auto [facts, gate, backend, group] = CollectiveFixture(device, CollectiveOutcome::Success);
  const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
  auto parameter = torch::ones({4}, options).set_requires_grad(true);
  TrainingGradientReducer reducer(group, device, launch, {"parameter"}, {parameter}, {{parameter}});
  TrainingMetricHandoff metrics(device);
  const auto found_inf = torch::zeros({}, options);
  for (int attempt = 0; attempt < 2; ++attempt) {
   reducer.begin_attempt(1);
   reducer.enable_gradient_launch_after_counts();
   metrics.begin_attempt(1);
   metrics.accumulate_empty();
   reducer.arm(0);
   static_cast<void>(TrainingStep(1, 1, false, at::kFloat).gradients(parameter.square().sum(), {parameter}));
   reducer.collect(0);
   if (attempt == 0) {
    static_cast<void>(reducer.finish_attempt());
   } else {
    HeldCudaStream held(launch);
    require(held.await(), "reducer launch stream did not reach the deliberate hold");
    auto handoff = std::async(std::launch::async, [&] {
     tc::TorchCudaDeviceGuard worker(tc::checked_device_index(device));
     tc::TorchCudaStreamGuard stream(launch);
     static_cast<void>(reducer.finish_attempt());
     bool rejected = false;
     try {
      reducer.finalize_attempt();
     } catch (const std::logic_error&) { rejected = true; }
     require(rejected, "pending usage readback was projected before physical completion");
    });
    const bool returned = handoff.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    held.release();
    handoff.get();
    require(returned, "reducer device handoff waited on host completion");
   }
   require(!parameter.grad().defined() && facts->live_work == 2, "reducer published usage or retired Work before the metric handoff");
   require(facts->reductions == 3 * attempt + 2, "reducer changed its canonical bucket/usage collective count");
   static_cast<void>(metrics.complete_step(found_inf, 1, attempt + 1, group));
   require(facts->reductions == 3 * (attempt + 1) && facts->live_work == 2, "metric control/sums/statistics were not one collective");
   reducer.finalize_attempt();
   require(parameter.grad().defined() && facts->live_work == 0, "numerical handoff failed to publish usage and reclaim reducer Work");
  }
  constexpr auto metric_elements = 5 + 2 * TrainingScalarPacket::size + DetectionStatisticsPacket::size;
  const std::vector<std::pair<at::ScalarType, std::int64_t>> expected_shapes{
   {at::kFloat, 4}, {at::kInt, 1}, {at::kFloat, metric_elements}, {at::kFloat, 4}, {at::kInt, 1}, {at::kFloat, metric_elements}
  };
  require(facts->reduction_shapes == expected_shapes && facts->reduction_storage.size() == 3, "packed metric/reducer operations changed dtype, extent, order or persistent storage");
 }
 // K retained scalar collectives span multiple physical waves, including a
 // held conversion and an entirely empty rank on another launch stream. The
 // first pass warms the same Torch kernels and allocation sizes before gates.
 for (const bool distributed : {false, true}) {
  constexpr std::size_t k = 5, capacity = 2;
  auto [facts, gate, backend, group] = CollectiveFixture(device, CollectiveOutcome::Success);
  if (!distributed) {
   group.enabled = false;
   group.world_size = 1;
  }
  const auto launch = tc::getStreamFromPool(false, tc::checked_device_index(device));
  const auto consumer = tc::getStreamFromPool(false, tc::checked_device_index(device));
  const auto handoff = tc::getStreamFromPool(false, tc::checked_device_index(device));
  tc::TorchCudaStreamGuard count_stream(launch);
  TrainingTargetCounts counts(capacity, k, device, group);
  tc::TorchCudaStreamGuard handoff_stream(handoff);
  TrainingMetricHandoff metrics(device);
  const auto finite = torch::zeros({}, options);
  for (int attempt = 0; attempt < 3; ++attempt) {
   std::array<torch::Tensor, k> values;
   counts.begin_attempt();
   metrics.begin_attempt(k);
   for (std::size_t micro = 0; micro < k; ++micro) metrics.accumulate_empty();
   std::optional<HeldCudaStream> held;
   if (attempt) {
    held.emplace(attempt == 1 ? consumer : launch);
    require(held->await(), "count attempt did not enter its CUDA gate");
   }
   auto queued = std::async(std::launch::async, [&] {
    tc::TorchCudaStreamGuard worker(handoff);
    for (std::size_t start = 0; start < k; start += capacity) {
     const auto slots = std::min(capacity, k - start);
     counts.begin_wave(slots);
     for (std::size_t lane = 0; lane < slots; ++lane) counts.publish(lane, 16777217 + 2 * (start + lane));
     counts.resolve();
     counts.resolve();
     if (attempt != 2)
      for (std::size_t lane = 0; lane < slots; ++lane) values[start + lane] = counts.consume(lane, consumer.stream()).target_count;
     counts.end_wave();
    }
    counts.finish_attempt();
   });
   // This timeout is solely a deadlock watchdog. Assertions below observe
   // actual gated device work and owned Work handles, with no throughput target.
   const bool returned = queued.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
   if (!returned && held) held->release();
   queued.get();
   require(returned, "count wave admission or attempt handoff waited for physical completion");
   const auto retained = distributed ? static_cast<int>(k) : 0;
   require(facts->live_work == retained, "count waves reclaimed Work early or exceeded K retained slots");
   require(facts->reductions == (distributed ? attempt * static_cast<int>(k + 1) + static_cast<int>(k) : 0), "count waves changed scalar collective admission/order");
   if (held) {
    bool premature = false;
    try {
     counts.finalize_attempt();
    } catch (const std::logic_error&) { premature = true; }
    require(premature && facts->live_work == retained, "pending count work was reclaimed before the numerical boundary");
    held->release();
   }
   static_cast<void>(metrics.complete_step(finite, k, (attempt + 1) * k, group));
   require(facts->live_work == retained, "metric handoff changed count ownership before explicit finalization");
   counts.finalize_attempt();
   require(facts->live_work == 0, "metric completion did not reclaim retained count Work");
   if (attempt != 2)
    for (std::size_t micro = 0; micro < k; ++micro)
     require(values[micro].item<float>() == static_cast<float>(16777217 + 2 * micro + (distributed ? 1 : 0)), "count scalar overwrite or FP32-before-SUM changed normalization");
  }
  require(facts->waits == facts->reductions && facts->aborts == 0, "successful count attempts changed stream joins or aborted transport");
  require(facts->reduction_storage.size() == (distributed ? capacity + 1 : 0), "count/metric storage grew across waves or attempts");
  if (distributed) {
   constexpr auto metric_elements = 5 + 2 * TrainingScalarPacket::size + k * DetectionStatisticsPacket::size;
   for (std::size_t operation = 0; operation < facts->reduction_shapes.size(); ++operation) {
    const auto expected = operation % (k + 1) == k ? std::pair{at::kFloat, static_cast<std::int64_t>(metric_elements)} : std::pair{at::kLong, std::int64_t{1}};
    require(facts->reduction_shapes[operation] == expected, "retained count attempts changed collective dtype, shape or order");
   }
  }
 }
 // Count publication preserves the first useful cause and drains waiting
 // futures after submit/wait failure, including an already-admitted wave.
 for (const auto outcome : {CollectiveOutcome::SubmitThrow, CollectiveOutcome::WaitFalse, CollectiveOutcome::WaitThrow})
  for (const int completed_wave : {0, 2}) {
   auto [facts, gate, backend, group] = CollectiveFixture(device, outcome, true, completed_wave);
   TrainingTargetCounts counts(2, 4, device, group);
   counts.begin_attempt();
   counts.begin_wave(2);
   CollectiveRelease release(*backend);
   const auto custody = TrainingDistributedTestAccess::custody(counts);
   if (completed_wave) {
    counts.publish(0, 0);
    counts.publish(1, 1);
    counts.resolve();
    counts.end_wave();
    counts.begin_wave(2);
   }
   std::array<std::future<std::string>, 2> waiters;
   for (std::size_t i = 0; i < waiters.size(); ++i)
    waiters[i] = std::async(std::launch::async, [&, i] {
     tc::TorchCudaDeviceGuard worker(tc::checked_device_index(device));
     try {
      static_cast<void>(counts.consume(i, tc::getCurrentCUDAStream(tc::checked_device_index(device)).stream()));
     } catch (const std::exception& error) { return std::string(error.what()); }
     return std::string{};
    });
   counts.publish(0, 1);
   counts.publish(1, 2);
   std::string first;
   try {
    counts.resolve();
   } catch (const std::exception& error) { first = error.what(); }
   const std::string expected = outcome == CollectiveOutcome::SubmitThrow ? "injected collective exception after queue"
                                : outcome == CollectiveOutcome::WaitFalse ? "training collective wait returned false"
                                                                          : "injected collective wait exception";
   for (auto& waiter : waiters) require(waiter.get() == first && first == expected, "count waiters lost the initiating failure");
   require(gate->await(), "count failure did not retain in-flight device work");
   require(facts->live_work == completed_wave + (outcome == CollectiveOutcome::SubmitThrow ? 0 : 1), "count failure lost previously submitted Work");
   counts.fail(std::make_exception_ptr(std::runtime_error("secondary count cancellation")));
   bool sealed = false;
   try {
    counts.begin_attempt();
   } catch (const std::exception& error) { sealed = error.what() == first; }
   require(sealed && facts->aborts == 1, "count failure reopened admission or replaced the first cause");
   release.finish();
   TrainingDistributedTestAccess::retire(counts);
   require(TrainingDistributedTestAccess::terminal(counts) && !custody.expired(), "unjoined count custody was released");
  }
 // The entire metric owner contains both its device inventory and pinned host
 // destination. A failed collective cannot leave only an event shell alive.
 for (const auto outcome : {CollectiveOutcome::SubmitThrow, CollectiveOutcome::WaitFalse, CollectiveOutcome::WaitThrow}) {
  auto [facts, gate, backend, group] = CollectiveFixture(device, outcome, true);
  TrainingMetricHandoff metrics(device);
  metrics.begin_attempt(1);
  metrics.accumulate_empty();
  CollectiveRelease release(*backend);
  const auto custody = TrainingDistributedTestAccess::custody(metrics);
  bool failed = false;
  try {
   static_cast<void>(metrics.complete_step(torch::zeros({}, options), 1, 1, group));
  } catch (...) { failed = true; }
  require(gate->await(), "metric failure did not retain in-flight device work");
  release.finish();
  TrainingDistributedTestAccess::retire(metrics);
  require(failed && TrainingDistributedTestAccess::terminal(metrics) && !custody.expired(), "uncertain metric device/pinned custody was released");
 }
 for (const auto outcome : {CollectiveOutcome::SubmitThrow, CollectiveOutcome::WaitFalse, CollectiveOutcome::WaitThrow}) {
  auto [facts, gate, backend, group] = CollectiveFixture(device, outcome, true);
  const auto transport = TrainingDistributedTestAccess::custody(group);
  std::vector<torch::Tensor> tensors;
  for (int i = 0; i < 4; ++i) tensors.push_back(torch::full({4}, i, options));
  CollectiveRelease release(*backend);
  bool failed = false;
  try {
   broadcast_training_tensors(group, tensors, 64);
  } catch (...) { failed = true; }
  require(gate->await(), "broadcast failure did not reach in-flight device work");
  group = {};
  require(failed && !transport.expired(), "failed coalesced submission lost its transport");
  for (const auto& tensor : tensors) require(tensor.use_count() > 1, "failed coalesced submission lost original tensor custody");
  require(facts->live_work == (outcome == CollectiveOutcome::SubmitThrow ? 0 : 1), "failed coalesced submission lost returned Work custody");
  release.finish();
 }
 // Coalescing is observable through the actual broadcast primitive. Eight
 // tensor inventories fit two reusable buffers, not one submission per tensor.
 {
  auto [facts, gate, backend, group] = CollectiveFixture(device, CollectiveOutcome::Success);
  std::vector<torch::Tensor> tensors;
  for (int i = 0; i < 33; ++i) tensors.push_back(torch::full({2, 2}, i, options).transpose(0, 1));
  broadcast_training_tensors(group, tensors, 64);
  require(facts->broadcasts == 9 && facts->live_work == 0, "coalesced initialization submitted one broadcast per tensor or retained completed Work");
  require(facts->broadcast_storage.size() <= 2, "coalesced initialization did not reuse its bounded flattened buffers");
  for (std::size_t i = 0; i < tensors.size(); ++i)
   require(tensors[i].stride(0) == 1 && tensors[i].eq(static_cast<std::int64_t>(i)).all().item<bool>(), "coalescing changed original strides or values");
  std::vector<torch::Tensor> ema{torch::ones({2}, options), torch::full({2}, 2, options), torch::full({2}, 3, options)};
  broadcast_training_tensors(group, ema, 64);
  require(facts->broadcasts == 10, "EMA initialization failed to coalesce its parameter inventory");
  std::vector<torch::Tensor> mixed{
   torch::arange(6, options).reshape({2, 3}).transpose(0, 1), torch::ones({2}, options), torch::full({2}, .25, options.dtype(torch::kFloat64)), torch::full({2}, 7, options.dtype(torch::kInt64))
  };
  const auto before = mixed.front().clone();
  broadcast_training_tensors(group, mixed, 64);
  require(facts->broadcasts == 13 && torch::equal(mixed.front(), before) && mixed.front().stride(0) == 1 && mixed.back().eq(7).all().item<bool>(),
   "coalesced mixed dtype/stride/integer initialization differs");
  std::vector<torch::Tensor> oversized;
  for (int i = 0; i < 3; ++i) oversized.push_back(torch::full({32}, i, options));
  broadcast_training_tensors(group, oversized, 64);
  for (std::size_t i = 0; i < oversized.size(); ++i)
   require(oversized[i].eq(static_cast<std::int64_t>(i)).all().item<bool>(), "single-tensor flattened storage aliased a previous model tensor during reuse");
 }
 // Full owners reclaim on settled local paths; repeated count waves preserve
 // a delayed consumer's count-to-float conversion before scalar-slot overwrite.
 {
  DistributedContext local;
  TrainingTargetCounts counts(1, 2, device, local);
  counts.begin_attempt();
  counts.begin_wave(1);
  auto count_owner = TrainingDistributedTestAccess::custody(counts);
  const auto stream = tc::getStreamFromPool(false, tc::checked_device_index(device));
  counts.publish(0, 7);
  counts.resolve();
  // Prepare the conversion kernel before deliberately preventing GPU progress.
  {
   tc::TorchCudaStreamGuard consumer(stream);
   static_cast<void>(torch::ones({}, options.dtype(torch::kInt64)).to(torch::kFloat32));
  }
  stream.synchronize();
  HeldCudaStream delayed(stream);
  torch::Tensor previous;
  {
   tc::TorchCudaStreamGuard consumer(stream);
   previous = counts.consume(0, stream.stream()).target_count;
  }
  require(delayed.await(), "count consumer never entered its CUDA hold");
  counts.end_wave();
  counts.begin_wave(1);
  counts.publish(0, 13);
  counts.resolve();
  counts.end_wave();
  counts.finish_attempt();
  delayed.release();
  stream.synchronize();
  require(previous.item<float>() == 7, "count storage reuse changed an earlier conversion");
  tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize();
  counts.finalize_attempt();
  TrainingDistributedTestAccess::retire(counts);
  require(count_owner.expired(), "settled local counts leaked");
  TrainingMetricHandoff metrics(device);
  auto metric_owner = TrainingDistributedTestAccess::custody(metrics);
  metrics.begin_attempt(1);
  metrics.accumulate_empty();
  static_cast<void>(metrics.complete_step(torch::zeros({}, options), 1, 1));
  metrics.restore_epoch({3, 12, 6, 9, {}});
  const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
  std::future<double> mean;
  {
   // Hold actual GPU progress after restoration. Reading the settled mean
   // must return while this stream remains held, without a second readback.
   HeldCudaStream pending(launch);
   const bool entered = pending.await();
   mean = std::async(std::launch::async, [&] {
    tc::TorchCudaDeviceGuard worker(tc::checked_device_index(device));
    tc::TorchCudaStreamGuard stream_guard(launch);
    return metrics.epoch_average();
   });
   const bool completed = mean.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
   pending.release();
   require(entered && completed && mean.get() == 4, "settled epoch mean required another GPU handoff");
  }
  TrainingDistributedTestAccess::retire(metrics);
  require(metric_owner.expired(), "settled local metrics leaked");
 }
}
void exercise_collective_cancellation(DistributedContext& group, int device, std::string_view operation) {
 const auto store = TrainingDistributedTestAccess::store(group);
 if (group.rank == 0) {
  static_cast<void>(store->get("custody-submitted"));
  store->set("custody-cancel", std::vector<std::uint8_t>{1});
  throw std::runtime_error("injected " + std::string(operation) + " cancellation");
 }
 const auto real_backend = TrainingDistributedTestAccess::backend(group);
 group = TrainingDistributedTestAccess::backend(c10::make_intrusive<CancelCollectiveBackend>(real_backend, store), group.rank, device);
 const auto transport = TrainingDistributedTestAccess::custody(group);
 std::exception_ptr failure;
 if (operation == "count") {
  TrainingTargetCounts counts(2, 2, device, group);
  counts.begin_attempt();
  counts.begin_wave(2);
  const auto custody = TrainingDistributedTestAccess::custody(counts);
  auto waiting = std::async(std::launch::async, [&] {
   tc::TorchCudaDeviceGuard worker(tc::checked_device_index(device));
   try {
    static_cast<void>(counts.consume(1, tc::getCurrentCUDAStream(tc::checked_device_index(device)).stream()));
   } catch (const std::exception& error) { return std::string(error.what()); }
   return std::string{};
  });
  counts.publish(0, 1);
  try {
   counts.resolve();
  } catch (...) { failure = std::current_exception(); }
  require(waiting.get() == "peer training cancellation", "in-flight count cancellation did not drain its waiter");
  TrainingDistributedTestAccess::retire(counts);
  require(TrainingDistributedTestAccess::terminal(counts) && !custody.expired(), "in-flight NCCL count lost terminal custody");
 } else if (operation == "metric") {
  TrainingMetricHandoff metrics(device);
  metrics.begin_attempt(1);
  metrics.accumulate_empty();
  const auto custody = TrainingDistributedTestAccess::custody(metrics);
  try {
   static_cast<void>(metrics.complete_step(torch::zeros({}, torch::TensorOptions().device(tc::cuda_device(device))), 1, 1, group));
  } catch (...) { failure = std::current_exception(); }
  TrainingDistributedTestAccess::retire(metrics);
  require(TrainingDistributedTestAccess::terminal(metrics) && !custody.expired(), "in-flight NCCL metric lost full device/pinned custody");
 } else if (operation == "broadcast") {
  std::vector<torch::Tensor> tensors;
  for (int i = 0; i < 8; ++i) tensors.push_back(torch::ones({4}, torch::TensorOptions().device(tc::cuda_device(device))));
  try {
   broadcast_training_tensors(group, tensors, 64);
  } catch (...) { failure = std::current_exception(); }
  for (const auto& tensor : tensors) require(tensor.use_count() > 1, "in-flight coalesced broadcast lost its tensor inventory");
 } else
  throw std::invalid_argument("unknown collective cancellation fixture");
 const auto rank = group.rank;
 group = {};
 group.rank = rank;
 require(failure != nullptr && !transport.expired(), "real collective cancellation failed to retain transport");
 std::fprintf(stderr, "%.*s collective terminal custody verified\n", static_cast<int>(operation.size()), operation.data());
 std::rethrow_exception(failure);
}
namespace {
struct PreparedBackwardGate final {
 explicit PreparedBackwardGate(int device) { tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize(); }
 ~PreparedBackwardGate() { release(); }
 void release() noexcept {
  if (released) return;
  released = true;
  gate.release();
  if (!armed.load()) try {
    complete.synchronize();
   } catch (...) {}
 }
 torch::Tensor hold(const torch::Tensor& gradient) {
  if (!armed.exchange(false)) return gradient;
  const auto stream = tc::getCurrentCUDAStream(tc::checked_device_index(gradient.get_device()));
  gate.hold(stream.stream());
  // Autograd submits a real derivative after the device gate and returns to
  // the existing worker. It never blocks CPU submission on this test gate.
  auto retained = gradient.clone();
  complete.record(stream);
  return retained;
 }
 mmltk::testsupport::CudaTestGate gate;
 at::cuda::CUDAEvent complete;
 std::atomic<bool> armed{true};
 bool released = false;
};
struct PreparedUploadSignal final {
 std::atomic<bool> delivered{false};
 std::promise<torch::Tensor> uploaded;
 std::binary_semaphore release_worker{0};
 void fail(std::exception_ptr failure) {
  if (!delivered.exchange(true)) uploaded.set_exception(std::move(failure));
 }
};
std::vector<NormalizedModelStateEntry> host_training_state(TrainingModel& model) {
 auto state = collect_module_state(model.model());
 for (auto& value : state) value.tensor = value.tensor.detach().cpu().clone();
 return state;
}
void equal_training_state(const std::vector<NormalizedModelStateEntry>& actual, const std::vector<NormalizedModelStateEntry>& expected, std::string_view transition) {
 require(actual.size() == expected.size(), "prepared training parameter inventory differs");
 for (std::size_t i = 0; i < actual.size(); ++i) {
  require(actual[i].name == expected[i].name, "prepared training parameter identity differs");
  if (torch::equal(actual[i].tensor, expected[i].tensor)) continue;
  std::ostringstream message;
  message.precision(17);
  message << transition << " changed the exact parameter trajectory: " << actual[i].name
          << ", maximum difference=" << (actual[i].tensor.to(torch::kFloat64) - expected[i].tensor.to(torch::kFloat64)).abs().max().item<double>();
  throw std::runtime_error(message.str());
 }
}
}  // namespace
void exercise_prepared_training(const DistributedContext& distributed, int device) {
 namespace data = mmltk::backend::data;
 const bool deterministic = at::globalContext().deterministicCuDNN();
 const bool benchmark = at::globalContext().benchmarkCuDNN();
 const mmltk::testsupport::ScopedTestCleanup restore_convolution([=] {
  at::globalContext().setDeterministicCuDNN(deterministic);
  at::globalContext().setBenchmarkCuDNN(benchmark);
 });
 // Exact trajectory comparison requires repeatable convolution reductions;
 // seeding alone does not select deterministic cuDNN backward algorithms.
 at::globalContext().setDeterministicCuDNN(true);
 at::globalContext().setBenchmarkCuDNN(false);
 tc::TorchCudaDeviceGuard device_guard(tc::checked_device_index(device));
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 const std::array batches{static_cast<std::size_t>(distributed.world_size), std::size_t{1}};
 for (std::size_t batch_case = 0; batch_case < (distributed.world_size > 1 ? 2U : 1U); ++batch_case) {
  mmltk::testsupport::ScopedTempDir root("prepared-training");
  data::testsupport::FixtureSpec fixture;
  fixture.root_dir = root.path().string();
  fixture.width = fixture.height = 64;
  fixture.num_images = static_cast<int>(4 * batches[batch_case] + 1);
  fixture.background_images = 0;
  fixture.pixel_evidence = true;
  data::testsupport::create_synthetic_dataset(fixture);
  data::testsupport::compile_existing_fixture(fixture);
  auto runtime_config = resolve_runtime_config(4, 1, 2, {}, device);
  RuntimeContext runtime(runtime_config);
  ScopedRuntimeContext runtime_scope(&runtime);
  auto workers = std::make_shared<mmltk::common::concurrency::WorkerPool>(1, runtime.lane_cpus(), "preparefixture", 0, &runtime.execution().placement, false);
  data::DatasetLoader::Config loader_config;
  loader_config.compiled_path = data::testsupport::compiled_bin_path(fixture);
  loader_config.batch_size = std::max<std::uint64_t>(1, training_rank_slice(batches[batch_case], distributed.rank, distributed.world_size).count);
  loader_config.prefetch_factor = 2;
  loader_config.device_id = device;
  loader_config.execution = runtime.execution();
  auto catalog_loader = std::make_unique<data::DatasetLoader>(loader_config);
  loader_config.source = catalog_loader->compiled_source();
  TrainRequest request;
  request.train_compiled_path = loader_config.compiled_path;
  request.val_compiled_path = request.train_compiled_path;
  request.output_dir = root.path() / "training";
  request.weights_path = root.path() / "seeded-native-fixture.pt";
  request.batch_size = batches[batch_case];
  request.device_id = device;
  request.epochs = 1;
  request.lanes = request.grad_accum_steps = 1;
  request.compilation_mode = CompilationMode::kNone;
  request.amp = request.fused_optimizer = request.use_ema = false;
  request.freeze_encoder = true;
  request.recipe.optimizer = TrainOptimizerKind::SGD;
  request.recipe.lr = 1e-4;
  request.recipe.weight_decay = 0;
  request.gpu_augmentation = {.enabled = true, .geometry = {}, .resize = {}, .color = {}, .noise = {}, .blur = {}, .occlusion = {}, .copy_paste_probability = .5F};
  request.training_supervision.assignment = TrainAssignmentKind::MatchFree;
  TrainingDataPlan plan(*catalog_loader, request);
  const auto draws = plan.epoch(0, 0);
  const auto slice = plan.rank_slice(distributed.rank, distributed.world_size);
  const auto initial_draws = *draws.schedule;
  const auto annotations = catalog_loader->describe_batch(*draws.schedule, 0, request.batch_size, slice.begin, slice.count);
  require(!annotations.owner && !annotations.device_images && annotations.image_custody.expired(), "annotation preparation fabricated a raw-image lease");
  auto config = native_config_from_preset(model_presets().front());
  config.resolution = 64;
  config.num_classes = static_cast<int>(catalog_loader->num_classes()) + 1;
  config.num_queries = config.num_select = 2;
  config.dec_layers = config.group_detr = 1;
  config.training_supervision = request.training_supervision;
  request.num_queries = 2;
  auto detection = detection_fixture_base(config);
  project_loss_coefficients(detection, config);
  populate_default_detection_weight_dict(detection);
  std::shared_ptr<PreparedUploadSignal> signal;
  std::atomic<std::size_t> failures{0};
  const auto construct = [&](const std::filesystem::path& resume = {}) {
   torch::manual_seed(719);
   auto native = std::make_shared<NativeRfDetrModel>(config, native_training_class_layout(*catalog_loader->class_catalog()));
   native->initialize_training_supervision(request.seed);
   native->to(tc::cuda_device(device));
   native->set_force_pytorch_deformable_attn(true);
   auto owner = std::make_unique<TrainingModel>(
    request, 0, runtime, std::make_unique<data::DatasetLoader>(loader_config), native, plan, distributed, TrainingPrecision{}, detection, [&](std::uint64_t, std::exception_ptr error) {
    ++failures;
    if (signal) signal->fail(error);
   });
   if (!resume.empty()) {
    auto state = decode_model_state(resume);
    const auto saved = detail::read_training_continuation(*state.admitted_archive());
    require(saved.has_value(), "prepared training checkpoint lost continuation");
    owner->stage_resume(state, *saved);
    owner->commit_resume();
   }
   owner->start(workers);
   owner->begin_epoch(0, plan.epoch(0, 0));
   return owner;
  };
  const auto save = [&](TrainingModel& owner, const std::filesystem::path& path) {
   auto metadata = synthetic_training_metadata();
   metadata.class_layout = owner.model().class_layout()->record();
   metadata.preset_name = config.preset_name;
   metadata.num_classes = config.num_classes;
   metadata.num_queries = config.num_queries;
   metadata.num_select = config.num_select;
   auto publication = owner.begin_publication();
   owner.save_resume(path, metadata, "prepared-training", "original.json");
   publication.finish();
   auto state = decode_model_state(path);
   return detail::read_training_continuation(*state.admitted_archive())->values;
  };
  std::vector<TrainingScheduleState> clocks;
  std::vector<double> losses;
  detail::TrainingContinuationValues admitted;
  std::vector<NormalizedModelStateEntry> expected;
  {
   auto immediate = construct();
   while (!immediate->exhausted()) {
    require(immediate->attempt() == request.batch_size, "immediate training skipped a finite update");
    // Exercise normal admission with the very same owner, seed, and schedule.
    // Discarding ephemeral work changes neither RNG nor saved donor history.
    TrainingPreparationTestAccess::lanes(*immediate).discard_prepared();
    clocks.push_back(immediate->schedule());
    losses.push_back(immediate->progress(TrainingPhase::Train).step_loss);
    if (clocks.size() == 3) admitted = save(*immediate, root.path() / "immediate.pt");
   }
   immediate->end_epoch();
   expected = host_training_state(*immediate);
  }
  const auto checkpoint = root.path() / "prepared.pt";
  {
   auto prepared = construct();
   auto& lanes = TrainingPreparationTestAccess::lanes(*prepared);
   for (std::size_t step = 0; !prepared->exhausted(); ++step) {
    if (step == 2 && slice.count) {
     // Both target slots and all model workspaces have completed ordinary
     // attempts before the deterministic hold; first-use growth is separate.
     lanes.discard_prepared();
     PreparedBackwardGate gate(device);
     signal = std::make_shared<PreparedUploadSignal>();
     auto uploaded = signal->uploaded.get_future();
     auto parameter = prepared->model().named_parameters(true)["class_embed.weight"];
     const auto hook = parameter.register_hook([&](const torch::Tensor& gradient) { return gate.hold(gradient); });
     TrainingPreparationTestAccess::observe(lanes, [&](const PreparedTargets* targets, std::uintptr_t copy_stream, std::exception_ptr failure) {
      if (failure) {
       signal->fail(std::move(failure));
       return;
      }
      try {
       require(targets != nullptr, "successful preparation has no target view");
       const auto stream = tc::external_torch_cuda_stream(copy_stream, tc::checked_device_index(device));
       tc::TorchCudaStreamGuard guard(stream);
       stream.synchronize();
       require(!gate.armed.load() && !gate.complete.query(), "next targets did not finish uploading during held backward");
       auto identities = targets->all_image_ids.cpu();
       require(targets->microbatch_key == draws.schedule->microbatch_keys[3], "prepared target draw key differs");
       if (!signal->delivered.exchange(true)) signal->uploaded.set_value(std::move(identities));
       signal->release_worker.acquire();
      } catch (...) {
       signal->fail(std::current_exception());
       throw;
      }
     });
     auto attempt = std::async(std::launch::async, [&] {
      tc::TorchCudaStreamGuard guard(launch);
      ScopedRuntimeContext scope(&runtime);
      return prepared->attempt();
     });
     try {
      const auto identities = uploaded.get();
      for (std::size_t image = 0; image < slice.count; ++image)
       require(identities[image].item<std::int64_t>() == draws.schedule->image_indices[3 * request.batch_size + slice.begin + image] + 1,
        "prepared target identities differ from the exact future rank slice");
      TrainingDonorHistory extra(1, request.batch_size);
      bool duplicate_rejected = false;
      try {
       lanes.prepare_next(extra, draws.schedule, slice, request.seed, 0, distributed.rank, 4, false);
      } catch (const std::logic_error&) { duplicate_rejected = true; }
      require(duplicate_rejected, "training admitted a second future preparation");
      gate.release();
      require(attempt.get() == request.batch_size, "held training update was lost");
      // The worker is still holding only the completed future preparation.
      // Publication must freeze the admitted trajectory without waiting for it.
      const auto saved = save(*prepared, checkpoint);
      require(saved.data == admitted.data && saved.schedule == admitted.schedule, "speculative preparation entered saved donor or sampler state");
      signal->release_worker.release();
      parameter.remove_hook(hook);
      TrainingPreparationTestAccess::observe(lanes, {});
      workers->wait_idle();
      signal.reset();
     } catch (...) {
      gate.release();
      signal->release_worker.release();
      if (attempt.valid()) try {
        (void)attempt.get();
       } catch (...) {}
      parameter.remove_hook(hook);
      TrainingPreparationTestAccess::observe(lanes, {});
      workers->wait_idle();
      signal.reset();
      throw;
     }
    } else {
     require(prepared->attempt() == request.batch_size, "prepared training skipped a finite update");
     if (step == 2) {
      const auto saved = save(*prepared, checkpoint);
      require(saved.data == admitted.data && saved.schedule == admitted.schedule, "empty rank preparation changed continuation");
     }
    }
    require(prepared->schedule() == clocks.at(step), "lookahead changed the optimizer clock");
    require(prepared->progress(TrainingPhase::Train).step_loss == losses.at(step), "lookahead used a different parameter version");
   }
   require(!lanes.has_prepared(), "training prepared across an epoch boundary");
   require(prepared->attempt() == 0, "exhausted epoch admitted another draw");
   prepared->end_epoch();
   equal_training_state(host_training_state(*prepared), expected, "lookahead");
  }
  {
   auto resumed = construct(checkpoint);
   require(resumed->schedule() == admitted.schedule, "Resume restored speculative progress");
   while (!resumed->exhausted()) require(resumed->attempt() == request.batch_size, "resumed training skipped a finite update");
   resumed->end_epoch();
   equal_training_state(host_training_state(*resumed), expected, "resume");
  }
  require(failures == 0, "healthy preparation invoked the global failure path");
  if (!distributed.enabled) {
   TrainingDonorHistory history(1, request.batch_size);
   auto cancelled = construct();
   require(cancelled->attempt() == request.batch_size, "deferred-failure fixture lost its initial update");
   auto& lanes = TrainingPreparationTestAccess::lanes(*cancelled);
   const auto prepare_invalid = [&] {
    lanes.discard_prepared();
    const auto before = cancelled->schedule();
    auto invalid = std::make_shared<data::DatasetIndexSchedule>(*draws.schedule);
    invalid->image_indices[before.consumed_microbatches * request.batch_size] = static_cast<std::uint32_t>(catalog_loader->num_images());
    lanes.prepare_next(history, invalid, slice, request.seed, 0, 0, before.consumed_microbatches, false);
    workers->wait_idle();
    require(failures == 0 && cancelled->schedule() == before, "future preparation failure cancelled an admitted update");
    return before;
   };
   (void)prepare_invalid();
   lanes.discard_prepared();
   require(cancelled->attempt() == request.batch_size, "discarded future failure leaked into normal admission");
   const auto before = prepare_invalid();
   bool original_failure = false;
   try {
    (void)cancelled->attempt();
   } catch (const std::invalid_argument& error) { original_failure = std::string_view(error.what()) == "scheduled annotation image is outside dataset"; }
   require(original_failure && failures > 0 && cancelled->schedule() == before, "future failure lost its original cause or advanced admission");
  }
  require(draws.schedule->image_indices == initial_draws.image_indices && draws.schedule->draw_keys == initial_draws.draw_keys && draws.schedule->microbatch_keys == initial_draws.microbatch_keys,
   "preparation mutated the immutable sampler");
 }
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
