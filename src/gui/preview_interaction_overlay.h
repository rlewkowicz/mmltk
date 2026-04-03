#pragma once

#include "annotation_core.h"
#include "live/live_helpers.h"
#include "mmltk/live/live_types.h"

#include <cuda.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
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

namespace mmltk::gui {

struct PreviewInteractionOverlayBox {
    AnnotationBox box{};
    std::uint8_t r = 255U;
    std::uint8_t g = 255U;
    std::uint8_t b = 255U;
    int thickness = 1;
    bool draw_handles = false;
    int handle_radius = 4;
};

struct PreviewInteractionOverlayPoint {
    int x = 0;
    int y = 0;
};

struct PreviewInteractionOverlayEdge {
    std::uint32_t source_index = 0;
    std::uint32_t target_index = 0;
};

struct PreviewInteractionOverlayPolyline {
    std::vector<PreviewInteractionOverlayPoint> points;
    bool closed = false;
    std::uint8_t r = 255U;
    std::uint8_t g = 255U;
    std::uint8_t b = 255U;
    int thickness = 1;
};

struct PreviewInteractionOverlayMarkerSet {
    std::vector<PreviewInteractionOverlayPoint> points;
    int radius = 3;
    std::uint8_t r = 255U;
    std::uint8_t g = 255U;
    std::uint8_t b = 255U;
    std::uint8_t alpha = 255U;
};

struct PreviewInteractionOverlaySkeleton {
    std::vector<PreviewInteractionOverlayPoint> points;
    std::vector<PreviewInteractionOverlayEdge> edges;
    std::uint8_t r = 255U;
    std::uint8_t g = 255U;
    std::uint8_t b = 255U;
    int thickness = 1;
};

struct PreviewInteractionOverlaySnapshot {
    std::uint64_t generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int cuda_device_index = 0;
    std::vector<PreviewInteractionOverlayBox> boxes;
    std::vector<PreviewInteractionOverlayPolyline> polylines;
    std::vector<PreviewInteractionOverlayMarkerSet> marker_sets;
    std::vector<PreviewInteractionOverlaySkeleton> skeletons;
};

struct PreviewInteractionOverlayState {
    bool initialized = false;
    bool has_frame = false;
    ImTextureID texture_id = {};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string last_error;
    bool interop_failed = false;
};

class PreviewInteractionOverlaySurface {
public:
    explicit PreviewInteractionOverlaySurface(GLFWwindow* shared_context_window);
    ~PreviewInteractionOverlaySurface();

    bool initialize(std::string* error_message);
    void shutdown();
    void pump();
    void publish_snapshot(PreviewInteractionOverlaySnapshot snapshot);

    [[nodiscard]] PreviewInteractionOverlayState snapshot() const;

private:
    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        CUdeviceptr device_ptr = 0;
        std::size_t pitch_bytes = 0;
        std::uint32_t capacity_width = 0;
        std::uint32_t capacity_height = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        int cuda_device_index = 0;
        cudaStream_t stream = nullptr;
        cudaEvent_t ready_event = nullptr;
        std::atomic<std::uint32_t> state{mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree)};
        std::uint64_t generation = 0;
    };

    struct PendingUpload {
        std::uint32_t slot_index = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        int cuda_device_index = 0;
        std::uint64_t generation = 0;
    };

    bool initialize_gl_resources(std::string* error_message);
    void destroy_gl_resources();
    bool ensure_texture_storage(std::uint32_t width, std::uint32_t height, std::string* error_message);
    bool ensure_upload_resources(std::uint32_t width,
                                 std::uint32_t height,
                                 int cuda_device_index,
                                 std::string* error_message);
    void destroy_upload_resources();
    bool stage_pending_upload(const OverlaySlot& slot, std::string* error_message);
    bool finalize_pending_upload(std::string* error_message);
    void publish_ready_texture(std::uint32_t width, std::uint32_t height);
    void set_error(const std::string& error_message, bool interop_failed);
    void clear_error();

    void worker_thread_main();
    void render_snapshot(const PreviewInteractionOverlaySnapshot& snapshot);
    bool ensure_overlay_slot_capacity(OverlaySlot& slot,
                                      std::uint32_t width,
                                      std::uint32_t height,
                                      int cuda_device_index);
    void destroy_overlay_slots() noexcept;
    OverlaySlot* reserve_overlay_slot();
    bool try_acquire_overlay_slot(OverlaySlot& slot, OverlaySlot** out);
    void release_overlay_slot(std::uint32_t slot_index);

    GLFWwindow* shared_context_window_ = nullptr;
    mutable std::mutex mutex_;
    PreviewInteractionOverlayState state_{};
    bool initialized_ = false;

    std::mutex snapshot_mutex_;
    std::condition_variable snapshot_cv_;
    std::shared_ptr<const PreviewInteractionOverlaySnapshot> latest_snapshot_;
    std::uint64_t next_generation_ = 1;
    std::thread worker_thread_;
    std::atomic<bool> stop_requested_{false};

    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots_;
    std::atomic<int> latest_overlay_index_{-1};
    int worker_cuda_device_index_ = -1;

    std::optional<PendingUpload> pending_upload_;
    std::array<GLuint, 2> textures_{0U, 0U};
    int front_texture_index_ = 0;
    int back_texture_index_ = 1;
    GLuint pixel_buffer_ = 0U;
    cudaGraphicsResource_t graphics_resource_ = nullptr;
    cudaStream_t upload_stream_ = nullptr;
    cudaEvent_t upload_complete_event_ = nullptr;
    int upload_cuda_device_index_ = -1;
    std::uint32_t texture_width_ = 0;
    std::uint32_t texture_height_ = 0;
    mmltk::live::PinnedUploadBuffer<int> host_points_upload_;
    mmltk::live::DeviceUploadBuffer<int> device_points_upload_;
    mmltk::live::PinnedUploadBuffer<std::uint32_t> host_edges_upload_;
    mmltk::live::DeviceUploadBuffer<std::uint32_t> device_edges_upload_;
};

} // namespace mmltk::gui
