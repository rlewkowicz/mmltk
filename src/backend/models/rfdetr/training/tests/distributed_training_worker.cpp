#include "training_gradient_fixture.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
int main(int argc, char** argv) {
 using namespace mmltk::backend::models::rfdetr;
 DistributedContext group;
 try {
  if (argc != 5) throw std::invalid_argument("distributed training helper requires store, rank, selected device ordinal, case");
  group.rank = std::stoi(argv[2]); group.world_size = 2; group.enabled = true;
  const int device = std::stoi(argv[3]); group.device_id = device;
  mmltk::backend::ml::cuda::TorchCudaDeviceGuard guard(mmltk::backend::ml::cuda::checked_device_index(device));
#if defined(USE_C10D_NCCL)
  group.store = c10::make_intrusive<c10d::FileStore>(argv[1], group.world_size);
  auto options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>(); options->timeout = std::chrono::seconds(45);
  group.process_group = c10::make_intrusive<c10d::ProcessGroupNCCL>(group.store, group.rank, group.world_size, options);
  group.process_group->setBoundDeviceId(mmltk::backend::ml::cuda::cuda_device(device));
  agree_training_topology(group, device);
  const auto precision = agree_training_precision(group, device, true, true);
  const std::array<std::uint8_t, 2> precision_signature{static_cast<std::uint8_t>(precision.autocast_dtype), static_cast<std::uint8_t>(precision.fused_optimizer)};
  distributed_agree(group, "precision", precision_signature);
  const std::array<std::uint8_t, 3> signature{3, 2, 1}; distributed_agree(group, "fixture", signature);
  if (std::string_view(argv[4]) == "cancel") {
   auto ready = torch::ones({1}, torch::TensorOptions().device(mmltk::backend::ml::cuda::cuda_device(device)).dtype(torch::kInt32));
   distributed_all_reduce_tensor(group, ready);
   mmltk::backend::ml::cuda::getCurrentCUDAStream(mmltk::backend::ml::cuda::checked_device_index(device)).synchronize();
   if (group.rank == 0) throw std::runtime_error("injected training cancellation");
   // The peer's first failure aborts the communicator while this rank is
   // waiting on the next real collective. It must leave with a fatal result.
   distributed_all_reduce_tensor(group, ready);
   mmltk::backend::ml::cuda::getCurrentCUDAStream(mmltk::backend::ml::cuda::checked_device_index(device)).synchronize();
   throw std::runtime_error("cancelled peer unexpectedly completed its collective");
  }
  if (std::string_view(argv[4]) == "early-failure") testsupport::exercise_early_bucket_overlap(group, device, true);
  testsupport::exercise_training_initialization(group, device);
  testsupport::exercise_early_bucket_overlap(group, device);
  testsupport::exercise_gradient_trajectory(group, device);
  group.process_group->shutdown(); group.process_group.reset(); group.store.reset();
  return 0;
#else
  throw std::runtime_error("two-process training coverage requires NCCL");
#endif
 } catch (const std::exception& error) {
  std::fprintf(stderr, "distributed training rank %d failed: %s\n", group.rank, error.what());
  distributed_abort(group); return 1;
 }
}
