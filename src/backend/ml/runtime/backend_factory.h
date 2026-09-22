#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace mmltk::backend::ml::runtime {
using RuntimeStatus = std::int32_t;
inline constexpr RuntimeStatus kRuntimeSuccess = 0;
inline constexpr std::size_t kMaximumRuntimeRank = 8U;
inline constexpr std::size_t kMaximumRuntimeOutputs = 16U;
enum class RuntimeBackendKind : std::uint8_t {
 Onnx,
 TensorRt,
};
enum class RuntimeElementType : std::uint8_t {
 Float16,
 Float32,
 Int32,
 Int64,
 Bool,
};
struct BorrowedCommandStream final {
 std::uintptr_t native_handle = 0U;
 bool valid = false;
 [[nodiscard]] explicit operator bool() const noexcept { return valid; }
 bool operator==(const BorrowedCommandStream&) const noexcept = default;
};
class CudaOperationError final : public std::runtime_error {
public:
 CudaOperationError(RuntimeStatus status, std::string_view operation);
 [[nodiscard]] RuntimeStatus status() const noexcept { return status_; }

private:
 RuntimeStatus status_ = kRuntimeSuccess;
};
struct RuntimeShape final {
 std::uint8_t rank = 0U;
 std::array<std::int64_t, kMaximumRuntimeRank> extents{};
};
struct RuntimeTensorDescriptor final {
 std::string name;
 RuntimeShape shape{};
 RuntimeElementType element_type = RuntimeElementType::Float32;
};
struct RuntimeModelInfo final {
 std::filesystem::path model_path;
 RuntimeTensorDescriptor input;
 std::array<RuntimeTensorDescriptor, kMaximumRuntimeOutputs> outputs{};
 std::size_t output_count = 0U;
};
struct RuntimeTensorBuffer final {
 void* device_data = nullptr;
 std::size_t capacity_bytes = 0U;
 RuntimeShape shape{};
 RuntimeElementType element_type = RuntimeElementType::Float32;
};
// A submission-scoped, non-owning enqueue hook.  The context must remain valid
// only for the synchronous duration of RuntimeBackend::Run.
struct RuntimeContinuation final {
 using Enqueue = void (*)(void*, std::int32_t, std::uintptr_t);
 void* context = nullptr;
 Enqueue enqueue = nullptr;
 void Invoke(const std::int32_t device, const std::uintptr_t stream) const {
  if (enqueue != nullptr) { enqueue(context, device, stream); }
 }
};
struct RuntimeOutputBinding final {
 using Bind = void (*)(void*, std::int32_t, std::uintptr_t, std::span<RuntimeTensorBuffer>);
 void* context = nullptr;
 Bind bind = nullptr;
 void Invoke(const std::int32_t device, const std::uintptr_t stream, const std::span<RuntimeTensorBuffer> outputs) const {
  if (bind != nullptr) { bind(context, device, stream, outputs); }
 }
};
class RuntimeBackend;
class RuntimeSubmission final {
public:
 RuntimeSubmission() noexcept = default;
 RuntimeSubmission(const RuntimeSubmission&) = delete;
 RuntimeSubmission& operator=(const RuntimeSubmission&) = delete;
 RuntimeSubmission(RuntimeSubmission&& other) noexcept
     : owner_(std::move(other.owner_)),
       generation_(std::exchange(other.generation_, 0U)),
       device_(std::exchange(other.device_, -1)),
       stream_(std::exchange(other.stream_, {})),
       completion_event_(std::exchange(other.completion_event_, 0U)),
       output_count_(std::exchange(other.output_count_, 0U)) {}
 RuntimeSubmission& operator=(RuntimeSubmission&&) = delete;
 ~RuntimeSubmission() noexcept;
 void Abandon() noexcept;
 [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(owner_); }
 [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
 [[nodiscard]] std::int32_t device() const noexcept { return device_; }
 [[nodiscard]] BorrowedCommandStream stream() const noexcept { return stream_; }
 [[nodiscard]] std::uintptr_t completion_event() const noexcept { return completion_event_; }
 [[nodiscard]] std::size_t output_count() const noexcept { return output_count_; }

private:
 RuntimeSubmission(
  std::shared_ptr<RuntimeBackend> owner, std::uint64_t generation, std::int32_t device, BorrowedCommandStream stream, std::uintptr_t completion_event, std::size_t output_count) noexcept
     : owner_(std::move(owner)), generation_(generation), device_(device), stream_(stream), completion_event_(completion_event), output_count_(output_count) {}
 std::shared_ptr<RuntimeBackend> owner_;
 std::uint64_t generation_ = 0U;
 std::int32_t device_ = -1;
 BorrowedCommandStream stream_{};
 std::uintptr_t completion_event_ = 0U;
 std::size_t output_count_ = 0U;
 void Disarm() noexcept {
  owner_.reset();
  generation_ = 0U;
  device_ = -1;
  stream_ = {};
  completion_event_ = 0U;
  output_count_ = 0U;
 }
 friend class RuntimeBackend;
};
struct RuntimeBackendOptions final {
 RuntimeBackendKind kind = RuntimeBackendKind::Onnx;
 std::filesystem::path model_path;
 std::filesystem::path save_compiled_model_path;
 std::int32_t device = 0;
 BorrowedCommandStream command_stream{};
 bool allow_fp16 = true;
};
class RuntimeBackend : public std::enable_shared_from_this<RuntimeBackend> {
public:
 virtual ~RuntimeBackend();
 RuntimeBackend(const RuntimeBackend&) = delete;
 RuntimeBackend& operator=(const RuntimeBackend&) = delete;
 [[nodiscard]] virtual const RuntimeModelInfo& model_info() const noexcept = 0;
 [[nodiscard]] std::int32_t device() const noexcept;
 [[nodiscard]] BorrowedCommandStream command_stream() const noexcept;
 [[nodiscard]] RuntimeSubmission Run(
  const RuntimeTensorBuffer& input, std::span<RuntimeTensorBuffer> outputs, const RuntimeOutputBinding output_binding, const RuntimeContinuation continuation, std::shared_ptr<void> retained_storage);
 void ReleaseAfterCompletion(RuntimeSubmission&& submission);
 [[nodiscard]] RuntimeStatus Close() noexcept;
 [[nodiscard]] virtual std::shared_ptr<RuntimeBackend> MakeLane() const = 0;
 virtual void SaveCompiledModel(const std::filesystem::path& path) const = 0;

protected:
 RuntimeBackend(std::int32_t device, BorrowedCommandStream command_stream);
 struct RawSubmission final {
  std::int32_t device = -1;
  BorrowedCommandStream stream{};
  std::uintptr_t completion_event = 0U;
  std::size_t output_count = 0U;
 };
 class CudaLane final {
 public:
  CudaLane(std::int32_t device, BorrowedCommandStream command_stream);
  ~CudaLane() noexcept;
  CudaLane(const CudaLane&) = delete;
  CudaLane& operator=(const CudaLane&) = delete;
  void Activate() const;
  void Synchronize() const;
  void Record(RuntimeContinuation continuation) const;
  [[noreturn]] void RethrowAfterSynchronization(std::exception_ptr exception) const;
  [[nodiscard]] RuntimeStatus Select() const noexcept;
  [[nodiscard]] RuntimeStatus Observe() const noexcept;
  [[nodiscard]] RuntimeStatus Retire(const RawSubmission& submission) const noexcept;
  [[nodiscard]] RuntimeStatus Close() noexcept;
  [[nodiscard]] std::int32_t device() const noexcept;
  [[nodiscard]] BorrowedCommandStream command_stream() const noexcept;
  [[nodiscard]] std::uintptr_t native_stream() const noexcept;
  [[nodiscard]] RawSubmission Receipt(std::size_t output_count) const noexcept;

