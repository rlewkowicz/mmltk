#include "src/backend/ml/runtime/backend_factory.h"
#include <cuda_runtime_api.h>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
namespace mmltk::backend::ml::runtime {
namespace {
void require_cuda(const RuntimeStatus status, const char* operation) {
 if (status != kRuntimeSuccess) throw CudaOperationError{status, operation};
}
}  // namespace
CudaOperationError::CudaOperationError(const RuntimeStatus status, const std::string_view operation)
    : std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(static_cast<cudaError_t>(status))), status_(status) {}
RuntimeBackend::RuntimeBackend(const std::int32_t device, const BorrowedCommandStream command_stream) : lane_(device, command_stream) {}
std::int32_t RuntimeBackend::device() const noexcept { return lane_.device(); }
BorrowedCommandStream RuntimeBackend::command_stream() const noexcept { return lane_.command_stream(); }
RuntimeBackend::~RuntimeBackend() {
 std::lock_guard lock(state_mutex_);
 state_ = LaneState::Closed;
}
RuntimeSubmission RuntimeBackend::Run(const RuntimeTensorBuffer& input, std::span<RuntimeTensorBuffer> outputs, const RuntimeOutputBinding output_binding,
                                      const RuntimeContinuation continuation, std::shared_ptr<void> retained_storage) {
 std::shared_ptr<RuntimeBackend> owner = weak_from_this().lock();
 if (!owner) throw std::logic_error("runtime lane must have shared lifetime ownership");
 std::unique_lock lane_lock(state_mutex_);
 const std::uint64_t generation = ClaimIssuing();
 try {
  output_binding.Invoke(device(), command_stream().native_handle, outputs);
  ValidateRuntimeTensorSet(model_info(), input, outputs);
  const RawSubmission submitted = Submit(input, outputs, continuation);
  if (submitted.device != device() || submitted.output_count != outputs.size() || !submitted.stream || submitted.stream != command_stream() ||
      submitted.completion_event == 0U) {
   active_submission_.emplace(ActiveSubmission{.receipt = submitted, .retained_storage = std::move(retained_storage)});
   state_ = LaneState::Active;
   const auto retired = SettleActive(false, LaneState::Idle, LaneState::Active);
   if (retired != kRuntimeSuccess) throw CudaOperationError{retired, "invalid runtime submission retirement"};
   throw std::runtime_error("runtime backend returned an invalid submission receipt");
  }
  active_submission_.emplace(ActiveSubmission{.receipt = submitted, .retained_storage = std::move(retained_storage)});
  state_ = LaneState::Active;
  return RuntimeSubmission{std::move(owner), generation, submitted.device, submitted.stream, submitted.completion_event, submitted.output_count};
 } catch (...) {
  if (state_ == LaneState::Issuing) state_ = LaneState::Idle;
  throw;
 }
}
void RuntimeBackend::ReleaseAfterCompletion(RuntimeSubmission&& submission) {
 if (submission.owner_.get() != this) throw std::invalid_argument("runtime submission does not belong to this active lane");
 const std::shared_ptr<RuntimeBackend> keep_alive = submission.owner_;
 std::unique_lock lane_lock(state_mutex_);
 if (state_ != LaneState::Active || !active_submission_.has_value() || submission.generation_ != generation_)
  throw std::invalid_argument("runtime submission is not the active lane receipt");
 const RuntimeStatus completed = SettleActive(true, LaneState::Idle, LaneState::Active);
 submission.Disarm();
 lane_lock.unlock();
 static_cast<void>(keep_alive);
 if (completed != kRuntimeSuccess) throw CudaOperationError{completed, "runtime submission completion observation failed"};
}
RuntimeStatus RuntimeBackend::Close() noexcept {
 std::unique_lock lane_lock(state_mutex_);
 if (state_ == LaneState::Closed) return kRuntimeSuccess;
 if (state_ == LaneState::Active) state_ = LaneState::Closing;
 if (state_ == LaneState::Closing && active_submission_.has_value()) {
  const auto settled = SettleActive(true, LaneState::Closing, LaneState::Closing);
  if (settled != kRuntimeSuccess) return settled;
 }
 state_ = LaneState::Closing;
 const auto status = CloseResources();
 if (status == kRuntimeSuccess) state_ = LaneState::Closed;
 return status;
}
RuntimeStatus RuntimeBackend::ObserveCompletion() noexcept { return lane_.Observe(); }
RuntimeStatus RuntimeBackend::RetireIssuedSubmission(const RawSubmission& submission) noexcept { return lane_.Retire(submission); }
RuntimeStatus RuntimeBackend::CloseResources() noexcept {
 const RuntimeStatus selected = lane_.Select();
 if (selected != kRuntimeSuccess) return selected;
 ReleaseBackendResources();
 return lane_.Close();
}
std::uint64_t RuntimeBackend::ClaimIssuing() {
 if (state_ != LaneState::Idle) throw std::logic_error("runtime lane is not available for submission");
 state_ = LaneState::Issuing;
 if (++generation_ == 0U) ++generation_;
 return generation_;
}
RuntimeStatus RuntimeBackend::SettleActive(const bool observe_completion, const LaneState succeeded, const LaneState failed) noexcept {
 if (!active_submission_.has_value()) return kRuntimeSuccess;
 state_ = LaneState::Settling;
 const auto status = observe_completion ? ObserveCompletion() : RetireIssuedSubmission(active_submission_->receipt);
 if (status == kRuntimeSuccess) {
  active_submission_.reset();
  state_ = succeeded;
 } else {
  state_ = failed;
 }
 return status;
}
RuntimeBackend::CudaLane::CudaLane(const std::int32_t device, const BorrowedCommandStream command_stream)
    : device_(device), stream_(command_stream.native_handle), owns_stream_(!command_stream) {
 if (device_ < 0) throw std::invalid_argument("runtime CUDA lane device is invalid");
 Activate();
 if (owns_stream_) {
  cudaStream_t stream = nullptr;
  require_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
  stream_ = reinterpret_cast<std::uintptr_t>(stream);
 }
 cudaEvent_t completion = nullptr;
 const cudaError_t event_status = cudaEventCreateWithFlags(&completion, cudaEventDisableTiming);
 if (event_status != cudaSuccess) {
  if (owns_stream_ && stream_ != 0U) static_cast<void>(cudaStreamDestroy(reinterpret_cast<cudaStream_t>(stream_)));
  stream_ = 0U;
  throw CudaOperationError{static_cast<RuntimeStatus>(event_status), "cudaEventCreateWithFlags"};
 }
 completion_ = reinterpret_cast<std::uintptr_t>(completion);
 open_ = true;
}
RuntimeBackend::CudaLane::~CudaLane() noexcept { static_cast<void>(Close()); }
void RuntimeBackend::CudaLane::Activate() const { require_cuda(Select(), "cudaSetDevice"); }
void RuntimeBackend::CudaLane::Synchronize() const {
 Activate();
 require_cuda(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream_)), "cudaStreamSynchronize");
}
void RuntimeBackend::CudaLane::Record(const RuntimeContinuation continuation) const {
 Activate();
 try {
  continuation.Invoke(device_, stream_);
  require_cuda(cudaEventRecord(reinterpret_cast<cudaEvent_t>(completion_), reinterpret_cast<cudaStream_t>(stream_)), "cudaEventRecord");
 } catch (...) { RethrowAfterSynchronization(std::current_exception()); }
}
void RuntimeBackend::CudaLane::RethrowAfterSynchronization(std::exception_ptr exception) const {
 const auto settled = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream_));
 if (settled != cudaSuccess) {
  try {
   std::rethrow_exception(exception);
  } catch (const CudaOperationError&) { throw; } catch (...) {
   throw CudaOperationError{static_cast<RuntimeStatus>(settled), "cudaStreamSynchronize"};
  }
 }
 std::rethrow_exception(exception);
}
RuntimeStatus RuntimeBackend::CudaLane::Select() const noexcept { return static_cast<RuntimeStatus>(cudaSetDevice(device_)); }
RuntimeStatus RuntimeBackend::CudaLane::Observe() const noexcept {
 const auto selected = Select();
 return selected == kRuntimeSuccess ? static_cast<RuntimeStatus>(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(completion_))) : selected;
}
RuntimeStatus RuntimeBackend::CudaLane::Retire(const RawSubmission& submission) const noexcept {
 const auto selected = Select();
 if (selected != kRuntimeSuccess) return selected;
 const auto event = reinterpret_cast<cudaEvent_t>(submission.completion_event);
 if (event != nullptr) return static_cast<RuntimeStatus>(cudaEventSynchronize(event));
 if (!submission.stream) return static_cast<RuntimeStatus>(cudaErrorInvalidResourceHandle);
 return static_cast<RuntimeStatus>(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(submission.stream.native_handle)));
}
RuntimeStatus RuntimeBackend::CudaLane::Close() noexcept {
 const auto selected = Select();
 if (selected != kRuntimeSuccess) return selected;
 if (completion_ != 0U) {
  const auto status = cudaEventDestroy(reinterpret_cast<cudaEvent_t>(completion_));
  if (status != cudaSuccess) return static_cast<RuntimeStatus>(status);
  completion_ = 0U;
 }
 if (owns_stream_ && stream_ != 0U) {
  const auto status = cudaStreamDestroy(reinterpret_cast<cudaStream_t>(stream_));
  if (status != cudaSuccess) return static_cast<RuntimeStatus>(status);
 }
 stream_ = 0U;
 open_ = false;
 return kRuntimeSuccess;
}
std::int32_t RuntimeBackend::CudaLane::device() const noexcept { return device_; }
BorrowedCommandStream RuntimeBackend::CudaLane::command_stream() const noexcept { return {.native_handle = stream_, .valid = open_}; }
std::uintptr_t RuntimeBackend::CudaLane::native_stream() const noexcept { return stream_; }
RuntimeBackend::RawSubmission RuntimeBackend::CudaLane::Receipt(const std::size_t output_count) const noexcept {
 return {.device = device_, .stream = command_stream(), .completion_event = completion_, .output_count = output_count};
}
}  // namespace mmltk::backend::ml::runtime
