#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "training_data_state.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/augmentation_plan.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augmentation_donor_index.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
namespace mmltk::backend::data {
class DatasetLoader;
struct DatasetIndexSchedule;
}
namespace mmltk::backend::models::rfdetr {
struct TrainingImageClasses final { std::vector<std::uint32_t> classes; };
struct TrainingRankSlice final { std::uint64_t begin = 0; std::uint64_t count = 0; };
struct TrainingEpochDraws final {
 std::shared_ptr<const mmltk::backend::data::DatasetIndexSchedule> schedule;
 std::vector<std::uint64_t> repeated_exposure;
 std::uint64_t unused_tail = 0;
 std::uint64_t microbatches = 0;
 std::uint64_t attempts = 0;
};
struct TrainingDonorSource final {
 GpuAugmentationDonor metadata;
 // Borrowed from the loader mapping; consume or copy before its owner retires.
 std::span<const mmltk::backend::data::RLEPair> support;
};
[[nodiscard]] TrainingDonorSource resolve_training_donor(const mmltk::backend::data::DatasetLoader&, const TrainingDonorDescriptor&);
class TrainingDataPlan final {
public:
 TrainingDataPlan(std::vector<TrainingImageClasses>, std::uint32_t classes, const TrainRequest&);
 explicit TrainingDataPlan(const mmltk::backend::data::DatasetLoader&, const TrainRequest&);
 [[nodiscard]] const std::vector<TrainingShard>& shards() const noexcept { return shards_; }
 [[nodiscard]] TrainingEpochDraws epoch(std::size_t model, std::uint64_t epoch) const;
 [[nodiscard]] std::shared_ptr<const mmltk::backend::data::DatasetIndexSchedule> rank_schedule(const TrainingEpochDraws&, std::uint32_t rank, std::uint32_t world) const;
 [[nodiscard]] TrainingRankSlice rank_slice(std::uint32_t rank, std::uint32_t world) const;
 [[nodiscard]] static TrainingRankSlice rank_slice(std::uint64_t batch, std::uint32_t rank, std::uint32_t world);
 [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
 [[nodiscard]] std::uint64_t microbatch_images() const noexcept { return batch_; }
private:
 std::vector<TrainingImageClasses> membership_;
 std::vector<std::uint64_t> support_;
 std::vector<TrainingShard> shards_;
 TrainDataPolicy policy_;
 std::uint64_t batch_;
 std::uint64_t contributions_;
 std::uint64_t hash_ = 0;
};
// Logical history owns descriptors; physical caches only materialize them.
class TrainingDonorHistory final {
public:
 TrainingDonorHistory(std::size_t streams, std::size_t global_batch);
 // The planned span stays valid until the next plan/admit on this owner.
 [[nodiscard]] std::span<const TrainingDonorDescriptor> plan(std::size_t stream, std::span<const std::uint64_t> image_keys, std::span<const std::uint32_t> images);
 [[nodiscard]] std::span<const TrainingDonorDescriptor> admit(const mmltk::backend::data::DatasetLoader&, std::size_t stream, std::span<const std::uint64_t> keys, std::span<const std::uint32_t> images, const GpuAugmentationConfig&);
 void replace(std::size_t stream, std::span<const TrainingDonorDescriptor>);
 void restore(const mmltk::backend::data::DatasetLoader&, std::span<const TrainingDonorDescriptor>);
 [[nodiscard]] const std::vector<TrainingDonorDescriptor>& state() const noexcept { return slots_; }
private:
 std::size_t batch_;
 std::vector<TrainingDonorDescriptor> slots_;
 std::vector<TrainingDonorDescriptor> planned_;
 std::vector<TrainingDonorDescriptor> replacements_;
 std::vector<GpuAugmentationDonor> metadata_;
 CachedAugmentationDonorIndex index_;
};
}
