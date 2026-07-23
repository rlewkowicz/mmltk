#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_frame_id.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <cuda.h>
#include <cuda_runtime_api.h>

namespace mmltk::live {

inline constexpr std::uint32_t kWorkspaceDmaBufDrmFormatAbgr8888 = 0x34324241U;
inline constexpr std::uint64_t kWorkspaceDmaBufDrmFormatModLinear = 0U;
inline constexpr std::uint64_t kWorkspaceDmaBufDrmFormatModInvalid = (1ULL << 56U) - 1U;

enum class SlotState : std::uint8_t {
    kFree = 0,
    kWriting = 1,
    kPublished = 2,
    kAcquired = 3,
    kPendingFree = 4,
};

inline std::uint32_t to_slot_state_value(const SlotState state) noexcept {
    return static_cast<std::uint32_t>(state);
}

inline void* device_ptr_as_void(const CUdeviceptr ptr) noexcept {
    return reinterpret_cast<void*>(ptr);  // NOLINT(performance-no-int-to-ptr)
}

inline const void* device_ptr_as_const_void(const CUdeviceptr ptr) noexcept {
    return reinterpret_cast<const void*>(ptr);  // NOLINT(performance-no-int-to-ptr)
}

inline std::uint8_t* device_ptr_as_bytes(const CUdeviceptr ptr) noexcept {
    return reinterpret_cast<std::uint8_t*>(ptr);  // NOLINT(performance-no-int-to-ptr)
}

struct SourceFrameView {
    std::uint32_t buffer_index = 0;
    LiveFrameId frame_id{};
    CUdeviceptr data = 0;
    std::size_t pitch_bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    cudaEvent_t ready_event = nullptr;
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

struct DetectDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch_bytes = 0;
};

struct OutputDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch_bytes = 0;
};

struct WorkspaceDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch_bytes = 0;
};

struct WorkspaceDmaBufImage {
    int fd = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t stride_bytes = 0;
    std::uint64_t offset = 0;
    std::uint64_t allocation_size = 0;
    std::uint32_t drm_format = 0;
    std::uint64_t drm_modifier = kWorkspaceDmaBufDrmFormatModInvalid;

    [[nodiscard]] bool valid() const noexcept {
        return fd >= 0 && width > 0U && height > 0U && stride_bytes >= static_cast<std::uint64_t>(width) * 4U &&
               height <= std::numeric_limits<std::uint64_t>::max() / stride_bytes && allocation_size >= offset &&
               allocation_size - offset >= stride_bytes * height && drm_format == kWorkspaceDmaBufDrmFormatAbgr8888 &&
               drm_modifier != kWorkspaceDmaBufDrmFormatModInvalid;
    }
};

