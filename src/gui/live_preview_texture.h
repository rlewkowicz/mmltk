#pragma once

#include "fastloader/rfdetr/live_predict.h"

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

namespace fastloader::gui {

struct LivePreviewTextureState {
    bool initialized = false;
    bool has_frame = false;
    ImTextureID texture_id = {};
    fastloader::rfdetr::LiveCaptureRegion displayed_region{};
    std::uint64_t last_frame_id = 0;
    std::string last_error;
    bool interop_failed = false;
};

class LivePreviewTexture {
public:
    explicit LivePreviewTexture(GLFWwindow* shared_context_window);
    ~LivePreviewTexture();

    bool initialize(std::string* error_message);
    void shutdown();
    void begin_live_stream(fastloader::rfdetr::LivePredictSession& session, int cuda_device_index);
    void end_live_stream();
    void pump();
    bool submit_host_bgr(std::vector<std::uint8_t> pixels,
                         std::uint32_t width,
                         std::uint32_t height,
                         const fastloader::rfdetr::LiveCaptureRegion& region,
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
        fastloader::rfdetr::LiveCaptureRegion region{};
        std::uint64_t frame_id = 0;
        std::uint64_t generation = 0;
    };

    struct PendingLiveFrame {
        fastloader::rfdetr::LivePredictSession* session = nullptr;
        fastloader::rfdetr::LivePreviewFrame frame{};
    };

    bool initialize_gl_resources(std::string* error_message);
    void destroy_gl_resources();
    bool ensure_texture_storage(std::uint32_t width, std::uint32_t height, std::string* error_message);
    bool ensure_live_resources(std::uint32_t width,
                               std::uint32_t height,
                               int cuda_device_index,
                               std::string* error_message);
    void destroy_live_resources();
    bool stage_live_preview_copy(fastloader::rfdetr::LivePredictSession& session,
                                 const fastloader::rfdetr::LivePreviewFrame& frame,
                                 int cuda_device_index,
                                 std::string* error_message);
    bool finalize_live_preview_copy(std::string* error_message);
    void publish_ready_texture(std::uint32_t width,
                               std::uint32_t height,
                               const fastloader::rfdetr::LiveCaptureRegion& region,
                               std::uint64_t frame_id);
    void set_error(const std::string& error_message, bool interop_failed);
    void clear_error();
    bool upload_host_preview(const PendingHostFrame& frame, std::string* error_message);
    bool initialized() const;

    GLFWwindow* shared_context_window_ = nullptr;
    mutable std::mutex mutex_;
    LivePreviewTextureState state_{};
    bool initialized_ = false;
    fastloader::rfdetr::LivePredictSession* live_session_ = nullptr;
    int live_cuda_device_index_ = 0;
    std::uint64_t host_frame_generation_ = 1;
    std::optional<PendingHostFrame> pending_host_frame_;
    std::optional<PendingLiveFrame> pending_live_frame_;

    GLuint textures_[2] = {0U, 0U};
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

} // namespace fastloader::gui
