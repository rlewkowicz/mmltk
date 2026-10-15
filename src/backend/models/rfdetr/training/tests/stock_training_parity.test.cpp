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
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <tuple>
#include <algorithm>
#include <array>
#include <torch/csrc/autograd/custom_function.h>
#include <numbers>
#include <limits>
#include <meta>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "training_continuation_fixture.h"
#include "src/backend/ml/torch/archive.h"
using mmltk::backend::models::rfdetr::CriterionTerm;
using mmltk::backend::models::rfdetr::CriterionLayer;
namespace mmltk::backend::models::rfdetr::testsupport {
struct NativeAdamWTestAccess {
 static std::vector<std::pair<const void*, std::size_t>> storage(const NativeAdamW& optimizer) {
  std::vector<std::pair<const void*, std::size_t>> result;
  for (const auto& [key, batch] : optimizer.batches_) {
   [&]<class T>(const T& batch_view) {
    template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
     const auto& values = batch_view.[:member:];
     if (!values.empty()) throw std::runtime_error("AdamW retained a completed gradient/state inventory");
     result.emplace_back(values.data(), values.capacity());
    }
   }(batch);
  }
  return result;
 }
};
}  // namespace mmltk::backend::models::rfdetr::testsupport
namespace {
using mmltk::backend::ml::testsupport::independent_leaf;
namespace rf = mmltk::backend::models::rfdetr;
namespace tensor_fixture = mmltk::backend::ml::testsupport;
rf::TrainRequest optimizer_test_request(rf::TrainOptimizerKind kind) {
 rf::TrainRequest request;
 request.recipe.optimizer = kind;
 rf::reset_train_recipe(request.recipe);
 request.fused_optimizer = false;
 return request;
}
using rf::testsupport::optimizer_checkpoint;
// Independent equations: RF-DETR e9a138f module_model.py:607-611,1296;
// Lightning 2.6.0 ClosureResult; PyTorch 2.9 Adam and AveragedModel.
// Native FP32/FP64 policy, no TF32 changes, no epoch-trajectory/AP claim.
TEST_CASE("AdamW reuses batch inventories across missing gradients and activation", "[rfdetr][training][parity]") {
 for (const auto device : tensor_fixture::available_devices())
  for (const auto backend : {rf::NativeOptimizerBackend::foreach, rf::NativeOptimizerBackend::fused}) {
   if (backend == rf::NativeOptimizerBackend::fused && device.is_cpu()) continue;
   const auto options = torch::TensorOptions().dtype(torch::kFloat64).device(device);
   auto a = torch::full({4}, .7, options).set_requires_grad(true);
   auto b = torch::full({4}, -.3, options).set_requires_grad(true);
   auto expected_a = independent_leaf(a);
   auto expected_b = independent_leaf(b);
   const rf::NativeAdamW::Group group{{.003, .07, true}, {0, 1}};
   rf::NativeAdamW optimizer({group}, {{"a", a}, {"b", b}}, backend);
   rf::NativeAdamW reference({group}, {{"a", expected_a}, {"b", expected_b}}, rf::NativeOptimizerBackend::eager);
   std::vector<std::pair<const void*, std::size_t>> retained;
   for (int attempt = 0; attempt < 5; ++attempt) {
    CAPTURE(device, backend, attempt);
    optimizer.zero_grad(true);
    reference.zero_grad(true);
    a.set_requires_grad(attempt != 3);
    expected_a.set_requires_grad(attempt != 3);
    optimizer.activate();
    reference.activate();
    if (attempt != 1 && attempt != 3) {
     a.mutable_grad() = torch::full_like(a, attempt == 2 ? 0 : .2);
     expected_a.mutable_grad() = a.grad().clone();
    }
    if (attempt != 2) {
     b.mutable_grad() = torch::full_like(b, -.4);
     expected_b.mutable_grad() = b.grad().clone();
    }
    optimizer.step();
    reference.step();
    CHECK(torch::allclose(a, expected_a, 2e-6, 2e-8));
    CHECK(torch::allclose(b, expected_b, 2e-6, 2e-8));
    const auto storage = rf::testsupport::NativeAdamWTestAccess::storage(optimizer);
    if (!attempt) {
     REQUIRE_FALSE(storage.empty());
     retained = storage;
    } else {
     CHECK(storage == retained);
    }
   }
  }
}
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
 rf::TrainRecipeSettings config;
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
TEST_CASE("GradScaler retains finite growth state and recoverable backoff", "[rfdetr][training][parity]") {
 const auto upper = std::ldexp(1.F, 127);
 rf::GradScaler scaler(true, upper, 2.F, .5F, 2);
 scaler.update(false);
 REQUIRE(scaler.current_scale() == upper);
 REQUIRE(scaler.growth_tracker() == 1);
 scaler.update(false);
 REQUIRE(std::isfinite(scaler.current_scale()));
 REQUIRE(scaler.current_scale() == upper);
 REQUIRE(scaler.growth_tracker() == 0);
 scaler.update(true);
 REQUIRE(scaler.current_scale() == upper * .5F);
 REQUIRE(scaler.growth_tracker() == 0);
 scaler.update(false);
 scaler.update(false);
 REQUIRE(scaler.current_scale() == upper);
 REQUIRE(scaler.growth_tracker() == 0);
 rf::GradScaler disabled(false, upper, 2.F, .5F, 1);
 disabled.update(false);
 disabled.update(true);
 REQUIRE(disabled.current_scale() == upper);
 REQUIRE(disabled.growth_tracker() == 0);
 const auto loss = torch::tensor(3.F);
 REQUIRE(disabled.scale(loss).is_same(loss));
}
TEST_CASE("Stock target normalization keeps the global objective at the distributed clamp boundary", "[rfdetr][training][parity]") {
 rf::testsupport::MatcherExecutionFixture fixture;
 rf::ScopedRuntimeContext runtime(nullptr, 0, &fixture.workspace);
 for (const int global_targets : {0, 1, 2})
  for (const int groups : {1, 2}) {
   CAPTURE(global_targets, groups);
   rf::DetectionConfig config;
   config.num_classes = 1;
   config.group_detr = groups;
   config.world_size = 2;
   config.ia_bce_loss = false;
   config.use_jit_traced_loss_ops = false;
   rf::PreparedTargets empty;
   rf::PreparedTarget target;
   target.boxes = torch::empty({0, 4});
   target.labels = torch::empty({0}, torch::kInt64);
   empty.targets = {target};
   empty.all_boxes = target.boxes;
   empty.all_labels = target.labels;
   empty.offsets = {0};
   empty.counts = {0};
   auto logit = torch::tensor(.37F).set_requires_grad(true);
   rf::ModelOutputs outputs;
   outputs.main.pred_logits = logit.expand({1, groups, 1});
   outputs.main.pred_boxes = torch::ones({1, groups, 4});
   // This rank has no targets but still contributes background classification;
   // the other rank supplies the global count through the ordinary reducer.
   int reductions = 0;
   const auto loss = rf::detection_loss_terms(outputs, empty, config, true, true, [&](torch::Tensor& count) {
    ++reductions;
    count.add_(global_targets * groups);
   }).at({CriterionTerm::classification});
   REQUIRE(reductions == 1);
   const double native_divisor = std::max(global_targets * groups, 1);
   const double upstream_effective_divisor = std::max(global_targets * groups, 2);
   const auto numerator = .75 * torch::pow(logit.sigmoid(), 2.) * torch::softplus(logit) * groups;
   const auto native = numerator / native_divisor;
   const auto upstream_rank_contribution = numerator / upstream_effective_divisor;
   REQUIRE(torch::allclose(loss, native, 1e-6, 1e-6));
   const auto actual_grad = torch::autograd::grad({loss}, {logit}, {}, true)[0];
   const auto reference_grad = torch::autograd::grad({native}, {logit}, {}, true)[0];
   const auto upstream_grad = torch::autograd::grad({upstream_rank_contribution}, {logit})[0];
   REQUIRE(torch::allclose(actual_grad, reference_grad, 1e-6, 1e-6));
   REQUIRE(torch::allclose(actual_grad, upstream_grad * (upstream_effective_divisor / native_divisor), 1e-6, 1e-6));
   if (global_targets * groups < 2) REQUIRE_FALSE(torch::allclose(actual_grad, upstream_grad, 1e-6, 1e-6));
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
    request.recipe.optimizer = rf::TrainOptimizerKind::AdamW;
    request.recipe.lr = 0.003;
    request.recipe.lr_encoder = 0.002;
    request.recipe.encoder_layer_decay = 0.8;
    request.recipe.lr_component_decay = 0.7;
    request.recipe.weight_decay = 0.07;
    request.fused_optimizer = false;
    const std::array<std::string, 4> names{
     "backbone.0.encoder.encoder.layer.2.attention.weight", "backbone.0.encoder.embeddings.position_embeddings", "transformer.decoder.layers.0.linear1.weight", "class_embed.bias"
    };
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
    rf::TrainRecipeSettings schedule;
    schedule.warmup_epochs = 1.5;
    schedule.lr_drop = 10;
    schedule.lr_scheduler = rf::TrainLrSchedulerKind::Step;
    rf::GradScaler scaler(device.is_cuda(), 128.0F);
    for (int update = 0; update < 5; ++update) {
     CAPTURE(device, count, parallel, update);
     optimizer.zero_grad(true);
     std::vector<torch::Tensor> expected_parameters;
     for (const auto& value : optimizer.parameters()) expected_parameters.push_back(independent_leaf(value));
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
        loss = rf::detection_loss_terms(output, gt, config, true, true, [&](torch::Tensor& normalizer) { normalizer.add_(other_targets); }).at({CriterionTerm::classification});
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
       const auto logits = expected_parameters[0] + expected_parameters[1] + expected_parameters[2] + expected_parameters[3];
       auto labels = torch::zeros_like(logits);
       labels.index_put_({0, 0, 0}, 1);
       if (targets == 2) labels.index_put_({0, 1, 1}, 1);
       const auto focal = rf::testsupport::reference_focal_elements(logits, labels, 0.25, 2).sum();
       // Global logical count and gradient SUM, with both fixed K divisions.
       expected_loss += focal / 3 / count / count;
      }
     const auto gradients = torch::autograd::grad({expected_loss}, expected_parameters);
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
TEST_CASE("SGD preserves coupled decay first buffer Nesterov undefined gradients and exact Resume", "[rfdetr][training][optimizer]") {
 for (const double momentum : {0.0, .9})
  for (const bool nesterov : {false, true}) {
   if (nesterov && momentum == 0) continue;
   auto parameter = torch::tensor({1.25, -.5}, torch::kFloat64).set_requires_grad(true);
   auto expected = parameter.detach().clone();
   torch::Tensor buffer;
   rf::NativeSGD::Group group{{.05, .1, momentum, nesterov, rf::TrainingGroupRole::Ordinary}, {0}};
   rf::NativeOptimizer optimizer(rf::NativeSGD({group}, {{"weight", parameter}}));
   for (int step = 0; step < 4; ++step) {
    auto gradient = torch::tensor({.2 + .1 * step, -.3}, torch::kFloat64);
    parameter.mutable_grad() = gradient;
    auto direction = gradient + .1 * expected;
    if (momentum != 0) {
     buffer = buffer.defined() ? momentum * buffer + direction : direction.clone();
     direction = nesterov ? direction + momentum * buffer : buffer;
    }
    expected = expected - .05 * direction;
    optimizer.step();
    REQUIRE(torch::allclose(parameter, expected, 1e-12, 1e-12));
    optimizer.zero_grad(true);
    optimizer.step();
    REQUIRE(torch::allclose(parameter, expected, 1e-12, 1e-12));
    if (step == 1) {
     auto saved = optimizer_checkpoint(optimizer);
     auto candidate = optimizer.stage_load(saved);
     optimizer.commit(std::move(candidate));
    }
   }
  }
}
TEST_CASE("Optimizer inventories retain cold named parameters and initialize once upon activation", "[rfdetr][training][activation]") {
 // CLEANUP-IGNORE: Activation and Resume cases share only the recipe fixture and a seed tensor; their state transitions and assertions differ.
 for (const auto kind : {rf::TrainOptimizerKind::AdamW, rf::TrainOptimizerKind::Muon, rf::TrainOptimizerKind::SGD}) {
  const auto request = optimizer_test_request(kind);
  auto active = torch::full({2, 2}, .7).set_requires_grad(true);
  auto frozen = torch::full({2, 2}, .4);
  torch::OrderedDict<std::string, torch::Tensor> parameters;
  parameters.insert("transformer.encoder.weight", active);
  parameters.insert("backbone.0.encoder.layer.0.weight", frozen);
  auto built = rf::build_optimizer(parameters, request);
  auto& optimizer = built.optimizer;
  CHECK(optimizer.eligible_parameters().size() == 2);
  CHECK(optimizer.parameters().size() == 1);
  rf::ModelEma ema(optimizer.eligible_parameters(), .9, 0);
  CHECK(ema.shadow_params().size() == 2);
  frozen.mutable_grad() = torch::ones_like(frozen);  // Stale gradients never activate frozen state.
  active.mutable_grad() = torch::ones_like(active);
  optimizer.step();
  optimizer.zero_grad(true);
  CHECK(torch::equal(frozen, torch::full_like(frozen, .4)));
  auto saved = optimizer_checkpoint(optimizer);
  torch::serialize::InputArchive cold;
  saved.read(mmltk::backend::ml::serialization::archive_entry_name("param", 1), cold);
  if (kind == rf::TrainOptimizerKind::SGD) {
   CHECK(mmltk::backend::ml::serialization::require_int(cold, "step") == 0);
   CHECK(mmltk::backend::ml::serialization::require_int(cold, "has_momentum") == 0);
  } else
   CHECK(mmltk::backend::ml::serialization::require_int(cold, "initialized") == 0);
  torch::Tensor unused;
  CHECK_FALSE(cold.try_read("exp_avg", unused));
  CHECK_FALSE(cold.try_read("momentum_buffer", unused));
  auto restored = optimizer.stage_load(saved);
  optimizer.commit(std::move(restored));
  frozen.set_requires_grad(true);
  optimizer.activate();
  CHECK(optimizer.parameters().size() == 2);
  CHECK(optimizer.parameter_names() == optimizer.eligible_parameter_names());
  frozen.mutable_grad() = torch::ones_like(frozen);
  optimizer.step();
  optimizer.zero_grad(true);
  CHECK_FALSE(torch::equal(frozen, torch::full_like(frozen, .4)));
  auto initialized = optimizer_checkpoint(optimizer);
  optimizer.load(initialized);
  const auto before = frozen.detach().clone();
  frozen.set_requires_grad(false);
  optimizer.activate();
  optimizer.step();
  CHECK(torch::equal(frozen, before));
  frozen.set_requires_grad(true);
  optimizer.activate();
  CHECK(optimizer.eligible_parameter_names()[1] == "backbone.0.encoder.layer.0.weight");
 }
}
TEST_CASE("SGD bias warmup groups remain separate when ordinary LR and decay coincide", "[rfdetr][training][optimizer]") {
 rf::TrainRequest request;
 request.recipe.optimizer = rf::TrainOptimizerKind::SGD;
 rf::reset_train_recipe(request.recipe);
 torch::OrderedDict<std::string, torch::Tensor> parameters;
 parameters.insert("head.weight", torch::ones({2}).set_requires_grad(true));
 parameters.insert("head.bias", torch::ones({2}).set_requires_grad(true));
 const auto built = rf::build_optimizer(parameters, request);
 CHECK(built.base_lrs == std::vector<double>{.01, .01});
 CHECK((built.roles == std::vector<rf::TrainingGroupRole>{rf::TrainingGroupRole::Ordinary, rf::TrainingGroupRole::Bias}));
}
TEST_CASE("Optimizer Resume seals group policy while restoring held schedule and parameter state", "[rfdetr][training][optimizer][continuation]") {
 namespace io = mmltk::backend::ml::serialization;
 for (const auto kind : {rf::TrainOptimizerKind::AdamW, rf::TrainOptimizerKind::Muon, rf::TrainOptimizerKind::SGD}) {
  const auto request = optimizer_test_request(kind);
  auto parameter = torch::full({2, 2}, .7).set_requires_grad(true);
  torch::OrderedDict<std::string, torch::Tensor> parameters;
  parameters.insert("transformer.encoder.weight", parameter);
  auto built = rf::build_optimizer(parameters, request);
  auto& optimizer = built.optimizer;
  optimizer.set_lrs(built.base_lrs, .37);
  optimizer.set_momentum(.83);
  parameter.mutable_grad() = torch::full_like(parameter, .2);
  optimizer.step();
  optimizer.zero_grad(true);
  auto peer_parameter = independent_leaf(parameter);
  torch::OrderedDict<std::string, torch::Tensor> peer_parameters;
  peer_parameters.insert("transformer.encoder.weight", peer_parameter);
  auto peer = rf::build_optimizer(peer_parameters, request);
  auto saved = optimizer_checkpoint(optimizer);
  auto candidate = peer.optimizer.stage_load(saved);
  peer.optimizer.commit(std::move(candidate));
  auto restored = optimizer_checkpoint(peer.optimizer);
  torch::serialize::InputArchive restored_group;
  const auto group_name = io::archive_entry_name("group", 0);
  restored.read(group_name, restored_group);
  CHECK(io::require_double(restored_group, "lr") == built.base_lrs[0] * .37);
  if (kind != rf::TrainOptimizerKind::AdamW) CHECK(io::require_double(restored_group, "momentum") == .83);
  std::vector<std::pair<std::string, c10::IValue>> faults{{"weight_decay", request.recipe.weight_decay + .1}};
  const auto add_boolean_faults = [&](const char* field, bool original) {
   faults.emplace_back(field, static_cast<int64_t>(!original));
   faults.emplace_back(field, int64_t{2});
   faults.emplace_back(field, int64_t{-1});
  };
  if (kind == rf::TrainOptimizerKind::AdamW)
   add_boolean_faults("amsgrad", false);
  else if (kind == rf::TrainOptimizerKind::Muon) {
   add_boolean_faults("use_muon", true);
   add_boolean_faults("nesterov", true);
  } else {
   add_boolean_faults("nesterov", false);
   faults.emplace_back("role", static_cast<int64_t>(rf::TrainingGroupRole::Bias));
  }
  for (const auto& [field, value] : faults) {
   CAPTURE(kind, field);
   auto original = optimizer_checkpoint(optimizer);
   torch::serialize::InputArchive group;
   original.read(group_name, group);
   torch::serialize::OutputArchive changed_group;
   rf::testsupport::copy_checkpoint_archive(group, changed_group, field);
   changed_group.write(field, value);
   torch::serialize::OutputArchive forged;
   rf::testsupport::copy_checkpoint_archive(original, forged, group_name);
   forged.write(group_name, changed_group);
   auto input = rf::testsupport::checkpoint_input(forged);
   REQUIRE_THROWS(peer.optimizer.stage_load(input));
   CHECK(torch::equal(peer_parameter, parameter));
  }
  // The same next update proves held LR/momentum and accumulated tensor/age state
  // survived both the successful restore and all rejected admission attempts.
  parameter.mutable_grad() = torch::full_like(parameter, .3);
  peer_parameter.mutable_grad() = parameter.grad().clone();
  optimizer.step();
  peer.optimizer.step();
  CHECK(torch::allclose(peer_parameter, parameter, 1e-6, 1e-6));
 }
}
}  // namespace

TEST_CASE("Atto trains encoder predictions while retaining cold dormant state without moments", "[rfdetr][training][atto]") {
 auto config = rf::native_config_from_preset(*rf::find_model_preset_by_weight_filename("rf-detr-atto.pth"));
 config.resolution = 80;
 config.num_queries = config.num_select = 3;
 config.group_detr = 2;
 rf::NativeRfDetrModel model(config);
 model.train();
 const auto kind = GENERATE(rf::TrainOptimizerKind::AdamW, rf::TrainOptimizerKind::Muon, rf::TrainOptimizerKind::SGD);
 auto built = rf::build_optimizer(model.named_parameters(), optimizer_test_request(kind), config.backbone);
 const auto dormant = [](std::string_view name) {
  return name.starts_with("class_embed.") || name.starts_with("bbox_embed.") || name == "query_feat.weight" || name == "refpoint_embed.weight" || name.starts_with("transformer.decoder.");
 };
 std::vector<std::pair<std::string, torch::Tensor>> retained;
 for (const auto& parameter : model.named_parameters()) {
  REQUIRE_FALSE(parameter.key().starts_with("transformer.decoder.layers."));
  if (dormant(parameter.key())) {
   CHECK_FALSE(parameter.value().requires_grad());
   retained.emplace_back(parameter.key(), parameter.value().detach().clone());
  }
 }
 REQUIRE_FALSE(retained.empty());
 for (const auto& name : built.optimizer.parameter_names()) CHECK_FALSE(dormant(name));
 const auto input = torch::rand({1, 3, config.resolution, config.resolution});
 for (int step = 0; step < 2; ++step) {
  auto outputs = model.forward({input, {}}, false);
  REQUIRE(outputs.main.pred_logits.size(1) == config.num_queries * config.group_detr);
  REQUIRE(outputs.aux_outputs.empty());
  REQUIRE_FALSE(outputs.enc_outputs);
  (outputs.main.pred_logits.square().mean() + outputs.main.pred_boxes.square().mean()).backward();
  built.optimizer.step();
  built.optimizer.zero_grad(true);
 }
 const auto parameters = model.named_parameters();
 for (const auto& [name, expected] : retained) {
  CHECK_FALSE(parameters[name].grad().defined());
  CHECK(torch::equal(parameters[name], expected));
 }
 auto archived = optimizer_checkpoint(built.optimizer);
 for (std::size_t index = 0; index < built.optimizer.eligible_parameter_names().size(); ++index) {
  const auto& name = built.optimizer.eligible_parameter_names()[index];
  if (!dormant(name)) continue;
  torch::serialize::InputArchive parameter;
  archived.read(mmltk::backend::ml::serialization::archive_entry_name("param", index), parameter);
  CHECK(mmltk::backend::ml::serialization::require_int(parameter, kind == rf::TrainOptimizerKind::SGD ? "step" : "initialized") == 0);
  torch::Tensor tensor;
  CHECK_FALSE(parameter.try_read("exp_avg", tensor));
  CHECK_FALSE(parameter.try_read("exp_avg_sq", tensor));
  CHECK_FALSE(parameter.try_read("momentum_buffer", tensor));
 }
 auto detection = rf::testsupport::detection_fixture_base(config);
 rf::populate_default_detection_weights(detection);
 for (const auto& weight : detection.weights) CHECK(weight.key.layer.kind() == CriterionLayer::Kind::Main);
 model.eval();
 torch::NoGradGuard guard;
 const auto output = model.forward({input, {}}, false);
 CHECK(output.main.pred_logits.size(1) == config.num_queries);
 CHECK_FALSE(output.enc_outputs);
}

TEST_CASE("PE optimizer groups use actual depth token and decay policy for every optimizer", "[rfdetr][training][optimizer][pe]") {
 namespace serial = mmltk::backend::ml::serialization;
 const auto backbone = rf::native_config_from_preset(*rf::find_model_preset_by_weight_filename("rf-detr-femto.pth")).backbone;
 const std::array cases{
  std::tuple{"backbone.0.encoder.model.pos_embed",14,0.},
  std::tuple{"backbone.0.encoder.model.patch_embed.proj.weight",14,0.},
  std::tuple{"backbone.0.encoder.model.cls_token",14,0.},
  std::tuple{"backbone.0.encoder.model.reg_token",14,0.},
  std::tuple{"backbone.0.encoder.model.blocks.0.attn.qkv.weight",13,.1},
  std::tuple{"backbone.0.encoder.model.blocks.11.mlp.fc1.weight",2,.1},
  std::tuple{"backbone.0.encoder.model.blocks.4.norm1.weight",9,0.},
  std::tuple{"backbone.0.encoder.model.norm.weight",0,0.}};
 for (const auto kind : {rf::TrainOptimizerKind::AdamW,rf::TrainOptimizerKind::Muon,rf::TrainOptimizerKind::SGD})
  for (const auto& [name,exponent,decay] : cases) {
   CAPTURE(kind,name);
   auto request=optimizer_test_request(kind);
   request.recipe.lr_encoder=.002; request.recipe.encoder_layer_decay=.8; request.recipe.lr_component_decay=.7; request.recipe.weight_decay=.1;
   torch::OrderedDict<std::string,torch::Tensor> parameters;
   parameters.insert(name,torch::ones({2,2}).set_requires_grad(true));
   auto built=rf::build_optimizer(parameters,request,backbone);
   REQUIRE(built.base_lrs.size()==1);
   CHECK(built.base_lrs[0]==Catch::Approx(.002*std::pow(.8,exponent)*.7*.7));
   auto archive=optimizer_checkpoint(built.optimizer);
   torch::serialize::InputArchive group;
   archive.read(serial::archive_entry_name("group",0),group);
   CHECK(serial::require_double(group,"weight_decay")==decay);
  }
}
TEST_CASE("Every stock catalog model takes real task loss and optimizer steps", "[rfdetr][training][stock-suite][gpu]") {
 REQUIRE(mmltk::testsupport::checked_cuda_device_count()>0);
 const tensor_fixture::FullMatrixPrecision precision;
 const torch::Device device(torch::kCUDA,0);
 for (const auto& preset : rf::model_presets()) {
  CAPTURE(preset.preset_name);
  auto config=rf::native_config_from_preset(preset);
  torch::manual_seed(715);
  rf::NativeRfDetrModel model(config);
  model.to(device);
  model.train();
  auto request=optimizer_test_request(rf::TrainOptimizerKind::AdamW);
  auto built=rf::build_optimizer(model.named_parameters(),request,config.backbone);
  auto criterion=rf::testsupport::detection_fixture_base(config);
  criterion.include_masks=config.has_masks();
  criterion.mask_point_sample_ratio=config.mask_point_sample_ratio;
  rf::populate_default_detection_weights(criterion);
  const auto options=torch::TensorOptions().device(device);
  const auto input=torch::rand({1,3,config.resolution,config.resolution},options);
  auto targets=rf::testsupport::one_image_targets(torch::tensor({{.5F,.5F,.4F,.6F}},options),torch::tensor({1},options.dtype(torch::kInt64)));
  if (config.has_masks()) {
   const auto words=(config.resolution*config.resolution+63)/64;
   targets.packed_masks=rf::PackedTargetMasks{torch::full({1,words},-1,options.dtype(torch::kInt64)),config.resolution,config.resolution};
  }
  if (config.has_keypoints()) {
   const auto count=config.keypoints_per_class.at(1);
   targets.all_keypoints=torch::full({1,count,3},.4,options);
   targets.all_keypoints.select(-1,2).fill_(2);
   targets.all_keypoint_counts=torch::tensor({static_cast<std::int64_t>(count)},options.dtype(torch::kInt64));
  }
  for (int step=0;step<2;++step) {
   auto output=model.forward({input,{}},config.has_masks());
   auto losses=rf::detection_loss_terms(output,targets,criterion,true,false);
   const auto total=rf::weighted_detection_loss(losses,criterion,device);
   REQUIRE(torch::isfinite(total).item<bool>());
   total.backward();
   bool joint_gradient=false;
   for (const auto& parameter:model.named_parameters()) {
    if (!parameter.value().requires_grad()) { CHECK_FALSE(parameter.value().grad().defined()); continue; }
    if (!parameter.value().grad().defined()) continue;
    REQUIRE(torch::isfinite(parameter.value().grad()).all().item<bool>());
    if (parameter.key().starts_with("keypoint_embed.") && parameter.value().grad().abs().sum().item<double>()>0) joint_gradient=true;
   }
   if (config.has_keypoints()) REQUIRE(joint_gradient);
   built.optimizer.clip_grad_norm_(.1);
   built.optimizer.step();
   built.optimizer.zero_grad(true);
  }
 }
}

TEST_CASE("Stock joint losses train through compilation AMP accumulation and every optimizer", "[rfdetr][training][keypoints][compilation][amp][gpu]") {
 REQUIRE(mmltk::testsupport::checked_cuda_device_count()>0);
 const tensor_fixture::FullMatrixPrecision precision;
 const torch::Device device(torch::kCUDA,0);
 for (const auto dtype:{torch::kFloat32,torch::kFloat16,torch::kBFloat16})
  for (const auto mode:{rf::CompilationMode::kNone,rf::CompilationMode::kSelective,rf::CompilationMode::kFullTrace}) {
   CAPTURE(dtype,mode);
   auto config=rf::native_config_from_preset(*rf::find_preset_catalog_entry("rf-detr-keypoint-preview"));
   config.resolution=config.patch_size*config.num_windows*2;
   config.num_queries=config.num_select=4;
   config.group_detr=2;
   config.num_classes=4;
   config.keypoints_per_class={2,0,1};
   torch::manual_seed(881);
   rf::NativeRfDetrModel model(config);
   model.to(device);
   model.optimize_for_inference(1,true,mode);
   const auto kind=mode==rf::CompilationMode::kNone?rf::TrainOptimizerKind::AdamW:mode==rf::CompilationMode::kSelective?rf::TrainOptimizerKind::Muon:rf::TrainOptimizerKind::SGD;
   auto optimizer=rf::build_optimizer(model.named_parameters(),optimizer_test_request(kind),config.backbone);
   auto criterion=rf::testsupport::detection_fixture_base(config);
   rf::populate_default_detection_weights(criterion);
   const auto options=torch::TensorOptions().device(device);
   const auto input=torch::rand({1,3,config.resolution,config.resolution},options);
   auto targets=rf::testsupport::one_image_targets(torch::tensor({{.4F,.5F,.5F,.5F},{.6F,.5F,.5F,.5F}},options),torch::tensor({0,2},options.dtype(torch::kInt64)));
   targets.all_keypoints=torch::tensor({{{.3F,.4F,2.F},{.5F,.6F,1.F}},{{.7F,.6F,2.F},{0.F,0.F,0.F}}},options);
   targets.all_keypoint_counts=torch::tensor({2,1},options.dtype(torch::kInt64));
   const auto parameter=model.named_parameters()["keypoint_embed.layers.2.weight"];
   const auto original=parameter.detach().clone();
   for (int attempt=0;attempt<2;++attempt) {
    for (int micro=0;micro<2;++micro) {
     const rf::TrainingStep step(2,1,dtype!=torch::kFloat32,dtype);
     torch::Tensor loss;
     step.forward([&] {
      auto output=model.forward({input,{}},false);
      REQUIRE(output.main.pred_keypoints);
      const auto values=rf::detection_loss_terms(output,targets,criterion,true,false);
      for (const auto term : {CriterionTerm::keypoint_l1, CriterionTerm::keypoint_findable, CriterionTerm::keypoint_visible, CriterionTerm::keypoint_nll}) {
       REQUIRE(values.contains({term}));
       REQUIRE(torch::isfinite(values.at({term})).item<bool>());
      }
      loss=rf::weighted_detection_loss(values,criterion,device);
     });
     REQUIRE(torch::isfinite(loss).item<bool>());
     step.backward(loss);
    }
    REQUIRE(parameter.grad().defined());
    CHECK(parameter.grad().abs().sum().item<double>()>0);
    for (const auto& value:optimizer.optimizer.parameters()) if (value.grad().defined()) REQUIRE(torch::isfinite(value.grad()).all().item<bool>());
    optimizer.optimizer.clip_grad_norm_(.1);
    optimizer.optimizer.step();
    optimizer.optimizer.zero_grad(true);
   }
   CHECK_FALSE(torch::equal(parameter,original));
  }
}
