#include "src/backend/models/rfdetr/core/detection_ops.h"
#include <torch/torch.h>
#include <cmath>
#include <ATen/cuda/CUDAContext.h>
#include <set>
#include <sstream>
#include <optional>
#include <cstring>
#include "src/common/io/file_digest.h"
#include <numbers>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_ops_private.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor) {
 if (!distributed.enabled) { return; }
#if defined(USE_C10D_NCCL)
 std::vector<torch::Tensor> tensors{tensor};
 distributed.process_group->allreduce(tensors)->wait();
#else
 (void)distributed;
 (void)tensor;
 throw std::runtime_error("distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}
void distributed_abort(const DistributedContext& distributed) noexcept {
#if defined(USE_C10D_NCCL)
 if (distributed.enabled && distributed.process_group) try { distributed.process_group->abort(); } catch (...) {}
#else
 (void)distributed;
#endif
}
void distributed_agree(const DistributedContext& distributed, std::string_view turn, std::span<const std::uint8_t> signature) {
 if (!distributed.enabled) return;
#if defined(USE_C10D_NCCL)
 // Agreement has a fixed bounded collective shape regardless of the encoded
 // schema/model inventory length. No epoch-indexed store history accumulates.
 const auto digest = mmltk::common::io::sha256_bytes(signature);
 auto host = torch::empty({static_cast<std::int64_t>(digest.size())}, torch::TensorOptions().dtype(torch::kUInt8));
 std::memcpy(host.data_ptr(), digest.data(), digest.size());
 auto local = host.to(torch_cuda::cuda_device(distributed.device_id));
 auto reference = local.clone(); distributed_broadcast(distributed, reference);
 auto differs = local.ne(reference).any().to(torch::kInt32);
 distributed_all_reduce_tensor(distributed, differs);
 if (differs.item<std::int32_t>()) throw std::runtime_error("distributed training admission differs at " + std::string(turn));
#else
 throw std::runtime_error("distributed training requires NCCL");
#endif
}
void distributed_broadcast(const DistributedContext& distributed, torch::Tensor& tensor) {
 if (!distributed.enabled) return;
#if defined(USE_C10D_NCCL)
 std::vector<torch::Tensor> values{tensor};
 c10d::BroadcastOptions options; options.rootRank = 0; options.rootTensor = 0;
 distributed.process_group->broadcast(values, options)->wait();
#else
 throw std::runtime_error("distributed training requires NCCL");
#endif
}
void agree_model_inventory(const DistributedContext& distributed, const NativeRfDetrModel& model, std::string_view turn) {
 if (!distributed.enabled) return;
 std::ostringstream signature;
 const auto append = [&](const auto& items) {
  for (const auto& item : items) signature << item.key() << ':' << item.value().sizes() << ':' << item.value().strides() << ':' << item.value().scalar_type() << ':' << item.value().requires_grad() << ';';
 };
 append(model.named_parameters(true)); append(model.named_buffers(true));
 const auto bytes = signature.str();
 distributed_agree(distributed, turn, {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
void broadcast_training_model(const DistributedContext& distributed, NativeRfDetrModel& model) {
 torch::NoGradGuard no_grad;
 const auto parameters = model.named_parameters(true);
 const auto buffers = model.named_buffers(true);
 auto invalid = torch::zeros({}, parameters.begin()->value().options().dtype(torch::kInt32));
 const auto validate = [&](const auto& items) {
  for (const auto& item : items) if (item.value().is_floating_point()) invalid.add_(torch::isfinite(item.value()).all().logical_not().to(torch::kInt32));
 };
 validate(parameters); validate(buffers);
 distributed_all_reduce_tensor(distributed, invalid);
 if (invalid.item<std::int32_t>() != 0) throw std::runtime_error("nonfinite initialized training model state on a selected rank");
 const auto broadcast = [&](const auto& items) { for (const auto& item : items) { auto value = item.value(); distributed_broadcast(distributed, value); } };
 broadcast(parameters); broadcast(buffers);
}
TrainingPrecision agree_training_precision(const DistributedContext& distributed, int device, bool amp, bool fused) {
 const auto* properties = at::cuda::getDeviceProperties(torch_cuda::checked_device_index(device));
 auto capabilities = torch::tensor({properties->major >= 8 ? 1 : 0, properties->major >= 7 ? 1 : 0}, torch::TensorOptions().dtype(torch::kInt32).device(torch_cuda::cuda_device(device)));
 distributed_all_reduce_tensor(distributed, capabilities);
 const auto common = capabilities.to(torch::kCPU);
 const auto* caps = common.const_data_ptr<std::int32_t>();
 const auto dtype = !amp ? at::kFloat : caps[0] == distributed.world_size ? at::kBFloat16 : caps[1] == distributed.world_size ? at::kHalf : at::kFloat;
 return {dtype, fused && caps[0] == distributed.world_size};
}
void agree_training_topology(const DistributedContext& distributed, int device) {
 if (!distributed.enabled) return;
#if defined(USE_C10D_NCCL)
 const auto* properties = at::cuda::getDeviceProperties(torch_cuda::checked_device_index(device));
 const auto* uuid = reinterpret_cast<const std::uint8_t*>(properties->uuid.bytes);
 distributed.store->set("training/device/" + std::to_string(distributed.rank), std::vector<std::uint8_t>(uuid, uuid + sizeof(properties->uuid.bytes)));
 std::set<std::vector<std::uint8_t>> identities;
 for (int rank = 0; rank < distributed.world_size; ++rank)
  if (!identities.insert(distributed.store->get("training/device/" + std::to_string(rank))).second) throw std::runtime_error("distributed training ranks selected the same physical CUDA device");
#else
 (void)device;
 throw std::runtime_error("distributed training requires NCCL");
#endif
}
struct TrainingTargetCounts::State final {
 State(std::size_t capacity, int device, const DistributedContext&);
 void begin(std::size_t);
 void publish(std::size_t, std::int64_t);
 void resolve(const DistributedContext&, const torch::Device&);
 DeviceLossNormalizer consume(std::size_t, cudaStream_t);
 void fail(std::exception_ptr) noexcept;
 void rethrow_failure_locked() const;
 torch_cuda::TorchCudaStream launch_;
 std::vector<std::optional<torch_cuda::TorchCudaStream>> consumers_;
 std::mutex mutex_;
 std::condition_variable condition_;
 std::vector<std::int64_t> host_counts_;
 std::vector<bool> published_;
 torch::Tensor device_counts_;
 std::exception_ptr failure_;
 std::size_t slots_ = 0;
 int device_id_ = -1;
 const DistributedContext* distributed_ = nullptr;
 std::vector<at::cuda::CUDAEvent> ready_;
 std::vector<bool> resolved_;
 bool distributed_abort_requested_ = false;
};
TrainingTargetCounts::State::State(const std::size_t capacity, const int device_id, const DistributedContext& distributed)
 : launch_(torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(device_id))), consumers_(capacity), host_counts_(capacity, 0), published_(capacity, false), device_id_(device_id), distributed_(&distributed), ready_(capacity), resolved_(capacity, false) {
 if (!capacity) throw std::invalid_argument("target count capacity must be positive");
 device_counts_ = torch::empty({static_cast<std::int64_t>(capacity)}, torch::TensorOptions().dtype(torch::kInt64).device(torch_cuda::cuda_device(device_id)));
 begin(capacity);
}
void TrainingTargetCounts::State::begin(std::size_t slots) {
 std::lock_guard lock(mutex_); rethrow_failure_locked();
 if (!slots || slots > host_counts_.size()) throw std::invalid_argument("invalid count slot admission");
 slots_ = slots; std::fill(published_.begin(), published_.end(), false); std::fill(resolved_.begin(), resolved_.end(), false);
}
void TrainingTargetCounts::State::publish(const std::size_t lane, const std::int64_t count) {
 std::lock_guard lock(mutex_); rethrow_failure_locked();
 if (lane >= slots_ || published_[lane] || count < 0) throw std::runtime_error("invalid logical target count publication");
 host_counts_[lane] = count; published_[lane] = true; condition_.notify_all();
}
void TrainingTargetCounts::State::resolve(const DistributedContext& distributed, const torch::Device& device) {
 (void)device;
 try {
  // Exactly one scalar collective per logical microbatch, in admission order.
  // A slot's worker resumes immediately; it never waits for another wave.
  for (std::size_t lane = 0; lane < slots_; ++lane) {
   {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] { return failure_ || published_[lane]; }); rethrow_failure_locked();
    if (resolved_[lane]) continue;
   }
   auto count = device_counts_.narrow(0, static_cast<std::int64_t>(lane), 1);
   count.fill_(host_counts_[lane]);
   distributed_all_reduce_tensor(distributed, count);
   ready_[lane].record(torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(device_id_)));
   { std::lock_guard lock(mutex_); rethrow_failure_locked(); resolved_[lane] = true; }
   condition_.notify_all();
  }
 } catch (...) { fail(std::current_exception()); throw; }
}
DeviceLossNormalizer TrainingTargetCounts::State::consume(const std::size_t lane, const cudaStream_t stream) {
 {
  std::unique_lock lock(mutex_);
  if (lane >= slots_) throw std::runtime_error("target count slot out of range");
  condition_.wait(lock, [&] { return failure_ || resolved_[lane]; }); rethrow_failure_locked();
  consumers_[lane] = torch_cuda::getStreamFromExternal(stream, torch_cuda::checked_device_index(device_id_));
 }
 const auto consumer = *consumers_[lane];
 ready_[lane].block(consumer); device_counts_.record_stream(consumer);
 return {device_counts_.select(0, static_cast<std::int64_t>(lane)).to(torch::kFloat32)};
}
void TrainingTargetCounts::State::fail(std::exception_ptr failure) noexcept {
 bool abort = false;
 { std::lock_guard lock(mutex_); if (!failure_) { failure_ = failure; abort = !distributed_abort_requested_; distributed_abort_requested_ = true; } }
 condition_.notify_all();
 if (abort && distributed_) distributed_abort(*distributed_);
}
void TrainingTargetCounts::State::rethrow_failure_locked() const { if (failure_) std::rethrow_exception(failure_); }
TrainingTargetCounts::TrainingTargetCounts(std::size_t capacity, int device, const DistributedContext& group)
 : state_(std::make_shared<State>(capacity, device, group)) {}
