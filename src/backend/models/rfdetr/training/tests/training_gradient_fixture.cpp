#include "training_gradient_fixture.h"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/backend/models/rfdetr/training/detail/training_gradient_reducer.h"
#include "src/backend/models/rfdetr/training/detail/training_lanes.h"
#include "src/backend/models/rfdetr/training/detail/training_data_plan.h"
#include "src/backend/models/rfdetr/training/detail/training_metrics.h"
#include "src/backend/models/rfdetr/training/detail/training_step.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/backend/models/rfdetr/training/detail/model_ema.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include <ATen/cuda/CUDAEvent.h>
#include <torch/csrc/autograd/custom_function.h>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace mmltk::backend::models::rfdetr::testsupport {
namespace tc = mmltk::backend::ml::cuda;
struct TrainingGradientReducerTestAccess {
 static std::weak_ptr<const void> custody(const TrainingGradientReducer& reducer) { return reducer.impl_; }
 static void retire(TrainingGradientReducer& reducer) { reducer.retire(); }
 static auto fact(const TrainingGradientReducer& reducer) { return reducer.retirement_.fact(); }
};
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
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
 for (auto& lane : result) for (const auto& value : optimizer.parameters()) lane.push_back(value.detach().clone().set_requires_grad(true));
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
 tc::TensorReadbackBuffers readback; readback.Begin(); optimizer.reserve_checkpoint(readback, 0);
 torch::serialize::OutputArchive archive; optimizer.save(archive, readback, 0); readback.Complete();
 std::stringstream stream; archive.save_to(stream);
 torch::serialize::InputArchive loaded; loaded.load_from(stream); optimizer.load(loaded);
}
struct BackwardGate {
 explicit BackwardGate(int device) : stream(tc::getStreamFromPool(false, tc::checked_device_index(device))) {}
 tc::TorchCudaStream stream;
 at::cuda::CUDAEvent event;
 std::mutex mutex;
 std::condition_variable changed;
 bool entered = false, released = false;
 void enter() {
  // Queue an actual CUDA completion event behind a held host callback, only
  // after the early backward branch has run. The late autograd derivative
  // cannot proceed until the test releases this device event.
  const auto status = cudaLaunchHostFunc(stream.stream(), [](void* context) {
   auto& gate = *static_cast<BackwardGate*>(context);
   std::unique_lock lock(gate.mutex); gate.changed.wait(lock, [&] { return gate.released; });
  }, this);
  require(status == cudaSuccess, "could not hold late backward CUDA event");
  event.record(stream);
  { std::lock_guard lock(mutex); entered = true; changed.notify_all(); }
  event.synchronize();
 }
 void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
 bool await() { std::unique_lock lock(mutex); return changed.wait_for(lock, std::chrono::seconds(20), [&] { return entered; }); }
};
class LateBackward final : public torch::autograd::Function<LateBackward> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* context, torch::Tensor input, std::int64_t gate) {
  context->saved_data["gate"] = gate; return input.clone();
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* context, torch::autograd::variable_list gradients) {
  // The device event holds the actual late autograd node. There is no
  // future-completion surrogate; the early bucket must launch meanwhile.
  reinterpret_cast<BackwardGate*>(context->saved_data["gate"].toInt())->enter();
  return {gradients[0], torch::Tensor{}};
 }
};
}
void exercise_training_initialization(const DistributedContext& distributed, int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 torch::manual_seed(713);
 auto config = native_config_from_preset(model_presets().front());
 config.resolution = 64; config.num_classes = 3; config.num_queries = 3; config.num_select = 3; config.dec_layers = 1; config.group_detr = 1;
 config.training_supervision.assignment = TrainAssignmentKind::MatchFree; config.training_supervision.denoising.enabled = true;
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
 prepare(model.named_parameters(true)); prepare(model.named_buffers(true));
 broadcast_training_model(distributed, model);
 std::size_t index = 0;
 auto valid = torch::ones({}, torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kBool));
 const auto verify = [&](const auto& inventory) { for (const auto& item : inventory) valid.logical_and_(item.value().eq(before[index++]).all()); };
 verify(model.named_parameters(true)); verify(model.named_buffers(true));
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
 reducer.begin_attempt(1);
 auto count = torch::zeros({1}, late.options().requires_grad(false)); distributed_all_reduce_tensor(distributed, count);
 reducer.enable_gradient_launch_after_counts();
 BackwardGate gate(device);
 at::cuda::CUDAEvent parameters_ready; parameters_ready.record(launch);
 const auto worker_stream = tc::getStreamFromPool(false, tc::checked_device_index(device));
 auto future = std::async(std::launch::async, [&] {
  tc::TorchCudaStreamGuard stream(worker_stream);
  parameters_ready.block(worker_stream);
  // Build the late branch first, then the early branch. Autograd's node
  // sequence priority executes the early independent branch first.
  auto delayed = LateBackward::apply(late, reinterpret_cast<std::int64_t>(&gate));
  auto loss = (delayed + early.square()).sum();
  reducer.arm(0); static_cast<void>(TrainingStep(1, 1, false, at::kFloat).gradients(loss, parameters)); reducer.collect(0);
 });
 const bool entered = gate.await();
 const auto launched_while_held = reducer.launched_buckets();
 if (abort_after_launch && entered && launched_while_held == 1) {
  const auto custody = TrainingGradientReducerTestAccess::custody(reducer);
  std::exception_ptr failure;
  try {
#if defined(USE_C10D_NCCL)
   if (distributed.enabled && distributed.rank != 0) {
    gate.release(); future.get();
    // The late bucket has no rank-zero counterpart. Tell the initiating
    // rank that this peer is now waiting for real collective completion.
    distributed.store->set("failure-peer-waiting", std::vector<std::uint8_t>{1});
    reducer.finish_attempt();
    throw std::runtime_error("cancelled peer unexpectedly completed its collective");
   }
   if (distributed.enabled) static_cast<void>(distributed.store->get("failure-peer-waiting"));
#endif
   throw std::runtime_error("injected failure after early gradient bucket");
  } catch (...) { failure = std::current_exception(); reducer.abort(failure); }
  gate.release();
  if (future.valid()) try { future.get(); } catch (...) {}
  worker_stream.synchronize();
  TrainingGradientReducerTestAccess::retire(reducer);
  if (distributed.enabled) {
   require(TrainingGradientReducerTestAccess::fact(reducer).terminal && !custody.expired(),
    "aborted NCCL bucket/model custody was reclaimed without physical completion");
   require(late.use_count() >= 4 && early.use_count() >= 4,
    "terminal reducer lost its master and lane leaf references");
  } else {
   require(custody.expired(), "settled local reducer custody was unnecessarily retained");
  }
  std::fprintf(stderr, "early bucket failure custody verified on rank %d\n", distributed.rank);
  std::rethrow_exception(failure);
 }
 gate.release(); future.get(); reducer.finish_attempt();
 require(entered, "late backward did not reach its hold event");
 require(launched_while_held == 1, "early bucket did not launch while late backward was held");
 equal(early.grad(), torch::full_like(early, 2 * distributed.world_size), "early NCCL SUM differs");
 equal(late.grad(), torch::full_like(late, distributed.world_size), "late NCCL SUM differs");
}
void exercise_gradient_trajectory(const DistributedContext& distributed, int device) {
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 const auto launch = tc::getCurrentCUDAStream(tc::checked_device_index(device));
 constexpr std::size_t k = 3 * 2;
 for (const auto kind : {TrainOptimizerKind::AdamW, TrainOptimizerKind::Muon, TrainOptimizerKind::SGD})
  for (const std::size_t batch : {1U, 3U})
   for (const std::size_t capacity : {1U, 2U}) {
    TrainRequest request; request.lanes = 3; request.grad_accum_steps = 2; request.batch_size = batch; request.recipe.optimizer = kind; request.recipe.lr = .003; request.recipe.lr_encoder = .0003;
    request.recipe.lr_scheduler = TrainLrSchedulerKind::Step; request.recipe.warmup_epochs = 0; request.fused_optimizer = false;
    auto working = inventory(device, distributed.rank == 0 ? .3 : .9);
    for (auto& item : working) { auto value = item.value(); distributed_broadcast(distributed, value); }
    auto integer_buffer = torch::full({2}, distributed.rank == 0 ? 7 : 11, torch::TensorOptions().device(tc::cuda_device(device)).dtype(torch::kInt64));
    distributed_broadcast(distributed, integer_buffer); require(integer_buffer.eq(7).all().item<bool>(), "initial nonfloating buffer broadcast differs");
    auto reference = inventory(device, .3);
    auto built = build_optimizer(working, request), reference_built = build_optimizer(reference, request);
    auto& optimizer = built.optimizer; auto& expected_optimizer = reference_built.optimizer;
    const auto physical = distributed.rank == 0 ? capacity : 1U; // Deliberately different P across ranks.
    auto facts = derive_execution_facts(request, 0); facts.admitted_capacity = physical;
    require(facts.microbatches_per_attempt == k && facts.effective_batch_per_model == batch * k, "physical capacity changed logical K or batch");
    auto leaves = lane_leaves(optimizer, physical);
    TrainingGradientReducer reducer(distributed, device, launch, optimizer.parameter_names(), optimizer.parameters(), leaves, 16);
    TrainingTargetCounts counts(physical, device, distributed);
    TrainingMetricHandoff metrics(device); metrics.reset_epoch();
    GradScaler scaler(true, 128, 2, .5, 2), expected_scaler(true, 128, 2, .5, 2);
    ModelEma ema(optimizer.eligible_parameters(), .9, 3), expected_ema(expected_optimizer.eligible_parameters(), .9, 3);
    TrainingSchedule schedule(request.recipe, built.base_lrs, built.roles, 2, 4 * k, k);
    schedule.begin_epoch(0);
    double epoch_loss = 0, epoch_class_error = 0, epoch_cardinality = 0;
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
     if (attempt == 2) {
      restore_optimizer(optimizer);
      const auto saved_metrics = metrics.state(); metrics.restore_epoch(saved_metrics);
      const auto saved = schedule.state(); schedule.restore(saved);
      scaler.load_state(scaler.current_scale(), scaler.growth_tracker());
      std::vector<torch::Tensor> cpu_shadow; for (const auto& tensor : ema.shadow_params()) cpu_shadow.push_back(tensor.cpu());
      ema = ModelEma::from_cpu_shadow(optimizer.eligible_parameters(), cpu_shadow, .9, 3, ema.completed_updates());
     }
     if (attempt == 3) {
      working["backbone.0.encoder.weight"].set_requires_grad(true); reference["backbone.0.encoder.weight"].set_requires_grad(true);
      optimizer.zero_grad(true); expected_optimizer.zero_grad(true); optimizer.activate(); expected_optimizer.activate();
      leaves = lane_leaves(optimizer, physical); reducer.rebuild(optimizer.parameter_names(), optimizer.parameters(), leaves);
     }
     { torch::NoGradGuard no_grad; for (auto& lane : leaves) for (std::size_t p = 0; p < lane.size(); ++p) lane[p].copy_(optimizer.parameters()[p]); }
     at::cuda::CUDAEvent parameters_ready; parameters_ready.record(launch);
     reducer.begin_attempt(k); metrics.begin_attempt(k); expected_optimizer.zero_grad(true);
     double attempt_loss = 0;
     const auto slice = TrainingDataPlan::rank_slice(batch, distributed.rank, distributed.world_size);
     for (std::size_t start = 0; start < k; start += physical) {
      const auto slots = std::min(physical, k - start); counts.begin(slots);
      std::vector<std::future<torch::Tensor>> futures(slots);
      std::vector<std::promise<void>> completed(slots);
      std::vector<std::shared_future<void>> completion;
      for (auto& promise : completed) completion.push_back(promise.get_future().share());
      for (std::size_t reverse = slots; reverse > 0; --reverse) {
       const auto lane = reverse - 1, micro = start + lane;
       std::int64_t local_count = 0; for (std::size_t image = slice.begin; image < slice.begin + slice.count; ++image) local_count += targets(micro, image);
       counts.publish(lane, local_count);
       if (!slice.count) { reducer.contribute_empty(); continue; }
       futures[lane] = std::async(std::launch::async, [&, lane, micro] {
        try {
        tc::TorchCudaStreamGuard stream(tc::getStreamFromPool(false, tc::checked_device_index(device)));
        parameters_ready.block(tc::getCurrentCUDAStream(tc::checked_device_index(device)));
        const auto normalizer = counts.consume(lane, tc::getCurrentCUDAStream(tc::checked_device_index(device)).stream());
        auto loss = objective(leaves[lane], micro, slice, normalizer.target_count, static_cast<int>(attempt % 3));
        reducer.arm(lane); static_cast<void>(TrainingStep(k, scaler.current_scale(), false, at::kFloat).gradients(loss, leaves[lane])); reducer.collect(lane);
        // Physically settle this test-only return before its scalar is borrowed.
        tc::getCurrentCUDAStream(tc::checked_device_index(device)).synchronize();
        if (lane + 1 < slots) completion[lane + 1].get();
        completed[lane].set_value();
        return loss.detach();
        } catch (...) { completed[lane].set_exception(std::current_exception()); counts.fail(std::current_exception()); reducer.abort(std::current_exception()); throw; }
       });
      }
      counts.resolve(distributed, tc::cuda_device(device));
      if (start + slots == k) reducer.enable_gradient_launch_after_counts();
      for (std::size_t lane = 0; lane < slots; ++lane) {
       const auto micro = start + lane;
       if (futures[lane].valid()) {
        const auto loss = futures[lane].get();
        double matched = 0, errors = 0, cardinality = 0;
        for (std::size_t image = slice.begin; image < slice.begin + slice.count; ++image) { matched += targets(micro, image); errors += targets(micro, image) * (10 + image); cardinality += image + 1; }
        const auto scalar = [&](double value) { return torch::full({}, value, loss.options()); };
        TrainingDiagnosticTensors diagnostics{scalar(errors), scalar(matched), scalar(cardinality), scalar(slice.count)};
        scalar_packet::Tensors scalars;
        scalar_packet::set<^^TrainingScalars::class_error>(scalars, scalar(matched ? errors / matched : 100));
        scalar_packet::set<^^TrainingScalars::cardinality_error>(scalars, scalar(cardinality / slice.count));
        metrics.accumulate(loss, loss, loss, scalars, diagnostics);
       }
       else metrics.accumulate_empty();
       std::int64_t global_count = 0; for (std::size_t image = 0; image < batch; ++image) global_count += targets(micro, image);
       const auto loss = objective(expected_optimizer.parameters(), micro, {0, batch}, torch::full({}, global_count, working.begin()->value().options().requires_grad(false)), static_cast<int>(attempt % 3));
       TrainingStep(k, expected_scaler.current_scale(), false, at::kFloat).backward(loss);
       attempt_loss += loss.detach().item<double>(); schedule.consume_microbatch();
       double errors = 0, cardinality = 0;
       for (std::size_t image = 0; image < batch; ++image) { errors += targets(micro, image) * (10 + image); cardinality += image + 1; }
       epoch_class_error += global_count ? errors / global_count : 100;
       epoch_cardinality += cardinality / batch;
      }
     }
     reducer.finish_attempt();
     for (std::size_t p = 0; p < optimizer.parameters().size(); ++p) equal(optimizer.parameters()[p].grad(), expected_optimizer.parameters()[p].grad(), "fixed-K global gradients differ across capacity/rank slicing");
     // Inject an overflow on one rank only after SUM, so the finite control
     // itself must agree the skipped update and scaler/EMA clocks.
     if (attempt == 1 && distributed.rank == distributed.world_size - 1) optimizer.parameters()[0].mutable_grad().fill_(std::numeric_limits<float>::infinity());
     const auto found_inf = scaler.check_and_unscale_(optimizer);
     const auto snapshot = metrics.complete_step(found_inf, k, (attempt + 1) * k, distributed);
     static_cast<void>(expected_scaler.check_and_unscale_(expected_optimizer));
     const bool skipped = attempt == 1;
     require(snapshot.loss_finite && snapshot.gradients_finite == !skipped, "global overflow decision differs");
     epoch_loss += attempt_loss;
     require(std::abs(snapshot.loss_sum - epoch_loss) < 2e-5, "global live epoch numerator differs");
     require(std::abs(snapshot.step_loss - attempt_loss / k) < 2e-5, "global live attempt mean differs");
     require(snapshot.scalars.class_error && std::abs(*snapshot.scalars.class_error - epoch_class_error / ((attempt + 1) * k)) < 2e-5, "matched-class sufficient statistics differ");
     require(snapshot.scalars.cardinality_error && std::abs(*snapshot.scalars.cardinality_error - epoch_cardinality / ((attempt + 1) * k)) < 2e-5, "global image/cardinality statistics differ");
     if (!skipped) { optimizer.clip_grad_norm_(.1); expected_optimizer.clip_grad_norm_(.1); }
     scaler.step(optimizer, skipped); expected_scaler.step(expected_optimizer, skipped); scaler.update(skipped); expected_scaler.update(skipped);
     ema.update(); expected_ema.update(); optimizer.zero_grad(false); optimizer.zero_grad(true); expected_optimizer.zero_grad(true);
     schedule.prepare_attempt(); schedule.finish_attempt(!skipped);
     require(scaler.current_scale() == expected_scaler.current_scale() && scaler.growth_tracker() == expected_scaler.growth_tracker(), "rank scaler clocks differ");
     require(ema.completed_updates() == static_cast<std::int64_t>(attempt + 1), "overflow did not consume EMA attempt");
     require(schedule.state().consumed_attempts == attempt + 1 && schedule.state().successful_updates == attempt + 1 - (attempt >= 1), "attempt/success clocks differ");
     require(checked_training_product(schedule.state().consumed_microbatches, batch) == (attempt + 1) * k * batch, "global processed images were counted per rank");
     require(checked_training_product(schedule.state().successful_updates, facts.effective_batch_per_model) == (attempt + 1 - (attempt >= 1)) * k * batch, "overflow changed successful-update image count");
     for (std::size_t p = 0; p < optimizer.eligible_parameters().size(); ++p) {
      equal(optimizer.eligible_parameters()[p], expected_optimizer.eligible_parameters()[p], "optimizer trajectory differs from global reference");
      equal(ema.shadow_params()[p], expected_ema.shadow_params()[p], "EMA trajectory differs from global reference");
     }
    }
    static_cast<void>(metrics.epoch_count(4 * k)); require(std::abs(metrics.epoch_average() - epoch_loss / (4 * k)) < 2e-5, "final temporal mean differs");
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
}
