#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <c10/util/intrusive_ptr.h>
#include <torch/types.h>
#include <cuda_runtime_api.h>
namespace c10d { class Backend; class Store; }
namespace mmltk::backend::models::rfdetr {
class NativeRfDetrModel;
struct TrainRequest;
struct TrainingFailure;
namespace detail { struct TrainingContinuationValues; }
namespace testsupport { struct TrainingDistributedTestAccess; }
// Copies retain the same selected-device transport, including terminal custody.
struct DistributedContext final {
 DistributedContext();
 ~DistributedContext();
 DistributedContext(const DistributedContext&);
 DistributedContext& operator=(const DistributedContext&);
 DistributedContext(DistributedContext&&) noexcept;
 DistributedContext& operator=(DistributedContext&&) noexcept;
 bool enabled = false;
 int rank = 0;
 int world_size = 1;
 int device_id = 0;
private:
 struct Transport;
 std::shared_ptr<Transport> transport_;
 static DistributedContext from_backend(int rank, int world, int device, c10::intrusive_ptr<c10d::Backend>);
 [[nodiscard]] c10::intrusive_ptr<c10d::Store> store() const;
 [[nodiscard]] c10::intrusive_ptr<c10d::Backend> backend() const;
 friend struct testsupport::TrainingDistributedTestAccess;
 friend class TrainingCollectiveWork;
 friend DistributedContext make_distributed_context(const std::filesystem::path&, int, int, int, std::optional<std::chrono::milliseconds>);
 friend void distributed_shutdown(const DistributedContext&);
 friend void distributed_abort(const DistributedContext&) noexcept;
 friend void agree_training_topology(const DistributedContext&, int);
 friend TrainingFailure claim_training_failure(const DistributedContext&, const TrainingFailure&);
};
[[nodiscard]] DistributedContext make_distributed_context(const TrainRequest&);
[[nodiscard]] DistributedContext make_distributed_context(const std::filesystem::path& store, int rank, int world, int device, std::optional<std::chrono::milliseconds> timeout = {});
void distributed_shutdown(const DistributedContext&);
void distributed_abort(const DistributedContext&) noexcept;
// Failure settlement uses the already-owned rendezvous store after ordinary
// turns fail. The first concrete cause is claimed before peer cancellation.
[[nodiscard]] TrainingFailure claim_training_failure(const DistributedContext&, const TrainingFailure&);
// An operation owns a fixed number of submissions. join() orders the launch
// stream only. settle() proves physical completion at a drained boundary and
// reuses its slots/event. Unjoined, aborted, or failed work stays in terminal
// CUDA custody; even a submission throwing before it returns Work is retained.
class TrainingCollectiveWork final {
public:
 explicit TrainingCollectiveWork(int device, std::size_t capacity = 1);
 ~TrainingCollectiveWork();
 TrainingCollectiveWork(TrainingCollectiveWork&&) noexcept;
 TrainingCollectiveWork& operator=(TrainingCollectiveWork&&) noexcept;
 TrainingCollectiveWork(const TrainingCollectiveWork&) = delete;
 TrainingCollectiveWork& operator=(const TrainingCollectiveWork&) = delete;
 [[nodiscard]] std::size_t all_reduce(const DistributedContext&, const torch::Tensor&);
 [[nodiscard]] std::size_t broadcast(const DistributedContext&, const torch::Tensor&);
 void retain(std::span<const torch::Tensor> tensors);
 void join(std::size_t slot);
 // Fence only this operation before unrelated launch-stream work is queued.
 void record_completion();
 void settle();
 [[nodiscard]] cudaError_t retire() noexcept;
 [[nodiscard]] bool uncertain() const noexcept;
private:
 enum class Operation { Sum, Broadcast };
 std::size_t submit(const DistributedContext&, const torch::Tensor&, Operation);
 struct Owner;
 std::unique_ptr<Owner> owner_;
 friend struct testsupport::TrainingDistributedTestAccess;
};
void distributed_all_reduce_tensor(const DistributedContext&, const torch::Tensor&);
void distributed_barrier(const DistributedContext&);
void distributed_agree(const DistributedContext&, std::string_view turn, std::span<const std::uint8_t> signature);
void agree_training_text(const DistributedContext&, std::string_view turn, std::string_view value);
void agree_training_request(const DistributedContext&, const TrainRequest&);
void agree_training_continuation(const DistributedContext&, const detail::TrainingContinuationValues&);
struct TrainingPrecision final { at::ScalarType autocast_dtype = at::kFloat; bool fused_optimizer = false; };
[[nodiscard]] TrainingPrecision agree_training_precision(const DistributedContext&, int device, bool amp, bool fused);
void agree_training_topology(const DistributedContext&, int device);
void agree_model_inventory(const DistributedContext&, const NativeRfDetrModel&, std::string_view turn);
// Uses c10d's dtype/device-aware buckets, bounded application-owned flattened
// storage, and the same direct broadcast primitive as all other operations.
void broadcast_training_tensors(const DistributedContext&, const std::vector<torch::Tensor>&, std::size_t bucket_bytes = 4U * 1024U * 1024U);
void broadcast_training_model(const DistributedContext&, NativeRfDetrModel&);
}  // namespace mmltk::backend::models::rfdetr
