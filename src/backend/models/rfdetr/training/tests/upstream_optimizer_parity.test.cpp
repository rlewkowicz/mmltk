#include "upstream_oracle_fixture.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/torch/archive.h"
#include "src/backend/models/rfdetr/core/detection_ops.h"
#include "src/backend/models/rfdetr/core/detail/training_mask_loss.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/models/rfdetr/training/detail/training_step.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <nlohmann/json.hpp>
#include <torch/serialize.h>
#include <algorithm>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
namespace rf = mmltk::backend::models::rfdetr;
namespace support = rf::testsupport;
namespace serialization = mmltk::backend::ml::serialization;
namespace {
std::vector<const rf::OutputLayer*> layers(const rf::ModelOutputs& outputs) {
 std::vector<const rf::OutputLayer*> result{&outputs.main};
 for (const auto& layer : outputs.aux_outputs) result.push_back(&layer);
 if (outputs.enc_outputs) result.push_back(&*outputs.enc_outputs);
 return result;
}
void compare_assignments(support::OracleComparison& comparison, const support::OracleArchive& archive, const rf::ModelOutputs& outputs, const rf::PreparedTargets& targets,
 const rf::DetectionConfig& config, bool training, std::span<const rf::LayerMaskSamples> samples, const std::string& prefix) {
 const auto output_layers = layers(outputs);
 for (std::size_t index = 0; index < output_layers.size(); ++index) {
  rf::ModelOutputs layer;
  layer.main = *output_layers[index];
  const auto matches = rf::matcher_indices(layer, targets, config, training, samples[index]);
  for (std::size_t image = 0; image < matches.size(); ++image) {
   const auto key = prefix + "assignment/" + std::to_string(index) + "/" + std::to_string(image) + "/";
   for (const auto& [suffix, actual] : {std::pair{"query", matches[image].first}, std::pair{"target", matches[image].second}}) {
    comparison.compare(key + suffix, actual, archive.tensor(key + suffix));
    if (prefix.empty()) comparison.compare("stock-policy/" + key + suffix, actual, archive.tensor("stock/" + key + suffix), true);
   }
  }
  if (!config.include_masks) continue;
  std::vector<torch::Tensor> selected_masks, selected_targets;
  for (std::size_t image = 0; image < matches.size(); ++image) {
   const auto query = matches[image].first.to(outputs.main.pred_logits.device());
   const auto target = matches[image].second.to(query.device()) + targets.offsets[image];
   selected_targets.push_back(target);
   if (query.numel() == 0) continue;
   const auto& value = *output_layers[index];
   selected_masks.push_back(value.sparse_pred_masks ? rf::matched_sparse_pred_masks_for_batch(*value.sparse_pred_masks, static_cast<int64_t>(image), query)
                                                   : value.pred_masks->index({static_cast<int64_t>(image)}).index_select(0, query));
  }
  const auto sampled = rf::sample_direct_masks(torch::cat(selected_masks), targets, torch::cat(selected_targets), config.mask_point_sample_ratio, samples[index]);
  const auto key = prefix + "direct/" + std::to_string(index) + "/";
  // Exact coordinates expose uncertainty top-k selection, including ties;
  // equal-looking interpolated logits cannot conceal different selected points.
  comparison.compare(key + "point_coords", sampled.coordinates, archive.tensor(key + "point_coords"));
  if (prefix.empty()) comparison.compare("stock-policy/" + key + "point_coords", sampled.coordinates, archive.tensor("stock/" + key + "point_coords"), true);
  comparison.compare(key + "point_logits", sampled.logits, archive.tensor(key + "point_logits"));
  comparison.compare(key + "point_labels", sampled.targets, archive.tensor(key + "point_labels"));
 }
}
torch::Tensor compare_loss(support::OracleComparison& comparison, const support::OracleArchive& archive, const rf::ModelOutputs& outputs, const rf::PreparedTargets& targets,
 const rf::DetectionConfig& config, bool training, const std::string& prefix) {
 const auto samples = support::oracle_samples(archive, layers(outputs).size(), outputs.main.pred_logits.device(), prefix);
 // Assignments are discrete gates and precede any continuous loss tolerance.
 compare_assignments(comparison, archive, outputs, targets, config, training, samples, prefix);
 const auto count = std::accumulate(targets.counts.begin(), targets.counts.end(), int64_t{0}) * (training ? config.group_detr : 1);
 const auto normalizer = torch::tensor(static_cast<float>(std::max<int64_t>(count, 1)), outputs.main.pred_boxes.options().dtype(torch::kFloat32));
 const auto losses = rf::detection_loss_dict(outputs, targets, config, training, normalizer, samples);
 std::set<std::string> actual_losses, actual_statistics;
 for (const auto& [key, value] : losses) (key.starts_with("loss_") ? actual_losses : actual_statistics).insert(key);
 auto expected_losses = archive.names(prefix + "loss/"); expected_losses.erase("total");
 comparison.inventory(prefix + "loss inventory", actual_losses, expected_losses);
 comparison.inventory(prefix + "statistic inventory", actual_statistics, archive.names(prefix + "statistic/"));
 for (const auto& [key, value] : losses) {
  const auto name = prefix + (key.starts_with("loss_") ? "loss/" : "statistic/") + key;
  comparison.compare(name, value, archive.tensor(name));
  if (prefix.empty()) comparison.compare("stock-policy/" + name, value, archive.tensor("stock/" + name), true);
 }
 auto total = rf::weighted_detection_loss(losses, config, outputs.main.pred_logits.device());
 comparison.compare(prefix + "loss/total", total, archive.tensor(prefix + "loss/total"));
 if (prefix.empty()) comparison.compare("stock-policy/loss/total", total, archive.tensor("stock/loss/total"), true);
 return total;
}
void compare_gradients(support::OracleComparison& comparison, const support::OracleArchive& archive, const rf::NativeRfDetrModel& model, const std::string& prefix, bool stock) {
 std::set<std::string> names;
 for (const auto& parameter : model.named_parameters(true)) {
  if (!parameter.value().requires_grad()) continue;
  names.insert(parameter.key());
  comparison.compare(prefix + parameter.key(), parameter.value().grad(), archive.tensor(prefix + parameter.key()));
  if (stock) comparison.compare("stock-policy/" + prefix + parameter.key(), parameter.value().grad(), archive.tensor("stock/" + prefix + parameter.key()), true);
 }
 comparison.inventory(prefix + "inventory", names, archive.names(prefix));
}
void compare_optimizer(support::OracleComparison& comparison, const support::OracleArchive& reference, rf::NativeOptimizer& optimizer, const support::OracleCase& entry, bool state) {
 // Read the owner's production checkpoint projection: no test-only optimizer or
 // duplicated AdamW equations are used to inspect groups, ages and moments.
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
 readback.Begin();
 optimizer.reserve_checkpoint(readback, 0);
 torch::serialize::OutputArchive output;
 optimizer.save(output, readback, 0);
 readback.Complete();
 std::stringstream bytes;
 output.save_to(bytes);
 torch::serialize::InputArchive archive;
 archive.load_from(bytes, torch::Device(torch::kCPU));
 REQUIRE(serialization::require_double(archive, "beta1") == .9);
 REQUIRE(serialization::require_double(archive, "beta2") == .999);
 REQUIRE(serialization::require_double(archive, "eps") == 1e-8);
 const auto parameters = serialization::require_int(archive, "param_count");
 std::vector<std::string> names;
 for (int64_t index = 0; index < parameters; ++index) {
  torch::serialize::InputArchive parameter;
  archive.read(serialization::archive_entry_name("param", static_cast<std::size_t>(index)), parameter);
  const auto name = serialization::require_string(parameter, "name");
  names.push_back(name);
  if (!state || !std::ranges::contains(entry.trainable, name)) continue;
  for (const auto* field : {"step", "exp_avg", "exp_avg_sq"}) {
   torch::Tensor value;
   (void)parameter.try_read(field, value);
   const auto key = "optimizer/" + name + "/" + field;
   const auto expected = reference.tensor(key);
   // Native eagerly owns zero moments/age for unused eligible parameters;
   // PyTorch lazily omits that state. Verify the zero state explicitly.
   if (!expected.defined()) {
    REQUIRE(value.defined());
    comparison.compare(key + "/unused-zero", value, torch::zeros_like(value));
   } else {
    comparison.compare(key, value, expected);
   }
   if (entry.dtype() != torch::kFloat32 && reference.contains("stock/" + key)) comparison.compare("stock-policy/" + key, value, reference.tensor("stock/" + key), true);
  }
 }
 const auto group_count = serialization::require_int(archive, "group_count");
 std::map<std::string, std::pair<double, double>> actual;
 for (int64_t index = 0; index < group_count; ++index) {
  torch::serialize::InputArchive group;
  archive.read(serialization::archive_entry_name("group", static_cast<std::size_t>(index)), group);
  const double lr = serialization::require_double(group, "lr"), decay = serialization::require_double(group, "weight_decay");
  REQUIRE(serialization::require_int(group, "amsgrad") == 0);
  const auto count = serialization::require_int(group, "param_index_count");
  for (int64_t item = 0; item < count; ++item) {
   const auto parameter = serialization::require_int(group, serialization::archive_entry_name("param_index", static_cast<std::size_t>(item)).c_str());
   const auto& name = names.at(static_cast<std::size_t>(parameter));
   if (std::ranges::contains(entry.trainable, name)) REQUIRE(actual.emplace(name, std::pair{lr, decay}).second);
  }
 }
 std::set<std::string> expected_names, actual_names;
 for (const auto& [name, hyperparameters] : actual) actual_names.insert(name);
 for (const auto& group : entry.parameter_groups) {
  REQUIRE(group.beta1 == .9); REQUIRE(group.beta2 == .999); REQUIRE(group.eps == 1e-8); REQUIRE_FALSE(group.amsgrad);
  for (const auto& name : group.names) {
   REQUIRE(expected_names.insert(name).second);
   const auto found = actual.find(name);
   REQUIRE(found != actual.end());
   comparison.compare("parameter-group/" + name, torch::tensor({found->second.first, found->second.second}, torch::kFloat64), torch::tensor({group.lr, group.weight_decay}, torch::kFloat64));
  }
 }
 comparison.inventory("optimizer parameter groups", actual_names, expected_names);
}
}
TEST_CASE("Actual upstream stock criterion gradients AdamW updates and bounded trajectory", "[rfdetr][parity][upstream_oracle][cuda]") {
 const auto requested = support::UpstreamOracle::requested();
 if (!requested) SKIP("optional independent upstream artifacts absent; default skip is not parity evidence");
 const support::UpstreamOracle oracle(*requested);
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 c10::cuda::CUDAGuard guard(0);
 mmltk::backend::ml::testsupport::FullMatrixPrecision precision;
 support::MatcherExecutionFixture matcher;
 rf::ScopedRuntimeContext runtime(nullptr, 0, &matcher.workspace);
 for (const auto& entry : oracle.cases()) {
  if (!entry.training) continue;
  DYNAMIC_SECTION(entry.id) {
   CAPTURE(entry.id);
   auto model = oracle.model(entry);
   const auto config = support::oracle_detection(model.config());
   rf::TrainRequest request;
   static_cast<rf::TrainRecipeValues&>(request.recipe) = entry.recipe;
   request.fused_optimizer = false;
   auto built = rf::build_optimizer(model.named_parameters(true), request);
   const rf::TrainingStep step(1, 1., entry.dtype() != torch::kFloat32, entry.dtype());
   support::OracleComparison comparison(*requested, "training-" + entry.id, entry.dtype());
   std::set<std::string> actual_weights, expected_weights;
   for (const auto& [name, weight] : entry.loss_weights) expected_weights.insert(name);
   for (const auto& [name, weight] : config.weight_dict) {
    actual_weights.insert(name);
    comparison.compare("parameter-group/loss-weight/" + name, torch::tensor(weight, torch::kFloat64), torch::tensor(entry.loss_weights.at(name), torch::kFloat64));
   }
   comparison.inventory("loss weight inventory", actual_weights, expected_weights);
   rf::TensorMap expected_before;
   for (const auto& parameter : model.named_parameters(true)) if (parameter.value().requires_grad()) expected_before.emplace(parameter.key(), parameter.value().detach().cpu().clone());
   comparison.fact("trajectory", {{"updates", entry.steps.size()}, {"optimizer_backend", built.optimizer.backend_name()},
                                    {"interpretation", "bounded matched updates; no dataset AP measurement"}});
   for (std::size_t update = 0; update < entry.steps.size(); ++update) {
    CAPTURE(update);
    comparison.fact("step", update);
    const auto archive = oracle.archive(entry, update);
    const auto targets = support::oracle_targets(archive, entry, torch::kCUDA);
    built.optimizer.zero_grad(true);
    auto input = archive.tensor("input", torch::kCUDA).clone().set_requires_grad(true);
    const auto mask = archive.tensor("input_mask", torch::kCUDA);
    std::vector<torch::Tensor> encoder;
    rf::ModelOutputs outputs;
    torch::Tensor total;
    step.forward([&] {
     comparison.fact("forward autocast", {{"enabled", at::autocast::is_autocast_enabled(at::kCUDA)}, {"dtype", static_cast<int>(at::autocast::get_autocast_dtype(at::kCUDA))}});
     outputs = model.forward_observed({input, mask}, model.config().segmentation, encoder);
     support::compare_oracle_outputs(comparison, archive, outputs, encoder);
     total = compare_loss(comparison, archive, outputs, targets, config, true, "");
    });
    step.backward(total);
    compare_gradients(comparison, archive, model, "gradient/", true);
    std::vector<torch::Tensor> norms;
    for (const auto& parameter : built.optimizer.parameters()) if (parameter.grad().defined()) norms.push_back(parameter.grad().norm());
    comparison.compare("clip_norm", torch::stack(norms).norm(), archive.tensor("clip_norm"));
    if (entry.dtype() != torch::kFloat32) comparison.compare("stock-policy/clip_norm", torch::stack(norms).norm(), archive.tensor("stock/clip_norm"), true);
    built.optimizer.clip_grad_norm_(entry.clip_max_norm);
    compare_gradients(comparison, archive, model, "clipped/", entry.dtype() != torch::kFloat32);
    rf::TensorMap actual_before;
    for (const auto& parameter : model.named_parameters(true)) if (parameter.value().requires_grad()) actual_before.emplace(parameter.key(), parameter.value().detach().cpu().clone());
    built.optimizer.step();
    for (const auto& parameter : model.named_parameters(true)) if (parameter.value().requires_grad()) {
     const auto key = "updated/" + parameter.key();
     comparison.compare(key, parameter.value(), archive.tensor(key));
     comparison.compare("update/" + parameter.key(), parameter.value().detach().cpu() - actual_before.at(parameter.key()), archive.tensor(key) - expected_before.at(parameter.key()));
     expected_before[parameter.key()] = archive.tensor(key);
     if (entry.dtype() != torch::kFloat32) comparison.compare("stock-policy/" + key, parameter.value(), archive.tensor("stock/" + key), true);
    }
    compare_optimizer(comparison, archive, built.optimizer, entry, true);
    model.eval();
    {
     torch::NoGradGuard no_grad;
     step.forward([&] {
      auto held = model.forward_observed({archive.tensor("held/input", torch::kCUDA), mask}, model.config().segmentation, encoder);
      support::compare_oracle_outputs(comparison, archive, held, encoder, "held/");
      (void)compare_loss(comparison, archive, held, targets, config, false, "held/");
     });
    }
    model.train();
   }
  }
 }
}
