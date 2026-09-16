#pragma once
#include <vector>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::backend::models::rfdetr {
struct DistributedTrainingPartition {
    int rank = 0;
    int world_size = 1;
    int device_id = 0;
    int worker_budget = 0;
    int numa_node = -1;
};
void apply_training_partition(TrainRequest&, const DistributedTrainingPartition&);
// Training owns deterministic device/rank selection. Plan 15 owns command-line
// projection and Plan 10 owns process creation and request serialization.
[[nodiscard]] std::vector<DistributedTrainingPartition> select_distributed_training_partitions(const TrainRequest& request);
}  // namespace mmltk::backend::models::rfdetr
