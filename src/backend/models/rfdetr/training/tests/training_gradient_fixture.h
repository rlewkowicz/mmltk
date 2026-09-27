#pragma once
#include "src/backend/models/rfdetr/training/detail/training_distributed.h"
namespace mmltk::backend::models::rfdetr {
class TrainingTargetCounts;
class TrainingMetricHandoff;
namespace testsupport {
struct TrainingDistributedTestAccess final {
 static DistributedContext backend(c10::intrusive_ptr<c10d::Backend>, int rank = 0, int device = 0);
 static c10::intrusive_ptr<c10d::Backend> backend(const DistributedContext&);
 static c10::intrusive_ptr<c10d::Store> store(const DistributedContext&);
 static std::weak_ptr<const void> custody(const DistributedContext&);
 static std::weak_ptr<const void> custody(const TrainingTargetCounts&);
 static std::weak_ptr<const void> custody(const TrainingMetricHandoff&);
 static void retire(TrainingTargetCounts&);
 static void retire(TrainingMetricHandoff&);
 static bool terminal(const TrainingTargetCounts&);
 static bool terminal(const TrainingMetricHandoff&);
};
void exercise_training_initialization(const DistributedContext&, int device);
void exercise_gradient_trajectory(const DistributedContext&, int device);
void exercise_early_bucket_overlap(const DistributedContext&, int device, bool abort_after_launch = false);
void exercise_bounded_gradient_buckets(int device);
void exercise_direct_gradients(int device);
void exercise_target_count_handoff(const DistributedContext&, int device);
void exercise_collective_custody(int device);
void exercise_collective_cancellation(DistributedContext&, int device, std::string_view operation);
}  // namespace testsupport
}  // namespace mmltk::backend::models::rfdetr
