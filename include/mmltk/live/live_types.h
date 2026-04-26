#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_frame_id.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <cuda.h>
#include <cuda_runtime_api.h>

namespace mmltk::live {

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
};

}  // namespace mmltk::live
