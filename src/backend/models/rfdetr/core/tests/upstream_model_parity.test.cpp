#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
namespace rf = mmltk::backend::models::rfdetr;
namespace {
using ComparisonPolicy = rf::testsupport::OracleComparisonPolicy;
using Json = nlohmann::json;
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
 constexpr std::array labels{"renamed/value", "parameter-group/renamed", "gradient/renamed", "optimizer/renamed/exp_avg_sq", "renamed/step", "renamed/point_coords", "optimizer/renamed/exp_avg/unused-zero"};
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
   if (index == 0) first = record;
   else CHECK(record == first);
  }
 }
}
}
TEST_CASE("Oracle numerical policies retain their bounds under renamed evidence labels", "[rfdetr][parity][oracle_policy]") {
 struct PrecisionCase {
  at::ScalarType dtype;
  double value_inside, value_outside, value_absolute_inside, value_absolute_outside, derivative_inside, derivative_outside;
 };
 constexpr std::array precisions{
  PrecisionCase{torch::kFloat32, .0002, .0004, 1e-5, 3e-5, .0015, .0025},
  PrecisionCase{torch::kFloat16, .01, .02, .001, .003, .01, .02},
  PrecisionCase{torch::kBFloat16, .02, .04, .001, .003, .02, .04}};
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
    {"value elementwise bound", ComparisonPolicy::Value, vector_inside, vector_expected, false},
    {"exact equal", ComparisonPolicy::Exact, scalar(1.), scalar(1.), true},
    {"exact unequal", ComparisonPolicy::Exact, scalar(1. + epsilon), scalar(1.), false},
    {"derivative relative inside", ComparisonPolicy::Derivative, scalar(1. + precision.derivative_inside), scalar(1.), true},
    {"derivative relative outside", ComparisonPolicy::Derivative, scalar(1. + precision.derivative_outside), scalar(1.), false},
    {"derivative absolute inside", ComparisonPolicy::Derivative, scalar(5e-8), scalar(0.), true},
    {"derivative absolute outside", ComparisonPolicy::Derivative, scalar(2e-7), scalar(0.), false},
    {"derivative global bound", ComparisonPolicy::Derivative, vector_inside, vector_expected, true},
    {"derivative L2 bound", ComparisonPolicy::Derivative, vector_l2_failure, vector_expected, false},
    {"derivative maximum bound", ComparisonPolicy::Derivative, vector_max_failure, torch::ones({2}, torch::kFloat64), false},
    {"second moment relative inside", ComparisonPolicy::SecondMoment, scalar(1. + precision.derivative_inside), scalar(1.), true},
    {"second moment relative outside", ComparisonPolicy::SecondMoment, scalar(1. + precision.derivative_outside), scalar(1.), false},
    {"second moment absolute inside", ComparisonPolicy::SecondMoment, scalar(5e-13), scalar(0.), true},
    {"second moment absolute outside", ComparisonPolicy::SecondMoment, scalar(2e-12), scalar(0.), false},
    {"second moment global bound", ComparisonPolicy::SecondMoment, vector_inside, vector_expected, true},
    {"second moment L2 bound", ComparisonPolicy::SecondMoment, vector_l2_failure, vector_expected, false},
    {"second moment maximum bound", ComparisonPolicy::SecondMoment, vector_max_failure, torch::ones({2}, torch::kFloat64), false},
    {"hyperparameter inside", ComparisonPolicy::Hyperparameter, scalar(1. + 4 * epsilon), scalar(1.), true},
    {"hyperparameter outside", ComparisonPolicy::Hyperparameter, scalar(1. + 16 * epsilon), scalar(1.), false},
    {"hyperparameter zero", ComparisonPolicy::Hyperparameter, scalar(0.), scalar(0.), true},
    {"hyperparameter zero is exact", ComparisonPolicy::Hyperparameter, scalar(std::numeric_limits<double>::denorm_min()), scalar(0.), false}};
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
    {"undefined pair", {}, {}, true},
    {"undefined actual", {}, one, false},
    {"undefined expected", one, {}, false},
    {"dtype mismatch", one.to(torch::kFloat64), one, false},
    {"shape mismatch", one.reshape({1, 1}), one, false},
    {"empty pair", one.narrow(0, 0, 0), one.narrow(0, 0, 0), true},
    {"integer equal", age, age, true},
    {"integer exact beyond double precision", age + 1, age, false},
    {"boolean equal", one.to(torch::kBool), one.to(torch::kBool), true},
    {"boolean unequal", one.to(torch::kBool), torch::zeros({1}, torch::kBool), false},
    {"NaN pair", nan, nan, false},
    {"NaN mask mismatch", nan, one, false},
    {"infinity pair", infinity, infinity, false},
    {"infinity sign mismatch", infinity, -infinity, false},
    {"infinity finite mismatch", one, infinity, false},
    {"nonfinite category mismatch", nan, infinity, false}};
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
TEST_CASE("Actual pinned upstream Nano and SegNano forward and fixed cotangent VJPs", "[rfdetr][parity][upstream_oracle][cuda]") {
 const auto requested = rf::testsupport::UpstreamOracle::requested();
 if (!requested) SKIP("optional upstream evidence not requested; generate with --diagnose-rfdetr-upstream for independent acceptance");
 const rf::testsupport::UpstreamOracle oracle(*requested);
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 c10::cuda::CUDAGuard guard(0);
 mmltk::backend::ml::testsupport::FullMatrixPrecision precision;
 for (const auto& entry : oracle.cases()) {
  DYNAMIC_SECTION(entry.id) {
   CAPTURE(entry.id);
   if (entry.dtype() == torch::kBFloat16) REQUIRE(at::cuda::getCurrentDeviceProperties()->major >= 8);
   auto model = oracle.model(entry);
   const auto archive = oracle.archive(entry, 0);
   rf::testsupport::OracleComparison comparison(*requested, "core-" + entry.id, entry.dtype(), "actual upstream forward and fixed cotangent VJP; eager native production CUDA");
   auto input = archive.tensor("input", torch::kCUDA).clone().set_requires_grad(true);
   const auto mask = archive.tensor("input_mask", torch::kCUDA);
   std::vector<torch::Tensor> indices;
   rf::ModelOutputs outputs;
   {
    mmltk::backend::ml::cuda::TorchAutocastScope scope(entry.dtype() != torch::kFloat32, entry.dtype());
    comparison.fact("forward autocast", {{"enabled", at::autocast::is_autocast_enabled(at::kCUDA)}, {"dtype", static_cast<int>(at::autocast::get_autocast_dtype(at::kCUDA))}});
    outputs = model.forward_observed({input, mask}, model.config().segmentation, indices);
   }
   comparison.fact("execution", {{"mode", "eager"}, {"input", "identical admitted normalized tensor"}, {"training", entry.training}});
   rf::testsupport::compare_oracle_outputs(comparison, archive, outputs, indices);
   rf::testsupport::compare_oracle_vjp(comparison, archive, model, outputs, input, false);
  }
 }
}
