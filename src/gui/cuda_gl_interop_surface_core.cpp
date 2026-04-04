#include "cuda_gl_interop_surface_core.h"

#include "cuda_priority.h"
#include "mmltk/live/live_types.h"

#include <utility>

namespace mmltk::gui {

CudaGlInteropSurfaceCore::CudaGlInteropSurfaceCore(const CudaGlInteropSurfaceConfig config)
    : config_(config) {}

CudaGlInteropSurfaceCore::~CudaGlInteropSurfaceCore() {
    shutdown();
}

bool CudaGlInteropSurfaceCore::initialize(std::string* error_message) {
    shutdown();
    initialize_gui_texture_pair(textures_);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void CudaGlInteropSurfaceCore::shutdown() {
    destroy_upload_resources();
    destroy_gui_texture_pair(textures_);
    width_ = 0;
    height_ = 0;
    front_texture_index_ = 0;
    back_texture_index_ = 1;
}

bool CudaGlInteropSurfaceCore::ensure_texture_storage(const std::uint32_t width,
                                                      const std::uint32_t height,
                                                      std::string* error_message) {
    return ensure_gui_texture_storage(textures_,
                                      width,
                                      height,
                                      config_.internal_format,
                                      config_.upload_format,
                                      config_.zero_dimension_message,
                                      width_,
                                      height_,
                                      error_message);
}

bool CudaGlInteropSurfaceCore::ensure_upload_resources(const std::uint32_t width,
                                                       const std::uint32_t height,
                                                       const int cuda_device_index,
                                                       std::string* error_message) {
    const bool storage_changed = width_ != width || height_ != height;
    if (cuda_device_index_ >= 0 && cuda_device_index_ != cuda_device_index) {
        destroy_upload_resources();
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

    if (completion_event_ == nullptr) {
        cuda_status = cudaEventCreateWithFlags(&completion_event_, cudaEventDisableTiming);
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
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
        const std::size_t required_bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * config_.bytes_per_pixel;
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

    cuda_device_index_ = cuda_device_index;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void CudaGlInteropSurfaceCore::reset_upload_resources() noexcept {
    destroy_upload_resources();
}

bool CudaGlInteropSurfaceCore::set_cuda_device(std::string* error_message) const {
    if (cuda_device_index_ < 0) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }
    const cudaError_t status = cudaSetDevice(cuda_device_index_);
    if (status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(status, "cudaSetDevice");
        }
        return false;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void CudaGlInteropSurfaceCore::synchronize_stream() const noexcept {
    if (stream_ != nullptr) {
        (void)cudaStreamSynchronize(stream_);
    }
}

bool CudaGlInteropSurfaceCore::update_back_texture_from_pixel_buffer(const std::uint32_t width,
                                                                     const std::uint32_t height,
                                                                     std::string* error_message) const {
    glBindTexture(GL_TEXTURE_2D, textures_[back_texture_index_]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height),
                    config_.upload_format,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void CudaGlInteropSurfaceCore::swap_ready_texture() noexcept {
    std::swap(front_texture_index_, back_texture_index_);
}

ImTextureID CudaGlInteropSurfaceCore::front_texture_id() const {
    return imgui_texture_id_from_gl_name(textures_[front_texture_index_]);
}

GLuint CudaGlInteropSurfaceCore::back_texture() const noexcept {
    return textures_[back_texture_index_];
}

GLuint CudaGlInteropSurfaceCore::pixel_buffer() const noexcept {
    return pixel_buffer_;
}

cudaGraphicsResource_t CudaGlInteropSurfaceCore::graphics_resource() const noexcept {
    return graphics_resource_;
}

cudaGraphicsResource_t* CudaGlInteropSurfaceCore::graphics_resource_ptr() noexcept {
    return &graphics_resource_;
}

cudaStream_t CudaGlInteropSurfaceCore::stream() const noexcept {
    return stream_;
}

cudaEvent_t CudaGlInteropSurfaceCore::completion_event() const noexcept {
    return completion_event_;
}

int CudaGlInteropSurfaceCore::cuda_device_index() const noexcept {
    return cuda_device_index_;
}

std::uint32_t CudaGlInteropSurfaceCore::width() const noexcept {
    return width_;
}

std::uint32_t CudaGlInteropSurfaceCore::height() const noexcept {
    return height_;
}

bool CudaGlInteropSurfaceCore::copy_pitched_to_gl_interop(const CUdeviceptr source_ptr,
                                                          const std::size_t source_pitch,
                                                          const std::uint32_t width,
                                                          const std::uint32_t height,
                                                          const std::size_t bytes_per_pixel,
                                                          const cudaEvent_t ready_event,
                                                          std::string* error_message) {
    cudaError_t cuda_status = cudaSuccess;
    const char* failed_cuda_label = nullptr;
    if (ready_event != nullptr) {
        failed_cuda_label = "cudaStreamWaitEvent";
        cuda_status = cudaStreamWaitEvent(stream_, ready_event, 0);
    }

    bool mapped = false;
    void* pixel_buffer_ptr = nullptr;
    std::size_t mapped_size = 0U;
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsMapResources";
        cuda_status = cudaGraphicsMapResources(1, &graphics_resource_, stream_);
        mapped = cuda_status == cudaSuccess;
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
        cuda_status = cudaGraphicsResourceGetMappedPointer(
            &pixel_buffer_ptr,
            &mapped_size,
            graphics_resource_);
    }

    const std::size_t copy_width_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
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
                                        mmltk::live::device_ptr_as_const_void(source_ptr),
                                        source_pitch,
                                        copy_width_bytes,
                                        copy_height,
                                        cudaMemcpyDeviceToDevice,
                                        stream_);
    }

    const cudaError_t unmap_status =
        mapped ? cudaGraphicsUnmapResources(1, &graphics_resource_, stream_)
               : cudaSuccess;

    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, failed_cuda_label);
        }
        return false;
    }
    if (unmap_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(unmap_status, "cudaGraphicsUnmapResources");
        }
        return false;
    }

    cuda_status = cudaEventRecord(completion_event_, stream_);
    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, "cudaEventRecord");
        }
        return false;
    }

    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void CudaGlInteropSurfaceCore::destroy_upload_resources() noexcept {
    if (cuda_device_index_ >= 0) {
        (void)cudaSetDevice(cuda_device_index_);
    }
    synchronize_stream();
    if (graphics_resource_ != nullptr) {
        (void)cudaGraphicsUnregisterResource(graphics_resource_);
        graphics_resource_ = nullptr;
    }
    if (completion_event_ != nullptr) {
        (void)cudaEventDestroy(completion_event_);
        completion_event_ = nullptr;
    }
    if (stream_ != nullptr) {
        (void)cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
    if (pixel_buffer_ != 0U) {
        glDeleteBuffers(1, &pixel_buffer_);
        pixel_buffer_ = 0U;
    }
    cuda_device_index_ = -1;
}

} // namespace mmltk::gui
