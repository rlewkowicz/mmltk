#include "src/backend/models/rfdetr/training/training_partition.h"
#include <set>
namespace mmltk::backend::models::rfdetr {
void apply_training_partition(TrainRequest& request, const DistributedTrainingPartition& partition) {
 request.device_id = partition.device_id;
 request.workers = partition.worker_budget;
 request.numa_node = partition.numa_node;
 request.device_ids.clear();
 request.numa_nodes.clear();
}
std::vector<DistributedTrainingPartition> select_distributed_training_partitions(const TrainRequest& request) {
 validate_train_placement(request);
 if (request.device_ids.empty()) { return {{0, 1, request.device_id, request.workers, request.numa_node}}; }
 const std::set<int> unique_devices(request.device_ids.begin(), request.device_ids.end());
 if (unique_devices.size() != request.device_ids.size() || *unique_devices.begin() < 0) {
  throw std::invalid_argument("distributed RF-DETR device identifiers must be unique and nonnegative");
 }
 const int world_size = static_cast<int>(request.device_ids.size());
 std::vector<DistributedTrainingPartition> partitions;
 partitions.reserve(request.device_ids.size());
 const int base_workers = request.workers > 0 ? request.workers / world_size : 0;
 const int extra_workers = request.workers > 0 ? request.workers % world_size : 0;
 for (int rank = 0; rank < world_size; ++rank) {
  partitions.push_back({
   rank,
   world_size,
   request.device_ids[static_cast<std::size_t>(rank)],
   base_workers + (rank < extra_workers ? 1 : 0),
   request.numa_nodes.empty() ? request.numa_node : request.numa_nodes[static_cast<std::size_t>(rank)],
  });
 }
 return partitions;
}
}  // namespace mmltk::backend::models::rfdetr
