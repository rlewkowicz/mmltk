#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/ml/torch/tests/tensor_fixture.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <nlohmann/json.hpp>
namespace rf = mmltk::backend::models::rfdetr;
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