TrainingTargetCounts::~TrainingTargetCounts() noexcept {
 // Waves drain their futures before the final borrower releases this owner.
 // Count conversions may have been queued before a worker failed, so settle
 // both the publisher and the bounded consumer-stream inventory.
 cudaError_t failure = cudaSuccess;
 try {
  torch_cuda::TorchCudaDeviceGuard device(torch_cuda::checked_device_index(state_->device_id_));
  failure = cudaStreamSynchronize(state_->launch_.stream());
  for (const auto& stream : state_->consumers_) if (stream) {
   const auto status = cudaStreamSynchronize(stream->stream());
   if (failure == cudaSuccess) failure = status;
  }
 } catch (...) { if (failure == cudaSuccess) failure = cudaErrorUnknown; }
 if (failure != cudaSuccess) std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state_)), failure);
}
void TrainingTargetCounts::begin(std::size_t slots) { state_->begin(slots); }
void TrainingTargetCounts::publish(std::size_t lane, std::int64_t count) { state_->publish(lane, count); }
void TrainingTargetCounts::resolve(const DistributedContext& group, const torch::Device& device) { state_->resolve(group, device); }
DeviceLossNormalizer TrainingTargetCounts::consume(std::size_t lane, cudaStream_t stream) { return state_->consume(lane, stream); }
void TrainingTargetCounts::fail(std::exception_ptr failure) noexcept { state_->fail(std::move(failure)); }
GradScaler::GradScaler(const bool enabled, const float init_scale, const float growth_factor, const float backoff_factor, const int growth_interval)
    : enabled_(enabled), scale_(init_scale), growth_factor_(growth_factor), backoff_factor_(backoff_factor), growth_interval_(growth_interval) {}
