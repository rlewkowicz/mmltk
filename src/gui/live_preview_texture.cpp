#include "live_preview_texture.h"
#include "cuda_priority.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

namespace fastloader::gui {

namespace {

constexpr std::size_t kBgr3BytesPerPixel = 3U;

ImTextureID texture_id_from_name(GLuint texture_name) {
    return (ImTextureID)(static_cast<std::uintptr_t>(texture_name));
}

std::string cuda_error_message(cudaError_t error, const char* label) {
    std::string message(label);
    message += " failed: ";
    message += cudaGetErrorString(error);
    if (error == cudaErrorInvalidGraphicsContext) {
        message +=
            ". CUDA-OpenGL interop requires a valid hardware OpenGL context. "
            "On Linux/NVIDIA, prefer an X11/GLX context when DISPLAY is available "
            "(for example FASTLOADER_GUI_PLATFORM=x11).";
    }
    return message;
}

bool is_invalid_graphics_context_error(const std::string& error_message) {
    return error_message.find("invalid OpenGL or DirectX context") != std::string::npos;
}

void release_preview_silently(fastloader::rfdetr::LivePredictSession& session, std::uint32_t buffer_index) {
    (void)session.release_preview(buffer_index);
}

void log_live_preview_error_to_stderr(const std::string& message) {
    if (message.empty()) {
        return;
    }
    std::fprintf(stderr, "fastloader live preview error: %s\n", message.c_str());
    std::fflush(stderr);
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
    live_session_ = nullptr;
    live_cuda_device_index_ = 0;
    host_frame_generation_ = 1;
    pending_host_frame_.reset();
    pending_live_frame_.reset();
    width_ = 0;
    height_ = 0;
    front_texture_index_ = 0;
    back_texture_index_ = 1;
}

void LivePreviewTexture::begin_live_stream(fastloader::rfdetr::LivePredictSession& session, int cuda_device_index) {
    end_live_stream();

    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error.clear();
    state_.interop_failed = false;
    ++host_frame_generation_;
    pending_host_frame_.reset();
    live_session_ = &session;
    live_cuda_device_index_ = cuda_device_index;
}

void LivePreviewTexture::end_live_stream() {
    std::optional<PendingLiveFrame> pending_live_frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++host_frame_generation_;
        live_session_ = nullptr;
        pending_host_frame_.reset();
        if (pending_live_frame_.has_value()) {
            pending_live_frame = std::move(*pending_live_frame_);
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
    if (pending_live_frame->session != nullptr) {
        release_preview_silently(*pending_live_frame->session, pending_live_frame->frame.buffer_index);
    }
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
    fastloader::rfdetr::LivePredictSession* live_session = nullptr;
    int cuda_device_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_host_frame_.has_value() && live_session_ == nullptr) {
            host_frame = std::move(*pending_host_frame_);
            pending_host_frame_.reset();
        } else if (live_session_ != nullptr) {
            live_session = live_session_;
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

    if (live_session == nullptr) {
        return;
    }

    fastloader::rfdetr::LivePreviewFrame preview_frame{};
    bool acquired = false;
    try {
        acquired = live_session->try_acquire_latest_preview(&preview_frame, &preview_error);
        if (acquired) {
            if (!stage_live_preview_copy(*live_session, preview_frame, cuda_device_index, &preview_error)) {
                set_error(preview_error, is_invalid_graphics_context_error(preview_error));
            } else {
                clear_error();
            }
        } else if (!preview_error.empty()) {
            set_error(preview_error, is_invalid_graphics_context_error(preview_error));
        }
    } catch (const std::exception& error) {
        set_error(error.what(), false);
    }
}

bool LivePreviewTexture::submit_host_bgr(std::vector<std::uint8_t> pixels,
                                         std::uint32_t width,
                                         std::uint32_t height,
                                         const fastloader::rfdetr::LiveCaptureRegion& region,
                                         std::uint64_t frame_id,
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
    if (live_session_ != nullptr || pending_live_frame_.has_value()) {
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
    state_.texture_id = {};
    state_.last_error.clear();
    state_.interop_failed = false;
}

LivePreviewTextureState LivePreviewTexture::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

ImVec2 LivePreviewTexture::uv0() {
    return ImVec2(0.0f, 0.0f);
}

ImVec2 LivePreviewTexture::uv1() {
    return ImVec2(1.0f, 1.0f);
}

bool LivePreviewTexture::initialize_gl_resources(std::string* error_message) {
    destroy_gl_resources();

    glGenTextures(2, textures_);
    for (GLuint texture : textures_) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::destroy_gl_resources() {
    if (textures_[0] != 0U || textures_[1] != 0U) {
        glDeleteTextures(2, textures_);
        textures_[0] = 0U;
        textures_[1] = 0U;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool LivePreviewTexture::ensure_texture_storage(std::uint32_t width,
                                                std::uint32_t height,
                                                std::string* error_message) {
    if (width == 0U || height == 0U) {
        if (error_message != nullptr) {
            *error_message = "preview dimensions must be non-zero";
        }
        return false;
    }
    if (width_ == width && height_ == height) {
        return true;
    }

    for (GLuint texture : textures_) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGB8,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    width_ = width;
    height_ = height;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

bool LivePreviewTexture::ensure_live_resources(std::uint32_t width,
                                               std::uint32_t height,
                                               int cuda_device_index,
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
            *error_message = cuda_error_message(cuda_status, "cudaSetDevice");
        }
        return false;
    }

    if (stream_ == nullptr) {
        cuda_status = fastloader::cuda_stream_create_with_highest_priority(&stream_, cudaStreamNonBlocking);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_error_message(cuda_status, "cudaStreamCreateWithPriority");
            }
            return false;
        }
    }

    if (copy_complete_event_ == nullptr) {
        cuda_status = cudaEventCreateWithFlags(&copy_complete_event_, cudaEventDisableTiming);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_error_message(cuda_status, "cudaEventCreateWithFlags");
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
                *error_message = cuda_error_message(cuda_status, "cudaGraphicsUnregisterResource");
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
                *error_message = cuda_error_message(cuda_status, "cudaGraphicsGLRegisterBuffer");
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

bool LivePreviewTexture::stage_live_preview_copy(fastloader::rfdetr::LivePredictSession& session,
                                                 const fastloader::rfdetr::LivePreviewFrame& frame,
                                                 int cuda_device_index,
                                                 std::string* error_message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_live_frame_.has_value()) {
            if (error_message != nullptr) {
                *error_message = "live preview copy is already in flight";
            }
            release_preview_silently(session, frame.buffer_index);
            return false;
        }
    }

    if (!ensure_live_resources(frame.buffer.width_px, frame.buffer.height_px, cuda_device_index, error_message)) {
        release_preview_silently(session, frame.buffer_index);
        return false;
    }

    const cudaError_t wait_status =
        cudaStreamWaitEvent(stream_, reinterpret_cast<cudaEvent_t>(frame.buffer.ready_event), 0);
    if (wait_status != cudaSuccess) {
        release_preview_silently(session, frame.buffer_index);
        if (error_message != nullptr) {
            *error_message = cuda_error_message(wait_status, "cudaStreamWaitEvent");
        }
        return false;
    }

    cudaError_t cuda_status = cudaGraphicsMapResources(1, &graphics_resource_, stream_);
    bool mapped = cuda_status == cudaSuccess;
    if (cuda_status != cudaSuccess) {
        release_preview_silently(session, frame.buffer_index);
        if (error_message != nullptr) {
            *error_message = cuda_error_message(cuda_status, "cudaGraphicsMapResources");
        }
        return false;
    }

    void* pixel_buffer_ptr = nullptr;
    std::size_t mapped_size = 0;
    const char* failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
    cuda_status = cudaGraphicsResourceGetMappedPointer(&pixel_buffer_ptr, &mapped_size, graphics_resource_);
    const std::size_t copy_width_bytes =
        static_cast<std::size_t>(frame.buffer.width_px) * kBgr3BytesPerPixel;
    const std::size_t copy_height = static_cast<std::size_t>(frame.buffer.height_px);
    const std::size_t required_bytes = copy_width_bytes * copy_height;
    if (cuda_status == cudaSuccess && mapped_size < required_bytes) {
        cuda_status = cudaErrorInvalidValue;
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaMemcpy2DAsync";
        cuda_status = cudaMemcpy2DAsync(pixel_buffer_ptr,
                                        copy_width_bytes,
                                        reinterpret_cast<const void*>(frame.buffer.data),
                                        frame.buffer.pitch_bytes,
                                        copy_width_bytes,
                                        copy_height,
                                        cudaMemcpyDeviceToDevice,
                                        stream_);
    }

    const cudaError_t unmap_status =
        mapped ? cudaGraphicsUnmapResources(1, &graphics_resource_, stream_) : cudaSuccess;

    if (cuda_status != cudaSuccess) {
        release_preview_silently(session, frame.buffer_index);
        if (error_message != nullptr) {
            *error_message = cuda_error_message(cuda_status, failed_cuda_label);
        }
        return false;
    }
    if (unmap_status != cudaSuccess) {
        release_preview_silently(session, frame.buffer_index);
        if (error_message != nullptr) {
            *error_message = cuda_error_message(unmap_status, "cudaGraphicsUnmapResources");
        }
        return false;
    }

    cuda_status = cudaEventRecord(copy_complete_event_, stream_);
    if (cuda_status != cudaSuccess) {
        release_preview_silently(session, frame.buffer_index);
        if (error_message != nullptr) {
            *error_message = cuda_error_message(cuda_status, "cudaEventRecord");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_live_frame_ = PendingLiveFrame{&session, frame};
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
                *error_message = cuda_error_message(set_device_status, "cudaSetDevice");
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
                pending_live_frame = std::move(*pending_live_frame_);
                pending_live_frame_.reset();
            }
        }
        if (pending_live_frame.has_value() && pending_live_frame->session != nullptr) {
            release_preview_silently(*pending_live_frame->session, pending_live_frame->frame.buffer_index);
        }
        if (error_message != nullptr) {
            *error_message = cuda_error_message(query_status, "cudaEventQuery");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_live_frame_.has_value()) {
            pending_live_frame = std::move(*pending_live_frame_);
            pending_live_frame_.reset();
        }
    }
    if (!pending_live_frame.has_value()) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

    const fastloader::rfdetr::LivePreviewFrame& frame = pending_live_frame->frame;
    glBindTexture(GL_TEXTURE_2D, textures_[back_texture_index_]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(frame.buffer.width_px),
                    static_cast<GLsizei>(frame.buffer.height_px),
                    GL_BGR,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();

    std::string release_error;
    if (pending_live_frame->session != nullptr &&
        !pending_live_frame->session->release_preview(frame.buffer_index, &release_error)) {
        if (error_message != nullptr) {
            *error_message = release_error.empty() ? "release_preview failed" : release_error;
        }
        return false;
    }

    publish_ready_texture(frame.buffer.width_px,
                          frame.buffer.height_px,
                          fastloader::rfdetr::LiveCaptureRegion{
                              frame.buffer.x_px,
                              frame.buffer.y_px,
                              frame.buffer.width_px,
                              frame.buffer.height_px,
                          },
                          frame.frame_id);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LivePreviewTexture::publish_ready_texture(std::uint32_t width,
                                               std::uint32_t height,
                                               const fastloader::rfdetr::LiveCaptureRegion& region,
                                               std::uint64_t frame_id) {
    std::swap(front_texture_index_, back_texture_index_);

    std::lock_guard<std::mutex> lock(mutex_);
    state_.has_frame = true;
    state_.texture_id = texture_id_from_name(textures_[front_texture_index_]);
    state_.displayed_region = region;
    state_.last_frame_id = frame_id;
    if (state_.displayed_region.width == 0U) {
        state_.displayed_region.width = width;
    }
    if (state_.displayed_region.height == 0U) {
        state_.displayed_region.height = height;
    }
}

void LivePreviewTexture::set_error(const std::string& error_message, bool interop_failed) {
    if (error_message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.last_error != error_message) {
        log_live_preview_error_to_stderr(error_message);
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
        publish = live_session_ == nullptr && frame.generation == host_frame_generation_;
    }
    if (publish) {
        publish_ready_texture(frame.width, frame.height, frame.region, frame.frame_id);
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

} // namespace fastloader::gui
