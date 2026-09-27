#include "detail/training_distributed.h"
#include "detail/training_continuation.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/common/io/file_digest.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include "src/backend/models/rfdetr/contract/training_artifacts.h"
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAEvent.h>
#include <torch/csrc/distributed/c10d/Backend.hpp>
#include <torch/csrc/distributed/c10d/Store.hpp>
#include <torch/csrc/distributed/c10d/reducer.hpp>
#include <torch/csrc/utils/tensor_flatten.h>
#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace tc = mmltk::backend::ml::cuda;
namespace gpu = mmltk::frameworks::gpu;
struct DistributedContext::Transport final {
 c10::intrusive_ptr<c10d::Store> store;
 c10::intrusive_ptr<c10d::Backend> backend;
 std::atomic<bool> aborted{false};
 std::atomic<bool> shutdown{false};
};
DistributedContext::DistributedContext() = default;
DistributedContext::~DistributedContext() = default;
DistributedContext::DistributedContext(const DistributedContext&) = default;
DistributedContext& DistributedContext::operator=(const DistributedContext&) = default;
DistributedContext::DistributedContext(DistributedContext&&) noexcept = default;
DistributedContext& DistributedContext::operator=(DistributedContext&&) noexcept = default;
DistributedContext DistributedContext::from_backend(int rank, int world, int device, c10::intrusive_ptr<c10d::Backend> backend) {
 DistributedContext result;
 result.enabled = true;
 result.rank = rank;
 result.world_size = world;
 result.device_id = device;
 result.transport_ = std::make_shared<Transport>();
 result.transport_->backend = std::move(backend);
 return result;
}
c10::intrusive_ptr<c10d::Backend> DistributedContext::backend() const { return transport_->backend; }
c10::intrusive_ptr<c10d::Store> DistributedContext::store() const { return transport_->store; }
DistributedContext make_distributed_context(const std::filesystem::path& path, int rank, int world, int device, std::optional<std::chrono::milliseconds> timeout) {
 if (path.empty()) throw std::runtime_error("distributed RF-DETR worker requires --dist-store-file");
 if (world <= 1 || rank < 0 || rank >= world) throw std::runtime_error("distributed RF-DETR worker rank is out of range");
#if defined(USE_C10D_NCCL)
 tc::TorchCudaDeviceGuard guard(tc::checked_device_index(device));
 auto store = c10::make_intrusive<c10d::FileStore>(path.string(), world);
 auto options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
 options->timeout = timeout.value_or(c10d::kProcessGroupNCCLDefaultTimeout);
 auto backend = c10::make_intrusive<c10d::ProcessGroupNCCL>(store, rank, world, std::move(options));
 backend->setBoundDeviceId(tc::cuda_device(device));
 auto result = DistributedContext::from_backend(rank, world, device, std::move(backend));
 result.transport_->store = std::move(store);
 return result;
#else
 (void)device;
 (void)timeout;
 throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}
