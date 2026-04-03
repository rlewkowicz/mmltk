#include "live_preview_texture.h"

#include "cuda_gl_interop_utils.h"
#include "cuda_priority.h"
#include "mmltk/live/live_session_controller.h"
#include "mmltk_logging.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace mmltk::gui {

namespace {

constexpr std::size_t kBgr3BytesPerPixel = 3U;

void release_controller_output_silently(mmltk::live::LiveSessionController& controller,
                                        const std::uint32_t slot_index) {
#if MMLTK_RFDETR_LIVE_CAPTURE
    try {
        controller.release_output(slot_index);
    } catch (const std::exception& error) {
        mmltk::logging::logger("gui")->error(
            "mmltk live preview release error: failed to release controller output slot {}: {}",
            slot_index,
            error.what());
    }
#else
    (void)controller;
    (void)slot_index;
#endif
}

} // namespace

LivePreviewTexture::LivePreviewTexture(GLFWwindow* shared_context_window)
    : shared_context_window_(shared_context_window) {}

LivePreviewTexture::~LivePreviewTexture() {
    shutdown();
}

bool LivePreviewTexture::initialize(std::string* error_message) {
    shutdown();

    if (shared_context_window_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "preview uploader requires a valid GLFW window";
        }
        return false;
    }

    std::string initialize_error;
    const bool ok = initialize_gl_resources(&initialize_error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = ok;
        state_.initialized = ok;
        state_.last_error = initialize_error;
        state_.interop_failed = is_invalid_graphics_context_error(initialize_error);
    }

    if (error_message != nullptr) {
        *error_message = initialize_error;
    }
    return ok;
}

void LivePreviewTexture::shutdown() {
    end_live_stream();
    destroy_live_resources();
    destroy_gl_resources();

    std::lock_guard<std::mutex> lock(mutex_);
    state_ = {};
    initialized_ = false;
    live_source_kind_ = LiveSourceKind::None;
    live_controller_ = nullptr;
    live_cuda_device_index_ = 0;
    host_frame_generation_ = 1;
    pending_host_frame_.reset();
    pending_live_frame_.reset();
    width_ = 0;
    height_ = 0;
    front_texture_index_ = 0;
    back_texture_index_ = 1;
}

void LivePreviewTexture::begin_live_stream(mmltk::live::LiveSessionController& controller,
                                           const int cuda_device_index) {
    end_live_stream();
#if MMLTK_RFDETR_LIVE_CAPTURE
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error.clear();
    state_.interop_failed = false;
    ++host_frame_generation_;
    pending_host_frame_.reset();
    live_source_kind_ = LiveSourceKind::ControllerOutput;
    live_controller_ = &controller;
    live_cuda_device_index_ = cuda_device_index;
#else
    (void)controller;
    (void)cuda_device_index;
    set_error("controller-backed live preview requires live capture support in this build", false);
#endif
}

void LivePreviewTexture::end_live_stream() {
    std::optional<PendingLiveFrame> pending_live_frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++host_frame_generation_;
        live_source_kind_ = LiveSourceKind::None;
        live_controller_ = nullptr;
        pending_host_frame_.reset();
        if (pending_live_frame_.has_value()) {
            pending_live_frame = *pending_live_frame_;
            pending_live_frame_.reset();
        }
    }

    if (!pending_live_frame.has_value()) {
        return;
    }

    if (current_cuda_device_index_ >= 0) {
        (void)cudaSetDevice(current_cuda_device_index_);
    }
    if (stream_ != nullptr) {
        (void)cudaStreamSynchronize(stream_);
    }
    release_pending_live_frame_silently(*pending_live_frame);
}

