#include "src/backend/ml/torch/tests/catch_support.h"
#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/models/rfdetr/core/detail/backbone.h"
#include "src/backend/models/rfdetr/core/detail/keypoint_head.h"
#include "src/backend/models/rfdetr/core/detail/class_tensor_axes.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/torch/scalar_type.h"
#include "src/backend/ml/layers/ms_deform_attn.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <torch/serialize.h>
#include <torch/autograd.h>
#include "src/common/io/file_digest.h"
#include <ATen/record_function.h>
#include <ATen/ops/_addmm_activation.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
namespace rf = mmltk::backend::models::rfdetr;
namespace {
using ComparisonPolicy = rf::testsupport::OracleComparisonPolicy;
using Json = nlohmann::json;
class ForwardOperatorObservation final {
 struct Probe {
  bool feed_forward = false;
  std::vector<torch::Tensor> inputs;
  torch::Tensor output;
  std::shared_ptr<torch::Tensor> incoming;
  int64_t im2col_step = 0;
 };
 struct Event final : at::ObserverContext {
  std::string name;
  std::optional<Probe> probe;
  explicit Event(std::string value) : name(std::move(value)) {}
 };
 inline static thread_local ForwardOperatorObservation* active_ = nullptr;
 at::CallbackHandle callback_ = 0;
 std::map<std::string, std::size_t> counts_;
 rf::TensorMap outputs_;
 std::shared_ptr<rf::TensorMap> backward_;
 rf::TensorMap watched_;
 std::vector<std::pair<torch::Tensor, unsigned>> gradient_hooks_;
 std::size_t depth_ = 0;
 bool finished_ = false;
 bool diagnostic_ = false;
 std::set<const c10::TensorImpl*> feed_forward_weights_;
 std::vector<Probe> probes_;

public:
 explicit ForwardOperatorObservation(bool enabled, const rf::NativeRfDetrModel& model, bool backward = false) {
  if (!enabled) return;
  if (backward) backward_ = std::make_shared<rf::TensorMap>();
  const auto* diagnostic = std::getenv("MMLTK_RFDETR_ORACLE_OPERATOR_TRACE");
  diagnostic_ = diagnostic && std::string_view(diagnostic) == "1";
  if (diagnostic_)
   for (const auto& parameter : model.named_parameters())
    if (parameter.key().ends_with(".linear1.weight")) feed_forward_weights_.insert(parameter.value().unsafeGetTensorImpl());
  TORCH_CHECK(!active_, "forward operator observation is already active");
  callback_ = at::addThreadLocalCallback(at::RecordFunctionCallback(
   [](const at::RecordFunction& event) -> std::unique_ptr<at::ObserverContext> {
   const std::string_view name(event.name());
   if (active_->diagnostic_ && active_->probes_.size() < 32) {
    const auto inputs = event.inputs();
    const bool feed_forward = name == "aten::linear" && inputs.size() == 3 && inputs[1].isTensor() && active_->feed_forward_weights_.contains(inputs[1].toTensor().unsafeGetTensorImpl());
    if (feed_forward || name == "mmltk::ms_deform_attn") {
     auto probe = std::make_unique<Event>(std::string(name));
     probe->probe.emplace();
     probe->probe->feed_forward = feed_forward;
     if (!feed_forward && inputs.size() == 6 && inputs[5].isInt()) probe->probe->im2col_step = inputs[5].toInt();
     for (const auto& input : inputs)
      if (input.isTensor()) probe->probe->inputs.push_back(input.toTensor().detach());
     return probe;
    }
   }
   if (name == "aten::topk") active_->finished_ = true;
   if (active_->finished_ && !active_->backward_) return {};
   if (name != "aten::conv2d" && name != "aten::layer_norm" && name != "aten::linear" && name != "aten::gelu" && name != "aten::scaled_dot_product_attention") return {};
   ++active_->depth_;
   return std::make_unique<Event>(std::string(name.substr(6)));
  },
   [](const at::RecordFunction& event, at::ObserverContext* context) {
   if (!context) return;
   auto& observed = *static_cast<Event*>(context);
   if (observed.probe) {
    if (event.outputs().size() == 1 && event.outputs().front().isTensor()) {
     observed.probe->output = event.outputs().front().toTensor();
     active_->probes_.push_back(std::move(*observed.probe));
    }
    return;
   }
   if (--active_->depth_) return;
   auto name = observed.name;
   if (active_->outputs_.size() >= 256 || event.outputs().size() != 1 || !event.outputs().front().isTensor()) {
    active_->finished_ = true;
    return;
   }
   auto value = event.outputs().front().toTensor();
   auto prefix = "forward-operator/" + name;
   if (active_->finished_) {
    prefix = "decoder-" + prefix + "/";
    for (const auto dimension : value.sizes()) prefix += std::to_string(dimension) + "x";
   }
   const auto index = active_->counts_[prefix]++;
   auto key = prefix + "/" + std::to_string(index);
   active_->outputs_.emplace(key, value.detach().clone());
   if (active_->backward_) {
    key.replace(key.find("forward"), 7, "backward");
    active_->backward_->emplace(key, torch::Tensor{});
    active_->watched_.emplace(key, value);
   }
  })
    .needsOutputs(true)
    .needsInputs(diagnostic_)
    .scopes({at::RecordScope::FUNCTION}));
  active_ = this;
 }
 ~ForwardOperatorObservation() {
  stop_forward();
  for (const auto& [value, handle] : gradient_hooks_) value.remove_hook(handle);
 }
 void stop_forward() {
  if (!callback_) return;
  at::removeCallback(callback_);
  callback_ = 0;
  active_ = nullptr;
  for (const auto& [key, value] : watched_) {
   if (!value.requires_grad()) continue;
   const auto handle = value.register_hook([values = backward_, key](const torch::Tensor& gradient) {
    values->at(key) = gradient.detach();
    return gradient;
   });
   gradient_hooks_.emplace_back(value, handle);
  }
  watched_.clear();
  for (auto& probe : probes_) {
   if (probe.feed_forward || !probe.output.requires_grad()) continue;
   probe.incoming = std::make_shared<torch::Tensor>();
   const auto handle = probe.output.register_hook([incoming = probe.incoming](const torch::Tensor& gradient) {
    *incoming = gradient.detach();
    return gradient;
   });
   gradient_hooks_.emplace_back(probe.output, handle);
  }
 }
 void report(rf::testsupport::OracleComparison& comparison, const rf::testsupport::OracleArchive& archive) {
  std::vector<std::string> read_names;
  for (const auto& [name, value] : outputs_)
   if (archive.contains(name)) read_names.push_back(name);
  const auto expected = archive.read_tensors(read_names);
  for (const auto& [name, value] : outputs_)
   if (archive.contains(name)) comparison.compare(name, value, expected.at(name), ComparisonPolicy::Value, true);
  std::array<std::size_t, 2> counts{};
  for (const auto& probe : probes_) {
   if (probe.output.scalar_type() != torch::kFloat32) continue;
   const auto name = std::string("diagnostic/") + (probe.feed_forward ? "feed-forward" : "deformable-attention") + "/" + std::to_string(counts[probe.feed_forward]++);
   torch::Tensor actual, reference;
   if (probe.feed_forward) {
    if (probe.inputs.size() != 3 || probe.inputs[0].scalar_type() != torch::kFloat32) continue;
    actual = torch::relu(probe.output.detach());
    reference = at::_addmm_activation(probe.inputs[2], probe.inputs[0].reshape({-1, probe.inputs[0].size(-1)}), probe.inputs[1].t()).reshape_as(actual);
    comparison.fact(name + "/activation-sign-differences", actual.gt(0).ne(reference.gt(0)).sum().item<int64_t>());
   } else {
    if (probe.inputs.size() != 5) continue;
    actual = probe.output.detach();
    reference = mmltk::backend::ml::layers::ms_deform_attn_reference(probe.inputs[0], probe.inputs[1], probe.inputs[3], probe.inputs[4]);
   }
   comparison.compare(name, actual, reference, ComparisonPolicy::Value, true);
  }
  outputs_.clear();
 }
 void report_backward(rf::testsupport::OracleComparison& comparison, const rf::testsupport::OracleArchive& archive) const {
  if (backward_) {
   std::vector<std::string> names;
   for (const auto& [name, value] : *backward_)
    if (archive.contains(name)) names.push_back(name);
   const auto references = archive.read_tensors(names);
   comparison.fact("diagnostic/backward-operator-count", names.size());
   for (const auto& name : names) comparison.compare(name, backward_->at(name), references.at(name), ComparisonPolicy::Derivative, true);
  }
  std::size_t count = 0;
  for (const auto& probe : probes_) {
   if (probe.feed_forward || probe.inputs.size() != 5 || !probe.incoming || !probe.incoming->defined()) continue;
   mmltk::backend::ml::cuda::TorchAutocastScope scope(false, torch::kFloat32);
   const auto name = "diagnostic/deformable-attention-vjp/" + std::to_string(count++);
   std::vector<torch::Tensor> native, reference;
   for (const auto index : {0U, 3U, 4U}) {
    native.push_back(mmltk::backend::ml::testsupport::independent_leaf(probe.inputs[index]));
    reference.push_back(mmltk::backend::ml::testsupport::independent_leaf(probe.inputs[index]));
   }
   const auto actual = mmltk::backend::ml::layers::ms_deform_attn_cuda_autograd(native[0], probe.inputs[1], probe.inputs[2], native[1], native[2], probe.im2col_step);
   // Upstream materializes contiguous [batch, heads, channels, spatial] before
   // grid_sample. Preserve that layout as well as its AMP float32 arithmetic.
   const auto source_value = reference[0].permute({0, 2, 3, 1}).contiguous().permute({0, 3, 1, 2});
   const auto expected = mmltk::backend::ml::layers::ms_deform_attn_reference(source_value.to(torch::kFloat32), probe.inputs[1], reference[1].to(torch::kFloat32), reference[2].to(torch::kFloat32))
                          .to(actual.scalar_type());
   comparison.fact(name + "/incoming-norm", probe.incoming->to(torch::kFloat32).norm().item<double>());
   comparison.compare(name + "/output", actual, expected, ComparisonPolicy::Value, true);
   const auto actual_gradients = torch::autograd::grad({actual}, native, {*probe.incoming});
   const auto expected_gradients = torch::autograd::grad({expected}, reference, {*probe.incoming});
   constexpr std::array operands{"value", "sampling-locations", "attention-weights"};
   for (std::size_t index = 0; index < operands.size(); ++index)
    comparison.compare(name + "/" + operands[index], actual_gradients[index], expected_gradients[index], ComparisonPolicy::Derivative, true);
  }
 }
};
std::vector<Json> oracle_comparison_records(const std::filesystem::path& root, const std::string& name) {
 std::ifstream stream(root / "comparisons" / (name + ".jsonl"));
 REQUIRE(stream.is_open());
 std::vector<Json> result;
 for (std::string line; std::getline(stream, line);) result.push_back(Json::parse(line));
 REQUIRE(stream.eof());
 return result;
}
void check_oracle_policy(at::ScalarType precision, ComparisonPolicy policy, const torch::Tensor& actual, const torch::Tensor& expected, bool passed) {
 const mmltk::testsupport::ScopedTempDir root("rfdetr-oracle-policy");
 constexpr std::array labels{
  "renamed/value", "parameter-group/renamed", "gradient/renamed", "optimizer/renamed/exp_avg_sq", "renamed/step", "renamed/point_coords", "optimizer/renamed/exp_avg/unused-zero"
 };
 for (const bool differential : {false, true}) {
  CAPTURE(differential);
  const std::string name = differential ? "stock" : "required";
  rf::testsupport::OracleComparison comparison(root.path(), name, precision);
  for (const auto* label : labels) {
   CAPTURE(label);
   if (passed || differential) {
    REQUIRE_NOTHROW(comparison.compare(label, actual, expected, policy, differential));
   } else {
    REQUIRE_THROWS_AS(comparison.compare(label, actual, expected, policy, differential), std::runtime_error);
   }
  }
  // Read while the writer is alive: every success and failure must be flushed.
  const auto records = oracle_comparison_records(root.path(), name);
  REQUIRE(records.size() == labels.size() + 1);
  REQUIRE(records.front().at("boundary") == "policy");
  Json first;
  for (std::size_t index = 0; index < labels.size(); ++index) {
   auto record = records[index + 1];
   CHECK(record.at("boundary") == labels[index]);
   CHECK(record.at("passed") == passed);
   CHECK(record.at("policy_difference") == differential);
   CHECK(record.at("first_difference") == (!passed && !differential && index == 0));
   record.erase("boundary");
   record.erase("first_difference");
   if (index == 0)
    first = record;
   else
    CHECK(record == first);
  }
 }
}
}  // namespace
TEST_CASE("Oracle numerical policies retain their bounds under renamed evidence labels", "[rfdetr][parity][oracle_policy]") {
 struct PrecisionCase {
  at::ScalarType dtype;
  double value_inside, value_outside, value_absolute_inside, value_absolute_outside, derivative_inside, derivative_outside;
 };
 constexpr std::array precisions{
  PrecisionCase{torch::kFloat32, .0002, .0004, 1e-5, 3e-5, .0015, .0025}, PrecisionCase{torch::kFloat16, .01, .02, .001, .003, .01, .02},
  PrecisionCase{torch::kBFloat16, .02, .04, .001, .003, .02, .04}
 };
 const auto scalar = [](double value) { return torch::tensor(value, torch::kFloat64); };
 const double epsilon = std::numeric_limits<double>::epsilon();
 for (const auto& precision : precisions) {
  DYNAMIC_SECTION("precision " << static_cast<int>(precision.dtype)) {
   struct PolicyCase {
    const char* name;
    ComparisonPolicy policy;
    torch::Tensor actual, expected;
    bool passed;
   };
   const auto vector_expected = torch::tensor({1., 0., 0., 0.}, torch::kFloat64);
   const auto vector_inside = torch::tensor({1., precision.derivative_inside, 0., 0.}, torch::kFloat64);
   const auto vector_l2_failure = torch::tensor({1., precision.derivative_inside, precision.derivative_inside, precision.derivative_inside}, torch::kFloat64);
   const auto vector_max_failure = torch::tensor({1. + precision.derivative_outside, 1.}, torch::kFloat64);
   const std::vector<PolicyCase> cases{
    {"value relative inside", ComparisonPolicy::Value, scalar(1. + precision.value_inside), scalar(1.), true},
    {"value relative outside", ComparisonPolicy::Value, scalar(1. + precision.value_outside), scalar(1.), false},
    {"value absolute inside", ComparisonPolicy::Value, scalar(precision.value_absolute_inside), scalar(0.), true},
    {"value absolute outside", ComparisonPolicy::Value, scalar(precision.value_absolute_outside), scalar(0.), false},
    {"value elementwise bound", ComparisonPolicy::Value, vector_inside, vector_expected, false}, {"exact equal", ComparisonPolicy::Exact, scalar(1.), scalar(1.), true},
    {"exact unequal", ComparisonPolicy::Exact, scalar(1. + epsilon), scalar(1.), false},
    {"derivative relative inside", ComparisonPolicy::Derivative, scalar(1. + precision.derivative_inside), scalar(1.), true},
    {"derivative relative outside", ComparisonPolicy::Derivative, scalar(1. + precision.derivative_outside), scalar(1.), false},
    {"derivative absolute inside", ComparisonPolicy::Derivative, scalar(5e-8), scalar(0.), true}, {"derivative absolute outside", ComparisonPolicy::Derivative, scalar(2e-7), scalar(0.), false},
    {"derivative global bound", ComparisonPolicy::Derivative, vector_inside, vector_expected, true}, {"derivative L2 bound", ComparisonPolicy::Derivative, vector_l2_failure, vector_expected, false},
    {"derivative maximum bound", ComparisonPolicy::Derivative, vector_max_failure, torch::ones({2}, torch::kFloat64), false},
    {"second moment relative inside", ComparisonPolicy::SecondMoment, scalar(1. + precision.derivative_inside), scalar(1.), true},
    {"second moment relative outside", ComparisonPolicy::SecondMoment, scalar(1. + precision.derivative_outside), scalar(1.), false},
    {"second moment absolute inside", ComparisonPolicy::SecondMoment, scalar(5e-13), scalar(0.), true},
    {"second moment absolute outside", ComparisonPolicy::SecondMoment, scalar(2e-12), scalar(0.), false},
    {"second moment global bound", ComparisonPolicy::SecondMoment, vector_inside, vector_expected, true},
    {"second moment L2 bound", ComparisonPolicy::SecondMoment, vector_l2_failure, vector_expected, false},
    {"second moment maximum bound", ComparisonPolicy::SecondMoment, vector_max_failure, torch::ones({2}, torch::kFloat64), false},
    {"hyperparameter inside", ComparisonPolicy::Hyperparameter, scalar(1. + 4 * epsilon), scalar(1.), true},
    {"hyperparameter outside", ComparisonPolicy::Hyperparameter, scalar(1. + 16 * epsilon), scalar(1.), false}, {"hyperparameter zero", ComparisonPolicy::Hyperparameter, scalar(0.), scalar(0.), true},
    {"hyperparameter zero is exact", ComparisonPolicy::Hyperparameter, scalar(std::numeric_limits<double>::denorm_min()), scalar(0.), false}
   };
   for (const auto& entry : cases) {
    CAPTURE(entry.name);
    check_oracle_policy(precision.dtype, entry.policy, entry.actual, entry.expected, entry.passed);
   }
  }
 }
}
TEST_CASE("Oracle policies retain structural and finite tensor admission", "[rfdetr][parity][oracle_policy]") {
 const auto one = torch::ones({1}, torch::kFloat32);
 const auto nan = torch::full({1}, std::numeric_limits<float>::quiet_NaN(), torch::kFloat32);
 const auto infinity = torch::full({1}, std::numeric_limits<float>::infinity(), torch::kFloat32);
 const auto age = torch::tensor({int64_t{1} << 54}, torch::kInt64);
 for (const auto policy : {ComparisonPolicy::Value, ComparisonPolicy::Exact, ComparisonPolicy::Derivative, ComparisonPolicy::SecondMoment, ComparisonPolicy::Hyperparameter}) {
  DYNAMIC_SECTION("policy " << static_cast<int>(policy)) {
   struct TensorCase {
    const char* name;
    torch::Tensor actual, expected;
    bool passed;
   };
   const std::vector<TensorCase> cases{
    {"undefined pair", {}, {}, true}, {"undefined actual", {}, one, false}, {"undefined expected", one, {}, false}, {"dtype mismatch", one.to(torch::kFloat64), one, false},
    {"shape mismatch", one.reshape({1, 1}), one, false}, {"empty pair", one.narrow(0, 0, 0), one.narrow(0, 0, 0), true}, {"integer equal", age, age, true},
    {"integer exact beyond double precision", age + 1, age, false}, {"boolean equal", one.to(torch::kBool), one.to(torch::kBool), true},
    {"boolean unequal", one.to(torch::kBool), torch::zeros({1}, torch::kBool), false}, {"NaN pair", nan, nan, false}, {"NaN mask mismatch", nan, one, false},
    {"infinity pair", infinity, infinity, false}, {"infinity sign mismatch", infinity, -infinity, false}, {"infinity finite mismatch", one, infinity, false},
    {"nonfinite category mismatch", nan, infinity, false}
   };
   for (const auto& entry : cases) {
    CAPTURE(entry.name);
    check_oracle_policy(torch::kFloat32, policy, entry.actual, entry.expected, entry.passed);
   }
  }
 }
}
TEST_CASE("Oracle stock differences retain required failure and inventory reporting", "[rfdetr][parity][oracle_policy]") {
 const mmltk::testsupport::ScopedTempDir root("rfdetr-oracle-failure");
 const auto actual = torch::ones({1}, torch::kFloat32), expected = torch::zeros_like(actual);
 rf::testsupport::OracleComparison comparison(root.path(), "failures", torch::kFloat32);
 REQUIRE_NOTHROW(comparison.compare("observed", actual, expected, ComparisonPolicy::Value, true));
 REQUIRE_THROWS_AS(comparison.compare("required", actual, expected, ComparisonPolicy::Value), std::runtime_error);
 REQUIRE_NOTHROW(comparison.compare("observed-after-failure", actual, expected, ComparisonPolicy::Value, true));
 REQUIRE_THROWS_AS(comparison.compare("required-after-failure", actual, expected, ComparisonPolicy::Value), std::runtime_error);
 const auto records = oracle_comparison_records(root.path(), "failures");
 REQUIRE(records.size() == 5);
 for (std::size_t index = 1; index < records.size(); ++index) {
  CHECK(records[index].at("passed") == false);
  CHECK(records[index].at("first_difference") == (index == 2));
 }
 rf::testsupport::OracleComparison inventory(root.path(), "inventory", torch::kFloat32);
 REQUIRE_NOTHROW(inventory.inventory("equal", {"tensor"}, {"tensor"}));
 REQUIRE_THROWS_AS(inventory.inventory("missing", {}, {"tensor"}), std::runtime_error);
 REQUIRE_THROWS_AS(inventory.inventory("unexpected", {"tensor", "extra"}, {"tensor"}), std::runtime_error);
 const auto inventories = oracle_comparison_records(root.path(), "inventory");
 REQUIRE(inventories.size() == 4);
 for (std::size_t index = 1; index < inventories.size(); ++index) {
  CHECK(inventories[index].at("passed") == (index == 1));
  CHECK(inventories[index].at("first_difference") == (index == 2));
 }
}
TEST_CASE("Oracle comparison collection retains numerical failures and immediate structural gates", "[rfdetr][parity][oracle_policy]") {
 const mmltk::testsupport::ScopedTempDir root("rfdetr-oracle-collection");
 const auto zero = torch::zeros({1}, torch::kFloat32), one = torch::ones_like(zero);
 rf::testsupport::OracleComparison comparison(root.path(), "collection", torch::kFloat32);
 int observed = 0;
 REQUIRE_THROWS_AS(comparison.compare_all([&] {
  comparison.compare("first", one, zero, ComparisonPolicy::Value);
  ++observed;
  comparison.compare("second", one, zero, ComparisonPolicy::Derivative);
  ++observed;
 }),
  std::runtime_error);
 CHECK(observed == 2);
 const auto records = oracle_comparison_records(root.path(), "collection");
 REQUIRE(records.size() == 3);
 CHECK(records[1].at("passed") == false);
 CHECK(records[2].at("passed") == false);
 CHECK(records[1].at("first_difference") == true);
 CHECK(records[2].at("first_difference") == false);
 for (const auto& invalid : {one.reshape({1, 1}), one.to(torch::kFloat64), one * std::numeric_limits<float>::infinity()}) {
  REQUIRE_THROWS_AS(comparison.compare_all([&] {
   comparison.compare("structural", invalid, zero, ComparisonPolicy::Value);
   ++observed;
  }),
   std::runtime_error);
 }
 REQUIRE_THROWS_AS(comparison.compare_all([&] {
  comparison.compare("discrete", one, zero, ComparisonPolicy::Exact);
  ++observed;
 }),
  std::runtime_error);
 CHECK(observed == 2);
 REQUIRE_NOTHROW(comparison.compare_all([&] { comparison.compare("equal", zero, zero, ComparisonPolicy::Value); }));
 REQUIRE_NOTHROW(comparison.observe_values([&] {
  comparison.compare("trajectory-selection", one, zero, ComparisonPolicy::Exact);
  comparison.compare("trajectory-value", one, zero, ComparisonPolicy::Value);
 }));
 for (const auto& invalid : {one.reshape({1, 1}), one.to(torch::kFloat64), one * std::numeric_limits<float>::infinity(), torch::Tensor{}}) {
  REQUIRE_THROWS_AS(comparison.observe_values([&] { comparison.compare("invalid-observation", invalid, zero, ComparisonPolicy::Value); }), std::runtime_error);
 }
 REQUIRE_NOTHROW(comparison.derivatives("complete-gradient/", {{"large", one}, {"near-null", one * 1e-6}}, {{"large", one}, {"near-null", zero}}));
 REQUIRE_THROWS_AS(comparison.derivatives("incorrect-gradient/", {{"large", one * 1.1}}, {{"large", one}}), std::runtime_error);
 REQUIRE_THROWS_AS(comparison.derivatives("missing-gradient/", {{"large", {}}}, {{"large", one}}), std::runtime_error);
 const auto next = torch::nextafter(one, one * 2);
 REQUIRE_NOTHROW(comparison.parameter_update("rounded-update", one, next, one, one));
 REQUIRE_THROWS_AS(comparison.parameter_update("incorrect-update", one, one + 1e-4, one, one), std::runtime_error);
}
TEST_CASE("Oracle shard indexes retain ordered tensors and reject incomplete or altered publication", "[rfdetr][parity][oracle_archive]") {
 namespace support = rf::testsupport;
 namespace io = mmltk::common::io;
 static_assert(!std::is_constructible_v<support::OracleArchive, support::OracleArchiveRecord>);
 static_assert(!std::is_constructible_v<support::OracleArchive, support::OracleArchiveRecord, std::function<void(std::size_t)>>);
 const mmltk::testsupport::ScopedTempDir root("oracle-shards");
 const auto reference = [&](const std::string& name) {
  const auto path = root.path() / name;
  return Json{{"file", name}, {"sha256", io::sha256_hex(io::sha256_file(path))}, {"bytes", std::filesystem::file_size(path)}};
 };
 const auto write = [&](const std::string& name, const Json& value) {
  std::ofstream file(root.path() / name);
  file << value.dump();
  file.close();
  return reference(name);
 };
 constexpr std::array families{"gradient/", "stock/gradient/", "updated/"};
 constexpr std::array loss_names{"z", "a", "m"};
 std::array<c10::Dict<std::string, torch::Tensor>, 3> values;
 std::array<Json, 3> shards;
 const auto write_shard = [&](std::size_t index) {
  const auto bytes = torch::pickle_save(c10::IValue(values[index]));
  const auto filename = std::to_string(index) + ".pt";
  std::ofstream data(root.path() / filename, std::ios::binary);
  data.write(bytes.data(), bytes.size());
  data.close();
  const auto physical = reference(filename);
  for (const auto& [name, value] : physical.items()) shards[index][name] = value;
 };
 for (std::size_t index = 0; index < shards.size(); ++index) {
  const auto add = [&](const std::string& name, const torch::Tensor& tensor) {
   values[index].insert(name, tensor);
   const auto finite = torch::isfinite(tensor);
   if (tensor.is_floating_point()) values[index].insert("finite/" + name, finite);
   shards[index]["tensors"][name] = {
    {"defined", true}, {"shape", tensor.sizes().vec()}, {"stride", tensor.strides().vec()}, {"dtype", "torch." + mmltk::backend::ml::serialization::scalar_type_name(tensor.scalar_type())},
    {"device", "cuda:0"}, {"finite_count", finite.sum().item<int64_t>()}, {"elements", tensor.numel()}
   };
  };
  add("loss/" + std::string(loss_names[index]), torch::tensor({float(index + 1)}));
  const auto backing = torch::arange(4096, torch::kFloat32) + static_cast<float>(5000 * index);
  const std::string family(families[index]);
  add(family + "unused", backing);
  add(family + "z", backing.narrow(0, 3, 4).reshape({2, 2}));
  add(family + "a", backing.narrow(0, 12, 4));
  if (index == 0) {
   shards[index]["tensors"]["optional"] = {{"defined", false}};
   add("typed/int64", torch::tensor({int64_t{1} << 54, (int64_t{1} << 54) + 1}, torch::kInt64));
  } else if (index == 1) {
   add("typed/bool", torch::tensor({true, false}, torch::kBool));
  } else {
   add("typed/empty", torch::empty({0, 2}, torch::kFloat64));
   add("typed/scalar", torch::tensor(1.25, torch::kFloat64));
  }
  write_shard(index);
 }
 const auto publish = [&] {
  Json refs = Json::array();
  std::uint64_t bytes = 0;
  std::size_t tensors = 0;
  for (std::size_t index = 0; index < shards.size(); ++index) {
   refs.push_back(write(std::to_string(index) + ".json", shards[index]));
   bytes += shards[index].at("bytes").get<std::uint64_t>();
   tensors += shards[index].at("tensors").size();
  }
  return write("step.json", {{"format", "mmltk.rfdetr.step.v2"}, {"shards", refs}, {"bytes", bytes}, {"tensor_count", tensors}});
 };
 auto committed = publish();
 SECTION("selected tensors authenticate retained payload after unrelated consumers retire") {
  std::filesystem::remove(root.path() / "1.pt");
  std::filesystem::remove(root.path() / "2.pt");
  const std::array<std::string, 2> selected{"loss/z", "loss/z"};
  const auto archive = support::OracleArchive::select(root.path(), committed, selected);
  CHECK(archive.tensor("loss/z").item<float>() == 1.F);
  CHECK((archive.names("loss/") == std::set<std::string>{"z"}));
  CHECK((archive.ordered_defined_names("loss/") == std::vector<std::string>{"z"}));
  CHECK_FALSE(archive.contains("gradient/z"));
  CHECK_THROWS(archive.tensor("gradient/z"));
  CHECK_THROWS(archive.tensor("loss/a"));
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
  const std::array<std::string, 1> absent{"missing"};
  CHECK_THROWS_AS(support::OracleArchive::select(root.path(), committed, absent), std::runtime_error);
 }
 SECTION("selected tensors still require their payload and the complete authenticated index") {
  const std::array<std::string, 1> selected{"loss/z"};
  SECTION("missing retained payload") {
   std::filesystem::remove(root.path() / "0.pt");
   CHECK_THROWS(support::OracleArchive::select(root.path(), committed, selected));
  }
  SECTION("altered unselected metadata") {
   std::ofstream changed(root.path() / "2.json", std::ios::app);
   changed << ' ';
   changed.close();
   CHECK_THROWS(support::OracleArchive::select(root.path(), committed, selected));
  }
 }
 SECTION("complete multi-shard view retains order and borrowed values") {
  const support::OracleArchive archive(root.path(), committed);
  CHECK((archive.ordered_defined_names("loss/") == std::vector<std::string>{"z", "a", "m"}));
  const auto first = archive.tensor("loss/z");
  CHECK(archive.tensor("loss/a").item<float>() == 2.F);
  CHECK(first.item<float>() == 1.F);
  CHECK_FALSE(archive.tensor("optional").defined());
  CHECK((archive.names("loss/") == std::set<std::string>{"a", "m", "z"}));
 }
 SECTION("grouped alternating families decode each required shard at most once") {
  std::array<std::size_t, 3> loads{};
  const support::OracleArchive archive(root.path(), committed, [&](std::size_t index) { ++loads.at(index); });
  CHECK((loads == std::array<std::size_t, 3>{1, 1, 1}));
  const auto check_grouped_loads = [&] {
   CHECK(std::ranges::all_of(loads, [](std::size_t count) { return count <= 1; }));
   CHECK(std::accumulate(loads.begin(), loads.end(), std::size_t{0}) == 2);
  };
  loads.fill(0);
  CHECK(archive.read_tensors(std::array<std::string, 3>{"gradient/z", "stock/gradient/z", "updated/z"}).size() == 3);
  check_grouped_loads();
  const std::vector<std::string> requested{"gradient/z", "stock/gradient/z", "updated/z", "gradient/a", "stock/gradient/a", "updated/a", "gradient/z", "optional"};
  for (int pass = 0; pass < 2; ++pass) {
   CAPTURE(pass);
   loads.fill(0);
   const auto selected = archive.read_tensors(requested);
   CHECK(selected.size() == 7);
   CHECK_FALSE(selected.at("optional").defined());
   check_grouped_loads();
   std::size_t selected_bytes = 0;
   for (std::size_t index = 0; index < families.size(); ++index) {
    for (const auto* suffix : {"z", "a"}) {
     const auto name = std::string(families[index]) + suffix;
     const auto& tensor = selected.at(name);
     const auto source = values[index].at(name);
     CHECK(torch::equal(tensor, source));
     CHECK(tensor.scalar_type() == source.scalar_type());
     CHECK(tensor.sizes() == source.sizes());
     CHECK(tensor.is_cpu());
     CHECK(tensor.storage_offset() == 0);
     CHECK(tensor.storage().nbytes() == tensor.nbytes());
     CHECK(tensor.storage().nbytes() < source.storage().nbytes());
     selected_bytes += tensor.storage().nbytes();
    }
   }
   CHECK(selected_bytes == 6 * 4 * sizeof(float));
  }
  // More logical lookups, including duplicate names, leave physical work fixed.
  std::vector<std::string> repeated;
  for (int count = 0; count < 32; ++count) repeated.insert(repeated.end(), requested.begin(), requested.end());
  loads.fill(0);
  CHECK(archive.read_tensors(repeated).size() == 7);
  check_grouped_loads();
  CHECK((archive.ordered_defined_names("loss/") == std::vector<std::string>{"z", "a", "m"}));
  CHECK((archive.ordered_defined_names("gradient/") == std::vector<std::string>{"unused", "z", "a"}));
 }
 SECTION("empty undefined and missing read sets do not decode shards") {
  std::array<std::size_t, 3> loads{};
  const support::OracleArchive archive(root.path(), committed, [&](std::size_t index) { ++loads.at(index); });
  loads.fill(0);
  CHECK(archive.read_tensors({}).empty());
  const auto undefined = archive.read_tensors(std::array<std::string, 2>{"optional", "optional"});
  REQUIRE(undefined.size() == 1);
  CHECK_FALSE(undefined.at("optional").defined());
  CHECK_THROWS_AS(archive.read_tensors(std::array<std::string, 3>{"gradient/z", "missing", "updated/z"}), std::runtime_error);
  CHECK_THROWS_AS(archive.tensor("missing"), std::runtime_error);
  CHECK((loads == std::array<std::size_t, 3>{0, 0, 0}));
  const auto selected = archive.read_tensors(std::array<std::string, 2>{"gradient/a", "optional"});
  CHECK(selected.size() == 2);
  CHECK((loads == std::array<std::size_t, 3>{1, 0, 0}));
 }
 SECTION("selected values retain exact dtype shape and independent storage through owner destruction") {
  rf::TensorMap selected;
  torch::Tensor isolated;
  {
   const support::OracleArchive archive(root.path(), committed);
   selected = archive.read_tensors(std::array<std::string, 6>{"gradient/z", "typed/int64", "typed/bool", "typed/empty", "typed/scalar", "optional"});
   isolated = archive.tensor("stock/gradient/a");
   auto modified = archive.read_tensors(std::array<std::string, 1>{"gradient/z"});
   modified.at("gradient/z").fill_(-1);
   CHECK(torch::equal(archive.tensor("gradient/z"), values[0].at("gradient/z")));
   CHECK(archive.tensor("loss/m").item<float>() == 3.F);
  }
  REQUIRE(selected.size() == 6);
  CHECK_FALSE(selected.at("optional").defined());
  for (std::size_t index = 0; index < values.size(); ++index) {
   for (const auto& [name, tensor] : selected) {
    if (!tensor.defined() || !values[index].contains(name)) continue;
    const auto source = values[index].at(name);
    CHECK(torch::equal(tensor, source));
    CHECK(tensor.scalar_type() == source.scalar_type());
    CHECK(tensor.sizes() == source.sizes());
    CHECK(tensor.storage().nbytes() == tensor.nbytes());
   }
  }
  CHECK(torch::equal(isolated, values[1].at("stock/gradient/a")));
  CHECK(isolated.storage().nbytes() == isolated.nbytes());
 }
 SECTION("a later physical read reauthenticates unchanged-size shard bytes") {
  const support::OracleArchive archive(root.path(), committed);
  const auto retained = archive.read_tensors(std::array<std::string, 1>{"gradient/z"});
  (void)archive.tensor("loss/m");
  const auto path = root.path() / "0.pt";
  const auto original_bytes = std::filesystem::file_size(path);
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  char byte = 0;
  file.read(&byte, 1);
  byte ^= 1;
  file.seekp(0);
  file.write(&byte, 1);
  file.close();
  REQUIRE(std::filesystem::file_size(path) == original_bytes);
  CHECK_THROWS_AS(archive.read_tensors(std::array<std::string, 2>{"updated/z", "gradient/z"}), std::runtime_error);
  CHECK_THROWS_AS(archive.tensor("gradient/z"), std::runtime_error);
  CHECK(torch::equal(retained.at("gradient/z"), values[0].at("gradient/z")));
 }
 SECTION("corruption in an unrequested shard fails complete admission") {
  values[2].at("finite/updated/unused").fill_(false);
  write_shard(2);
  committed = publish();
  CHECK_THROWS_AS(([&] {
   const support::OracleArchive archive(root.path(), committed);
   return archive.read_tensors(std::array<std::string, 1>{"gradient/z"});
  }()),
   std::runtime_error);
 }
 SECTION("unrequested noncontiguous serialized values fail admission") {
  values[2].insert_or_assign("updated/z", values[2].at("updated/z").transpose(0, 1));
  write_shard(2);
  committed = publish();
  CHECK_THROWS_AS(support::OracleArchive(root.path(), committed), std::runtime_error);
 }
 SECTION("unrequested unexpected dictionary values fail admission") {
  values[2].insert("unexpected", torch::zeros({1}));
  write_shard(2);
  committed = publish();
  CHECK_THROWS_AS(support::OracleArchive(root.path(), committed), std::runtime_error);
 }
 SECTION("unpublished leaves do not replace authenticated completion") {
  write("step.json.partial", {{"incomplete", true}});
  CHECK_NOTHROW(support::OracleArchive(root.path(), committed));
 }
 SECTION("missing shard") {
  std::filesystem::remove(root.path() / "1.pt");
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("changed completed shard") {
  std::ofstream file(root.path() / "1.pt", std::ios::app);
  file << "changed";
  file.close();
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("changed index") {
  write("0.json", {{"changed", true}});
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("escaping index") {
  committed["file"] = "../step.json";
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("symlinked index") {
  std::filesystem::create_symlink(root.path() / "step.json", root.path() / "linked.json");
  committed["file"] = "linked.json";
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("duplicate JSON members") {
  std::ofstream file(root.path() / "step.json");
  file << "{\"format\":\"mmltk.rfdetr.step.v2\",\"format\":\"mmltk.rfdetr.step.v2\"}";
  file.close();
  committed = reference("step.json");
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("oversized index") {
  committed["bytes"] = 32ULL * 1024 * 1024 + 1;
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("duplicate tensor across shards") {
  shards[1]["tensors"] = shards[0]["tensors"];
  committed = publish();
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("false finite inventory") {
  shards[0]["tensors"]["loss/z"]["finite_count"] = 0;
  committed = publish();
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("invalid source stride") {
  shards[0]["tensors"]["loss/z"]["stride"] = {-1};
  committed = publish();
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
 SECTION("unbounded storage") {
  shards[0]["bytes"] = 4ULL * 1024 * 1024 * 1024 + 1;
  committed = publish();
  CHECK_THROWS(support::OracleArchive(root.path(), committed));
 }
}
TEST_CASE("Actual pinned upstream catalog forward and fixed cotangent VJPs", "[rfdetr][parity][upstream_oracle][cuda]") {
 const auto requested = rf::testsupport::UpstreamOracle::requested();
 const rf::testsupport::UpstreamOracle oracle(requested);
 // CLEANUP-IGNORE: CUDA availability, device scope, and precision scope are separate prerequisites; model, training, and optimizer oracles retain independent setup.
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 c10::cuda::CUDAGuard guard(0);
 mmltk::backend::ml::testsupport::FullMatrixPrecision precision;
 for (const auto& entry : oracle.cases()) {
  for (const auto mode : rf::testsupport::oracle_execution_modes(entry)) {
   DYNAMIC_SECTION(entry.id << " mode=" << static_cast<int>(mode)) {
    CAPTURE(entry.id);
    if (entry.dtype() == torch::kBFloat16) REQUIRE(at::cuda::getCurrentDeviceProperties()->major >= 8);
    auto model = oracle.model(entry);
    if (mode != rf::CompilationMode::kNone) model.optimize_for_inference(entry.batch, entry.training, mode);
    const auto archive = oracle.archive(entry, 0);
    rf::testsupport::OracleComparison comparison(
     requested, "core-" + entry.id + "-mode" + std::to_string(static_cast<int>(mode)), entry.dtype(), "actual upstream forward and fixed cotangent VJP; eager native production CUDA");
    comparison.fact("SDPA comparison backend", entry.math_sdpa ? "math" : "default");
    comparison.fact("case topology", {{"protocol", entry.protocol}, {"groups", entry.groups}});
    auto input = archive.tensor("input", torch::kCUDA).clone().set_requires_grad(true);
    const auto mask = archive.tensor("input_mask", torch::kCUDA);
    if (mode == rf::CompilationMode::kNone) {
     if (model.config().task == rf::ModelTask::Keypoints) {
      for (const auto& family : {rf::detail::kKeypointQueryAxis, rf::detail::kEncoderKeypointQueryAxis}) {
       const auto prefix = "grouppose/" + std::string(family.module_name()) + "/";
       rf::detail::ConditionalQueryInitializer initializer(
        model.config().hidden_dim, rf::KeypointTensorLayout(model.config().keypoints_per_class).compact_count(), model.config().hidden_dim / model.config().keypoint_dimension_downscale);
       initializer.to(torch::kCUDA);
       torch::NoGradGuard no_grad;
       for (const auto& parameter : initializer.named_parameters()) parameter.value().copy_(model.named_parameters()[family.prefix() + parameter.key()]);
       mmltk::backend::ml::cuda::TorchAutocastScope scope(entry.dtype() != torch::kFloat32, entry.dtype());
       const auto expected = archive.read_tensors(std::array<std::string, 2>{prefix + "input", prefix + "output"});
       comparison.compare(prefix + "output", initializer.forward(expected.at(prefix + "input").to(torch::kCUDA)), expected.at(prefix + "output"), ComparisonPolicy::Value);
      }
     }
     rf::detail::NativeBackboneImpl backbone(model.config());
     backbone.to(torch::kCUDA);
     backbone.train(entry.training);
     backbone.set_force_math_attention(entry.math_sdpa);
     {
      torch::NoGradGuard no_grad;
      for (const auto& parameter : backbone.named_parameters()) parameter.value().copy_(model.named_parameters()["backbone.0." + parameter.key()]);
      for (const auto& buffer : backbone.named_buffers()) buffer.value().copy_(model.named_buffers()["backbone.0." + buffer.key()]);
      mmltk::backend::ml::cuda::TorchAutocastScope scope(entry.dtype() != torch::kFloat32, entry.dtype());
      const auto features = backbone.forward_features(input);
      REQUIRE(features.size() == archive.names("backbone/").size());
      std::vector<std::string> read_names;
      for (std::size_t index = 0; index < features.size(); ++index) read_names.push_back("backbone/" + std::to_string(index));
      const auto expected = archive.read_tensors(read_names);
      for (std::size_t index = 0; index < features.size(); ++index) comparison.compare(read_names[index], features[index], expected.at(read_names[index]), ComparisonPolicy::Value);
     }
    }
    std::vector<torch::Tensor> indices;
    rf::ModelOutputs outputs;
    const bool backward_operators = archive.contains("backward-operator/linear/0");
    ForwardOperatorObservation operators((mode == rf::CompilationMode::kNone || backward_operators) && archive.contains("forward-operator/linear/0"), model, backward_operators);
    {
     mmltk::backend::ml::cuda::TorchAutocastScope scope(entry.dtype() != torch::kFloat32, entry.dtype());
     comparison.fact("forward autocast", {{"enabled", at::autocast::is_autocast_enabled(at::kCUDA)}, {"dtype", static_cast<int>(at::autocast::get_autocast_dtype(at::kCUDA))}});
     outputs = mode == rf::CompilationMode::kNone ? model.forward_observed({input, mask}, model.config().has_masks(), indices) : model.forward({input, mask}, model.config().has_masks());
     operators.stop_forward();
     if (mode == rf::CompilationMode::kNone || backward_operators) operators.report(comparison, archive);
    }
    comparison.fact("execution", {{"mode", static_cast<int>(mode)}, {"input", "identical admitted normalized tensor"}, {"training", entry.training}});
    comparison.compare_all([&] {
     rf::testsupport::compare_oracle_outputs(comparison, archive, outputs, indices, "", mode == rf::CompilationMode::kNone);
     rf::testsupport::compare_oracle_vjp(comparison, archive, model, outputs, input, false);
     operators.report_backward(comparison, archive);
    });
   }
  }
 }
}
TEST_CASE("Independent upstream repetitions measure the numerical noise floor", "[rfdetr][parity][upstream_repeat]") {
 const auto requested = rf::testsupport::UpstreamOracle::requested();
 const auto* repeated = std::getenv("MMLTK_RFDETR_UPSTREAM_REPEAT_DIR");
 if (!repeated) SKIP("two independent upstream artifact generations were not requested");
 const rf::testsupport::UpstreamOracle oracle(requested), repeat(repeated);
 for (const auto& entry : oracle.cases()) {
  DYNAMIC_SECTION(entry.id) {
   const auto found = std::ranges::find_if(repeat.cases(), [&](const auto& value) { return value.id == entry.id; });
   REQUIRE(found != repeat.cases().end());
   rf::testsupport::OracleComparison comparison(requested, "repeat-" + entry.id, entry.dtype(), "two independent actual upstream executions; no native model execution");
   REQUIRE(found->steps.size() == entry.steps.size());
   comparison.observe_values([&] {
    for (std::size_t step = 0; step < entry.steps.size(); ++step) {
     comparison.fact("step", step);
     const auto actual = repeat.archive(*found, step), expected = oracle.archive(entry, step);
     for (const auto& [prefix, policy] : std::array{
           std::pair{"output/", ComparisonPolicy::Value}, std::pair{"vjp/", ComparisonPolicy::Derivative}, std::pair{"gradient/", ComparisonPolicy::Derivative},
           std::pair{"clipped/", ComparisonPolicy::Derivative}, std::pair{"updated/", ComparisonPolicy::Value}, std::pair{"encoder/", ComparisonPolicy::Exact},
           std::pair{"assignment/", ComparisonPolicy::Exact}, std::pair{"loss/", ComparisonPolicy::Value}, std::pair{"held/encoder/", ComparisonPolicy::Exact},
           std::pair{"held/assignment/", ComparisonPolicy::Exact}, std::pair{"held/output/", ComparisonPolicy::Value}, std::pair{"held/loss/", ComparisonPolicy::Value}
          }) {
      comparison.inventory(std::string(prefix) + "inventory", actual.names(prefix), expected.names(prefix));
      std::vector<std::string> read_names;
      for (const auto& name : expected.names(prefix)) read_names.push_back(std::string(prefix) + name);
      const auto actual_values = actual.read_tensors(read_names), expected_values = expected.read_tensors(read_names);
      if (policy == ComparisonPolicy::Derivative) {
       rf::TensorMap actual_derivatives, expected_derivatives;
       for (const auto& key : read_names) {
        actual_derivatives.emplace(key.substr(std::string_view(prefix).size()), actual_values.at(key));
        expected_derivatives.emplace(key.substr(std::string_view(prefix).size()), expected_values.at(key));
       }
       comparison.derivatives(prefix, actual_derivatives, expected_derivatives);
      } else {
       for (const auto& key : read_names) comparison.compare(key, actual_values.at(key), expected_values.at(key), policy);
      }
     }
    }
   });
  }
 }
}
TEST_CASE("Explicit RGB resampling matches actual upstream tensor resize and records PIL differences", "[rfdetr][parity][upstream_oracle][torch_resize]") {
 namespace resize = mmltk::backend::imaging::resample;
 const auto requested = rf::testsupport::UpstreamOracle::requested();
 const rf::testsupport::UpstreamOracle oracle(requested);
 const auto archive = oracle.resize_archive();
 rf::testsupport::OracleComparison comparison(requested, "rgb-resampling", torch::kFloat32, "actual torchvision float RGB bilinear align_corners=false; PIL RGB8 dataset path recorded separately");
 for (const auto* name : {"identity", "enlarge", "shrink", "odd_aspect", "high_frequency"}) {
  const std::string key(name);
  const auto rgb = archive.tensor(key + "/rgb8").contiguous();
  const auto input = archive.tensor(key + "/input");
  REQUIRE(rgb.scalar_type() == at::kByte);
  REQUIRE(rgb.dim() == 3);
  REQUIRE(rgb.size(2) == 3);
  const auto width = static_cast<std::uint32_t>(rgb.size(1)), height = static_cast<std::uint32_t>(rgb.size(0));
  auto converted = torch::empty_like(input);
  resize::rgb_hwc_u8_to_nchw_f32(rgb.data_ptr<std::uint8_t>(), converted.data_ptr<float>(), width, height);
  comparison.compare(key + "/unit-srgb", converted, input, ComparisonPolicy::Value);
  for (const auto method : {resize::RgbResizeMethod::TorchBilinear, resize::RgbResizeMethod::TorchBilinearAntialias}) {
   const bool antialias = method == resize::RgbResizeMethod::TorchBilinearAntialias;
   const auto output_key = key + (antialias ? "/torch_antialias" : "/torch");
   const auto expected = archive.tensor(output_key);
   auto actual = torch::empty_like(expected);
   const auto out_width = static_cast<std::uint32_t>(actual.size(2)), out_height = static_cast<std::uint32_t>(actual.size(1));
   const auto row = std::size_t(out_width) * sizeof(float), plane = row * out_height;
   resize::RgbImageResizer resizer(1, false, method);
   resizer.resize_to_planar({rgb.data_ptr<std::uint8_t>(), {width, height, std::size_t(width) * 3, 0, static_cast<std::size_t>(rgb.nbytes()), resize::RgbPixelFormat::RGB8}},
    {actual.data_ptr<float>(), {out_width, out_height, row, plane, plane * 3, resize::RgbPixelFormat::PlanarUnitSrgbF32}}, resize::ImageResizeMode::Stretch);
   comparison.compare(output_key, actual, expected, ComparisonPolicy::Value);
   comparison.compare(output_key + "/versus-pil", actual, archive.tensor(key + "/pil"), ComparisonPolicy::Value, true);
   const auto mean = torch::tensor({.485F, .456F, .406F}).view({3, 1, 1});
   const auto stddev = torch::tensor({.229F, .224F, .225F}).view({3, 1, 1});
   comparison.compare(output_key + "/normalized-versus-pil", (actual - mean) / stddev, archive.tensor(key + "/pil_normalized"), ComparisonPolicy::Value, true);
  }
  comparison.compare(key + "/pil-rgb8-conversion", archive.tensor(key + "/pil_rgb8").to(torch::kFloat32) / 255.F, archive.tensor(key + "/pil"), ComparisonPolicy::Value);
 }
}
