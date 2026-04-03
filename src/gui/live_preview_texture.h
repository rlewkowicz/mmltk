#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_frame_id.h"

#include <cuda.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>
#include <imgui.h>

struct GLFWwindow;

namespace mmltk::live {
class LiveSessionController;
}

namespace mmltk::gui {

struct LivePreviewTextureState {
    bool initialized = false;
    bool has_frame = false;
    ImTextureID texture_id = {};
    mmltk::live::LiveCaptureRegion displayed_region{};
    std::uint64_t last_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::string last_error;
    bool interop_failed = false;
};

class LivePreviewTexture {
public:
    explicit LivePreviewTexture(GLFWwindow* shared_context_window);
    ~LivePreviewTexture();

    bool initialize(std::string* error_message);
    void shutdown();
    void begin_live_stream(mmltk::live::LiveSessionController& controller, int cuda_device_index);
    void end_live_stream();
    void pump();
    bool submit_host_bgr(std::vector<std::uint8_t> pixels,
                         std::uint32_t width,
                         std::uint32_t height,
                         const mmltk::live::LiveCaptureRegion& region,
                         std::uint64_t frame_id,
                         std::string* error_message);
    void clear_frame();

    LivePreviewTextureState snapshot() const;
    static ImVec2 uv0();
    static ImVec2 uv1();

private:
    struct PendingHostFrame {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        mmltk::live::LiveCaptureRegion region{};
        std::uint64_t frame_id = 0;
        std::uint64_t generation = 0;
    };

    enum class LiveSourceKind : std::uint8_t {
        None = 0,
        ControllerOutput = 1,
    };

    struct PendingLiveFrame {
        LiveSourceKind source_kind = LiveSourceKind::None;
        mmltk::live::LiveSessionController* controller = nullptr;
        std::uint32_t release_index = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        mmltk::live::LiveCaptureRegion region{};
        std::uint64_t frame_id = 0;
        mmltk::live::LiveFrameId live_frame_id{};
    };

    bool initialize_gl_resources(std::string* error_message);
    void destroy_gl_resources();
    bool ensure_texture_storage(std::uint32_t width, std::uint32_t height, std::string* error_message);
    bool ensure_live_resources(std::uint32_t width,
                               std::uint32_t height,
                               int cuda_device_index,
                               std::string* error_message);
    void destroy_live_resources();
    bool stage_live_preview_copy(CUdeviceptr source_data,
                                 std::size_t source_pitch_bytes,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 cudaEvent_t ready_event,
                                 PendingLiveFrame pending_frame,
                                 int cuda_device_index,
                                 std::string* error_message);
    bool finalize_live_preview_copy(std::string* error_message);
    void release_pending_live_frame_silently(const PendingLiveFrame& pending_frame) noexcept;
    void publish_ready_texture(std::uint32_t width,
                               std::uint32_t height,
                               const mmltk::live::LiveCaptureRegion& region,
                               std::uint64_t frame_id,
                               std::optional<mmltk::live::LiveFrameId> live_frame_id);
    void set_error(const std::string& error_message, bool interop_failed);
    void clear_error();
    bool upload_host_preview(const PendingHostFrame& frame, std::string* error_message);
    bool initialized() const;

    GLFWwindow* shared_context_window_ = nullptr;
    mutable std::mutex mutex_;
    LivePreviewTextureState state_{};
    bool initialized_ = false;
    LiveSourceKind live_source_kind_ = LiveSourceKind::None;
    mmltk::live::LiveSessionController* live_controller_ = nullptr;
    int live_cuda_device_index_ = 0;
    std::uint64_t host_frame_generation_ = 1;
    std::optional<PendingHostFrame> pending_host_frame_;
    std::optional<PendingLiveFrame> pending_live_frame_;

    std::array<GLuint, 2> textures_{0U, 0U};
    int front_texture_index_ = 0;
    int back_texture_index_ = 1;
    GLuint pixel_buffer_ = 0U;
    cudaGraphicsResource_t graphics_resource_ = nullptr;
    cudaStream_t stream_ = nullptr;
    cudaEvent_t copy_complete_event_ = nullptr;
    int current_cuda_device_index_ = -1;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace mmltk::gui