void LivePreviewTexture::pump() {
    if (!initialized()) {
        return;
    }

    std::string preview_error;
    if (!finalize_live_preview_copy(&preview_error)) {
        set_error(preview_error, is_invalid_graphics_context_error(preview_error));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_live_frame_.has_value()) {
            return;
        }
    }

    std::optional<PendingHostFrame> host_frame;
    LiveSourceKind source_kind = LiveSourceKind::None;
    mmltk::live::LiveSessionController* live_controller = nullptr;
    int cuda_device_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_host_frame_.has_value() && live_source_kind_ == LiveSourceKind::None) {
            host_frame = std::move(*pending_host_frame_);
            pending_host_frame_.reset();
        } else {
            source_kind = live_source_kind_;
            live_controller = live_controller_;
            cuda_device_index = live_cuda_device_index_;
        }
    }

    if (host_frame.has_value()) {
        if (!upload_host_preview(*host_frame, &preview_error)) {
            set_error(preview_error, is_invalid_graphics_context_error(preview_error));
        } else {
            clear_error();
        }
        return;
    }

    if (source_kind != LiveSourceKind::ControllerOutput || live_controller == nullptr) {
        return;
    }

    try {
#if MMLTK_RFDETR_LIVE_CAPTURE
        mmltk::live::OutputBundle output_bundle{};
        if (!live_controller->try_acquire_latest_output(&output_bundle)) {
            const std::string controller_error = live_controller->last_error();
            if (!controller_error.empty()) {
                set_error(controller_error, is_invalid_graphics_context_error(controller_error));
            }
            return;
        }

        PendingLiveFrame pending_frame;
        pending_frame.source_kind = LiveSourceKind::ControllerOutput;
        pending_frame.controller = live_controller;
        pending_frame.release_index = output_bundle.slot_index;
        pending_frame.width = output_bundle.dims.width;
        pending_frame.height = output_bundle.dims.height;
        pending_frame.region = output_bundle.region;
        pending_frame.frame_id = output_bundle.frame_id.sequence;
        pending_frame.live_frame_id = output_bundle.frame_id;
        if (!stage_live_preview_copy(output_bundle.data,
                                     output_bundle.dims.pitch_bytes,
                                     output_bundle.dims.width,
                                     output_bundle.dims.height,
                                     output_bundle.ready_event,
                                     pending_frame,
                                     cuda_device_index,
                                     &preview_error)) {
            set_error(preview_error, is_invalid_graphics_context_error(preview_error));
        } else {
            clear_error();
        }
#else
        (void)cuda_device_index;
        set_error("controller-backed live preview requires live capture support in this build", false);
#endif
    } catch (const std::exception& error) {
        set_error(error.what(), false);
    }
}

