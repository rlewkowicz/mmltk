#include "detail/live_session_owner.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
namespace mmltk::backend::media::live {
namespace capture = mmltk::backend::media::capture;
namespace gpu = mmltk::frameworks::gpu;
namespace runtime = mmltk::backend::ml::runtime;
LiveMediaDataPlane::LiveMediaDataPlane(LiveDataPlaneConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
LiveMediaDataPlane::~LiveMediaDataPlane() = default;
capture::CaptureSessionStartResult LiveMediaDataPlane::start(const LiveAnalysisProviderFactory provider_factory) { return impl_->start(provider_factory); }
capture::Status LiveMediaDataPlane::stop() noexcept { return impl_->stop(); }
std::shared_ptr<const LivePhysicalTerminal> LiveMediaDataPlane::admitted_run_terminal() const noexcept { return impl_->admitted_run_terminal(); }
capture::Status LiveMediaDataPlane::first_failure() const { return impl_->first_failure(); }
std::optional<PhysicalFrameRevision> LiveMediaDataPlane::newest_revision() const noexcept { return impl_->newest_revision(); }
bool LiveMediaDataPlane::try_acquire_output(const PhysicalFrameRevision revision, LiveCompositeOutputLease* const output) {
 return impl_->try_acquire_output(revision, output);
}
bool LiveMediaDataPlane::request_raw_readback(LiveRawFrameReadback request) { return impl_->request_raw_readback(std::move(request)); }
void LiveMediaDataPlane::set_revision_listener(const LiveRevisionListener listener) { impl_->set_revision_listener(listener); }
void LiveMediaDataPlane::set_admitted_run_terminal_listener(const LiveAdmittedRunTerminalListener listener) {
 impl_->set_admitted_run_terminal_listener(listener);
}
void LiveMediaDataPlane::publish_manual_overlay(ManualOverlayDocumentSnapshot snapshot) { impl_->publish_manual_overlay(std::move(snapshot)); }
void LiveMediaDataPlane::clear_manual_overlay(const std::uint32_t width, const std::uint32_t height) { impl_->clear_manual_overlay(width, height); }
LiveDataPlaneConfig LiveMediaDataPlane::Impl::Validate(LiveDataPlaneConfig config) {
 if (!config.resource_worker.valid() || config.capture.cuda_device_index < 0 || config.ingress_slots == 0U || config.fanout_slots == 0U ||
     config.analysis_slots == 0U || config.composite_slots == 0U || config.analysis_regions == 0U || config.manual_overlay_slots == 0U ||
     config.maximum_manual_instances == 0U || !config.manual_overlay_uploads.valid())
  throw std::invalid_argument("Live data plane requires a worker, device, and fixed storage");
 if (!config.capture.execution)
  config.capture.execution = gpu::resolve_device_execution(config.capture.cuda_device_index, mmltk::common::system::NumaTopology::Capture(),
                                                           mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy()));
 if (config.capture.execution->device != config.capture.cuda_device_index) throw std::invalid_argument("Live placement device mismatch");
 return config;
}
LiveMediaDataPlane::Impl::Impl(LiveDataPlaneConfig config)
    : config_(Validate(std::move(config))), output_callbacks_(config_.composite_slots, this, &Impl::WakeOutputCallback) {}
LiveMediaDataPlane::Impl::~Impl() { static_cast<void>(stop()); }
LiveMediaDataPlane::Impl::PhysicalResources::PhysicalResources(Impl& owner_in, const LiveDataPlaneConfig& config)
    : owner(owner_in),
      commands(config.resource_worker),
      cuda(commands.binding(), gpu::CudaDeviceOwner{.device = config.capture.cuda_device_index, .context = &owner, .record_failure = &Impl::RecordCudaFailure}),
      ingress(config.capture, config.ingress_slots, cuda),
      fanout(ingress, config.fanout_slots, config.capture.width, config.capture.height, cuda),
      analyzer(fanout, config.analysis_slots, config.analysis_regions, config.capture.width, config.capture.height, cuda),
      manual_overlay(owner.manual_document_, config.manual_overlay_slots, config.capture.width, config.capture.height, config.maximum_manual_instances,
                     config.manual_overlay_uploads, cuda),
      compositor(fanout, &analyzer, &manual_overlay, owner.completed_frames_, config.composite_slots, config.capture.width, config.capture.height, cuda) {
 ingress.set_ingestion_wake([this] { owner.wake(); });
 ingress.set_ready_listener([this] { owner.wake(); });
 ingress.set_failure_listener([this](capture::Status failure) { owner.record_failure(std::move(failure)); });
 fanout.set_ready_listener([this] { owner.wake(); });
 fanout.set_raw_readback_listener([this] { owner.wake(); });
 analyzer.set_ready_listener([this] { owner.wake(); });
 manual_overlay.set_ready_listener([this] { owner.wake(); });
 compositor.set_completion_listener([this] { owner.wake(); });
 compositor.set_revision_listener([this] { owner.publish_revision(); });
 auto scope = cuda.scope();
 if (!scope || scope.Record(cudaStreamCreateWithFlags(&analysis_stream, cudaStreamNonBlocking)) != cudaSuccess)
  throw std::runtime_error("create Live canonical analysis command stream");
}
LiveMediaDataPlane::Impl::PhysicalResources::~PhysicalResources() { settle(); }
capture::CaptureSessionStartResult LiveMediaDataPlane::Impl::PhysicalResources::start(const LiveAnalysisProviderFactory provider_factory) {
 std::shared_ptr<runtime::AnalysisProvider> provider;
 if (provider_factory.valid()) {
  provider = provider_factory({.native_handle = reinterpret_cast<std::uintptr_t>(analysis_stream), .valid = analysis_stream != nullptr});
  if (provider == nullptr) throw std::runtime_error("Live analysis provider construction failed");
 }
 analyzer.set_provider(std::move(provider));
 analyzer.start();
 manual_overlay.start();
 fanout.start();
 compositor.start();
 auto result = ingress.start();
 started = result.has_custody();
 if (!result.running()) close_admission();
 return result;
}
void LiveMediaDataPlane::Impl::PhysicalResources::close_admission() noexcept {
 compositor.close_admission();
 manual_overlay.close_admission();
 analyzer.close_admission();
 fanout.close_admission();
}
void LiveMediaDataPlane::Impl::PhysicalResources::settle() noexcept {
 if (resources_settled) return;
 std::optional<RawReadbackDelivery> raw_delivery;
 bool raw_active = false;
 {
  std::lock_guard lock(owner.lifecycle_);
  if (owner.raw_readback_.phase == RawReadbackPhase::Pending) {
   owner.raw_readback_.phase = RawReadbackPhase::Delivered;
   const LiveFrameId frame = owner.raw_readback_.request->frame;
   raw_delivery = RawReadbackDelivery{std::move(*owner.raw_readback_.request), {frame, nullptr, 0U, false}};
   owner.raw_readback_.request.reset();
  } else {
   raw_active = owner.raw_readback_.phase == RawReadbackPhase::Active;
  }
 }
 std::optional<LiveRawFrameReadbackResult> raw_result;
 compositor.stop();
 manual_overlay.stop();
 analyzer.stop();
 analyzer.set_provider({});
 if (raw_active) raw_result = fanout.settle_raw_readback();
 fanout.stop();
 ingress.settle();
 {
  auto owner_scope = cuda.scope();
  const cudaError_t release = retire_live_local_handle(analysis_stream, static_cast<cudaStream_t>(nullptr), [&owner_scope](const cudaStream_t stream) noexcept {
   if (!owner_scope) return cudaErrorNotPermitted;
   const cudaError_t synchronized = owner_scope.Record(cudaStreamSynchronize(stream));
   const cudaError_t destroyed = owner_scope.Record(cudaStreamDestroy(stream));
   return synchronized != cudaSuccess ? synchronized : destroyed;
  });
  cuda.Record(release);
 }
 if (raw_active && !raw_result.has_value()) {
  LiveFrameId frame{};
  {
   std::lock_guard lock(owner.lifecycle_);
   if (owner.raw_readback_.request.has_value()) frame = owner.raw_readback_.request->frame;
  }
  raw_result = LiveRawFrameReadbackResult{frame, nullptr, 0U, false};
 }
 if (raw_result.has_value()) raw_delivery = owner.finish_raw_readback(std::move(*raw_result));
 owner.deliver(std::move(raw_delivery));
 started = false;
 resources_settled = true;
}
bool LiveMediaDataPlane::Impl::PhysicalResources::drain() {
 bool progressed = false;
 std::optional<RawReadbackDelivery> raw_delivery;
 std::optional<LiveRawFrameReadbackWork> raw_work;
 {
  std::lock_guard lock(owner.lifecycle_);
  if (owner.raw_readback_.phase == RawReadbackPhase::Pending) {
   if (owner.phase_ == PhysicalPhase::Running && !owner.stop_requested_ && owner.first_failure_.ok()) {
    owner.raw_readback_.phase = RawReadbackPhase::Active;
    raw_work = LiveRawFrameReadbackWork{owner.raw_readback_.request->frame, owner.raw_readback_.request->destination.size()};
   } else {
    owner.raw_readback_.phase = RawReadbackPhase::Delivered;
    const LiveFrameId frame = owner.raw_readback_.request->frame;
    raw_delivery = RawReadbackDelivery{std::move(*owner.raw_readback_.request), {frame, nullptr, 0U, false}};
    owner.raw_readback_.request.reset();
   }
  }
 }
 std::optional<LiveRawFrameReadbackResult> raw_result;
 {
  auto owner_scope = cuda.scope();
  if (!owner_scope) {
   if (raw_work.has_value()) raw_result = LiveRawFrameReadbackResult{raw_work->frame, nullptr, 0U, false};
  } else {
   raw_result = fanout.take_raw_readback_result();
   if (raw_result.has_value()) progressed = true;
   if (raw_work.has_value()) {
    if (fanout.begin_raw_readback(*raw_work)) {
     progressed = true;
    } else {
     raw_result = LiveRawFrameReadbackResult{raw_work->frame, nullptr, 0U, false};
    }
   }
   if (compositor.drain_completions()) progressed = true;
   if (analyzer.drain_releases()) progressed = true;
   if (manual_overlay.render_pending()) progressed = true;
   while (ingress.ingest_next()) progressed = true;
   while (fanout.process_latest()) progressed = true;
   while (analyzer.process_latest()) progressed = true;
   while (compositor.process_latest()) progressed = true;
  }
 }
 if (raw_result.has_value()) raw_delivery = owner.finish_raw_readback(std::move(*raw_result));
 if (raw_delivery.has_value()) progressed = true;
 owner.deliver(std::move(raw_delivery));
 return progressed;
}
bool LiveMediaDataPlane::Impl::PhysicalResources::settled() const noexcept {
 std::lock_guard lock(owner.lifecycle_);
 return resources_settled && compositor.settled() && owner.output_callbacks_.idle() && owner.raw_readback_.phase == RawReadbackPhase::Idle;
}
capture::CaptureSessionStartResult LiveMediaDataPlane::Impl::start(const LiveAnalysisProviderFactory provider_factory) {
 std::unique_lock lock(lifecycle_);
 if (ingestion_thread_.joinable() || phase_ == PhysicalPhase::Starting || phase_ == PhysicalPhase::Running || phase_ == PhysicalPhase::Closing) {
  return {capture::CaptureSessionStartPhase::NoCustody, {capture::StatusCode::kAlreadyRunning, "Live physical data plane is already active"}, {}};
 }
 pending_provider_factory_ = provider_factory;
 completed_frames_.clear();
 start_result_ = {};
 terminal_.reset();
 first_failure_ = capture::Status::Ok();
 phase_ = PhysicalPhase::Starting;
 startup_ready_ = false;
 wake_pending_ = true;
 stop_requested_ = false;
 close_issued_ = false;
 failure_reported_ = false;
 raw_readback_ = {};
 ingestion_thread_ = std::jthread([this](const std::stop_token stop) { run(stop); });
 wake_condition_.wait(lock, [this] { return startup_ready_; });
 return start_result_;
}
capture::Status LiveMediaDataPlane::Impl::stop() noexcept {
 bool owner_thread = false;
 {
  std::lock_guard lock(lifecycle_);
  if (!ingestion_thread_.joinable()) {
   if (terminal_.has_value()) return terminal_->status();
   if (!first_failure_.ok()) return first_failure_;
   return capture::Status::Ok();
  }
  stop_requested_ = true;
  wake_pending_ = true;
  if (phase_ == PhysicalPhase::Starting || phase_ == PhysicalPhase::Running) phase_ = PhysicalPhase::Closing;
  owner_thread = ingestion_thread_.get_id() == std::this_thread::get_id();
 }
 wake_condition_.notify_all();
 if (owner_thread) {
  std::lock_guard lock(lifecycle_);
  if (terminal_.has_value()) return terminal_->status();
  return !first_failure_.ok() ? first_failure_ : capture::Status::Ok();
 }
 ingestion_thread_.join();
 std::lock_guard lock(lifecycle_);
 if (terminal_.has_value()) return terminal_->status();
 if (!first_failure_.ok()) return first_failure_;
 return capture::Status::Ok();
}
std::shared_ptr<const LivePhysicalTerminal> LiveMediaDataPlane::Impl::admitted_run_terminal() const noexcept {
 std::lock_guard lock(lifecycle_);
 if (!terminal_.has_value()) return nullptr;
 try {
  return std::make_shared<const LivePhysicalTerminal>(*terminal_);
 } catch (...) { return nullptr; }
}
capture::Status LiveMediaDataPlane::Impl::first_failure() const {
 std::lock_guard lock(lifecycle_);
 if (terminal_.has_value()) return terminal_->status();
 return first_failure_;
}
std::optional<PhysicalFrameRevision> LiveMediaDataPlane::Impl::newest_revision() const noexcept {
 std::lock_guard lock(lifecycle_);
 const PhysicalFrameRevision revision = completed_frames_.snapshot();
 return phase_ == PhysicalPhase::Running && revision.valid() ? std::optional<PhysicalFrameRevision>{revision} : std::nullopt;
}
bool LiveMediaDataPlane::Impl::try_acquire_output(const PhysicalFrameRevision revision, LiveCompositeOutputLease* output) {
 std::unique_lock lock(lifecycle_);
 if (phase_ != PhysicalPhase::Running || !resources_.has_value() || !output_callbacks_.acquire()) return false;
 if (resources_->compositor.try_acquire(revision, output, this, &Impl::CompleteOutput, &Impl::AbandonOutput)) return true;
 lock.unlock();
 output_callbacks_.release();
 return false;
}
bool LiveMediaDataPlane::Impl::request_raw_readback(LiveRawFrameReadback request) {
 if (!request.valid()) return false;
 {
  std::lock_guard lock(lifecycle_);
  if (phase_ != PhysicalPhase::Running || !resources_.has_value() || raw_readback_.phase != RawReadbackPhase::Idle) return false;
  raw_readback_.request.emplace(std::move(request));
  raw_readback_.phase = RawReadbackPhase::Pending;
  wake_pending_ = true;
 }
 wake_condition_.notify_one();
 return true;
}
void LiveMediaDataPlane::Impl::set_revision_listener(const LiveRevisionListener listener) {
 std::lock_guard lock(lifecycle_);
 if (phase_ != PhysicalPhase::Idle) throw std::logic_error("Live revision listener must be installed before start");
 revision_listener_ = listener;
}
void LiveMediaDataPlane::Impl::set_admitted_run_terminal_listener(const LiveAdmittedRunTerminalListener listener) {
 std::lock_guard lock(lifecycle_);
 if (phase_ != PhysicalPhase::Idle) throw std::logic_error("Live terminal listener must be installed before start");
 admitted_run_terminal_listener_ = listener;
}
void LiveMediaDataPlane::Impl::publish_manual_overlay(ManualOverlayDocumentSnapshot snapshot) {
 manual_document_.publish_snapshot(std::move(snapshot));
 wake();
}
void LiveMediaDataPlane::Impl::clear_manual_overlay(const std::uint32_t width, const std::uint32_t height) {
 manual_document_.clear(width, height);
 wake();
}
void LiveMediaDataPlane::Impl::RecordCudaFailure(void* context, const cudaError_t failure) noexcept {
 if (context != nullptr) static_cast<Impl*>(context)->record_cuda_failure(failure);
}
void LiveMediaDataPlane::Impl::CompleteOutput(void* context, const PhysicalFrameRevision revision) noexcept {
 static_cast<Impl*>(context)->complete_output(revision);
}
void LiveMediaDataPlane::Impl::AbandonOutput(void* context, const PhysicalFrameRevision revision) noexcept {
 static_cast<Impl*>(context)->abandon_output(revision);
}
void LiveMediaDataPlane::Impl::WakeOutputCallback(void* context) noexcept { static_cast<Impl*>(context)->wake(); }
void LiveMediaDataPlane::Impl::complete_output(const PhysicalFrameRevision revision) noexcept {
 LiveOutputCallbackGuard callback{output_callbacks_};
 resources_->compositor.complete_output(revision);
}
void LiveMediaDataPlane::Impl::abandon_output(const PhysicalFrameRevision revision) noexcept {
 LiveOutputCallbackGuard callback{output_callbacks_};
 resources_->compositor.abandon_output(revision);
}
void LiveMediaDataPlane::Impl::record_cuda_failure(const cudaError_t failure) noexcept {
 if (failure == cudaSuccess) return;
 capture::Status status;
 try {
  status = {capture::StatusCode::kCudaError, "Live CUDA failure " + std::to_string(static_cast<int>(failure))};
 } catch (...) { status = {capture::StatusCode::kCudaError, "Live CUDA failure"}; }
 record_failure(std::move(status));
}
void LiveMediaDataPlane::Impl::record_failure(capture::Status failure) noexcept {
 if (failure.ok()) return;
 {
  std::lock_guard lock(lifecycle_);
  record_failure_locked(std::move(failure));
 }
 wake_condition_.notify_all();
}
void LiveMediaDataPlane::Impl::record_failure_locked(capture::Status failure) noexcept {
 if (failure.ok()) return;
 if (first_failure_.ok()) first_failure_ = std::move(failure);
 wake_pending_ = true;
 if (phase_ == PhysicalPhase::Starting || phase_ == PhysicalPhase::Running) phase_ = PhysicalPhase::Closing;
}
void LiveMediaDataPlane::Impl::wake() noexcept {
 {
  std::lock_guard lock(lifecycle_);
  wake_pending_ = true;
 }
 wake_condition_.notify_one();
}
void LiveMediaDataPlane::Impl::publish_revision() noexcept {
 LiveRevisionListener listener;
 {
  std::lock_guard lock(lifecycle_);
  wake_pending_ = true;
  listener = revision_listener_;
 }
 wake_condition_.notify_one();
 listener();
}
std::optional<LiveMediaDataPlane::Impl::RawReadbackDelivery> LiveMediaDataPlane::Impl::finish_raw_readback(LiveRawFrameReadbackResult result) noexcept {
 std::lock_guard lock(lifecycle_);
 if (raw_readback_.phase != RawReadbackPhase::Active || !raw_readback_.request.has_value()) return std::nullopt;
 const LiveFrameId frame = raw_readback_.request->frame;
 if (!result.valid() || result.frame != frame) result = {frame, nullptr, 0U, false};
 raw_readback_.phase = RawReadbackPhase::Delivered;
 RawReadbackDelivery delivery{std::move(*raw_readback_.request), result};
 raw_readback_.request.reset();
 return delivery;
}
void LiveMediaDataPlane::Impl::deliver(std::optional<RawReadbackDelivery> raw) noexcept {
 if (!raw.has_value()) return;
 gpu::ResourceOwnerDeliveryScope delivery_scope;
 if (!delivery_scope) std::terminate();
 if (raw.has_value()) {
  const bool completed = raw->valid() && raw->result.completed && raw->request.destination.size() >= raw->result.bytes;
  const std::size_t bytes = completed ? raw->result.bytes : 0U;
  if (completed) std::memcpy(raw->request.destination.data(), raw->result.pixels, bytes);
  raw->request.complete(raw->request.context, raw->request.frame, bytes, completed);
  std::lock_guard lock(lifecycle_);
  if (raw_readback_.phase == RawReadbackPhase::Delivered) raw_readback_.phase = RawReadbackPhase::Idle;
 }
}
void LiveMediaDataPlane::Impl::publish_start(capture::CaptureSessionStartResult result) noexcept {
 {
  std::lock_guard lock(lifecycle_);
  try {
   start_result_ = std::move(result);
  } catch (...) {
   start_result_ = {capture::CaptureSessionStartPhase::NoCustody, {capture::StatusCode::kInternalError, "Live start result publication failed"}, {}};
  }
  startup_ready_ = true;
  phase_ = start_result_.running() ? (stop_requested_ || !first_failure_.ok() ? PhysicalPhase::Closing : PhysicalPhase::Running)
                                   : (start_result_.has_custody() ? PhysicalPhase::Closing : PhysicalPhase::Terminal);
 }
 wake_condition_.notify_all();
}
void LiveMediaDataPlane::Impl::publish_terminal(std::shared_ptr<const capture::CaptureStopTerminal> capture_terminal) noexcept {
 resources_.reset();
 LiveAdmittedRunTerminalListener listener;
 const LivePhysicalTerminal* published = nullptr;
 {
  std::lock_guard lock(lifecycle_);
  terminal_.emplace(std::move(capture_terminal), std::move(first_failure_));
  phase_ = PhysicalPhase::Terminal;
  listener = admitted_run_terminal_listener_;
  published = &*terminal_;
 }
 listener(*published);
}
void LiveMediaDataPlane::Impl::finish_admitted_run(const std::stop_token stop, const bool admission_closing) noexcept {
 std::shared_ptr<const capture::CaptureStopTerminal> observed_terminal;
 bool capture_finalized = false;
 for (;;) {
  try {
   if (!capture_finalized) {
    bool request_close = false;
    bool report_failure = false;
    capture::Status failure;
    {
     std::lock_guard lock(lifecycle_);
     request_close = stop_requested_ || stop.stop_requested() || !first_failure_.ok() || admission_closing;
     report_failure = !first_failure_.ok() && !failure_reported_;
     if (report_failure) {
      failure = first_failure_;
      failure_reported_ = true;
     }
    }
    if (request_close && !close_issued_) {
     resources_->close_admission();
     capture::Status close_status = report_failure ? resources_->ingress.report_failure(std::move(failure)) : resources_->ingress.request_stop();
     close_issued_ = true;
     if (!close_status.ok() && close_status.code != capture::StatusCode::kNotRunning) record_failure(std::move(close_status));
    } else if (report_failure) {
     const capture::Status reported = resources_->ingress.report_failure(std::move(failure));
     if (!reported.ok() && reported.code != capture::StatusCode::kNotRunning) record_failure(reported);
    }
    static_cast<void>(resources_->drain());
    if (observed_terminal == nullptr) observed_terminal = resources_->ingress.try_take_terminal();
    if (observed_terminal != nullptr) {
     capture_finalized = true;
     resources_->close_admission();
     capture::Status finalized;
     try {
      finalized = resources_->ingress.finalize_terminal(observed_terminal);
     } catch (...) { finalized = {capture::StatusCode::kInternalError, "Live capture finalization failed"}; }
     resources_->settle();
     {
      std::lock_guard lock(lifecycle_);
      phase_ = PhysicalPhase::Closing;
     }
     record_failure(observed_terminal->status);
     if (!finalized.ok()) record_failure(std::move(finalized));
    }
   }
  } catch (...) { record_failure({capture::StatusCode::kInternalError, "Live admitted operation failed"}); }
  if (capture_finalized && resources_->settled()) break;
  try {
   std::unique_lock lock(lifecycle_);
   wake_condition_.wait(lock, [this] { return wake_pending_; });
   wake_pending_ = false;
  } catch (...) { record_failure({capture::StatusCode::kInternalError, "Live admitted wait failed"}); }
 }
 publish_terminal(std::move(observed_terminal));
}
void LiveMediaDataPlane::Impl::run(const std::stop_token stop) noexcept {
 std::optional<mmltk::common::system::ScopedExecutionPolicy> policy;
 bool admitted = false;
 bool admission_closing = false;
 try {
  const auto& placement = config_.capture.execution->placement;
  policy.emplace(mmltk::common::system::ExecutionPolicyRequest{placement.cpus, "live-ingress", 0, placement.numa_node, -10, false});
  resources_.emplace(*this, config_);
  LiveAnalysisProviderFactory provider_factory;
  {
   std::lock_guard lock(lifecycle_);
   provider_factory = std::exchange(pending_provider_factory_, {});
  }
  capture::CaptureSessionStartResult admission = resources_->start(provider_factory);
  if (!admission.has_custody()) {
   resources_.reset();
   publish_start(std::move(admission));
   return;
  }
  admitted = true;
  admission_closing = admission.closing();
  publish_start(std::move(admission));
 } catch (const std::exception& error) { record_failure({capture::StatusCode::kInternalError, error.what()}); } catch (...) {
  record_failure({capture::StatusCode::kInternalError, "Live physical resource construction failed"});
 }
 if (admitted) {
  finish_admitted_run(stop, admission_closing);
  return;
 }
 resources_.reset();
 bool publish = false;
 {
  std::lock_guard lock(lifecycle_);
  publish = !startup_ready_;
  phase_ = PhysicalPhase::Terminal;
 }
 if (publish) publish_start({capture::CaptureSessionStartPhase::NoCustody, first_failure(), {}});
}
}  // namespace mmltk::backend::media::live
