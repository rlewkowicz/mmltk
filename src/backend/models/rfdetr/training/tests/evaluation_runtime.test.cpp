#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include "src/backend/models/rfdetr/training/detail/evaluation_runtime.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <set>
using namespace mmltk::backend::models::rfdetr;
TEST_CASE("Training validation owns independent capacity and versioned immutable lane weights", "[rfdetr][evaluation][gpu]") {
 namespace data = mmltk::backend::data;
 const mmltk::testsupport::ScopedTempDir root("validation-forward-lanes");
 const data::testsupport::FixtureSpec fixture{.root_dir = root.path().string(), .width = 64, .height = 64, .num_images = 7, .background_images = 1};
 data::testsupport::create_synthetic_dataset(fixture);
 data::testsupport::compile_existing_fixture(fixture);
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 TrainRequest request;
 request.batch_size = request.val_batch_size = 1;
 request.lanes = 1;
 request.validation_lanes = 4;
 request.amp = false;
 request.progress_bar = false;
 request.compilation_mode = CompilationMode::kNone;
 auto runtime_config = resolve_runtime_config(12, 1, 2, {}, 0);
 runtime_config.workers = 12;
 RuntimeContext training(runtime_config);
 const data::DatasetLoader::Config loader_config{
  .compiled_path = data::testsupport::compiled_bin_path(fixture), .batch_size = 1, .shuffle = false, .prefetch_factor = 2, .gather_workers = 1, .loading = data::data_loading_options(true)
 };
 TrainingValidationRuntime validation(request, training, std::make_unique<data::DatasetLoader>(loader_config), 1, true, EvaluationMetricSet::BBox, 3, "validation", false);
 REQUIRE(training.split().lane_threads == 1);
 REQUIRE(validation.lane_capacity() == 4);
 CHECK(validation.execution_facts().configured_capacity == 4);
 CHECK(validation.execution_facts().admitted_capacity == 4);
 auto config = native_config_from_preset(model_presets().front());
 config.resolution = 64;
 config.num_classes = 7;
 config.num_queries = config.num_select = 3;
 NativeRfDetrModel master(config, native_training_class_layout(data::catalog::ClassCatalog({"person", "ret", "scope", "iron_sight", "anchor_dot", "glint"})));
 master.to(torch::Device(torch::kCUDA, 0));
 master.eval();
 validation.bind_model(master, 1, EvaluatedWeights::Ordinary);
 std::set<std::uintptr_t> streams;
 torch::Tensor first;
 for (std::size_t lane = 0; lane < 4; ++lane) {
  CHECK(validation.admit_lane() == lane);
  streams.insert(validation.active_stream());
  auto value = validation.active_model().named_parameters()["class_embed.bias"];
  if (!first.defined()) first = value;
  CHECK(value.data_ptr() == first.data_ptr());
  CHECK(value.data_ptr() != master.named_parameters()["class_embed.bias"].data_ptr());
  validation.submitted();
 }
 CHECK(streams.size() == 4);
 validation.drain();
 validation.bind_model(master, 1, EvaluatedWeights::Ordinary);
 CHECK(validation.admit_lane() == 0);
 CHECK(validation.active_model().named_parameters()["class_embed.bias"].data_ptr() == first.data_ptr());
 validation.drain();
 {
  torch::NoGradGuard guard;
  master.named_parameters()["class_embed.bias"].add_(2);
 }
 validation.bind_model(master, 2, EvaluatedWeights::Ordinary);
 CHECK(validation.admit_lane() == 0);
 const auto changed = validation.active_model().named_parameters()["class_embed.bias"];
 CHECK(torch::allclose(changed, first + 2));
 validation.drain();
 validation.bind_model(master, 2, EvaluatedWeights::Ema);
 CHECK(validation.admit_lane() == 0);
 CHECK(validation.active_model().named_parameters()["class_embed.bias"].data_ptr() != changed.data_ptr());
 validation.drain();
 DetectionConfig detection;
 detection.num_classes = config.num_classes;
 detection.num_select = config.num_select;
 populate_default_detection_weight_dict(detection);
 TrainingMetricHandoff metrics(0);
 const auto evaluated = evaluate_model(request, validation, master, detection, true, EvaluationPurpose::SelectionValidation, EvaluatedWeights::Ordinary, std::nullopt, &metrics, 2);
 CHECK(evaluated.timing.images == 7);
 CHECK(validation.execution_facts().admitted_capacity == 4);
 request.validation_lanes = 1;
 TrainingValidationRuntime serial(request, training, std::make_unique<data::DatasetLoader>(loader_config), 1, true, EvaluationMetricSet::BBox, 3, "validation", false);
 const auto reference = evaluate_model(request, serial, master, detection, true, EvaluationPurpose::SelectionValidation, EvaluatedWeights::Ordinary, std::nullopt, &metrics, 2);
 CHECK(reference.timing.images == evaluated.timing.images);
 REQUIRE(evaluated.loss);
 REQUIRE(reference.loss);
 CHECK(std::isfinite(*evaluated.loss));
 CHECK(std::abs(*evaluated.loss - *reference.loss) <= 1e-5 * std::max(1.0, std::abs(*reference.loss)));
 CHECK(evaluated.summary.bbox.ap == reference.summary.bbox.ap);
}
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
 PostprocessedBatch batch{
  torch::tensor({.9F, .8F, .7F}).view({1, 3}).to(device), torch::tensor({0L, 1L, -1L}, torch::kInt64).view({1, 3}).to(device),
  torch::tensor({0.F, 0.F, 2.F, 2.F, 3.F, 3.F, 5.F, 5.F, 0.F, 0.F, 0.F, 0.F}).view({1, 3, 4}).to(device), std::nullopt, torch::tensor({2L}, torch::kInt64).to(device)
 };
 auto staged = stage_prediction_batch(std::move(batch), 2, 1, std::move(lease), 0, stream.stream());
 mmltk::common::concurrency::WorkerPool workers(1);
 const auto images = collect_prediction_batch_encoding(enqueue_prediction_batch_encoding(workers, std::move(staged)));
 REQUIRE(images.size() == 1);
 REQUIRE(images[0].predictions.size() == 2);
 CHECK(images[0].predictions[0].class_reference == 0);
 CHECK(images[0].predictions[1].class_reference == 1);
}
