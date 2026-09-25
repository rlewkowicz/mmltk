#pragma once
#include <sstream>
#include "src/backend/models/rfdetr/training/detail/training_continuation.h"
#include <string_view>
#include <torch/types.h>
#include <torch/serialize.h>
namespace mmltk::backend::models::rfdetr::testsupport {
inline detail::TrainingContinuationValues continuation_values(const TrainRequest& request, detail::TrainingContinuationValues values) {
 values.execution = derive_execution_facts(request, 0);
 const auto k = values.execution.microbatches_per_attempt;
 TrainingSchedule schedule(request.recipe, {request.recipe.lr}, {TrainingGroupRole::Ordinary}, request.epochs, k, k);
 schedule.begin_epoch(values.epoch);
 for (std::uint64_t i = 0; i < k; ++i) schedule.consume_microbatch();
 schedule.prepare_attempt(); schedule.finish_attempt(true);
 values.schedule = schedule.state();
 values.data.plan_hash = 1;
 values.data.epoch = values.epoch + 1;
 if (request.lane_configuration.mode == TrainLaneMode::SharedGradients)
  values.data.shards = {{0, static_cast<std::uint64_t>(request.seed), {0}, {1}, {}}};
 else
  for (const auto& model : request.lane_configuration.models) values.data.shards.push_back({model.model_id, model.seed, {0}, {1}, {}});
 values.data.donors.resize(checked_training_product(request.batch_size, request.lane_configuration.mode == TrainLaneMode::SharedGradients ? request.lanes : 1));
 return values;
}
// Preserve nested optimizer/state archives while changing one top-level fact.
inline void copy_checkpoint_archive(torch::serialize::InputArchive& source, torch::serialize::OutputArchive& destination, std::string_view omitted = {}) {
 for (const auto& key : source.keys()) {
  if (key == omitted) continue;
  torch::serialize::InputArchive child;
  if (source.try_read(key, child)) {
   torch::serialize::OutputArchive output;
   copy_checkpoint_archive(child, output);
   destination.write(key, output);
  } else {
   c10::IValue value;
   source.read(key, value);
   destination.write(key, value);
  }
 }
}
inline torch::serialize::InputArchive checkpoint_input(torch::serialize::OutputArchive& output) {
 std::stringstream bytes;
 output.save_to(bytes);
 torch::serialize::InputArchive input;
 input.load_from(bytes, torch::Device(torch::kCPU));
 return input;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
