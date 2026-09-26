#include "src/backend/ml/torch/tests/catch_support.h"
#include "training_gradient_fixture.h"
#include "src/backend/models/rfdetr/training/detail/training_ops_private.h"
#include "src/backend/models/rfdetr/training/detail/training_gradient_reducer.h"
#include "src/backend/models/rfdetr/training/detail/training_step.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <future>
#include <catch2/matchers/catch_matchers.hpp>
namespace rf = mmltk::backend::models::rfdetr;
TEST_CASE("Gradient attempts preserve logical objectives across physical capacity", "[rfdetr][training][gradient]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; reducer coverage unverified");
 const rf::DistributedContext group;
 REQUIRE_NOTHROW(rf::testsupport::exercise_training_initialization(group, 0));
 REQUIRE_NOTHROW(rf::testsupport::exercise_gradient_trajectory(group, 0));
 REQUIRE_NOTHROW(rf::testsupport::exercise_bounded_gradient_buckets(0));
}
TEST_CASE("Early gradient bucket launches before a held late backward returns", "[rfdetr][training][gradient][overlap]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; overlap coverage unverified");
 const rf::DistributedContext group;
 REQUIRE_NOTHROW(rf::testsupport::exercise_early_bucket_overlap(group, 0));
}
TEST_CASE("Gradient cancellation seals admission and wakes count consumers", "[rfdetr][training][gradient][cancel]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; cancellation coverage unverified");
 namespace tc = mmltk::backend::ml::cuda;
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(0));
 const rf::DistributedContext group;
 auto parameter = torch::ones({2}, torch::TensorOptions().device(tc::cuda_device(0)).requires_grad(true));
 rf::TrainingGradientReducer reducer(group, 0, tc::getCurrentCUDAStream(0), {"parameter"}, {parameter}, {{parameter}});
 rf::TrainingTargetCounts counts(1, 0, group);
 reducer.begin_attempt(2);
 reducer.arm(0);
 static_cast<void>(rf::TrainingStep(2, 1, false, at::kFloat).gradients(parameter.square().sum(), {parameter}));
 reducer.collect(0);
 auto waiting = std::async(std::launch::async, [&] {
  tc::TorchCudaDeviceGuard device(tc::checked_device_index(0));
  try {
   static_cast<void>(counts.consume(0, tc::getCurrentCUDAStream(0).stream()));
  } catch (const std::runtime_error& error) { return std::string(error.what()); }
  return std::string{};
 });
 const auto failure = std::make_exception_ptr(std::runtime_error("first training failure"));
 counts.fail(failure);
 reducer.abort(failure);
 CHECK(waiting.get() == "first training failure");
 CHECK_THROWS_WITH(reducer.contribute_empty(), "first training failure");
 CHECK_THROWS_WITH(reducer.begin_attempt(1), "first training failure");
 CHECK_FALSE(parameter.grad().defined());
}
TEST_CASE("Failure after an early bucket retains custody until retirement", "[rfdetr][training][gradient][cancel]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; early failure custody unverified");
 const rf::DistributedContext group;
 REQUIRE_THROWS_WITH(rf::testsupport::exercise_early_bucket_overlap(group, 0, true), "injected failure after early gradient bucket");
}
TEST_CASE("Collective slots retain uncertain physical work and reclaim settled buffers", "[rfdetr][training][gradient][custody]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("CUDA unavailable; collective custody unverified");
 REQUIRE_NOTHROW(rf::testsupport::exercise_collective_custody(0));
}