bool LivePreviewTexture::submit_host_bgr(std::vector<std::uint8_t> pixels,
                                         const std::uint32_t width,
                                         const std::uint32_t height,
                                         const mmltk::live::LiveCaptureRegion& region,
                                         const std::uint64_t frame_id,
                                         std::string* error_message) {
    if (pixels.empty()) {
        if (error_message != nullptr) {
            *error_message = "host preview pixels must not be empty";
        }
        return false;
    }
    if (width == 0U || height == 0U) {
        if (error_message != nullptr) {
            *error_message = "host preview dimensions must be non-zero";
        }
        return false;
    }
    if (!initialized()) {
        if (error_message != nullptr) {
            const LivePreviewTextureState state = snapshot();
            *error_message = state.last_error.empty() ? "preview uploader is not initialized" : state.last_error;
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (live_source_kind_ != LiveSourceKind::None || pending_live_frame_.has_value()) {
        if (error_message != nullptr) {
            *error_message = "cannot submit a static preview while live preview is active";
        }
        return false;
    }

    state_.last_error.clear();
    state_.interop_failed = false;
    pending_host_frame_ = PendingHostFrame{
        std::move(pixels),
        width,
        height,
        region,
        frame_id,
        host_frame_generation_,
    };
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::clear_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++host_frame_generation_;
    pending_host_frame_.reset();
    state_.has_frame = false;
    state_.displayed_region = {};
    state_.last_frame_id = 0;
    state_.live_frame_id.reset();
    state_.texture_id = {};
    state_.last_error.clear();
    state_.interop_failed = false;
}

LivePreviewTextureState LivePreviewTexture::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

ImVec2 LivePreviewTexture::uv0() {
    return {0.0f, 0.0f};
}

ImVec2 LivePreviewTexture::uv1() {
    return {1.0f, 1.0f};
}

bool LivePreviewTexture::initialize_gl_resources(std::string* error_message) {
    destroy_gl_resources();
    initialize_gui_texture_pair(textures_);

    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::destroy_gl_resources() {
    destroy_gui_texture_pair(textures_);
}

bool LivePreviewTexture::ensure_texture_storage(const std::uint32_t width,
                                                const std::uint32_t height,
                                                std::string* error_message) {
    return ensure_gui_texture_storage(textures_,
                                      width,
                                      height,
                                      GL_RGB8,
                                      GL_RGB,
                                      "preview dimensions must be non-zero",
                                      width_,
                                      height_,
                                      error_message);
}

bool LivePreviewTexture::ensure_live_resources(const std::uint32_t width,
                                               const std::uint32_t height,
                                               const int cuda_device_index,
                                               std::string* error_message) {
    const bool storage_changed = width_ != width || height_ != height;
    if (current_cuda_device_index_ >= 0 && current_cuda_device_index_ != cuda_device_index) {
        destroy_live_resources();
    }

    if (!ensure_texture_storage(width, height, error_message)) {
        return false;
    }

    cudaError_t cuda_status = cudaSetDevice(cuda_device_index);
    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, "cudaSetDevice");
        }
        return false;
    }

    if (stream_ == nullptr) {
        cuda_status = mmltk::cuda_stream_create_with_highest_priority(&stream_, cudaStreamNonBlocking);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaStreamCreateWithPriority");
            }
            return false;
        }
    }

    if (copy_complete_event_ == nullptr) {
        cuda_status = cudaEventCreateWithFlags(&copy_complete_event_, cudaEventDisableTiming);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaEventCreateWithFlags");
            }
            return false;
        }
    }

    if (pixel_buffer_ == 0U) {
        glGenBuffers(1, &pixel_buffer_);
    }

    if (graphics_resource_ != nullptr && storage_changed) {
        cuda_status = cudaGraphicsUnregisterResource(graphics_resource_);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaGraphicsUnregisterResource");
            }
            return false;
        }
        graphics_resource_ = nullptr;
    }

    if (graphics_resource_ == nullptr) {
        const std::size_t required_bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kBgr3BytesPerPixel;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(required_bytes), nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        cuda_status =
            cudaGraphicsGLRegisterBuffer(&graphics_resource_, pixel_buffer_, cudaGraphicsRegisterFlagsWriteDiscard);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaGraphicsGLRegisterBuffer");
            }
            return false;
        }
    }

    current_cuda_device_index_ = cuda_device_index;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::destroy_live_resources() {
    if (current_cuda_device_index_ >= 0) {
        (void)cudaSetDevice(current_cuda_device_index_);
    }
    if (stream_ != nullptr) {
        (void)cudaStreamSynchronize(stream_);
    }
    if (graphics_resource_ != nullptr) {
        (void)cudaGraphicsUnregisterResource(graphics_resource_);
        graphics_resource_ = nullptr;
    }
    if (copy_complete_event_ != nullptr) {
        (void)cudaEventDestroy(copy_complete_event_);
        copy_complete_event_ = nullptr;
    }
    if (stream_ != nullptr) {
        (void)cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
    if (pixel_buffer_ != 0U) {
        glDeleteBuffers(1, &pixel_buffer_);
        pixel_buffer_ = 0U;
    }
    current_cuda_device_index_ = -1;
}

bool LivePreviewTexture::stage_live_preview_copy(const CUdeviceptr source_data,
                                                 const std::size_t source_pitch_bytes,
                                                 const std::uint32_t width,
                                                 const std::uint32_t height,
                                                 const cudaEvent_t ready_event,
                                                 PendingLiveFrame pending_frame,
                                                 const int cuda_device_index,
                                                 std::string* error_message) {
    if (source_data == 0) {
        release_pending_live_frame_silently(pending_frame);
        if (error_message != nullptr) {
            *error_message = "live preview source buffer must not be null";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_live_frame_.has_value()) {
            if (error_message != nullptr) {
                *error_message = "live preview copy is already in flight";
            }
            release_pending_live_frame_silently(pending_frame);
            return false;
        }
    }

    if (!ensure_live_resources(width, height, cuda_device_index, error_message)) {
        release_pending_live_frame_silently(pending_frame);
        return false;
    }

    if (current_cuda_device_index_ >= 0) {
        const cudaError_t set_device_status = cudaSetDevice(current_cuda_device_index_);
        if (set_device_status != cudaSuccess) {
            release_pending_live_frame_silently(pending_frame);
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(set_device_status, "cudaSetDevice");
            }
            return false;
        }
    }

    cudaError_t cuda_status = cudaSuccess;
    const char* failed_cuda_label = nullptr;
    if (ready_event != nullptr) {
        failed_cuda_label = "cudaStreamWaitEvent";
        cuda_status = cudaStreamWaitEvent(stream_, ready_event, 0);
    }

    bool mapped = false;
    void* pixel_buffer_ptr = nullptr;
    std::size_t mapped_size = 0;
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsMapResources";
        cuda_status = cudaGraphicsMapResources(1, &graphics_resource_, stream_);
        mapped = cuda_status == cudaSuccess;
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
        cuda_status = cudaGraphicsResourceGetMappedPointer(&pixel_buffer_ptr, &mapped_size, graphics_resource_);
    }

    const std::size_t copy_width_bytes = static_cast<std::size_t>(width) * kBgr3BytesPerPixel;
    const auto copy_height = static_cast<std::size_t>(height);
    const std::size_t required_bytes = copy_width_bytes * copy_height;
    if (cuda_status == cudaSuccess && mapped_size < required_bytes) {
        cuda_status = cudaErrorInvalidValue;
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaMemcpy2DAsync";
        cuda_status = cudaMemcpy2DAsync(pixel_buffer_ptr,
                                        copy_width_bytes,
                                        mmltk::live::device_ptr_as_const_void(source_data),
                                        source_pitch_bytes,
                                        copy_width_bytes,
                                        copy_height,
                                        cudaMemcpyDeviceToDevice,
                                        stream_);
    }

    const cudaError_t unmap_status =
        mapped ? cudaGraphicsUnmapResources(1, &graphics_resource_, stream_) : cudaSuccess;

    if (cuda_status != cudaSuccess) {
        release_pending_live_frame_silently(pending_frame);
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, failed_cuda_label);
        }
        return false;
    }
    if (unmap_status != cudaSuccess) {
        release_pending_live_frame_silently(pending_frame);
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(unmap_status, "cudaGraphicsUnmapResources");
        }
        return false;
    }

    cuda_status = cudaEventRecord(copy_complete_event_, stream_);
    if (cuda_status != cudaSuccess) {
        release_pending_live_frame_silently(pending_frame);
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, "cudaEventRecord");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_live_frame_ = pending_frame;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

