#include "detail/capture_session_impl.hpp"
#include "src/common/io/event_fd.h"
#include "src/backend/media/capture/capture_session.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/execution_policy.h"
#include <cuda_runtime_api.h>
#include <linux/videodev2.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
namespace {
using AtomicListener = std::atomic<std::shared_ptr<const std::function<void()>>>;
void store_listener(AtomicListener& destination, std::function<void()> listener) {
 std::shared_ptr<const std::function<void()>> stored;
 if (listener) { stored = std::make_shared<const std::function<void()>>(std::move(listener)); }
 destination.store(std::move(stored), std::memory_order_release);
}
void retain_first_failure(mmltk::backend::media::capture::Status* target, mmltk::backend::media::capture::Status candidate) {
 if (target != nullptr && target->ok() && !candidate.ok()) { *target = std::move(candidate); }
}
std::uint64_t allocate_capture_session_id() noexcept {
 static std::atomic<std::uint64_t> source{0};
 std::uint64_t current = source.load(std::memory_order_relaxed);
 while (current != std::numeric_limits<std::uint64_t>::max()) {
  if (source.compare_exchange_weak(current, current + 1U, std::memory_order_relaxed, std::memory_order_relaxed)) { return current + 1U; }
 }
 return 0U;
}
}  // namespace
namespace mmltk::backend::media::capture {
using capture_internal::kPackedRegionFieldLimit;
using capture_internal::MakeErrnoStatus;
using capture_internal::MakeStatus;
using capture_internal::PackRegion;
using capture_internal::UnpackRegion;
struct CaptureSession::OwnerHandle {
 explicit OwnerHandle(CaptureConfig config) : state(std::make_shared<Impl>(std::move(config))) {}
 ~OwnerHandle() {
  if (!thread.joinable()) { return; }
  // Releasing the public owner is safe only after the device-owning
  // thread has published its terminal teardown result. The ordinary
  // path finalizes and joins before destruction; detaching here merely
  // releases an already-terminal thread handle without blocking.
  if (!state->owner_release_permitted()) { std::terminate(); }
  thread.detach();
 }
 std::shared_ptr<Impl> state;
 std::thread thread;
};
CaptureSession::Impl::Impl(CaptureConfig config_in) : config(std::move(config_in)), session_id_(allocate_capture_session_id()) {}
CaptureSession::Impl::~Impl() {
 if (stop_event_fd_ >= 0) {
  ::close(stop_event_fd_);
  stop_event_fd_ = -1;
 }
 if (terminal_event_fd_ >= 0) {
  ::close(terminal_event_fd_);
  terminal_event_fd_ = -1;
 }
 if (completion_event_fd_ >= 0) {
  ::close(completion_event_fd_);
  completion_event_fd_ = -1;
 }
}
CaptureSessionStartResult CaptureSession::Impl::prepare_start() {
 if (!finalized_.load(std::memory_order_acquire)) { return {.status = MakeStatus(StatusCode::kAlreadyRunning, "capture session owner has not been finalized")}; }
 Status status = ValidateConfig();
 if (!status.ok()) { return {.status = std::move(status)}; }
 if (session_id_ == 0U) { return {.status = MakeStatus(StatusCode::kUnsupported, "capture session identity space exhausted")}; }
 status = EnsureEventFds();
 if (!status.ok()) { return {.status = std::move(status)}; }
 const std::uint64_t previous_generation = active_generation_.load(std::memory_order_acquire);
 if (previous_generation == std::numeric_limits<std::uint64_t>::max()) { return {.status = MakeStatus(StatusCode::kUnsupported, "capture session generation space exhausted")}; }
 mmltk::common::io::drain_event_fd(stop_event_fd_);
 mmltk::common::io::drain_event_fd(terminal_event_fd_);
 ResetStats();
 ResetRuntimeState();
 terminal_storage_ = std::make_shared<CaptureStopTerminal>();
 terminal_.store(nullptr, std::memory_order_release);
 {
  std::lock_guard lock(failure_mutex_);
  first_failure_.reset();
 }
 terminal_published_.store(false, std::memory_order_release);
 terminal_delivered_.store(false, std::memory_order_release);
 stop_requested_.store(false, std::memory_order_release);
 camera_fault_.store(false, std::memory_order_release);
 shutdown_.store(false, std::memory_order_release);
 finalized_.store(false, std::memory_order_release);
 active_generation_.store(previous_generation + 1U, std::memory_order_release);
 return {.phase = CaptureSessionStartPhase::Running, .status = Status::Ok(), .identity = active_identity()};
}
void CaptureSession::Impl::cancel_prepared_start() noexcept {
 shutdown_.store(true, std::memory_order_release);
 finalized_.store(true, std::memory_order_release);
 terminal_storage_.reset();
}
void CaptureSession::Impl::capture_owner_main() {
 std::optional<mmltk::common::system::ScopedExecutionPolicy> placement_policy;
 CaptureLoopResult result{};
 bool setup_complete = false;
 try {
  Status status = Status::Ok();
  const auto advance_startup = [this, &status](const auto operation) {
   if (status.ok() && !stop_requested_.load(std::memory_order_acquire)) { status = (this->*operation)(); }
  };
  advance_startup(&Impl::InitializeCuda);
  if (status.ok()) {
   const auto execution = config.execution ? *config.execution
                                           : mmltk::frameworks::gpu::resolve_device_execution(config.cuda_device_index, mmltk::common::system::NumaTopology::Capture(),
                                              mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy()));
   if (execution.device != config.cuda_device_index) throw std::invalid_argument("capture placement device mismatch");
   placement_policy.emplace(mmltk::common::system::ExecutionPolicyRequest{execution.placement.cpus, "capture", 0, execution.placement.numa_node, -10, false});
  }
  advance_startup(&Impl::OpenDevice);
  advance_startup(&Impl::ConfigureDevice);
  if (status.ok() && !stop_requested_.load(std::memory_order_acquire)) { packed_region_.store(PackRegion(NormalizeRegion(config.initial_region)), std::memory_order_release); }
  advance_startup(&Impl::AllocateHostSlots);
  advance_startup(&Impl::QueueAllV4l2Buffers);
  advance_startup(&Impl::StartStreaming);
  if (!status.ok()) {
   result.kind = CaptureStopKind::kStartupFailed;
   result.status = std::move(status);
   result.teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost;
   SetLastError(result.status.message);
  } else if (stop_requested_.load(std::memory_order_acquire)) {
   result = StopRequestedResult();
   if (!streaming_.load(std::memory_order_acquire)) result.teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost;
  } else {
   setup_complete = true;
   running_.store(true, std::memory_order_release);
   NotifyState();
   result = CaptureLoop();
  }
 } catch (const std::exception& error) {
  result.kind = CaptureStopKind::kOwnerFailure;
  result.status = MakeStatus(StatusCode::kInternalError, error.what());
  result.teardown = setup_complete && !camera_fault_.load(std::memory_order_acquire) ? CaptureTeardownDisposition::kRequeueThenStreamOff : CaptureTeardownDisposition::kNoStreamOrDeviceLost;
  SetLastError(result.status.message);
 } catch (...) {
  result.kind = CaptureStopKind::kOwnerFailure;
  result.status = MakeStatus(StatusCode::kInternalError, "unknown capture owner failure");
  result.teardown = setup_complete && !camera_fault_.load(std::memory_order_acquire) ? CaptureTeardownDisposition::kRequeueThenStreamOff : CaptureTeardownDisposition::kNoStreamOrDeviceLost;
  SetLastError(result.status.message);
 }
 running_.store(false, std::memory_order_release);
 CloseFilledAdmission();
 RetainFirstFailure(active_identity(), result.status);
 const Status settlement = SettleSlotsBeforeTeardown(&result.teardown);
 RetainFirstFailure(active_identity(), settlement);
 const Status teardown_status = TeardownSession(result.teardown);
 RetainFirstFailure(active_identity(), teardown_status);
 const Status authoritative_failure = FirstFailure();
 if (!authoritative_failure.ok()) result.status = authoritative_failure;
 if (!result.status.ok() && result.kind == CaptureStopKind::kRequested) {
  result.kind = CaptureStopKind::kOwnerFailure;
  SetLastError(result.status.message);
 }
 PublishStopTerminal(std::move(result));
}
Status CaptureSession::Impl::request_stop(const CaptureSessionIdentity identity) {
 if (finalized_.load(std::memory_order_acquire) || active_generation_.load(std::memory_order_acquire) == 0U) { return MakeStatus(StatusCode::kNotRunning, "capture session has no active owner"); }
 if (!identity.valid() || identity != active_identity()) { return MakeStatus(StatusCode::kInvalidArgument, "capture stop identity does not match the active owner"); }
 if (terminal_published_.load(std::memory_order_acquire)) { return Status::Ok(); }
 stop_requested_.store(true, std::memory_order_release);
 CloseFilledAdmission();
 if (!mmltk::common::io::signal_event_fd(stop_event_fd_)) {
  const Status status = MakeErrnoStatus(StatusCode::kInternalError, "capture stop eventfd write");
  RetainFirstFailure(identity, status);
  SetLastError(status.message);
  static_cast<void>(mmltk::common::io::signal_event_fd(completion_event_fd_));
  return status;
 }
 return Status::Ok();
}
CaptureSessionIdentity CaptureSession::Impl::active_identity() const noexcept {
 return CaptureSessionIdentity{
  .session = session_id_,
  .generation = active_generation_.load(std::memory_order_acquire),
 };
}
int CaptureSession::Impl::stop_terminal_event_fd() const noexcept { return terminal_event_fd_; }
std::shared_ptr<const CaptureStopTerminal> CaptureSession::Impl::try_take_stop_terminal() noexcept {
 if (!terminal_published_.load(std::memory_order_acquire)) { return {}; }
 bool expected = false;
 if (!terminal_delivered_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) { return {}; }
 mmltk::common::io::drain_event_fd(terminal_event_fd_);
 return terminal_.load(std::memory_order_acquire);
}
Status CaptureSession::Impl::validate_finalize(const std::shared_ptr<const CaptureStopTerminal>& terminal) const {
 if (terminal == nullptr) { return MakeStatus(StatusCode::kInvalidArgument, "capture finalization requires a terminal"); }
 const std::shared_ptr<const CaptureStopTerminal> authoritative = terminal_.load(std::memory_order_acquire);
 if (!terminal_published_.load(std::memory_order_acquire) || authoritative == nullptr) { return MakeStatus(StatusCode::kNotReady, "capture owner teardown is not terminal"); }
 if (terminal != authoritative || terminal->identity != active_identity()) { return MakeStatus(StatusCode::kInvalidArgument, "capture terminal does not match the active owner"); }
 if (finalized_.load(std::memory_order_acquire)) { return MakeStatus(StatusCode::kNotRunning, "capture owner was already finalized"); }
 return Status::Ok();
}
void CaptureSession::Impl::mark_finalized() noexcept {
 finalized_.store(true, std::memory_order_release);
 terminal_storage_.reset();
}
bool CaptureSession::Impl::owner_release_permitted() const noexcept { return terminal_published_.load(std::memory_order_acquire); }
Status CaptureSession::Impl::set_capture_region(CaptureRegion region) {
 packed_region_.store(PackRegion(NormalizeRegion(region)), std::memory_order_release);
 return Status::Ok();
}
CaptureRegion CaptureSession::Impl::snapshot_capture_region() const { return UnpackRegion(packed_region_.load(std::memory_order_acquire)); }
CaptureFormatInfo CaptureSession::Impl::snapshot_format() const {
 return CaptureFormatInfo{
  .width = published_width_.load(std::memory_order_acquire),
  .height = published_height_.load(std::memory_order_acquire),
  .bytes_per_line = published_bytes_per_line_.load(std::memory_order_acquire),
 };
}
CaptureStats CaptureSession::Impl::snapshot_stats() const {
 const auto load = [](const auto& counter) { return counter.load(std::memory_order_acquire); };
 CaptureStats stats{};
 stats.queued_v4l2_buffers = load(queued_v4l2_buffers_);
 stats.dequeued_v4l2_buffers = load(dequeued_v4l2_buffers_);
 stats.bytes_captured = load(bytes_captured_);
 stats.filled_frames_published = load(filled_frames_published_);
 stats.h2d_frames_admitted = load(h2d_frames_admitted_);
 stats.h2d_frames_completed = load(h2d_frames_completed_);
 stats.frames_dropped = load(frames_dropped_);
 stats.empty_frames_dropped = load(empty_frames_dropped_);
 stats.replaced_filled_frames = load(replaced_filled_frames_);
 stats.active_occupancy_drops = load(active_occupancy_drops_);
 stats.short_frames = load(short_frames_);
 stats.sequence_gaps = load(sequence_gaps_);
 stats.requeue_failures = load(requeue_failures_);
 stats.running = load(running_);
 return stats;
}
std::string CaptureSession::Impl::last_error() const {
 std::lock_guard<std::mutex> lock(error_mutex_);
 return last_error_;
}
Status CaptureSession::Impl::ValidateConfig() const {
 if (config.width == 0U || config.height == 0U) { return MakeStatus(StatusCode::kInvalidArgument, "width and height must be non-zero"); }
 if (config.width > kPackedRegionFieldLimit || config.height > kPackedRegionFieldLimit) { return MakeStatus(StatusCode::kUnsupported, "dimensions above 65535 are not supported"); }
 if (config.fps == 0U) { return MakeStatus(StatusCode::kInvalidArgument, "fps must be non-zero"); }
 if (config.v4l2_buffer_count == 0U) { return MakeStatus(StatusCode::kInvalidArgument, "v4l2_buffer_count must be non-zero"); }
 return Status::Ok();
}
void CaptureSession::Impl::ResetRuntimeState() {
 streaming_.store(false, std::memory_order_release);
 actual_v4l2_buffer_count_ = 0;
 bytes_per_line_ = 0;
 size_image_ = 0;
 published_width_.store(config.width, std::memory_order_release);
 published_height_.store(config.height, std::memory_order_release);
 published_bytes_per_line_.store(0U, std::memory_order_release);
 {
  std::lock_guard lock(filled_mutex_);
  replaceable_filled_index_ = -1;
 }
 last_sequence_.reset();
 next_frame_id_ = 1;
 ClearLastError();
}
CaptureRegion CaptureSession::Impl::NormalizeRegion(CaptureRegion region) const {
 const std::uint32_t width = published_width_.load(std::memory_order_acquire);
 const std::uint32_t height = published_height_.load(std::memory_order_acquire);
 if (width == 0U || height == 0U) { return CaptureRegion{}; }
 region.x = std::min(region.x, width - 1U);
 region.y = std::min(region.y, height - 1U);
 const std::uint32_t max_width = width - region.x;
 const std::uint32_t max_height = height - region.y;
 region.width = region.width == 0U ? max_width : std::min(region.width, max_width);
 region.height = region.height == 0U ? max_height : std::min(region.height, max_height);
 return region;
}
Status CaptureSession::Impl::EnsureEventFds() {
 if (stop_event_fd_ < 0) {
  stop_event_fd_ = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_event_fd_ < 0) { return MakeErrnoStatus(StatusCode::kInternalError, "capture stop eventfd"); }
 }
 if (terminal_event_fd_ < 0) {
  terminal_event_fd_ = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (terminal_event_fd_ < 0) { return MakeErrnoStatus(StatusCode::kInternalError, "capture terminal eventfd"); }
 }
 if (completion_event_fd_ < 0) {
  completion_event_fd_ = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (completion_event_fd_ < 0) { return MakeErrnoStatus(StatusCode::kInternalError, "capture completion eventfd"); }
 }
 return Status::Ok();
}
Status CaptureSession::Impl::TeardownSession(const CaptureTeardownDisposition teardown) {
 Status status = Status::Ok();
 switch (teardown) {
  case CaptureTeardownDisposition::kRequeueThenStreamOff:
  case CaptureTeardownDisposition::kOwnerRetainThenStreamOff:
   retain_first_failure(&status, StopStreaming());
   // Closing the device after STREAMOFF also makes a failed
   // STREAMOFF unable to retain access to USERPTR storage.
   retain_first_failure(&status, CloseDevice());
   retain_first_failure(&status, DestroyHostSlots());
   break;
  case CaptureTeardownDisposition::kNoStreamOrDeviceLost:
   streaming_.store(false, std::memory_order_release);
   retain_first_failure(&status, CloseDevice());
   retain_first_failure(&status, DestroyHostSlots());
   break;
 }
 return status;
}
void CaptureSession::Impl::PublishStopTerminal(CaptureLoopResult result) {
 if (terminal_published_.load(std::memory_order_acquire)) { return; }
 terminal_storage_->identity = active_identity();
 terminal_storage_->kind = result.kind;
 terminal_storage_->status = std::move(result.status);
 terminal_storage_->final_stats = snapshot_stats();
 terminal_storage_->streamoff_skipped = result.teardown == CaptureTeardownDisposition::kNoStreamOrDeviceLost;
 NotifyFilledFramePublished();
 NotifyState();
 std::shared_ptr<const CaptureStopTerminal> immutable = terminal_storage_;
 terminal_.store(std::move(immutable), std::memory_order_release);
 terminal_published_.store(true, std::memory_order_release);
 static_cast<void>(mmltk::common::io::signal_event_fd(terminal_event_fd_));
}
void CaptureSession::Impl::CloseFilledAdmission() noexcept { shutdown_.store(true, std::memory_order_release); }
void CaptureSession::Impl::ReportCameraFault(const std::string& message) {
 camera_fault_.store(true, std::memory_order_release);
 SetLastError(message);
 static_cast<void>(mmltk::common::io::signal_event_fd(stop_event_fd_));
}
void CaptureSession::Impl::ResetStats() {
 queued_v4l2_buffers_.store(0, std::memory_order_release);
 dequeued_v4l2_buffers_.store(0, std::memory_order_release);
 bytes_captured_.store(0, std::memory_order_release);
 filled_frames_published_.store(0, std::memory_order_release);
 h2d_frames_admitted_.store(0, std::memory_order_release);
 h2d_frames_completed_.store(0, std::memory_order_release);
 frames_dropped_.store(0, std::memory_order_release);
 empty_frames_dropped_.store(0, std::memory_order_release);
 replaced_filled_frames_.store(0, std::memory_order_release);
 active_occupancy_drops_.store(0, std::memory_order_release);
 short_frames_.store(0, std::memory_order_release);
 sequence_gaps_.store(0, std::memory_order_release);
 requeue_failures_.store(0, std::memory_order_release);
}
void CaptureSession::Impl::ClearLastError() {
 bool changed = false;
 {
  std::lock_guard<std::mutex> lock(error_mutex_);
  changed = !last_error_.empty();
  last_error_.clear();
 }
 if (changed) { NotifyState(); }
}
void CaptureSession::Impl::SetLastError(const std::string& message) {
 bool changed = false;
 {
  std::lock_guard<std::mutex> lock(error_mutex_);
  changed = last_error_ != message;
  last_error_ = message;
 }
 if (changed) { NotifyState(); }
}
void CaptureSession::Impl::set_state_listener(std::function<void()> listener) { store_listener(state_listener_, std::move(listener)); }
void CaptureSession::Impl::set_filled_frame_listener(std::function<void()> listener) { store_listener(filled_frame_listener_, std::move(listener)); }
void CaptureSession::Impl::NotifyState() const noexcept {
 const std::shared_ptr<const std::function<void()>> listener = state_listener_.load(std::memory_order_acquire);
 if (listener == nullptr || !*listener) { return; }
 try {
  (*listener)();
 } catch (...) { return; }
}
CaptureSession::CaptureSession(CaptureConfig config) : owner_(std::make_unique<OwnerHandle>(std::move(config))) {}
CaptureSession::~CaptureSession() = default;
CaptureSession::CaptureSession(CaptureSession&&) noexcept = default;
CaptureSession& CaptureSession::operator=(CaptureSession&&) noexcept = default;
CaptureSessionStartResult CaptureSession::start() {
 if (owner_->thread.joinable()) { return {.status = MakeStatus(StatusCode::kAlreadyRunning, "capture owner thread has not been finalized")}; }
 CaptureSessionStartResult admission{};
 try {
  admission = owner_->state->prepare_start();
 } catch (const std::exception& error) { return {.status = MakeStatus(StatusCode::kInternalError, std::string("failed to prepare capture owner: ") + error.what())}; } catch (...) {
  return {.status = MakeStatus(StatusCode::kInternalError, "failed to prepare capture owner")};
 }
 if (!admission.running()) { return admission; }
 try {
  const std::shared_ptr<Impl> state = owner_->state;
  owner_->thread = std::thread([state] { state->capture_owner_main(); });
 } catch (const std::exception& error) {
  owner_->state->cancel_prepared_start();
  return {.status = MakeStatus(StatusCode::kInternalError, std::string("failed to start capture owner thread: ") + error.what())};
 } catch (...) {
  owner_->state->cancel_prepared_start();
  return {.status = MakeStatus(StatusCode::kInternalError, "failed to start capture owner thread")};
 }
 return admission;
}
Status CaptureSession::request_stop(const CaptureSessionIdentity identity) { return owner_->state->request_stop(identity); }
int CaptureSession::stop_terminal_event_fd() const noexcept { return owner_ != nullptr ? owner_->state->stop_terminal_event_fd() : -1; }
std::shared_ptr<const CaptureStopTerminal> CaptureSession::try_take_stop_terminal() noexcept { return owner_ != nullptr ? owner_->state->try_take_stop_terminal() : nullptr; }
Status CaptureSession::finalize_stop(const std::shared_ptr<const CaptureStopTerminal>& terminal) {
 Status status = owner_->state->validate_finalize(terminal);
 if (!status.ok()) { return status; }
 if (!owner_->thread.joinable()) { return MakeStatus(StatusCode::kNotRunning, "capture owner thread is not joinable"); }
 if (owner_->thread.get_id() == std::this_thread::get_id()) { return MakeStatus(StatusCode::kNotReady, "capture owner cannot finalize itself"); }
 try {
  owner_->thread.join();
 } catch (const std::exception& error) { return MakeStatus(StatusCode::kInternalError, std::string("failed to finalize capture owner thread: ") + error.what()); }
 owner_->state->mark_finalized();
 return Status::Ok();
}
Status CaptureSession::set_capture_region(CaptureRegion region) { return owner_->state->set_capture_region(region); }
CaptureRegion CaptureSession::snapshot_capture_region() const { return owner_->state->snapshot_capture_region(); }
CaptureFormatInfo CaptureSession::snapshot_format() const { return owner_->state->snapshot_format(); }
FilledCaptureSlotLease CaptureSession::MakeFilledSlotLease(const CaptureSessionIdentity identity, const std::uint32_t slot, const std::uint64_t sequence, const std::uint8_t* const data,
 const std::size_t bytes, const std::size_t stride_bytes, const std::uint32_t pixel_format, const CaptureRegion region, const std::uint64_t capture_ns, const bool short_frame) noexcept {
 return FilledCaptureSlotLeaseAuthority::Create(identity, slot, sequence, data, bytes, stride_bytes, pixel_format, region, capture_ns, short_frame);
}
void CaptureSession::ConsumeFilledSlotLease(FilledCaptureSlotLease& lease) noexcept { FilledCaptureSlotLeaseAuthority::Consume(lease); }
Status CaptureSession::try_take_filled(FilledCaptureSlotLease* out_lease) { return owner_->state->try_take_filled(out_lease); }
Status CaptureSession::mark_h2d_completion_pending(const FilledCaptureSlotLease& lease) { return owner_->state->mark_h2d_completion_pending(lease); }
Status CaptureSession::return_after_h2d(FilledCaptureSlotLease&& lease) noexcept { return owner_->state->return_after_h2d(std::move(lease)); }
Status CaptureSession::report_failure(const CaptureSessionIdentity identity, Status failure) noexcept { return owner_->state->report_failure(identity, std::move(failure)); }
CaptureStats CaptureSession::snapshot_stats() const { return owner_->state->snapshot_stats(); }
std::string CaptureSession::last_error() const { return owner_->state->last_error(); }
void CaptureSession::set_state_listener(std::function<void()> listener) { owner_->state->set_state_listener(std::move(listener)); }
void CaptureSession::set_filled_frame_listener(std::function<void()> listener) { owner_->state->set_filled_frame_listener(std::move(listener)); }
}  // namespace mmltk::backend::media::capture
