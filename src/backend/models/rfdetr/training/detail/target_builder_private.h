#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/augmentation/augmentation_plan.h"
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] std::int64_t packed_mask_words_for_shape(int height, int width) noexcept;
void pack_compiled_rle_pairs(std::span<const mmltk::backend::data::RLEPair> pairs, std::span<std::int64_t> words);
struct BatchStaticTensors {
 torch::Tensor sizes;
 torch::Tensor nested_mask;
 int device_id = -1;
 int image_height = 0;
 int image_width = 0;
 int64_t batch_capacity = 0;
 void ensure(int64_t batch_size, int height, int width, int target_device_id);
 [[nodiscard]] torch::Tensor sizes_view(int64_t batch_size) const;
 [[nodiscard]] torch::Tensor nested_mask_view(int64_t batch_size) const;
};
struct TargetStagingSlot {
 torch::Tensor boxes;
 torch::Tensor labels;
 torch::Tensor area;
 torch::Tensor iscrowd;
 torch::Tensor inverse_transforms;
 torch::Tensor occluder_mask_indices;
 torch::Tensor occluder_inverse_transforms;
 torch::Tensor erasure;
 torch::Tensor packed_masks;
 torch::Tensor image_ids;
 torch::Tensor offsets;
 torch::Tensor counts;
 std::uintptr_t copy_complete_event = 0U;
 int64_t batch_capacity = 0;
 int64_t instance_capacity = 0;
 int mask_height = 0;
 int mask_width = 0;
 int64_t mask_words_per_instance = 0;
 bool copy_pending = false;
};
// CLEANUP-IGNORE: TargetScratch is mutable GPU-build storage, not a duplicate of the immutable preset catalog row.
struct TargetScratch {
 // CLEANUP-IGNORE: Scratch construction is intentionally trivial while every tensor retains distinct storage
 // meaning.
 explicit TargetScratch(std::size_t staging_depth = 1);
 BatchStaticTensors batch;
 torch::Tensor offsets_gpu;
 torch::Tensor counts_gpu;
 torch::Tensor target_indices_gpu;
 std::vector<int64_t> offsets;
 std::vector<int64_t> counts;
 int device_id = -1;
 int image_height = 0;
 int image_width = 0;
 int64_t instance_capacity = 0;
 int mask_height = 0;
 int mask_width = 0;
 int64_t mask_words_per_instance = 0;
 ~TargetScratch() noexcept;
 TargetScratch(const TargetScratch&) = delete;
 TargetScratch& operator=(const TargetScratch&) = delete;
 TargetScratch(TargetScratch&&) = delete;
 TargetScratch& operator=(TargetScratch&&) = delete;
 void ensure_batch(size_t batch_size, int height, int width, int target_device_id);
 void ensure_instance_capacity(int64_t instances);
 void ensure_packed_mask_capacity(int64_t instances, int height, int width);
 void ensure_copy_resources(int target_device_id);
 TargetStagingSlot& acquire_staging_slot(std::size_t batch_size, int64_t instances, bool include_masks, int height, int width);
 void record_staging_copy_on_stream(std::uintptr_t stream);
 void wait_for_pending_copy();
 void handoff_pending_copy_to_current_stream(int target_device_id) const;
 void wait_for_pending_copy_on_stream(std::uintptr_t stream) const;
 void record_pending_copy_on_stream(std::uintptr_t stream);
 void retire_consumers_on_current_stream(int target_device_id);
 void retire_consumer_on_stream(std::uintptr_t stream);
 [[nodiscard]] inline std::uintptr_t copy_stream_handle() const noexcept { return copy_stream_; }

private:
 void release_copy_resources();
 std::vector<TargetStagingSlot> staging_slots_;
 std::size_t next_staging_slot_ = 0;
 std::size_t active_staging_slot_ = 0;
 bool staging_slot_acquired_ = false;
 int copy_stream_device_id_ = -1;
 std::uintptr_t copy_stream_ = 0U;
 std::uintptr_t copy_complete_event_ = 0U;
 std::uintptr_t consumers_retired_event_ = 0U;
 bool copy_pending_ = false;
 bool consumers_pending_ = false;
};
class TargetConsumerLease final {
public:
 TargetConsumerLease(TargetScratch& scratch, const PreparedTargets& targets, int device_id);
 ~TargetConsumerLease() noexcept;
 TargetConsumerLease(const TargetConsumerLease&) = delete;
 TargetConsumerLease& operator=(const TargetConsumerLease&) = delete;
 void handoff();
 void retire();

private:
 TargetScratch* scratch_;
 int device_id_;
 bool handed_off_ = false;
};
class LoaderBatchGuard {
public:
 LoaderBatchGuard(mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch, int device_id);
 ~LoaderBatchGuard() noexcept;
 LoaderBatchGuard(const LoaderBatchGuard&) = delete;
 LoaderBatchGuard& operator=(const LoaderBatchGuard&) = delete;
 inline void set_consumer_stream(void* stream) noexcept { consumer_stream_ = stream; }
 void release();

private:
 mmltk::backend::data::DatasetLoader* loader_ = nullptr;
 mmltk::backend::data::Batch batch_{};
 int device_id_ = 0;
 void* consumer_stream_ = nullptr;
};
torch::Tensor make_size_tensor(int64_t batch_size, int64_t image_height, int64_t image_width, const torch::Device& device);
torch::Tensor make_device_batch_tensor(const mmltk::backend::data::Batch& batch, int device_id, int64_t image_height, int64_t image_width);
void validate_feature_active_target(int64_t label, std::span<const float, 4> normalized_cxcywh, int64_t object_classes);
PreparedTargets build_targets(const mmltk::backend::data::Batch& batch, int image_height, int image_width, bool include_masks, bool require_masks, int device_id, TargetScratch& scratch,
 std::string_view split, int64_t resolved_query_count, const TrainingSupervisionConfig& supervision, int64_t object_classes, AugmentationBatchPlan* augmentation_plan = nullptr);
}  // namespace mmltk::backend::models::rfdetr