 private:
  std::int32_t device_ = -1;
  std::uintptr_t stream_ = 0U;
  std::uintptr_t completion_ = 0U;
  bool owns_stream_ = false;
  bool open_ = false;
 };
 [[nodiscard]] CudaLane& cuda_lane() noexcept { return lane_; }
 [[nodiscard]] const CudaLane& cuda_lane() const noexcept { return lane_; }
 [[nodiscard]] virtual RawSubmission Submit(const RuntimeTensorBuffer& input, std::span<RuntimeTensorBuffer> outputs, RuntimeContinuation continuation) = 0;
 virtual void ReleaseBackendResources() noexcept = 0;

private:
 enum class LaneState : std::uint8_t {
  Idle,
  Issuing,
  Active,
  Settling,
  Closing,
  Closed,
 };
 struct ActiveSubmission final {
  RawSubmission receipt{};
  std::shared_ptr<void> retained_storage;
 };
 [[nodiscard]] RuntimeStatus ObserveCompletion() noexcept;
 [[nodiscard]] RuntimeStatus RetireIssuedSubmission(const RawSubmission& submission) noexcept;
 [[nodiscard]] RuntimeStatus CloseResources() noexcept;
 static void ValidateRuntimeTensorSet(const RuntimeModelInfo& model, const RuntimeTensorBuffer& input, std::span<RuntimeTensorBuffer> outputs);
 [[nodiscard]] std::uint64_t ClaimIssuing();
 [[nodiscard]] RuntimeStatus SettleActive(bool observe_completion, LaneState succeeded, LaneState failed) noexcept;
 void AbandonSubmission(const std::uint64_t generation) noexcept {
  std::lock_guard lock(state_mutex_);
  if (state_ == LaneState::Active && generation == generation_ && active_submission_.has_value()) { static_cast<void>(SettleActive(false, LaneState::Idle, LaneState::Active)); }
 }
 mutable std::mutex state_mutex_;
 CudaLane lane_;
 LaneState state_ = LaneState::Idle;
 std::uint64_t generation_ = 0U;
 std::optional<ActiveSubmission> active_submission_;
 friend class RuntimeSubmission;
};
inline RuntimeSubmission::~RuntimeSubmission() noexcept { Abandon(); }
inline void RuntimeSubmission::Abandon() noexcept {
 if (owner_) {
  owner_->AbandonSubmission(generation_);
  Disarm();
 }
}
[[nodiscard]] std::shared_ptr<RuntimeBackend> make_runtime_backend(const RuntimeBackendOptions& options);
}  // namespace mmltk::backend::ml::runtime
namespace mmltk::backend::ml::runtime {
[[nodiscard]] std::size_t validate_runtime_tensor_buffer(const RuntimeTensorDescriptor& descriptor, const RuntimeTensorBuffer& buffer, const RuntimeShape* resolved_shape = nullptr);
[[nodiscard]] std::shared_ptr<RuntimeBackend> make_onnx_runtime_backend(const RuntimeBackendOptions& options);
inline void RuntimeBackend::ValidateRuntimeTensorSet(const RuntimeModelInfo& model, const RuntimeTensorBuffer& input, const std::span<RuntimeTensorBuffer> outputs) {
 if (model.input.name.empty() || model.output_count > kMaximumRuntimeOutputs || outputs.size() != model.output_count) { throw std::invalid_argument("runtime tensor-set contract mismatch"); }
 static_cast<void>(validate_runtime_tensor_buffer(model.input, input));
 for (std::size_t index = 0U; index < outputs.size(); ++index) { static_cast<void>(validate_runtime_tensor_buffer(model.outputs[index], outputs[index])); }
}
}  // namespace mmltk::backend::ml::runtime