struct DetectBundle {
    std::uint32_t slot_index = 0;
    LiveFrameId frame_id{};
    CUdeviceptr data = 0;
    DetectDimensions dims{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

struct OutputBundle {
    std::uint32_t slot_index = 0;
    LiveFrameId frame_id{};
    CUdeviceptr data = 0;
    OutputDimensions dims{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

struct WorkspaceOutputBundle {
    std::uint32_t slot_index = 0;
    LiveFrameId frame_id{};
    CUdeviceptr data = 0;
    WorkspaceDimensions dims{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
    WorkspaceDmaBufImage dmabuf_image{};
};

struct WorkspaceSwapchainSlotDescriptor {
    std::uint32_t slot_index = 0;
    WorkspaceDimensions dims{};
    WorkspaceDmaBufImage dmabuf_image{};
    std::uint64_t revision = 0;

    [[nodiscard]] bool valid() const noexcept {
        return dims.width > 0U && dims.height > 0U && dims.pitch_bytes >= static_cast<std::size_t>(dims.width) * 4U &&
               dmabuf_image.valid();
    }
};

struct WorkspaceSwapchainDescriptor {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0;
    int cuda_device_index = -1;
    std::string render_node_path;
    std::vector<WorkspaceSwapchainSlotDescriptor> slots;

    [[nodiscard]] bool valid() const noexcept {
        if (width == 0U || height == 0U || cuda_device_index < 0 || render_node_path.empty() || slots.empty()) {
            return false;
        }
        for (const WorkspaceSwapchainSlotDescriptor& slot : slots) {
            if (!slot.valid()) {
                return false;
            }
        }
        return true;
    }
};

struct WorkspaceFrameMetadata {
    std::uint64_t revision = 0;
    std::uint64_t swapchain_generation = 0;
    LiveFrameId frame_id{};
    WorkspaceDimensions dims{};
    WorkspaceDmaBufImage dmabuf_image{};
    LiveCaptureRegion source_region{};
    LiveCaptureRegion dirty_rect{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;

    WorkspaceFrameMetadata() = default;

    WorkspaceFrameMetadata(const std::uint64_t in_revision, const std::uint64_t in_swapchain_generation,
                           const LiveFrameId in_frame_id, const WorkspaceDimensions in_dims,
                           const WorkspaceDmaBufImage in_dmabuf_image, const LiveCaptureRegion in_source_region,
                           const LiveCaptureRegion in_dirty_rect, const std::uint64_t in_capture_ns,
                           const std::uint64_t in_ready_ns, const bool in_short_frame) noexcept
        : revision(in_revision),
          swapchain_generation(in_swapchain_generation),
          frame_id(in_frame_id),
          dims(in_dims),
          dmabuf_image(in_dmabuf_image),
          source_region(in_source_region),
          dirty_rect(in_dirty_rect),
          capture_ns(in_capture_ns),
          ready_ns(in_ready_ns),
          short_frame(in_short_frame) {}
};

struct WorkspacePresentSnapshot : WorkspaceFrameMetadata {
    bool valid = false;
    std::uint32_t front_slot_index = 0;

    WorkspacePresentSnapshot() = default;

    WorkspacePresentSnapshot(const bool in_valid, const std::uint32_t in_front_slot_index,
                             const WorkspaceFrameMetadata& metadata) noexcept
        : WorkspaceFrameMetadata(metadata), valid(in_valid), front_slot_index(in_front_slot_index) {}
};

struct WorkspaceSourceFrameLease : WorkspaceFrameMetadata {
    bool valid = false;
    std::uint32_t slot_index = 0;

    WorkspaceSourceFrameLease() = default;

    WorkspaceSourceFrameLease(const bool in_valid, const std::uint32_t in_slot_index,
                              const WorkspaceFrameMetadata& metadata) noexcept
        : WorkspaceFrameMetadata(metadata), valid(in_valid), slot_index(in_slot_index) {}

    [[nodiscard]] bool matches(const std::uint64_t generation, const std::uint32_t slot,
                               const std::uint64_t frame_revision) const noexcept {
        return valid && swapchain_generation == generation && slot_index == slot && revision == frame_revision;
    }

    [[nodiscard]] WorkspacePresentSnapshot present_snapshot() const noexcept;

    [[nodiscard]] WorkspaceFrameMetadata frame_metadata() const noexcept {
        return static_cast<const WorkspaceFrameMetadata&>(*this);
    }
};

[[nodiscard]] inline WorkspacePresentSnapshot make_workspace_present_snapshot(
    const bool valid, const std::uint32_t front_slot_index, const WorkspaceFrameMetadata& metadata) noexcept {
    return WorkspacePresentSnapshot{valid, front_slot_index, metadata};
}

[[nodiscard]] inline WorkspaceSourceFrameLease make_workspace_source_frame_lease(
    const bool valid, const std::uint32_t slot_index, const WorkspaceFrameMetadata& metadata) noexcept {
    return WorkspaceSourceFrameLease{valid, slot_index, metadata};
}

[[nodiscard]] inline WorkspacePresentSnapshot WorkspaceSourceFrameLease::present_snapshot() const noexcept {
    return make_workspace_present_snapshot(valid, slot_index, frame_metadata());
}

}  
