#pragma once
#include <cstdint>
#include <limits>
#include <vector>
#include "src/backend/data/compiled_format_limits.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingShard final {
 std::uint64_t model_id = 0;
 std::uint64_t seed = 42;
 [[= mmltk::frameworks::reflection::MaxItems{std::numeric_limits<std::uint32_t>::max()}]] std::vector<std::uint32_t> images;
 [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::data::MAX_CLASSES}]] std::vector<std::uint64_t> unique_support;
 [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::data::MAX_CLASSES}]] std::vector<std::uint32_t> missing_classes;
 bool operator==(const TrainingShard&) const = default;
};
struct TrainingDonorDescriptor final {
 std::uint32_t image_index = 0;
 std::uint32_t annotation_index = 0;
 bool valid = false;
 bool operator==(const TrainingDonorDescriptor&) const = default;
};
// A complete window cannot exceed the compiled image inventory times the
// maximum admitted draw multiplier. This is a format bound, not a worker limit.
inline constexpr std::uint64_t kMaximumTrainingDonorSlots = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} * 16U;
struct TrainingDataContinuation final {
 std::uint64_t model_id = 0;
 std::uint64_t plan_hash = 0;
 std::uint64_t epoch = 0;
 std::uint64_t next_microbatch = 0;
 [[= mmltk::frameworks::reflection::MaxItems{kMaximumTrainingDonorSlots}]] std::vector<TrainingDonorDescriptor> donors;
 bool operator==(const TrainingDataContinuation&) const = default;
};
struct TrainingPlanState final {
 std::uint64_t plan_hash = 0;
 [[= mmltk::frameworks::reflection::MaxItems{kMaximumTrainingModels}]] std::vector<TrainingShard> shards;
 bool operator==(const TrainingPlanState&) const = default;
};
MMLTK_REFLECT_FIELDS(TrainingShard)
MMLTK_REFLECT_FIELDS(TrainingDonorDescriptor)
MMLTK_REFLECT_FIELDS(TrainingDataContinuation)
MMLTK_REFLECT_FIELDS(TrainingPlanState)
}  // namespace mmltk::backend::models::rfdetr
