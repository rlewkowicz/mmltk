#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <ATen/record_function.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace rf = mmltk::backend::models::rfdetr;
namespace {
using ComparisonPolicy = rf::testsupport::OracleComparisonPolicy;
using Json = nlohmann::json;
class ForwardOperatorObservation final {
 struct Event final : at::ObserverContext {
  std::string name;
  explicit Event(std::string value) : name(std::move(value)) {}
 };
 inline static thread_local ForwardOperatorObservation* active_ = nullptr;
 at::CallbackHandle callback_ = 0;
 std::map<std::string, std::size_t> counts_;
 rf::TensorMap outputs_;
 std::size_t depth_ = 0;
 bool finished_ = false;

public:
 explicit ForwardOperatorObservation(bool enabled) {
  if (!enabled) return;
  TORCH_CHECK(!active_, "forward operator observation is already active");
  callback_ = at::addThreadLocalCallback(at::RecordFunctionCallback(
   [](const at::RecordFunction& event) -> std::unique_ptr<at::ObserverContext> {
   const std::string_view name(event.name());
   if (name == "aten::topk") active_->finished_ = true;
   if (active_->finished_) return {};
   if (name != "aten::conv2d" && name != "aten::layer_norm" && name != "aten::linear" && name != "aten::gelu" && name != "aten::scaled_dot_product_attention") return {};
   ++active_->depth_;
   return std::make_unique<Event>(std::string(name.substr(6)));
  },
   [](const at::RecordFunction& event, at::ObserverContext* context) {
   if (!context) return;
   if (--active_->depth_) return;
   const auto& name = static_cast<Event*>(context)->name;
   if (active_->outputs_.size() >= 256 || event.outputs().size() != 1 || !event.outputs().front().isTensor()) {
    active_->finished_ = true;
    return;
   }
   const auto index = active_->counts_[name]++;
   active_->outputs_.emplace("forward-operator/" + name + "/" + std::to_string(index), event.outputs().front().toTensor().detach().clone());
  })
    .needsOutputs(true)
    .scopes({at::RecordScope::FUNCTION}));
  active_ = this;
 }
 ~ForwardOperatorObservation() {
  if (!callback_) return;
  at::removeCallback(callback_);
  active_ = nullptr;
 }
 void report(rf::testsupport::OracleComparison& comparison, const rf::testsupport::OracleArchive& archive) const {
  for (const auto& [name, value] : outputs_)
   if (archive.contains(name)) comparison.compare(name, value, archive.tensor(name), ComparisonPolicy::Value, true);
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
   comparison.fact("SDPA comparison backend", entry.math_sdpa ? "math" : "default");
   auto input = archive.tensor("input", torch::kCUDA).clone().set_requires_grad(true);
   const auto mask = archive.tensor("input_mask", torch::kCUDA);
   std::vector<torch::Tensor> indices;
   rf::ModelOutputs outputs;
   {
    ForwardOperatorObservation operators(archive.contains("forward-operator/linear/0"));
    mmltk::backend::ml::cuda::TorchAutocastScope scope(entry.dtype() != torch::kFloat32, entry.dtype());
    comparison.fact("forward autocast", {{"enabled", at::autocast::is_autocast_enabled(at::kCUDA)}, {"dtype", static_cast<int>(at::autocast::get_autocast_dtype(at::kCUDA))}});
    outputs = model.forward_observed({input, mask}, model.config().segmentation, indices);
    operators.report(comparison, archive);
   }
   comparison.fact("execution", {{"mode", "eager"}, {"input", "identical admitted normalized tensor"}, {"training", entry.training}});
   comparison.compare_all([&] {
    rf::testsupport::compare_oracle_outputs(comparison, archive, outputs, indices);
    rf::testsupport::compare_oracle_vjp(comparison, archive, model, outputs, input, false);
   });
  }
 }
}
TEST_CASE("Independent upstream repetitions measure the numerical noise floor", "[rfdetr][parity][upstream_repeat]") {
 const auto requested = rf::testsupport::UpstreamOracle::requested();
 const auto* repeated = std::getenv("MMLTK_RFDETR_UPSTREAM_REPEAT_DIR");
 if (!requested || !repeated) SKIP("two independent upstream artifact generations were not requested");
 const rf::testsupport::UpstreamOracle oracle(*requested), repeat(repeated);
 for (const auto& entry : oracle.cases()) {
  DYNAMIC_SECTION(entry.id) {
   const auto found = std::ranges::find_if(repeat.cases(), [&](const auto& value) { return value.id == entry.id; });
   REQUIRE(found != repeat.cases().end());
   rf::testsupport::OracleComparison comparison(*requested, "repeat-" + entry.id, entry.dtype(), "two independent actual upstream executions; no native model execution");
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
      for (const auto& name : expected.names(prefix)) {
       const auto key = std::string(prefix) + name;
       comparison.compare(key, actual.tensor(key), expected.tensor(key), policy);
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
 if (!requested) SKIP("optional upstream evidence not requested; generate with --diagnose-rfdetr-upstream for independent resize acceptance");
 const rf::testsupport::UpstreamOracle oracle(*requested);
 const auto archive = oracle.resize_archive();
 rf::testsupport::OracleComparison comparison(*requested, "rgb-resampling", torch::kFloat32, "actual torchvision float RGB bilinear align_corners=false; PIL RGB8 dataset path recorded separately");
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
