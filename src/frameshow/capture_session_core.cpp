#include "capture_session_impl.hpp"

#include <algorithm>

namespace {

template <typename FrameView, typename Buffer, typename SlotRuntime>
[[nodiscard]] FrameView make_latest_frame_view_from_slot(const SlotRuntime& slot) {
    return FrameView{
        .buffer_index = slot.slot_index,
        .frame_id = slot.frame_id,
        .buffer =
            Buffer{
                .data = reinterpret_cast<CUdeviceptr>(slot.device_ptr),
                .pitch_bytes = slot.pitch_bytes,
                .x_px = slot.region.x,
                .y_px = slot.region.y,
                .width_px = slot.region.width,
                .height_px = slot.region.height,
                .ready_event = slot.ready_event,
            },
        .capture_ns = slot.capture_ns,
        .ready_ns = slot.ready_ns,
        .short_frame = slot.short_frame,
    };
}

}  // namespace

namespace frameshow {

using capture_internal::InferenceState;
using capture_internal::MakeStatus;
using capture_internal::PackRegion;
using capture_internal::PreviewState;
using capture_internal::ToStateValue;
using capture_internal::UnpackRegion;
using capture_internal::kPackedRegionFieldLimit;

CaptureSession::Impl::Impl(CaptureConfig config_in) : config(std::move(config_in)) {}

CaptureSession::Impl::~Impl() {
    stop();
}

Status CaptureSession::Impl::start() {
    if (running_.load(std::memory_order_acquire)) {
        return MakeStatus(StatusCode::kAlreadyRunning, "capture session already running");
    }

    Status status = ValidateConfig();
    if (!status.ok()) {
        return status;
    }

    ResetStats();
    ResetRuntimeState();
    shutdown_.store(false, std::memory_order_release);

    status = InitializeCuda();
    if (!status.ok()) {
        return status;
    }

    status = OpenDevice();
    if (!status.ok()) {
        return status;
    }

    status = ConfigureDevice();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    packed_region_.store(PackRegion(NormalizeRegion(config.initial_region)), std::memory_order_release);

    status = AllocateHostSlots();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    status = AllocateInferenceSlots();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    status = AllocatePreviewSlots();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    status = QueueAllV4l2Buffers();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    status = StartStreaming();
    if (!status.ok()) {
        TeardownSession();
        return status;
    }

    running_.store(true, std::memory_order_release);
    capture_thread_ = std::thread([this] { CaptureLoop(); });
    return Status::Ok();
}

Status CaptureSession::Impl::stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running && fd_ == -1 && host_slots_.empty() && inference_slots_.empty() && preview_slots_.empty()) {
        return Status::Ok();
    }

    shutdown_.store(true, std::memory_order_release);
    inference_publish_cv_.notify_all();
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    TeardownSession();
    latest_inference_index_.store(-1, std::memory_order_release);
    latest_preview_index_.store(-1, std::memory_order_release);

    return was_running ? Status::Ok() : MakeStatus(StatusCode::kNotRunning, "capture session was not running");
}

Status CaptureSession::Impl::set_capture_region(CaptureRegion region) {
    packed_region_.store(PackRegion(NormalizeRegion(region)), std::memory_order_release);
    return Status::Ok();
}

CaptureRegion CaptureSession::Impl::snapshot_capture_region() const {
    return UnpackRegion(packed_region_.load(std::memory_order_acquire));
}

CaptureFormatInfo CaptureSession::Impl::snapshot_format() const {
    return CaptureFormatInfo{
        .width = config.width,
        .height = config.height,
        .bytes_per_line = static_cast<std::uint32_t>(bytes_per_line_),
    };
}

