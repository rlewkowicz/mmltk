#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_frame_id.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <cuda.h>
#include <cuda_runtime_api.h>

namespace mmltk::live {

inline constexpr std::uint32_t kWorkspaceDmaBufDrmFormatAbgr8888 = 0x34324241U;
inline constexpr std::uint64_t kWorkspaceDmaBufDrmFormatModLinear = 0U;
inline constexpr std::uint64_t kWorkspaceDmaBufDrmFormatModInvalid = (1ULL << 56U) - 1U;

enum class DmaBufModifierMode : std::uint8_t {
    Unknown = 0,
    Implicit = 1,
    Explicit = 2,
};

enum class SlotState : std::uint8_t {
    kFree = 0,
    kWriting = 1,
    kPublished = 2,
    kAcquired = 3,
};

inline std::uint32_t to_slot_state_value(SlotState state) noexcept {
    return static_cast<std::uint32_t>(state);
}

inline void* device_ptr_as_void(CUdeviceptr ptr) noexcept {
    return reinterpret_cast<void*>(ptr);  // NOLINT(performance-no-int-to-ptr)
}

inline const void* device_ptr_as_const_void(CUdeviceptr ptr) noexcept {
    return reinterpret_cast<const void*>(ptr);  // NOLINT(performance-no-int-to-ptr)
}

inline std::uint8_t* device_ptr_as_bytes(CUdeviceptr ptr) noexcept {
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
    std::uint64_t drm_modifier = 0;
    DmaBufModifierMode modifier_mode = DmaBufModifierMode::Unknown;

    [[nodiscard]] bool valid() const noexcept {
        const bool modifier_valid =
            (modifier_mode == DmaBufModifierMode::Implicit && drm_modifier == kWorkspaceDmaBufDrmFormatModInvalid) ||
            (modifier_mode == DmaBufModifierMode::Explicit && drm_modifier != kWorkspaceDmaBufDrmFormatModInvalid);
        return fd >= 0 && width > 0U && height > 0U && stride_bytes >= static_cast<std::uint64_t>(width) * 4U &&
               height <= std::numeric_limits<std::uint64_t>::max() / stride_bytes && allocation_size >= offset &&
               allocation_size - offset >= stride_bytes * height && drm_format == kWorkspaceDmaBufDrmFormatAbgr8888 &&
               modifier_valid;
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
    std::uintptr_t ready_event_handle = 0;
    std::uintptr_t producer_stream_handle = 0;
    std::uint64_t revision = 0;

    [[nodiscard]] bool valid() const noexcept {
        return dims.width > 0U && dims.height > 0U &&
               dims.pitch_bytes >= static_cast<std::size_t>(dims.width) * 4U && dmabuf_image.valid();
    }
};

struct WorkspaceSwapchainDescriptor {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0;
    std::vector<WorkspaceSwapchainSlotDescriptor> slots;

    [[nodiscard]] bool valid() const noexcept {
        if (width == 0U || height == 0U || slots.empty()) {
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

struct WorkspacePresentSnapshot {
    bool valid = false;
    std::uint32_t front_slot_index = 0;
    std::uint64_t revision = 0;
    std::uint64_t swapchain_generation = 0;
    LiveFrameId frame_id{};
    WorkspaceDimensions dims{};
    WorkspaceDmaBufImage dmabuf_image{};
    LiveCaptureRegion source_region{};
    LiveCaptureRegion dirty_rect{};
    std::uintptr_t ready_event_handle = 0;
    std::uintptr_t producer_stream_handle = 0;
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

}  // namespace mmltk::live
