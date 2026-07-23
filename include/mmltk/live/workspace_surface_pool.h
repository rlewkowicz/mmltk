#pragma once

#include "mmltk/live/live_types.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmltk::live {

inline constexpr std::size_t kWorkspaceDeviceUuidBytes = 16U;

struct WorkspaceVulkanAdapterCapabilities {
    std::uint32_t render_major = 0U;
    std::uint32_t render_minor = 0U;
    std::array<std::uint8_t, kWorkspaceDeviceUuidBytes> device_uuid{};
    std::vector<std::uint64_t> rgba8_modifiers;
    bool timeline_semaphore = false;

    [[nodiscard]] bool valid() const noexcept {
        return render_major != 0U &&
               std::any_of(device_uuid.begin(), device_uuid.end(),
                           [](const std::uint8_t byte) { return byte != 0U; }) &&
               !rgba8_modifiers.empty() && timeline_semaphore;
    }
};

void publish_workspace_vulkan_adapter(WorkspaceVulkanAdapterCapabilities capabilities);
void clear_workspace_vulkan_adapter() noexcept;
[[nodiscard]] bool workspace_vulkan_adapter_ready() noexcept;
[[nodiscard]] std::uint64_t require_workspace_drm_modifier(int cuda_device_index);

struct WorkspaceSurfacePoolConfig {
    int cuda_device_index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slot_count = 4;
    std::uint32_t semantic_cache_slot_count = 3;
    std::uint64_t negotiated_drm_modifier = kWorkspaceDmaBufDrmFormatModInvalid;
};

struct WorkspaceSurfaceWriteLease {
    bool valid = false;
    std::uint64_t generation = 0;
    std::uint32_t slot_index = 0;
    CUdeviceptr data = 0;
    std::size_t pitch_bytes = 0;
    cudaSurfaceObject_t surface_object = 0;
    cudaStream_t stream = nullptr;
    bool pitched = false;
    bool array = false;
};

struct WorkspaceSurfacePublishInfo {
    LiveFrameId frame_id{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    LiveCaptureRegion source_region{};
    LiveCaptureRegion dirty_rect{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

struct WorkspaceSurfacePoolStatus {
    std::uint64_t source_acquire_waits = 0;
    std::uint64_t source_leases_acquired = 0;
    std::uint64_t source_leases_released = 0;
    std::uint64_t source_stale_releases = 0;
    std::uint64_t source_skipped_stale_frames = 0;
    std::uint64_t source_slot_pressure = 0;
    std::uint64_t source_release_latency_ns = 0;
    std::int32_t front_slot_index = -1;
    std::uint64_t front_slot_revision = 0;
};

class WorkspaceSurfacePool {
   public:
    explicit WorkspaceSurfacePool(WorkspaceSurfacePoolConfig config);
    ~WorkspaceSurfacePool();

    WorkspaceSurfacePool(const WorkspaceSurfacePool&) = delete;
    WorkspaceSurfacePool& operator=(const WorkspaceSurfacePool&) = delete;

    [[nodiscard]] WorkspaceSwapchainDescriptor descriptor() const;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] WorkspacePresentSnapshot latest_present() const noexcept;
    [[nodiscard]] WorkspaceSurfacePoolStatus snapshot_status() const noexcept;

    [[nodiscard]] bool try_reserve_write(WorkspaceSurfaceWriteLease* out);
    [[nodiscard]] WorkspacePresentSnapshot publish_write(WorkspaceSurfaceWriteLease* lease,
                                                         const WorkspaceSurfacePublishInfo& info);
    void cancel_write(WorkspaceSurfaceWriteLease* lease) noexcept;

    [[nodiscard]] bool request_next_source_frame(std::uint64_t after_revision,
                                                 const std::atomic<bool>& caller_stop_requested,
                                                 const std::atomic<bool>& owner_stop_requested,
                                                 const std::atomic<bool>& owner_running,
                                                 WorkspaceSourceFrameLease* out);
    void release_source_frame(const WorkspaceSourceFrameLease& lease) noexcept;
    [[nodiscard]] bool try_acquire_latest(WorkspaceOutputBundle* out);
    void release_acquired(std::uint32_t slot_index);

    [[nodiscard]] bool acquire_slot_for_import(std::uint32_t slot_index);
    [[nodiscard]] bool mark_slot_in_flight(std::uint32_t slot_index, bool in_flight);
    [[nodiscard]] bool import_timeline_semaphore(std::uint64_t generation, std::uint32_t slot_index, int semaphore_fd,
                                                 std::string* error_message);
    [[nodiscard]] bool replace_timeline_semaphore(std::uint64_t generation, std::uint32_t slot_index, int semaphore_fd,
                                                  std::string* error_message);
    [[nodiscard]] bool timeline_slot_ready(std::uint64_t generation, std::uint32_t slot_index) const noexcept;
    [[nodiscard]] bool timeline_sync_ready(std::uint64_t generation) const noexcept;
    void reset_timeline_semaphores() noexcept;
    void invalidate_presentation() noexcept;
    [[nodiscard]] std::uint64_t presentation_epoch() const noexcept;
    [[nodiscard]] std::uint64_t availability_epoch() const noexcept;

    void cache_semantic_source(cudaStream_t source_stream, const OutputBundle& source);
    [[nodiscard]] bool readback_semantic_source(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                                std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                                std::string* error_message);

    void reset_for_producer_restart() noexcept;
    void reset_status() noexcept;
    void notify_waiters() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  