Status CaptureSession::Impl::try_acquire_latest_inference_frame(InferenceFrameView* out_view) {
    if (out_view == nullptr) {
        return MakeStatus(StatusCode::kInvalidArgument, "output view is null");
    }

    const int latest_index = latest_inference_index_.load(std::memory_order_acquire);
    if (latest_index < 0 || latest_index >= static_cast<int>(inference_slots_.size())) {
        return MakeStatus(StatusCode::kNotReady, "no inference frame is available");
    }

    InferenceSlotRuntime& slot = *inference_slots_[static_cast<std::size_t>(latest_index)];
    std::uint32_t expected = ToStateValue(InferenceState::kPublished);
    if (!slot.state.compare_exchange_strong(expected, ToStateValue(InferenceState::kAcquired),
                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return MakeStatus(StatusCode::kNotReady, "latest inference frame is already in use");
    }

    *out_view = make_latest_frame_view_from_slot<InferenceFrameView, InferenceBuffer>(slot);
    return Status::Ok();
}

Status CaptureSession::Impl::wait_acquire_latest_inference_frame(InferenceFrameView* out_view,
                                                                 const std::atomic<bool>& stop_requested) {
    if (out_view == nullptr) {
        return MakeStatus(StatusCode::kInvalidArgument, "output view is null");
    }

    while (!stop_requested.load(std::memory_order_acquire) && running_.load(std::memory_order_acquire) &&
           !shutdown_.load(std::memory_order_acquire)) {
        Status status = try_acquire_latest_inference_frame(out_view);
        if (status.ok() || status.code != StatusCode::kNotReady) {
            return status;
        }

        std::unique_lock<std::mutex> lock(inference_publish_mutex_);
        const std::uint64_t start_generation = inference_publish_generation_;
        lock.unlock();
        status = try_acquire_latest_inference_frame(out_view);
        if (status.ok() || status.code != StatusCode::kNotReady) {
            return status;
        }
        lock.lock();
        inference_publish_cv_.wait(lock, [&] {
            return stop_requested.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire) ||
                   inference_publish_generation_ != start_generation;
        });
    }

    return MakeStatus(StatusCode::kNotReady, "capture session stopped before an inference frame was available");
}

void CaptureSession::Impl::notify_inference_waiters() {
    inference_publish_cv_.notify_all();
}

Status CaptureSession::Impl::release_inference_frame(std::uint32_t buffer_index) {
    if (buffer_index >= inference_slots_.size()) {
        return MakeStatus(StatusCode::kInvalidArgument, "inference buffer index out of range");
    }

    InferenceSlotRuntime& slot = *inference_slots_[buffer_index];
    if (slot.state.load(std::memory_order_acquire) != ToStateValue(InferenceState::kAcquired)) {
        return MakeStatus(StatusCode::kNotReady, "inference buffer is not in use");
    }

    slot.state.store(ToStateValue(InferenceState::kFree), std::memory_order_release);
    return Status::Ok();
}

