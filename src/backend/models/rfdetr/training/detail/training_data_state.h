#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <cstdint>
#include <limits>
#include <vector>
#include "src/backend/data/compiled/compiled_format_limits.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingShard final {
 std::uint64_t model_id = 0;
 std::uint64_t seed = 42;
 MMLTK_MAX_ITEMS(std::numeric_limits<std::uint32_t>::max()) std::vector<std::uint32_t> images;
 MMLTK_MAX_ITEMS(mmltk::backend::data::MAX_CLASSES) std::vector<std::uint64_t> unique_support;
 MMLTK_MAX_ITEMS(mmltk::backend::data::MAX_CLASSES) std::vector<std::uint32_t> missing_classes {};
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
 MMLTK_MAX_ITEMS(kMaximumTrainingDonorSlots) std::vector<TrainingDonorDescriptor> donors;
 bool operator==(const TrainingDataContinuation&) const = default;
};
struct TrainingPlanState final {
 std::uint64_t plan_hash = 0;
 MMLTK_MAX_ITEMS(kMaximumTrainingModels) std::vector<TrainingShard> shards;
 bool operator==(const TrainingPlanState&) const = default;
};
MMLTK_REFLECT_FIELDS(TrainingShard)
MMLTK_REFLECT_FIELDS(TrainingDonorDescriptor)
MMLTK_REFLECT_FIELDS(TrainingDataContinuation)
MMLTK_REFLECT_FIELDS(TrainingPlanState)
}  // namespace mmltk::backend::models::rfdetr
