#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/backend/models/rfdetr/training/detail/training_step.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include <ATen/Context.h>
#include "src/backend/models/rfdetr/training/detail/model_ema.h"
#include "src/backend/models/rfdetr/training/detail/training_ops_private.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <cmath>
#include <array>
#include <torch/csrc/autograd/custom_function.h>
#include <numbers>
#include <limits>
#include <sstream>
#include "src/backend/ml/torch/archive.h"
namespace {
namespace rf = mmltk::backend::models::rfdetr;
namespace tensor_fixture = mmltk::backend::ml::testsupport;
torch::serialize::InputArchive optimizer_checkpoint(rf::NativeOptimizer& optimizer) {
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
 readback.Begin();
 optimizer.reserve_checkpoint(readback, 0);
 torch::serialize::OutputArchive archive;
 optimizer.save(archive, readback, 0);
 readback.Complete();
 std::stringstream bytes;
 archive.save_to(bytes);
 torch::serialize::InputArchive restored;
 restored.load_from(bytes);
 return restored;
}
// Independent equations: RF-DETR e9a138f module_model.py:607-611,1296;
// Lightning 2.6.0 ClosureResult; PyTorch 2.9 Adam and AveragedModel.
// Native FP32/FP64 policy, no TF32 changes, no epoch-trajectory/AP claim.
TEST_CASE("Stock AdamW owns each parameter age and double accumulation division", "[rfdetr][training][parity]") {
 for (const auto device : tensor_fixture::available_devices())
  for (const auto backend : {rf::NativeOptimizerBackend::eager, rf::NativeOptimizerBackend::foreach, rf::NativeOptimizerBackend::fused}) {
   if (backend == rf::NativeOptimizerBackend::fused && device.is_cpu()) continue;
   for (const int count : {1, 4})
    for (const bool parallel : {false, true}) {
     auto a = torch::tensor({0.713, -0.281}, torch::TensorOptions().dtype(torch::kFloat64).device(device)).set_requires_grad(true);
     auto b = torch::tensor({-0.411, 0.937}, torch::TensorOptions().dtype(torch::kFloat64).device(device)).set_requires_grad(true);
     rf::NativeAdamW::Group group{{0.003, 0.07, true}, {0, 1}};
     rf::NativeOptimizer optimizer(rf::NativeAdamW({group}, {{"a", a}, {"b", b}}, backend));
     std::array<torch::Tensor, 2> reference{a.detach().clone(), b.detach().clone()};
     std::array<torch::Tensor, 2> moment{torch::zeros_like(a), torch::zeros_like(b)};
     auto variance = moment;
     auto maximum = moment;
     std::array<int, 2> age{};
     for (int step = 0; step < 4; ++step) {
      CAPTURE(device, backend, count, parallel, step);
      optimizer.zero_grad(true);
      const std::array<torch::Tensor, 2> operands{a.detach().clone(), b.detach().clone()};
      std::array<torch::Tensor, 2> gradients{torch::zeros_like(a), torch::zeros_like(b)};
      for (int micro = 0; micro < count; ++micro) {
       // Nonuniform local target denominators are criterion policy, independent
       // from the two accumulation divisors. Metrics retain this unscaled loss.
       const double targets = micro + 1;
       torch::Tensor loss;
       const rf::TrainingStep execution(count, 1.0, false, torch::kFloat32);
       execution.forward([&] { loss = ((a * (0.3 + micro)).square().sum() + (step ? b.square().sum() * (step == 2 ? 0.0 : 1.0) : torch::zeros({}, b.options()))) / targets; });
       if (parallel) {
        const auto harvested = execution.gradients(loss, {a, b});
        torch::NoGradGuard guard;
        for (size_t i = 0; i < harvested.size(); ++i)
         if (harvested[i].defined()) {
          auto parameter = i == 0 ? a : b;
          if (parameter.grad().defined())
           parameter.mutable_grad().add_(harvested[i]);
          else
           parameter.mutable_grad() = harvested[i].detach().clone();
         }
       } else
        execution.backward(loss);
       gradients[0] += 2 * operands[0] * std::pow(0.3 + micro, 2) / targets / count / count;
       if (step && step != 2) gradients[1] += 2 * operands[1] / targets / count / count;
      }
      REQUIRE(torch::allclose(a.grad(), gradients[0], 1e-10, 1e-10));
      if (step)
       REQUIRE(torch::allclose(b.grad(), gradients[1], 1e-10, 1e-10));
      else
       REQUIRE_FALSE(b.grad().defined());
      const auto norm = torch::sqrt(gradients[0].square().sum() + gradients[1].square().sum());
      const auto factor = (0.2 / (norm + 1e-6)).clamp_max(1.0);
      optimizer.clip_grad_norm_(0.2);
      for (size_t index = 0; index < 2; ++index) {
       if (index == 1 && step == 0) continue;
       ++age[index];
       const auto g = gradients[index] * factor;
       moment[index] = 0.9 * moment[index] + 0.1 * g;
       variance[index] = 0.999 * variance[index] + 0.001 * g.square();
       maximum[index] = torch::maximum(maximum[index], variance[index]);
       reference[index] = reference[index] * (1 - 0.003 * 0.07) - 0.003 * (moment[index] / (1 - std::pow(0.9, age[index]))) / (torch::sqrt(maximum[index] / (1 - std::pow(0.999, age[index]))) + 1e-8);
      }
      optimizer.step();
      // Foreach step tensors are float32 even for float64 parameters.
      REQUIRE(torch::allclose(a, reference[0], 2e-6, 2e-8));
      REQUIRE(torch::allclose(b, reference[1], 2e-6, 2e-8));
      if (step == 1) {
       // Save and reload genuinely unequal ages (2 versus 1), including moments.
       auto restored = optimizer_checkpoint(optimizer);
       optimizer.load(restored);
      }
     }
    }
  }
}
TEST_CASE("Stock EMA copies first then averages completed attempts and resumes its count", "[rfdetr][training][parity]") {
 for (const double tau : {0.0, 3.0}) {
  auto weight = torch::tensor({1.3, -0.8}, torch::kFloat64);
  rf::ModelEma ema({weight}, 0.9, tau);
  weight.add_(0.7);
  ema.update();
  REQUIRE(torch::equal(ema.shadow_params()[0], weight));
  auto expected = weight.clone();
  for (int attempt = 2; attempt <= 5; ++attempt) {
   // Recoverable overflow attempts leave working weights unchanged but count.
   if (attempt != 2 && attempt != 4) weight.add_(0.31);
   const double decay = tau > 0 ? 0.9 * (1 - std::exp(-attempt / tau)) : 0.9;
   expected = decay * expected + (1 - decay) * weight;
   ema.update();
   REQUIRE(ema.completed_updates() == attempt);
   REQUIRE(torch::allclose(ema.shadow_params()[0], expected, 1e-12, 1e-12));
   auto restored = rf::ModelEma::from_cpu_shadow({weight}, {expected}, 0.9, tau, attempt);
   restored.update();
   const double next_decay = tau > 0 ? 0.9 * (1 - std::exp(-(attempt + 1) / tau)) : 0.9;
   REQUIRE(torch::allclose(restored.shadow_params()[0], expected * next_decay + weight * (1 - next_decay), 1e-12, 1e-12));
  }
 }
}
TEST_CASE("Managed stock warmup truncates to whole optimizer steps", "[rfdetr][training][parity]") {
 rf::LrScheduleConfig config;
 config.lr_min_factor = 0.1;
 config.lr_drop = 2;
 for (const auto scheduler : {rf::TrainLrSchedulerKind::Step, rf::TrainLrSchedulerKind::Cosine}) {
  config.lr_scheduler = scheduler;
  for (double epochs : {0.0, 0.2, 0.5, 1.0}) {
   config.warmup_epochs = epochs;
   const auto warmup = static_cast<int64_t>(3 * epochs);
   for (int64_t step = 0; step < 10; ++step) {
    const double expected = step < warmup                                   ? double(step) / static_cast<double>(warmup)
                            : scheduler == rf::TrainLrSchedulerKind::Cosine ? 0.1 + 0.9 * 0.5 * (1 + std::cos(std::numbers::pi * double(step - warmup) / static_cast<double>(12 - warmup)))
                             : step < 6                                     ? 1.0
                                                                            : 0.1;
    REQUIRE(std::abs(rf::compute_lr_scale(config, step, 3, 12) - expected) < 1e-12);
   }
  }
 }
}
TEST_CASE("Device clipping retains global norm empty zero and nonfinite behavior", "[rfdetr][training][parity]") {
 for (const double magnitude : {0.0, 0.01, 10.0}) {
  auto a = torch::ones({3}, torch::kFloat32).set_requires_grad(true), b = torch::ones({2}, torch::kFloat64).set_requires_grad(true);
  rf::NativeOptimizer optimizer(rf::NativeAdamW({{{0.001, 0, false}, {0, 1}}}, {{"a", a}, {"b", b}}, rf::NativeOptimizerBackend::eager));
  optimizer.clip_grad_norm_(1.0);
  REQUIRE_FALSE(a.grad().defined());
  REQUIRE_FALSE(b.grad().defined());
  a.mutable_grad() = torch::full_like(a, magnitude);
  b.mutable_grad() = torch::full_like(b, -magnitude);
  const double coefficient = std::min(1.0, 1.0 / (std::sqrt(5.0 * magnitude * magnitude) + 1e-6));
  optimizer.clip_grad_norm_(1.0);
  REQUIRE(torch::allclose(a.grad(), torch::full_like(a, magnitude * coefficient), 1e-6, 1e-7));
  REQUIRE(torch::allclose(b.grad(), torch::full_like(b, -magnitude * coefficient), 1e-6, 1e-7));
 }
}
TEST_CASE("Recoverable AMP overflow preserves AdamW and advances EMA attempts", "[rfdetr][training][parity]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; AMP overflow evidence unverified");
 auto weight = torch::tensor({0.713F, -0.281F}, torch::TensorOptions().device(torch::kCUDA)).set_requires_grad(true);
 rf::NativeOptimizer optimizer(rf::NativeAdamW({{{0.003, 0.07, false}, {0}}}, {{"weight", weight}}, rf::NativeOptimizerBackend::foreach));
 rf::GradScaler scaler(true, 128.0F);
 rf::ModelEma ema({weight}, 0.9, 3.0);
 auto expected = weight.detach().clone();
 for (int attempt = 0; attempt < 5; ++attempt) {
  const bool overflow = attempt == 0 || attempt == 3;
  const auto before = weight.detach().clone();
  const auto old_scale = scaler.current_scale();
  torch::Tensor loss;
  const rf::TrainingStep execution(1, old_scale, true, torch::kFloat16);
  execution.forward([&] {
   // A finite half loss whose scaled derivative overflows during cast
   // backward, exactly the recoverable AMP case in the production payload.
   loss = weight.to(torch::kFloat16).square().sum() * (overflow ? 65536.0 / old_scale : 1.0);
  });
  REQUIRE(torch::isfinite(loss).item<bool>());
  execution.backward(loss);
  const auto found = scaler.check_and_unscale_(optimizer);
  REQUIRE((found.item<float>() != 0.0F) == overflow);
  optimizer.clip_grad_norm_(0.1);
  scaler.step(optimizer, overflow);
  scaler.update(overflow);
  optimizer.zero_grad(true);
  ema.update();
  REQUIRE(ema.completed_updates() == attempt + 1);
  if (overflow) {
   REQUIRE(torch::equal(weight, before));
   REQUIRE(scaler.current_scale() == old_scale * 0.5F);
  }
  const double decay = 0.9 * (1 - std::exp(-(attempt + 1) / 3.0));
  expected = attempt == 0 ? weight.detach().clone() : expected * decay + weight.detach() * (1 - decay);
  REQUIRE(torch::allclose(ema.shadow_params()[0], expected, 1e-6, 1e-7));
 }
}
class FullPrecisionProbe : public torch::autograd::Function<FullPrecisionProbe> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& input, const torch::Tensor& weight) {
  ctx->save_for_backward({input, weight});
  mmltk::backend::ml::cuda::TorchAutocastScope full(false, torch::kFloat32);
  return torch::mm(input, weight);
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list incoming) {
  TORCH_CHECK(!at::autocast::is_autocast_enabled(at::kCUDA), "FP32 probe backward inherited forward autocast");
  const auto saved = ctx->get_saved_variables();
  return {torch::mm(incoming[0], saved[1].t()), torch::mm(saved[0].t(), incoming[0])};
 }
};
TEST_CASE("Main backward and parallel gradient harvesting execute FP32 probe derivatives outside AMP", "[rfdetr][training][parity]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; backward dispatch evidence unverified");
 tensor_fixture::FullMatrixPrecision precision;
 for (const auto dtype : {torch::kFloat16, torch::kBFloat16})
  for (const bool parallel : {false, true})
   for (int count : {1, 4}) {
    auto x = torch::tensor({{0.71317F, -0.28137F}, {0.21913F, 0.83971F}}, torch::TensorOptions().device(torch::kCUDA)).set_requires_grad(true);
    auto weight = torch::tensor({{0.41713F, 0.93719F}, {-0.61137F, 0.18371F}}, x.options()).set_requires_grad(true);
    const auto upstream = torch::tensor({{0.73117F, -0.81371F}, {0.29113F, 0.39137F}}, x.options());
    torch::Tensor loss;
    const rf::TrainingStep execution(count, 128.0, true, dtype);
    execution.forward([&] { loss = (FullPrecisionProbe::apply(x, weight) * upstream).sum(); });
    std::vector<torch::Tensor> actual;
    if (parallel)
     actual = execution.gradients(loss, {x, weight});
    else {
     execution.backward(loss);
     actual = {x.grad(), weight.grad()};
    }
    const auto dx = upstream.mm(weight.detach().t()) * 128 / count / count, dw = x.detach().t().mm(upstream) * 128 / count / count;
    REQUIRE(torch::allclose(actual[0], dx, 1e-6, 1e-7));
    REQUIRE(torch::allclose(actual[1], dw, 1e-6, 1e-7));
    const auto updated = weight.detach() - 0.001 * (actual[1] / (actual[1].abs() + 1e-8));
    const auto expected = weight.detach() - 0.001 * (dw / (dw.abs() + 1e-8));
    REQUIRE(torch::allclose(updated, expected, 1e-6, 1e-7));
   }
}
TEST_CASE("Production step and canonical groups match distributed stock AdamW with resumed warmup", "[rfdetr][training][parity]") {
 rf::testsupport::MatcherExecutionFixture fixture;
 rf::ScopedRuntimeContext runtime(nullptr, 0, &fixture.workspace);
 for (const auto device : tensor_fixture::available_devices())
  for (int count : {1, 4})
   for (bool parallel : {false, true}) {
    rf::TrainRequest request;
    request.optimizer = rf::TrainOptimizerKind::AdamW;
    request.lr = 0.003;
    request.lr_encoder = 0.002;
    request.encoder_layer_decay = 0.8;
    request.lr_component_decay = 0.7;
    request.weight_decay = 0.07;
    request.fused_optimizer = false;
    const std::array<std::string, 4> names{
     "backbone.0.encoder.encoder.layer.2.attention.weight", "backbone.0.encoder.embeddings.position_embeddings", "transformer.decoder.layers.0.linear1.weight", "class_embed.bias"};
    // Literal upstream grouping: 13 ViT layers + terminal id; embeddings id=0,
    // layer.2 id=3; decoder one component decay, heads none. Only encoder
    // embeddings/bias/norm/gamma receive the stock decay exclusion.
    const std::array<double, 4> rates{0.002 * std::pow(0.8, 11) * 0.49, 0.002 * std::pow(0.8, 14) * 0.49, 0.003 * 0.7, 0.003};
    const std::array<double, 4> decays{0.07, 0., 0.07, 0.07};
    torch::OrderedDict<std::string, torch::Tensor> inventory;
    std::array<torch::Tensor, 4> reference, moment, variance;
    const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    for (size_t i = 0; i < 4; ++i) {
     auto value = (torch::tensor({{{0.13F, -0.07F}, {0.21F, -0.19F}}}, options) + 0.031 * static_cast<double>(i)).set_requires_grad(true);
     inventory.insert(names[i], value);
     reference[i] = value.detach().clone();
     moment[i] = torch::zeros_like(value);
     variance[i] = torch::zeros_like(value);
    }
    inventory.insert("frozen.weight", torch::ones({1}, options));
    auto built = rf::build_optimizer(inventory, request);
    auto& optimizer = built.optimizer;
    REQUIRE(optimizer.parameter_names() == std::vector<std::string>(names.begin(), names.end()));
    REQUIRE(built.base_lrs.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
     REQUIRE(std::abs(built.base_lrs[i] - rates[i]) < 1e-14);
     REQUIRE(optimizer.parameters()[i].data_ptr() == inventory[names[i]].data_ptr());
    }
    rf::LrScheduleConfig schedule;
    schedule.warmup_epochs = 1.5;
    schedule.lr_drop = 10;
    schedule.lr_scheduler = rf::TrainLrSchedulerKind::Step;
    rf::GradScaler scaler(device.is_cuda(), 128.0F);
    for (int update = 0; update < 5; ++update) {
     CAPTURE(device, count, parallel, update);
     optimizer.zero_grad(true);
     std::vector<torch::Tensor> expected_parameters;
     for (const auto& value : optimizer.parameters()) expected_parameters.push_back(value.detach().clone().set_requires_grad(true));
     auto expected_loss = torch::zeros({}, options);
     for (int rank = 0; rank < 2; ++rank)
      for (int micro = 0; micro < count; ++micro) {
       const int targets = (rank + micro) % 2 + 1, other_targets = 3 - targets;
       rf::PreparedTargets gt;
       gt.all_boxes = torch::tensor({{0.2F, 0.2F, 0.1F, 0.1F}, {0.8F, 0.8F, 0.1F, 0.1F}}, options).narrow(0, 0, targets);
       gt.all_labels = torch::arange(targets, torch::kInt64).to(device);
       gt.offsets = {0};
       gt.counts = {targets};
       rf::PreparedTarget target;
       target.boxes = gt.all_boxes;
       target.labels = gt.all_labels;
       gt.targets = {target};
       rf::DetectionConfig config;
       config.num_classes = 2;
       config.group_detr = 1;
       config.world_size = 2;
       config.ia_bce_loss = false;
       config.use_jit_traced_loss_ops = false;
       config.set_cost_bbox = 1000;
       config.set_cost_class = 0;
       config.set_cost_giou = 0;
       torch::Tensor loss;
       const rf::TrainingStep execution(count, scaler.enabled() ? scaler.current_scale() : 1.0, device.is_cuda(), torch::kFloat16);
       execution.forward([&] {
        auto logits = optimizer.parameters()[0] + optimizer.parameters()[1] + optimizer.parameters()[2] + optimizer.parameters()[3];
        rf::ModelOutputs output;
        output.main.pred_logits = logits;
        output.main.pred_boxes = torch::tensor({{{0.2F, 0.2F, 0.1F, 0.1F}, {0.8F, 0.8F, 0.1F, 0.1F}}}, options);
        loss = rf::detection_loss_dict(output, gt, config, true, true, [&](torch::Tensor& normalizer) { normalizer.add_(other_targets); }).at("loss_ce");
       });
       if (parallel) {
        const auto gradients = execution.gradients(loss, optimizer.parameters());
        torch::NoGradGuard guard;
        for (size_t i = 0; i < gradients.size(); ++i) {
         auto& p = optimizer.parameters()[i];
         if (p.grad().defined())
          p.mutable_grad().add_(gradients[i]);
         else
          p.mutable_grad() = gradients[i].detach().clone();
        }
       } else
        execution.backward(loss);
       const auto logits = expected_parameters[0] + expected_parameters[1] + expected_parameters[2] + expected_parameters[3], probability = logits.sigmoid();
       auto labels = torch::zeros_like(logits);
       labels.index_put_({0, 0, 0}, 1);
       if (targets == 2) labels.index_put_({0, 1, 1}, 1);
       const auto focal = ((labels * torch::softplus(-logits) + (1 - labels) * torch::softplus(logits)) * torch::pow(1 - (probability * labels + (1 - probability) * (1 - labels)), 2) *
                           (0.25 * labels + 0.75 * (1 - labels)))
                           .sum();
       // Upstream user loss / K, Lightning closure / K, distributed gradient
       // average / 2. Target normalization already used global count / 2.
       expected_loss += focal / 1.5 / count / count / 2;
      }
     const auto gradients = torch::autograd::grad({expected_loss}, expected_parameters);
     {
      torch::NoGradGuard guard;
      for (auto& p : optimizer.parameters()) p.mutable_grad().div_(2);
     }
     REQUIRE(scaler.check_and_unscale_(optimizer).item<float>() == 0.0F);
     auto squared = torch::zeros({}, options);
     for (size_t i = 0; i < 4; ++i) {
      CAPTURE(i, optimizer.parameters()[i].grad(), gradients[i]);
      REQUIRE(torch::allclose(optimizer.parameters()[i].grad(), gradients[i], 2e-5, 2e-6));
      squared += gradients[i].square().sum();
     }
     const auto clip = (0.2 / (squared.sqrt() + 1e-6)).clamp_max(1.0);
     optimizer.clip_grad_norm_(0.2);
     const double lr_scale = update < 4 ? double(update) / 4 : 1.0;
     REQUIRE(std::abs(rf::compute_lr_scale(schedule, update, 3, 12) - lr_scale) < 1e-12);
     optimizer.set_lrs(built.base_lrs, rf::compute_lr_scale(schedule, update, 3, 12));
     for (size_t i = 0; i < 4; ++i) {
      const auto g = gradients[i] * clip;
      moment[i] = 0.9 * moment[i] + 0.1 * g;
      variance[i] = 0.999 * variance[i] + 0.001 * g.square();
      reference[i] = reference[i] * (1 - rates[i] * lr_scale * decays[i]) -
                     rates[i] * lr_scale * (moment[i] / (1 - std::pow(0.9, update + 1))) / (torch::sqrt(variance[i] / (1 - std::pow(0.999, update + 1))) + 1e-8);
     }
     scaler.step(optimizer, false);
     scaler.update(false);
     for (size_t i = 0; i < 4; ++i) REQUIRE(torch::allclose(optimizer.parameters()[i], reference[i], 2e-5, 2e-6));
     if (update == 2) {
      auto restored = optimizer_checkpoint(optimizer);
      auto candidate = optimizer.stage_load(restored);
      optimizer.commit(std::move(candidate));
      // Next update is epoch 1 * three steps/epoch, still inside floor(4.5).
     }
    }
   }
}
TEST_CASE("Training step restores precision on payload failure and rejects an empty effective batch", "[rfdetr][training][parity]") {
 const bool enabled = at::autocast::is_autocast_enabled(at::kCUDA);
 const auto dtype = at::autocast::get_autocast_dtype(at::kCUDA);
 rf::TrainingStep step(4, 1.0, true, torch::kFloat16);
 REQUIRE_THROWS(step.forward([] { throw std::runtime_error("forward failed"); }));
 REQUIRE(at::autocast::is_autocast_enabled(at::kCUDA) == enabled);
 REQUIRE(at::autocast::get_autocast_dtype(at::kCUDA) == dtype);
 REQUIRE_THROWS_AS(rf::TrainingStep(0, 1.0, false, torch::kFloat32), std::invalid_argument);
}
}  // namespace