Status CaptureSession::Impl::try_acquire_latest_preview(PreviewFrameView* out_view) {
    if (out_view == nullptr) {
        return MakeStatus(StatusCode::kInvalidArgument, "output view is null");
    }

    RecycleDisplayedPreviewSlots();

    const int latest_index = latest_preview_index_.load(std::memory_order_acquire);
    if (latest_index < 0 || latest_index >= static_cast<int>(preview_slots_.size())) {
        return MakeStatus(StatusCode::kNotReady, "no preview frame is available");
    }

    PreviewSlotRuntime& slot = *preview_slots_[static_cast<std::size_t>(latest_index)];
    std::uint32_t expected = ToStateValue(PreviewState::kPublished);
    if (!slot.state.compare_exchange_strong(expected, ToStateValue(PreviewState::kDisplaying),
                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return MakeStatus(StatusCode::kNotReady, "latest preview is already in use");
    }
    slot.displayed_event_pending.store(false, std::memory_order_release);

    *out_view = make_latest_frame_view_from_slot<PreviewFrameView, PreviewBuffer>(slot);
    return Status::Ok();
}

Status CaptureSession::Impl::mark_preview_displayed(std::uint32_t buffer_index, cudaStream_t stream) {
    if (buffer_index >= preview_slots_.size()) {
        return MakeStatus(StatusCode::kInvalidArgument, "preview buffer index out of range");
    }

    PreviewSlotRuntime& slot = *preview_slots_[buffer_index];
    if (slot.state.load(std::memory_order_acquire) != ToStateValue(PreviewState::kDisplaying)) {
        return MakeStatus(StatusCode::kNotReady, "preview buffer is not in use");
    }

    const cudaError_t cuda_status = cudaEventRecord(slot.displayed_event, stream);
    if (cuda_status != cudaSuccess) {
        return capture_internal::MakeCudaStatus(cuda_status, "cudaEventRecord");
    }
    slot.displayed_event_pending.store(true, std::memory_order_release);
    return Status::Ok();
}

Status CaptureSession::Impl::release_preview(std::uint32_t buffer_index) {
    if (buffer_index >= preview_slots_.size()) {
        return MakeStatus(StatusCode::kInvalidArgument, "preview buffer index out of range");
    }

    PreviewSlotRuntime& slot = *preview_slots_[buffer_index];
    if (slot.state.load(std::memory_order_acquire) != ToStateValue(PreviewState::kDisplaying)) {
        return MakeStatus(StatusCode::kNotReady, "preview buffer is not in use");
    }

    slot.displayed_event_pending.store(false, std::memory_order_release);
    slot.state.store(ToStateValue(PreviewState::kFree), std::memory_order_release);
    return Status::Ok();
}

CaptureStats CaptureSession::Impl::snapshot_stats() const {
    CaptureStats stats{};
    stats.queued_v4l2_buffers = queued_v4l2_buffers_.load(std::memory_order_acquire);
    stats.dequeued_v4l2_buffers = dequeued_v4l2_buffers_.load(std::memory_order_acquire);
    stats.bytes_captured = bytes_captured_.load(std::memory_order_acquire);
    stats.inference_frames_published = inference_frames_published_.load(std::memory_order_acquire);
    stats.preview_frames_published = preview_frames_published_.load(std::memory_order_acquire);
    stats.frames_dropped = frames_dropped_.load(std::memory_order_acquire);
    stats.empty_frames_dropped = empty_frames_dropped_.load(std::memory_order_acquire);
    stats.inference_backpressure_drops = inference_backpressure_drops_.load(std::memory_order_acquire);
    stats.preview_backpressure_drops = preview_backpressure_drops_.load(std::memory_order_acquire);
    stats.short_frames = short_frames_.load(std::memory_order_acquire);
    stats.sequence_gaps = sequence_gaps_.load(std::memory_order_acquire);
    stats.requeue_failures = requeue_failures_.load(std::memory_order_acquire);
    stats.running = running_.load(std::memory_order_acquire);
    return stats;
}

std::string CaptureSession::Impl::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void CUDART_CB CaptureSession::Impl::RequeueCapturedBufferThunk(void* user_data) {
    auto* context = static_cast<RequeueContext*>(user_data);
    if (context != nullptr && context->owner != nullptr) {
        context->owner->OnPreviewCopyComplete(context->slot_index);
    }
}

Status CaptureSession::Impl::ValidateConfig() const {
    if (config.width == 0U || config.height == 0U) {
        return MakeStatus(StatusCode::kInvalidArgument, "width and height must be non-zero");
    }
    if (config.width > kPackedRegionFieldLimit || config.height > kPackedRegionFieldLimit) {
        return MakeStatus(StatusCode::kUnsupported, "dimensions above 65535 are not supported");
    }
    if (config.fps == 0U) {
        return MakeStatus(StatusCode::kInvalidArgument, "fps must be non-zero");
    }
    if (config.v4l2_buffer_count == 0U) {
        return MakeStatus(StatusCode::kInvalidArgument, "v4l2_buffer_count must be non-zero");
    }
    return Status::Ok();
}

void CaptureSession::Impl::ResetRuntimeState() {
    streaming_ = false;
    actual_v4l2_buffer_count_ = 0;
    bytes_per_line_ = 0;
    size_image_ = 0;
    latest_inference_index_.store(-1, std::memory_order_release);
    latest_preview_index_.store(-1, std::memory_order_release);
    last_sequence_.reset();
    next_frame_id_ = 1;
    next_inference_slot_ = 0;
    next_preview_slot_ = 0;
    ClearLastError();
}

CaptureRegion CaptureSession::Impl::NormalizeRegion(CaptureRegion region) const {
    if (config.width == 0U || config.height == 0U) {
        return CaptureRegion{};
    }

    region.x = std::min(region.x, config.width - 1U);
    region.y = std::min(region.y, config.height - 1U);

    const std::uint32_t max_width = config.width - region.x;
    const std::uint32_t max_height = config.height - region.y;
    region.width = region.width == 0U ? max_width : std::min(region.width, max_width);
    region.height = region.height == 0U ? max_height : std::min(region.height, max_height);
    return region;
}

void CaptureSession::Impl::TeardownSession() {
    StopStreaming();
    DestroyPreviewSlots();
    DestroyInferenceSlots();
    DestroyHostSlots();
    CloseDevice();
}

void CaptureSession::Impl::ResetStats() {
    queued_v4l2_buffers_.store(0, std::memory_order_release);
    dequeued_v4l2_buffers_.store(0, std::memory_order_release);
    bytes_captured_.store(0, std::memory_order_release);
    inference_frames_published_.store(0, std::memory_order_release);
    preview_frames_published_.store(0, std::memory_order_release);
    frames_dropped_.store(0, std::memory_order_release);
    empty_frames_dropped_.store(0, std::memory_order_release);
    inference_backpressure_drops_.store(0, std::memory_order_release);
    preview_backpressure_drops_.store(0, std::memory_order_release);
    short_frames_.store(0, std::memory_order_release);
    sequence_gaps_.store(0, std::memory_order_release);
    requeue_failures_.store(0, std::memory_order_release);
}

void CaptureSession::Impl::ClearLastError() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
}