DistributedContext make_distributed_context(const TrainRequest& request) {
 if (request.distributed_worker && request.distributed_world_size > 1)
  return make_distributed_context(request.distributed_store_path, request.distributed_rank, request.distributed_world_size, request.device_id);
 DistributedContext result;
 result.device_id = request.device_id;
 return result;
}
void distributed_abort(const DistributedContext& group) noexcept {
 if (group.transport_ && !group.transport_->aborted.exchange(true)) try {
   group.transport_->backend->abort();
  } catch (...) {}
}
TrainingFailure claim_training_failure(const DistributedContext& group, const TrainingFailure& cause) {
 if (!group.enabled || !group.transport_->store) return cause;
 namespace serial = mmltk::frameworks::serialization;
 serial::wire::ByteBuffer encoded;
 constexpr serial::wire::Limits limits{.max_bytes = 68U * 1024U, .max_items = 32, .max_depth = 8};
 if (!serial::encode(cause, encoded, limits)) throw std::runtime_error("training failure exceeds canonical bounds");
 const std::vector<std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(encoded.data()), reinterpret_cast<const std::uint8_t*>(encoded.data()) + encoded.size());
 const auto retained = group.transport_->store->compareSet("training/session-first-cause", {}, bytes);
 const auto decoded = serial::decode<TrainingFailure>({std::span(reinterpret_cast<const std::byte*>(retained.data()), retained.size()), {}}, limits);
 if (!decoded) throw std::runtime_error("invalid distributed training first cause");
 return *decoded;
}
void distributed_shutdown(const DistributedContext& group) {
 if (group.transport_ && !group.transport_->shutdown.exchange(true)) group.transport_->backend->shutdown();
}
struct TrainingCollectiveWork::Owner final {
 struct Slot final {
  DistributedContext group;
  std::vector<torch::Tensor> tensors;
  c10::intrusive_ptr<c10d::Work> work;
  bool submitted = false, joined = false;
  Slot() { tensors.reserve(1); }
 };
 struct State final {
  State(int device_id, std::size_t capacity) : device(device_id), stream(tc::getCurrentCUDAStream(tc::checked_device_index(device_id))), slots(capacity) {}
  int device;
  tc::TorchCudaStream stream;
  std::vector<Slot> slots;
  std::size_t used = 0;
  std::vector<torch::Tensor> retained;
  at::cuda::CUDAEvent completed;
  cudaError_t failure = cudaSuccess;
  bool completion_recorded = false;
 };
 Owner(int device, std::size_t capacity) : state(std::make_shared<State>(device, capacity)) {
  if (!capacity) throw std::invalid_argument("collective slot capacity must be positive");
 }
 gpu::TerminalCudaRetirementOwner retirement{1U};
 gpu::TerminalCudaRetirementLease terminal = gpu::ReserveTerminalCudaLease(retirement);
 std::shared_ptr<State> state;
};
TrainingCollectiveWork::TrainingCollectiveWork(int device, std::size_t capacity) : owner_(std::make_unique<Owner>(device, capacity)) {}
TrainingCollectiveWork::~TrainingCollectiveWork() {
 if (owner_ && retire() != cudaSuccess) std::move(owner_->terminal).Install(gpu::TerminalCudaCustody::Share(std::move(owner_->state)), cudaErrorUnknown);
}
TrainingCollectiveWork::TrainingCollectiveWork(TrainingCollectiveWork&&) noexcept = default;
TrainingCollectiveWork& TrainingCollectiveWork::operator=(TrainingCollectiveWork&& other) noexcept {
 if (this != &other) {
  TrainingCollectiveWork previous(std::move(*this));
  owner_ = std::move(other.owner_);
 }
 return *this;
}
std::size_t TrainingCollectiveWork::submit(const DistributedContext& group, const torch::Tensor& tensor, Operation operation) {
 auto& state = *owner_->state;
 if (state.failure != cudaSuccess) throw std::runtime_error("training collective owner cannot admit unsettled failure");
 if (state.used == state.slots.size()) throw std::logic_error("training collective capacity exhausted before settlement");
 if (!tensor.is_cuda() || tensor.device().index() != state.device) throw std::invalid_argument("training collective requires its selected CUDA device");
 if (group.enabled && group.device_id != state.device) throw std::invalid_argument("training collective transport uses a different CUDA device");
 if (group.enabled && (!group.transport_ || !group.transport_->backend || group.transport_->aborted || group.transport_->shutdown))
  throw std::runtime_error("training collective transport is unavailable");
 const auto stream = tc::getCurrentCUDAStream(tc::checked_device_index(state.device));
 if (state.used && stream != state.stream) throw std::logic_error("collective operation changed its launch stream before settlement");
 state.stream = stream;
 const auto index = state.used;
 auto& slot = state.slots[index];
 // Establish every application reference before calling the backend. The
 // backend may queue device work and throw before returning its Work handle.
 slot.group = group;
 slot.tensors.assign(1, tensor);
 slot.joined = false;
 ++state.used;
 state.completion_recorded = false;
 if (!group.enabled) {
  slot.joined = true;
  return index;
 }
 slot.submitted = true;
 try {
  if (operation == Operation::Sum)
   slot.work = group.transport_->backend->allreduce(slot.tensors);
  else {
   c10d::BroadcastOptions options;
   options.rootRank = 0;
   options.rootTensor = 0;
   slot.work = group.transport_->backend->broadcast(slot.tensors, options);
  }
  if (!slot.work) throw std::runtime_error("training collective returned no Work handle");
 } catch (...) {
  state.failure = cudaErrorUnknown;
  distributed_abort(group);
  throw;
 }
 return index;
}
std::size_t TrainingCollectiveWork::all_reduce(const DistributedContext& group, const torch::Tensor& tensor) { return submit(group, tensor, Operation::Sum); }
std::size_t TrainingCollectiveWork::broadcast(const DistributedContext& group, const torch::Tensor& tensor) { return submit(group, tensor, Operation::Broadcast); }
void TrainingCollectiveWork::retain(std::span<const torch::Tensor> tensors) {
 auto& state = *owner_->state;
 if (state.used || state.completion_recorded || state.failure != cudaSuccess) throw std::logic_error("collective storage must be retained before submission");
 state.stream = tc::getCurrentCUDAStream(tc::checked_device_index(state.device));
 state.retained.assign(tensors.begin(), tensors.end());
}
void TrainingCollectiveWork::join(std::size_t index) {
 auto& state = *owner_->state;
 if (index >= state.used) throw std::logic_error("training collective join outside submitted slots");
 auto& slot = state.slots[index];
 if (slot.joined) return;
 try {
  tc::TorchCudaStreamGuard guard(state.stream);
  if (!slot.work) throw std::runtime_error("training collective has no Work handle to join");
  if (slot.group.transport_->aborted) throw std::runtime_error("training collective was aborted before its stream join");
  if (!slot.work->wait()) throw std::runtime_error("training collective wait returned false");
  if (slot.group.transport_->aborted) throw std::runtime_error("training collective was aborted during its stream join");
  slot.joined = true;
 } catch (...) {
  state.failure = cudaErrorUnknown;
  distributed_abort(slot.group);
  throw;
 }
}
bool TrainingCollectiveWork::uncertain() const noexcept {
 if (!owner_) return false;
 const auto& state = *owner_->state;
 if (state.failure != cudaSuccess) return true;
 for (std::size_t i = 0; i < state.used; ++i) {
  const auto& slot = state.slots[i];
  if (slot.submitted && (!slot.joined || slot.group.transport_->aborted)) return true;
 }
 return false;
}
void TrainingCollectiveWork::record_completion() {
 auto& state = *owner_->state;
 if (uncertain()) throw std::runtime_error("training collective physical completion is unproved");
 if (!state.used && state.retained.empty()) state.stream = tc::getCurrentCUDAStream(tc::checked_device_index(state.device));
 try {
  tc::TorchCudaStreamGuard guard(state.stream);
  state.completed.record(state.stream);
  state.completion_recorded = true;
 } catch (...) {
  state.failure = cudaErrorUnknown;
  throw;
 }
}
void TrainingCollectiveWork::block_current_stream() {
 auto& state = *owner_->state;
 if (!state.completion_recorded) throw std::logic_error("training collective completion was not recorded");
 try {
  const auto current = tc::getCurrentCUDAStream(tc::checked_device_index(state.device));
  if (current != state.stream) state.completed.block(current);
 } catch (...) {
  state.failure = cudaErrorUnknown;
  throw;
 }
}
void TrainingCollectiveWork::settle() {
 auto& state = *owner_->state;
 if (uncertain()) throw std::runtime_error("training collective physical completion is unproved");
 if (!state.completion_recorded) record_completion();
 try {
  state.completed.synchronize();
 } catch (...) {
  state.failure = cudaErrorUnknown;
  throw;
 }
 clear_completed();
}
bool TrainingCollectiveWork::release_completed() {
 auto& state = *owner_->state;
 if (uncertain()) throw std::runtime_error("training collective physical completion is unproved");
 if (!state.completion_recorded) throw std::logic_error("training collective completion was not recorded");
 try {
  if (!state.completed.query()) return false;
 } catch (...) {
  state.failure = cudaErrorUnknown;
  throw;
 }
 clear_completed();
 return true;
}
void TrainingCollectiveWork::clear_completed() {
 auto& state = *owner_->state;
 for (std::size_t i = 0; i < state.used; ++i) {
  auto& slot = state.slots[i];
  slot.work.reset();
  slot.tensors.clear();
  slot.group = {};
  slot.submitted = slot.joined = false;
 }
 state.used = 0;
 state.retained.clear();
 state.completion_recorded = false;
}
cudaError_t TrainingCollectiveWork::retire() noexcept {
 if (!owner_) return cudaSuccess;
 const auto& state = *owner_->state;
 if (!state.used && state.retained.empty() && !state.completion_recorded && state.failure == cudaSuccess) return cudaSuccess;
 try {
  settle();
  return cudaSuccess;
 } catch (...) { return cudaErrorUnknown; }
}
void distributed_all_reduce_tensor(const DistributedContext& group, const torch::Tensor& tensor) {
 if (!group.enabled) return;
 TrainingCollectiveWork work(group.device_id);
 work.join(work.all_reduce(group, tensor));
 work.settle();
}
void distributed_barrier(const DistributedContext& group) {
 if (!group.enabled) return;
 // NCCL's barrier is itself an all-reduce. Own its scalar explicitly so even
 // a throwing backend submission cannot hide the barrier's device allocation.
 auto value = torch::zeros({1}, torch::TensorOptions().device(tc::cuda_device(group.device_id)).dtype(torch::kInt32));
 distributed_all_reduce_tensor(group, value);
}
void distributed_agree(const DistributedContext& group, std::string_view turn, std::span<const std::uint8_t> signature) {
 if (!group.enabled) return;
 const auto digest = mmltk::common::io::sha256_bytes(signature);
 auto host = torch::empty({static_cast<std::int64_t>(digest.size())}, torch::TensorOptions().dtype(torch::kUInt8));
 std::memcpy(host.data_ptr(), digest.data(), digest.size());
 auto local = host.to(tc::cuda_device(group.device_id));
 auto reference = local.clone();
 TrainingCollectiveWork work(group.device_id, 2);
 work.join(work.broadcast(group, reference));
 auto differs = local.ne(reference).any().to(torch::kInt32);
 work.join(work.all_reduce(group, differs));
 work.settle();
 if (differs.item<std::int32_t>()) throw std::runtime_error("distributed training admission differs at " + std::string(turn));
}
void agree_training_text(const DistributedContext& group, std::string_view turn, std::string_view value) {
 distributed_agree(group, turn, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
namespace {
template <class Value>
void agree_training_value(const DistributedContext& distributed, std::string_view turn, const Value& value) {
 if (!distributed.enabled) return;
 namespace serialization = mmltk::frameworks::serialization;
 serialization::wire::ByteBuffer bytes;
 mmltk::common::io::Sha256Hasher signature;
 // Project canonical fields, including inherited request fields. Archive
 // scalar doubles preserve IEEE identity: an unset best metric is -infinity,
 // which the nested finite-value CBOR representation deliberately rejects.
 mmltk::frameworks::reflection::visit_materialized_members<Value>([&]<class Declaration>(const auto&) {
  const auto& field = value.*Declaration::pointer;
  using Field = std::remove_cvref_t<decltype(field)>;
  mmltk::common::io::Sha256Digest digest;
  if constexpr (std::is_floating_point_v<Field>)
   digest = mmltk::common::io::sha256_bytes(std::bit_cast<std::array<std::uint8_t, sizeof(Field)>>(field));
  else {
   if (!serialization::encode(field, bytes, {.max_bytes = serialization::reflected_maximum_cbor_bytes<Value>(), .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32}))
    throw std::runtime_error("cannot encode training admission");
   digest = mmltk::common::io::sha256_bytes({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
  }
  signature.Update(digest);
 });
 const auto digest = signature.Finish();
 distributed_agree(distributed, turn, digest);
}
}  // namespace
void agree_training_request(const DistributedContext& group, const TrainRequest& request) {
 if (!group.enabled) return;
 auto common = request;
 // Ordered topology owns rank placement. Physical CPU budgets may differ and
 // never enter the mathematical request signature.
 common.device_id = 0;
 common.distributed_rank = 0;
 common.numa_node = -1;
 common.cpu_affinity.clear();
 common.workers = 0;
 agree_training_value(group, "request", common);
}
void agree_training_continuation(const DistributedContext& group, const detail::TrainingContinuationValues& value) { agree_training_value(group, "continuation", value); }
void agree_model_inventory(const DistributedContext& distributed, const NativeRfDetrModel& model, std::string_view turn) {
 if (!distributed.enabled) return;
 std::ostringstream signature;
 const auto append = [&](const auto& items) {
  for (const auto& item : items)
   signature << item.key() << ':' << item.value().sizes() << ':' << item.value().strides() << ':' << item.value().scalar_type() << ':' << item.value().requires_grad() << ';';
 };
 append(model.named_parameters(true));
 append(model.named_buffers(true));
 const auto bytes = signature.str();
 distributed_agree(distributed, turn, {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
TrainingPrecision agree_training_precision(const DistributedContext& distributed, int device, bool amp, bool fused) {
 const auto* properties = at::cuda::getDeviceProperties(tc::checked_device_index(device));
 auto capabilities = torch::tensor({properties->major >= 8 ? 1 : 0, properties->major >= 7 ? 1 : 0}, torch::TensorOptions().dtype(torch::kInt32).device(tc::cuda_device(device)));
 distributed_all_reduce_tensor(distributed, capabilities);
 const auto common = capabilities.to(torch::kCPU);
 const auto* caps = common.const_data_ptr<std::int32_t>();
 const auto dtype = !amp ? at::kFloat : caps[0] == distributed.world_size ? at::kBFloat16 : caps[1] == distributed.world_size ? at::kHalf : at::kFloat;
 return {dtype, fused && caps[0] == distributed.world_size};
}
void agree_training_topology(const DistributedContext& distributed, int device) {
 if (!distributed.enabled) return;
#if defined(USE_C10D_NCCL)
 const auto* properties = at::cuda::getDeviceProperties(tc::checked_device_index(device));
 const auto* uuid = reinterpret_cast<const std::uint8_t*>(properties->uuid.bytes);
 distributed.transport_->store->set("training/device/" + std::to_string(distributed.rank), std::vector<std::uint8_t>(uuid, uuid + sizeof(properties->uuid.bytes)));
 std::set<std::vector<std::uint8_t>> identities;
 for (int rank = 0; rank < distributed.world_size; ++rank)
  if (!identities.insert(distributed.transport_->store->get("training/device/" + std::to_string(rank))).second)
   throw std::runtime_error("distributed training ranks selected the same physical CUDA device");
#else
 (void)device;
 throw std::runtime_error("distributed training requires NCCL");
#endif
}
void broadcast_training_tensors(const DistributedContext& group, const std::vector<torch::Tensor>& tensors, std::size_t bucket_bytes) {
 if (!group.enabled || tensors.empty()) return;
 if (!bucket_bytes) throw std::invalid_argument("broadcast bucket capacity must be positive");
 torch::NoGradGuard no_grad;
 const auto [buckets, limits] = c10d::compute_bucket_assignment_by_size(tensors, {bucket_bytes});
 struct Buffer final {
  explicit Buffer(int device) : work(device) {}
  TrainingCollectiveWork work;
  torch::Tensor storage, flat;
  std::vector<torch::Tensor> items, views;
  bool pending = false;
  void finish() {
   if (!pending) return;
   work.join(0);
   for (std::size_t i = 0; i < items.size(); ++i)
    if (views[i].numel()) items[i].copy_(views[i], true);
   work.settle();
   pending = false;
  }
 };
 std::array<Buffer, 2> buffers{Buffer(group.device_id), Buffer(group.device_id)};
 std::size_t next = 0;
 for (const auto& bucket : buckets) {
  auto& buffer = buffers[next++ % buffers.size()];
  buffer.finish();
  buffer.items.clear();
  buffer.items.reserve(bucket.size());
  std::int64_t elements = 0;
  for (auto index : bucket) {
   buffer.items.push_back(tensors[index]);
   elements += tensors[index].numel();
  }
  buffer.work.retain(buffer.items);
  // Flatten once per required allocation; subsequent compatible buckets reuse
  // the allocation through Torch's unflattened structural views.
  if (!buffer.storage.defined() || buffer.storage.scalar_type() != buffer.items.front().scalar_type() || buffer.storage.numel() < elements) {
   buffer.storage = torch::utils::flatten_dense_tensors(buffer.items);
   // Torch's single contiguous input fast path aliases that model tensor.
   // Reusable scratch must have its own allocation before a later bucket fills it.
   if (buffer.items.size() == 1 && buffer.items.front().is_contiguous()) buffer.storage = buffer.storage.clone();
   buffer.flat = buffer.storage;
   buffer.views = torch::utils::unflatten_dense_tensors(buffer.flat, buffer.items);
  } else {
   buffer.flat = buffer.storage.narrow(0, 0, elements);
   buffer.views = torch::utils::unflatten_dense_tensors(buffer.flat, buffer.items);
   for (std::size_t i = 0; i < buffer.items.size(); ++i) buffer.views[i].copy_(buffer.items[i], true);
  }
  // Mark the application slot pending before a potentially throwing submission.
  buffer.pending = true;
  static_cast<void>(buffer.work.broadcast(group, buffer.flat));
 }
 for (auto& buffer : buffers) buffer.finish();
}
void broadcast_training_model(const DistributedContext& group, NativeRfDetrModel& model) {
 torch::NoGradGuard no_grad;
 const auto parameters = model.named_parameters(true);
 const auto buffers = model.named_buffers(true);
 auto invalid = torch::zeros({}, parameters.begin()->value().options().dtype(torch::kInt32));
 std::vector<torch::Tensor> tensors;
 tensors.reserve(parameters.size() + buffers.size());
 const auto append = [&](const auto& items) {
  for (const auto& item : items) {
   tensors.push_back(item.value());
   if (item.value().is_floating_point()) invalid.add_(torch::isfinite(item.value()).all().logical_not().to(torch::kInt32));
  }
 };
 append(parameters);
 append(buffers);
 distributed_all_reduce_tensor(group, invalid);
 if (invalid.item<std::int32_t>() != 0) throw std::runtime_error("nonfinite initialized training model state on a selected rank");
 broadcast_training_tensors(group, tensors);
}
}  // namespace mmltk::backend::models::rfdetr