bool LivePreviewTexture::finalize_live_preview_copy(std::string* error_message) {
    std::optional<PendingLiveFrame> pending_live_frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_live_frame_.has_value()) {
            if (error_message != nullptr) {
                error_message->clear();
            }
            return true;
        }
        pending_live_frame = pending_live_frame_;
    }

    if (current_cuda_device_index_ >= 0) {
        const cudaError_t set_device_status = cudaSetDevice(current_cuda_device_index_);
        if (set_device_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(set_device_status, "cudaSetDevice");
            }
            return false;
        }
    }

    const cudaError_t query_status = cudaEventQuery(copy_complete_event_);
    if (query_status == cudaErrorNotReady) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }
    if (query_status != cudaSuccess) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_live_frame_.has_value()) {
                pending_live_frame = *pending_live_frame_;
                pending_live_frame_.reset();
            }
        }
        if (pending_live_frame.has_value()) {
            release_pending_live_frame_silently(*pending_live_frame);
        }
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(query_status, "cudaEventQuery");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_live_frame_.has_value()) {
            pending_live_frame = *pending_live_frame_;
            pending_live_frame_.reset();
        }
    }
    if (!pending_live_frame.has_value()) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

    glBindTexture(GL_TEXTURE_2D, textures_[back_texture_index_]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(pending_live_frame->width),
                    static_cast<GLsizei>(pending_live_frame->height),
                    GL_BGR,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();

    release_pending_live_frame_silently(*pending_live_frame);

    publish_ready_texture(pending_live_frame->width,
                          pending_live_frame->height,
                          pending_live_frame->region,
                          pending_live_frame->frame_id,
                          pending_live_frame->source_kind == LiveSourceKind::ControllerOutput
                              ? std::optional<mmltk::live::LiveFrameId>(pending_live_frame->live_frame_id)
                              : std::nullopt);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::release_pending_live_frame_silently(const PendingLiveFrame& pending_frame) noexcept {
    if (pending_frame.source_kind == LiveSourceKind::ControllerOutput && pending_frame.controller != nullptr) {
        release_controller_output_silently(*pending_frame.controller, pending_frame.release_index);
    }
}

void LivePreviewTexture::publish_ready_texture(const std::uint32_t width,
                                               const std::uint32_t height,
                                               const mmltk::live::LiveCaptureRegion& region,
                                               const std::uint64_t frame_id,
                                               std::optional<mmltk::live::LiveFrameId> live_frame_id) {
    std::swap(front_texture_index_, back_texture_index_);

    std::lock_guard<std::mutex> lock(mutex_);
    state_.has_frame = true;
    state_.texture_id = imgui_texture_id_from_gl_name(textures_[front_texture_index_]);
    state_.displayed_region = region;
    state_.last_frame_id = frame_id;
    state_.live_frame_id = live_frame_id;
    if (state_.displayed_region.width == 0U) {
        state_.displayed_region.width = width;
    }
    if (state_.displayed_region.height == 0U) {
        state_.displayed_region.height = height;
    }
}

void LivePreviewTexture::set_error(const std::string& error_message, const bool interop_failed) {
    if (error_message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.last_error != error_message) {
        log_gui_surface_error("live preview", error_message);
    }
    state_.last_error = error_message;
    state_.interop_failed = interop_failed;
}

void LivePreviewTexture::clear_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error.clear();
    state_.interop_failed = false;
}

bool LivePreviewTexture::upload_host_preview(const PendingHostFrame& frame, std::string* error_message) {
    if (!ensure_texture_storage(frame.width, frame.height, error_message)) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textures_[back_texture_index_]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(frame.width),
                    static_cast<GLsizei>(frame.height),
                    GL_BGR,
                    GL_UNSIGNED_BYTE,
                    frame.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();

    bool publish = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publish = live_source_kind_ == LiveSourceKind::None && frame.generation == host_frame_generation_;
    }
    if (publish) {
        publish_ready_texture(frame.width, frame.height, frame.region, frame.frame_id, std::nullopt);
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

bool LivePreviewTexture::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

} // namespace mmltk::gui