void CaptureSession::Impl::SetLastError(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = message;
}

CaptureSession::CaptureSession(CaptureConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

CaptureSession::~CaptureSession() = default;
CaptureSession::CaptureSession(CaptureSession&&) noexcept = default;
CaptureSession& CaptureSession::operator=(CaptureSession&&) noexcept = default;

Status CaptureSession::start() {
    return impl_->start();
}
Status CaptureSession::stop() {
    return impl_->stop();
}

Status CaptureSession::set_capture_region(CaptureRegion region) {
    return impl_->set_capture_region(region);
}

CaptureRegion CaptureSession::snapshot_capture_region() const {
    return impl_->snapshot_capture_region();
}

CaptureFormatInfo CaptureSession::snapshot_format() const {
    return impl_->snapshot_format();
}

Status CaptureSession::try_acquire_latest_inference_frame(InferenceFrameView* out_view) {
    return impl_->try_acquire_latest_inference_frame(out_view);
}

Status CaptureSession::wait_acquire_latest_inference_frame(InferenceFrameView* out_view,
                                                           const std::atomic<bool>& stop_requested) {
    return impl_->wait_acquire_latest_inference_frame(out_view, stop_requested);
}

void CaptureSession::notify_inference_waiters() {
    impl_->notify_inference_waiters();
}

Status CaptureSession::release_inference_frame(std::uint32_t buffer_index) {
    return impl_->release_inference_frame(buffer_index);
}

Status CaptureSession::try_acquire_latest_preview(PreviewFrameView* out_view) {
    return impl_->try_acquire_latest_preview(out_view);
}

Status CaptureSession::mark_preview_displayed(std::uint32_t buffer_index, cudaStream_t stream) {
    return impl_->mark_preview_displayed(buffer_index, stream);
}

Status CaptureSession::release_preview(std::uint32_t buffer_index) {
    return impl_->release_preview(buffer_index);
}

CaptureStats CaptureSession::snapshot_stats() const {
    return impl_->snapshot_stats();
}

std::string CaptureSession::last_error() const {
    return impl_->last_error();
}

}  // namespace frameshow
