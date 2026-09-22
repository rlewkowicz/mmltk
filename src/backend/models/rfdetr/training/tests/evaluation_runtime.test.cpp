#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include "src/backend/models/rfdetr/training/detail/evaluation_runtime.h"
using namespace mmltk::backend::models::rfdetr;
TEST_CASE("Evaluation encoding collection preserves image order", "[rfdetr][evaluation][settlement]") {
 PendingPredictionBatchEncoding pending;
 for (int image_id : {7, 3, 11}) {
  pending.images.push_back(std::async(std::launch::deferred, [image_id] {
   PredictionBatchItem image;
   image.image_id = image_id;
   return image;
  }));
 }
 const auto results = collect_prediction_batch_encoding(std::move(pending));
 REQUIRE(results.size() == 3U);
 CHECK(results[0].image_id == 7);
 CHECK(results[1].image_id == 3);
 CHECK(results[2].image_id == 11);
 for (const auto& future : pending.images) CHECK_FALSE(future.valid());
}
TEST_CASE("Evaluation encoding failure settles every sibling before propagating", "[rfdetr][evaluation][settlement]") {
 for (int failure_index : {0, 1, 2}) {
  PendingPredictionBatchEncoding pending;
  std::atomic<int> completed{0};
  auto lifetime = std::make_shared<int>(0);
  std::weak_ptr<int> borrowed_lifetime = lifetime;
  for (int index = 0; index < 3; ++index) {
   pending.images.push_back(std::async(std::launch::deferred, [&, index, lifetime]() mutable {
    auto retained = std::move(lifetime);
    ++completed;
    if (index == failure_index) throw std::runtime_error("first image failure");
    if (index > failure_index) throw std::runtime_error("later image failure");
    return PredictionBatchItem{};
   }));
  }
  lifetime.reset();
  CHECK_THROWS_WITH(collect_prediction_batch_encoding(std::move(pending)), "first image failure");
  CHECK(completed.load() == 3);
  CHECK(borrowed_lifetime.expired());
  for (const auto& future : pending.images) CHECK_FALSE(future.valid());
  CHECK(collect_prediction_batch_encoding(std::move(pending)).empty());
 }
}
TEST_CASE("Evaluation encoding collection joins a running sibling after a consumed failure", "[rfdetr][evaluation][settlement]") {
 PendingPredictionBatchEncoding pending;
 std::promise<PredictionBatchItem> failed;
 pending.images.push_back(failed.get_future());
 failed.set_exception(std::make_exception_ptr(std::runtime_error("already observed")));
 CHECK_THROWS_WITH(pending.images.front().get(), "already observed");
 std::promise<void> entered;
 std::promise<void> release;
 auto released = release.get_future();
 std::atomic<bool> completed{false};
 pending.images.push_back(std::async(std::launch::async, [&] {
  entered.set_value();
  released.get();
  completed = true;
  PredictionBatchItem image;
  image.image_id = 19;
  return image;
 }));
 entered.get_future().get();
 auto collection = std::async(std::launch::async, [&] { return collect_prediction_batch_encoding(std::move(pending)); });
 release.set_value();
 const auto results = collection.get();
 CHECK(completed.load());
 REQUIRE(results.size() == 1U);
 CHECK(results.front().image_id == 19);
 for (const auto& future : pending.images) CHECK_FALSE(future.valid());
}
TEST_CASE("Evaluation staging preserves categories beyond the per-category evaluator cap", "[rfdetr][evaluation][gpu]") {
 const auto device = torch::Device(torch::kCUDA, 0);
 // Bind the driver context even when earlier cases initialized LibTorch's stream pool.
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 const auto stream = c10::cuda::getStreamFromPool(false, 0);
 const c10::cuda::CUDAStreamGuard stream_guard(stream);
 auto slots = std::make_shared<PredictionBufferSlotPool>(1, PredictionBufferConfig{1, 3, std::nullopt, 0});
 auto lease = slots->acquire();
 lease.buffers->images.push_back({0, 17, {}});
 PostprocessedBatch batch{torch::tensor({.9F, .8F, .7F}).view({1, 3}).to(device), torch::tensor({0L, 1L, -1L}, torch::kInt64).view({1, 3}).to(device),
  torch::tensor({0.F, 0.F, 2.F, 2.F, 3.F, 3.F, 5.F, 5.F, 0.F, 0.F, 0.F, 0.F}).view({1, 3, 4}).to(device), std::nullopt, torch::tensor({2L}, torch::kInt64).to(device)};
 auto staged = stage_prediction_batch(std::move(batch), 2, 1, std::move(lease), 0, stream.stream());
 mmltk::common::concurrency::WorkerPool workers(1);
 const auto images = collect_prediction_batch_encoding(enqueue_prediction_batch_encoding(workers, std::move(staged)));
 REQUIRE(images.size() == 1);
 REQUIRE(images[0].predictions.size() == 2);
 CHECK(images[0].predictions[0].class_reference == 0);
 CHECK(images[0].predictions[1].class_reference == 1);
}