torch::Tensor GradScaler::scale(const torch::Tensor& loss) { return enabled_ ? loss * scale_ : loss; }
void GradScaler::update(const bool found_inf) {
 if (!enabled_) return;
 if (found_inf) {
  scale_ *= backoff_factor_;
  growth_tracker_ = 0;
 } else if (++growth_tracker_ >= growth_interval_) {
  scale_ *= growth_factor_;
  growth_tracker_ = 0;
 }
}
bool GradScaler::enabled() const noexcept { return enabled_; }
float GradScaler::current_scale() const noexcept { return scale_; }
int GradScaler::growth_tracker() const noexcept { return growth_tracker_; }
void GradScaler::load_state(const float scale, const int growth_tracker) noexcept {
 if (enabled_) {
  scale_ = scale;
  growth_tracker_ = std::max(0, growth_tracker);
 }
}
void GradScaler::ensure_device_state(const torch::Device& device) {
 if (found_inf_device_.defined() && found_inf_device_.device() == device) return;
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
 found_inf_device_ = torch::zeros({}, options);
 inverse_scale_device_ = torch::ones({}, options);
}
torch::Tensor loss_value_or_zero(const TensorMap& loss_dict, const torch::Device& device, std::string_view key) {
 const auto found = loss_dict.find(std::string(key));
 if (found != loss_dict.end()) { return found->second; }
 return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
}
scalar_packet::Tensors ordinary_scalar_tensors(const TensorMap& losses, const torch::Tensor& total, const torch::Tensor& auxiliary) {
 scalar_packet::Tensors values;
 scalar_packet::set<^^TrainingScalars::total>(values, total);
 scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(values, auxiliary);
 const auto assign = [&]<std::meta::info Member>(std::string_view name) {
  const auto found = losses.find(std::string(name));
  if (found != losses.end()) scalar_packet::set<Member>(values, found->second);
 };
 assign.operator()<^^TrainingScalars::classification>("loss_ce");
 assign.operator()<^^TrainingScalars::l1>("loss_bbox");
 assign.operator()<^^TrainingScalars::giou>("loss_giou");
 assign.operator()<^^TrainingScalars::mask_ce>("loss_mask_ce");
 assign.operator()<^^TrainingScalars::mask_dice>("loss_mask_dice");
 assign.operator()<^^TrainingScalars::class_error>("class_error");
 assign.operator()<^^TrainingScalars::cardinality_error>("cardinality_error");
 return values;
}
RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel& model, const TrainingSupervisionRoute route, const ModelOutputs& outputs, const PreparedTargets& targets,
 const DeviceLossNormalizer& normalizer, const DetectionConfig& detection_config) {
 if (route_uses_match_free(route)) {
  const auto loss = model.supervision_loss(outputs, targets, normalizer, true);
  scalar_packet::Tensors scalars;
  scalar_packet::set<^^TrainingScalars::total>(scalars, loss.total);
  scalar_packet::set<^^TrainingScalars::classification>(scalars, loss.main.classification);
  scalar_packet::set<^^TrainingScalars::l1>(scalars, loss.main.box);
  scalar_packet::set<^^TrainingScalars::giou>(scalars, loss.main.giou);
  scalar_packet::set<^^TrainingScalars::mask_ce>(scalars, loss.main.mask_ce);
  scalar_packet::set<^^TrainingScalars::mask_dice>(scalars, loss.main.mask_dice);
  scalar_packet::set<^^TrainingScalars::correspondence_weighted>(scalars, loss.correspondence);
  scalar_packet::set<^^TrainingScalars::auxiliary_weighted>(scalars, loss.auxiliary);
  if (route_uses_denoising(route)) scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, loss.denoising);
  return {loss.total, loss.classification, loss.box + loss.giou, {}, std::move(scalars)};
 }
 const double group_divisor = detection_config.sum_group_losses ? 1.0 : static_cast<double>(detection_config.group_detr);
 const auto num_boxes = torch::clamp_min(normalizer.target_count * group_divisor, 1.0);
 auto ordinary_terms = detection_loss_dict(outputs, targets, detection_config, true, num_boxes);
 torch::Tensor auxiliary;
 auto total = weighted_detection_loss(ordinary_terms, detection_config, outputs.main.pred_logits.device(), &auxiliary);
 auto scalars = ordinary_scalar_tensors(ordinary_terms, total, auxiliary);
 auto classification = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_ce");
 auto box = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_bbox");
 if (route_uses_denoising(route)) {
  const auto denoising = model.supervision_loss(outputs, targets, normalizer, true);
  total = total + denoising.total;
  scalar_packet::set<^^TrainingScalars::denoising_weighted>(scalars, denoising.total);
  classification = classification + denoising.classification;
  box = box + denoising.box + denoising.giou;
 }
 scalar_packet::set<^^TrainingScalars::total>(scalars, total);
 TrainingDiagnosticTensors diagnostics{ordinary_terms.at("class_error_sum"), ordinary_terms.at("matched_count"), ordinary_terms.at("cardinality_error_sum"), ordinary_terms.at("image_count")};
 return {std::move(total), std::move(classification), std::move(box), std::move(ordinary_terms), std::move(scalars), std::move(diagnostics)};
}
}  // namespace mmltk::backend::models::rfdetr
