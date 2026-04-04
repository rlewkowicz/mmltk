#pragma once

#include "cuda_gl_interop_utils.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <cuda.h>
#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>
#include <imgui.h>

namespace mmltk::gui {

struct CudaGlInteropSurfaceConfig {
    GLint internal_format = 0;
    GLenum upload_format = 0;
    std::size_t bytes_per_pixel = 0;
    const char* zero_dimension_message = "";
};

class CudaGlInteropSurfaceCore {
public:
    explicit CudaGlInteropSurfaceCore(CudaGlInteropSurfaceConfig config = {});
    ~CudaGlInteropSurfaceCore();

    CudaGlInteropSurfaceCore(const CudaGlInteropSurfaceCore&) = delete;
    CudaGlInteropSurfaceCore& operator=(const CudaGlInteropSurfaceCore&) = delete;

    bool initialize(std::string* error_message);
    void shutdown();

    bool ensure_texture_storage(std::uint32_t width,
                                std::uint32_t height,
                                std::string* error_message);
    bool ensure_upload_resources(std::uint32_t width,
                                 std::uint32_t height,
                                 int cuda_device_index,
                                 std::string* error_message);
    void reset_upload_resources() noexcept;
    bool set_cuda_device(std::string* error_message) const;
    void synchronize_stream() const noexcept;
    bool update_back_texture_from_pixel_buffer(std::uint32_t width,
                                               std::uint32_t height,
                                               std::string* error_message) const;
    void swap_ready_texture() noexcept;

    bool copy_pitched_to_gl_interop(CUdeviceptr source_ptr,
                                    std::size_t source_pitch,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    std::size_t bytes_per_pixel,
                                    cudaEvent_t ready_event,
                                    std::string* error_message);

    [[nodiscard]] ImTextureID front_texture_id() const;
    [[nodiscard]] GLuint back_texture() const noexcept;
    [[nodiscard]] GLuint pixel_buffer() const noexcept;
    [[nodiscard]] cudaGraphicsResource_t graphics_resource() const noexcept;
    [[nodiscard]] cudaGraphicsResource_t* graphics_resource_ptr() noexcept;
    [[nodiscard]] cudaStream_t stream() const noexcept;
    [[nodiscard]] cudaEvent_t completion_event() const noexcept;
    [[nodiscard]] int cuda_device_index() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

private:
    void destroy_upload_resources() noexcept;

    CudaGlInteropSurfaceConfig config_{};
    std::array<GLuint, 2> textures_{0U, 0U};
    int front_texture_index_ = 0;
    int back_texture_index_ = 1;
    GLuint pixel_buffer_ = 0U;
    cudaGraphicsResource_t graphics_resource_ = nullptr;
    cudaStream_t stream_ = nullptr;
    cudaEvent_t completion_event_ = nullptr;
    int cuda_device_index_ = -1;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace mmltk::gui
