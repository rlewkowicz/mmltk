#include "src/backend/models/rfdetr/core/inference_lanes.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <cuda_runtime_api.h>
#include <array>
#include <semaphore>
#include <set>
namespace {
namespace rf = mmltk::backend::models::rfdetr;
struct DelayedForward final {
 std::binary_semaphore release{0}, finished{0};
 bool enqueued = false;
 static void CUDART_CB Run(void* value) {
  auto& gate = *static_cast<DelayedForward*>(value);
  gate.release.acquire();
  gate.finished.release();
 }
 void Open() {
  if (!enqueued) return;
  release.release();
  finished.acquire();
  enqueued = false;
 }
 ~DelayedForward() { Open(); }
};
TEST_CASE("Inference lanes retain stable distinct streams and bounded ordered slots", "[rfdetr][inference][gpu]") {
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 for (const std::size_t capacity : {1U, 4U}) {
  rf::InferenceLanes lanes(0, capacity);
  std::set<std::uintptr_t> streams;
  for (std::size_t index = 0; index < capacity; ++index) streams.insert(lanes.stream(index).native_handle);
  REQUIRE(streams.size() == capacity);
  for (int round = 0; round < 3; ++round) {
   for (std::size_t index = 0; index < capacity; ++index) {
    CHECK(lanes.Admit() == index);
    lanes.Submitted(index);
   }
   CHECK(lanes.pending() == capacity);
   CHECK_THROWS_AS(lanes.Admit(), std::logic_error);
   for (std::size_t index = 0; index < capacity; ++index) {
    CHECK(lanes.WaitOldest() == index);
    lanes.ReleaseOldest();
    CHECK(lanes.capacity() == capacity);
   }
  }
  CHECK(lanes.Drain() == 0);
  CHECK(lanes.Close() == 0);
  CHECK(lanes.Close() == 0);
 }
 CHECK_THROWS_AS(rf::InferenceLanes(0, 0), std::invalid_argument);
 CHECK_THROWS_AS(rf::InferenceLanes(-1, 2), std::invalid_argument);
}
TEST_CASE("Inference source copies retire before delayed forward and next frame mutation", "[rfdetr][inference][gpu][custody]") {
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 const mmltk::testsupport::ScopedTestStream producer;
 const auto device = torch::TensorOptions().device(torch::kCUDA).dtype(torch::kInt32);
 auto source = torch::empty({1}, device);
 auto copies = torch::empty({2}, device);
 REQUIRE(cudaDeviceSynchronize() == cudaSuccess);
 rf::InferenceLanes lanes(0, 2);
 DelayedForward delayed;  // Opens before stream retirement, even on assertion failure.
 const mmltk::backend::ml::runtime::BorrowedCommandStream decode{reinterpret_cast<std::uintptr_t>(producer.get()), true};
 for (std::size_t index = 0; index < 2; ++index) {
  REQUIRE(lanes.Admit() == index);
  REQUIRE(cudaMemsetAsync(source.data_ptr(), static_cast<int>(index + 1), sizeof(int), producer.get()) == cudaSuccess);
  lanes.WaitSource(index, decode);
  const auto stream = reinterpret_cast<cudaStream_t>(lanes.stream(index).native_handle);
  REQUIRE(cudaMemcpyAsync(copies.data_ptr<int>() + index, source.data_ptr(), sizeof(int), cudaMemcpyDeviceToDevice, stream) == cudaSuccess);
  lanes.ReleaseSource(index, decode);
  if (index == 0) {
   REQUIRE(cudaLaunchHostFunc(stream, &DelayedForward::Run, &delayed) == cudaSuccess);
   delayed.enqueued = true;
  }
  lanes.Submitted(index);
 }
 // The second physical stream completes while the oldest forward is blocked.
 REQUIRE(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(lanes.stream(1).native_handle)) == cudaSuccess);
 CHECK(cudaStreamQuery(reinterpret_cast<cudaStream_t>(lanes.stream(0).native_handle)) == cudaErrorNotReady);
 delayed.Open();
 const auto values = copies.to(torch::kCPU);
 CHECK(values[0].item<int>() == 0x01010101);
 CHECK(values[1].item<int>() == 0x02020202);
 for (std::size_t index = 0; index < 2; ++index) {
  CHECK(lanes.WaitOldest() == index);
  lanes.ReleaseOldest();
 }
 // Partial admission has physical work but no forward-completion record.
 const auto partial = lanes.Admit();
 REQUIRE(cudaMemsetAsync(source.data_ptr(), 0, sizeof(int), reinterpret_cast<cudaStream_t>(lanes.stream(partial).native_handle)) == cudaSuccess);
 CHECK(lanes.Drain() == 0);
 CHECK(lanes.pending() == 0);
}
}  // namespace
