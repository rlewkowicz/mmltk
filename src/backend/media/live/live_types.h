#pragma once
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include "src/backend/media/live/live_frame_id.h"
namespace mmltk::backend::media::live {
inline constexpr std::size_t kManualOverlayBrushSegments = 64U;
inline constexpr std::size_t kManualOverlayBrushValueCount = kManualOverlayBrushSegments * 2U;
struct LiveManualOverlayUploadLimits final {
 std::size_t mask_bytes = 2'073'600U;
 std::size_t run_values = 1'048'576U;
 std::size_t point_values = 131'072U;
 std::size_t edge_values = 131'072U;
 std::size_t brush_values = kManualOverlayBrushValueCount;
 [[nodiscard]] bool valid() const noexcept;
};
struct PhysicalFrameRevision final {
 std::uint64_t revision = 0U;
 LiveFrameId frame{};
 std::uint32_t slot = 0U;
 std::uintptr_t ready_event = 0U;
 [[nodiscard]] bool valid() const noexcept { return revision != 0U && frame.valid() && ready_event != 0U; }
};
struct LiveOutputFrame final {
 std::uintptr_t pixels = 0U;
 std::size_t pitch_bytes = 0U;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::uintptr_t ready_event = 0U;
 [[nodiscard]] bool valid() const noexcept { return pixels != 0U && pitch_bytes != 0U && width != 0U && height != 0U && ready_event != 0U; }
};
struct LiveRawFrameReadback final {
 LiveFrameId frame{};
 std::span<std::uint8_t> destination{};
 void* context = nullptr;
 void (*complete)(void*, LiveFrameId, std::size_t, bool) noexcept = nullptr;
 [[nodiscard]] bool valid() const noexcept { return frame.valid() && !destination.empty() && context != nullptr && complete != nullptr; }
};
class LiveCompositeOutputLease final {
public:
 using CompleteCallback = void (*)(void*, PhysicalFrameRevision) noexcept;
 using AbandonCallback = void (*)(void*, PhysicalFrameRevision) noexcept;
 LiveCompositeOutputLease() noexcept = default;
 ~LiveCompositeOutputLease() noexcept;
 LiveCompositeOutputLease(const LiveCompositeOutputLease&) = delete;
 LiveCompositeOutputLease& operator=(const LiveCompositeOutputLease&) = delete;
 LiveCompositeOutputLease(LiveCompositeOutputLease&& other) noexcept;
 LiveCompositeOutputLease& operator=(LiveCompositeOutputLease&& other) noexcept;
 [[nodiscard]] static LiveCompositeOutputLease Create(void* owner, CompleteCallback complete, AbandonCallback abandon, LiveOutputFrame view, PhysicalFrameRevision frame_revision) noexcept;
 [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr && complete_ != nullptr && abandon_ != nullptr && view_.valid() && frame_revision_.valid(); }
 [[nodiscard]] const LiveOutputFrame& view() const noexcept { return view_; }
 [[nodiscard]] PhysicalFrameRevision frame_revision() const noexcept { return frame_revision_; }
 void Complete() && noexcept;

private:
 LiveCompositeOutputLease(void* owner, CompleteCallback complete, AbandonCallback abandon, LiveOutputFrame view, PhysicalFrameRevision frame_revision) noexcept;
 void Abandon() noexcept;
 void Clear() noexcept;
 void* owner_ = nullptr;
 CompleteCallback complete_ = nullptr;
 AbandonCallback abandon_ = nullptr;
 LiveOutputFrame view_{};
 PhysicalFrameRevision frame_revision_{};
};
using LiveOutputAcquire = bool (*)(void*, PhysicalFrameRevision, LiveCompositeOutputLease*);
[[nodiscard]] bool try_acquire_live_output(void* source, PhysicalFrameRevision, LiveOutputAcquire, LiveCompositeOutputLease*);
class LiveRevisionWait final {
public:
 using Snapshot = std::optional<PhysicalFrameRevision> (*)(void*) noexcept;
 void Reset() noexcept;
 void RevisionReady() noexcept;
 void Fail() noexcept;
 [[nodiscard]] bool failed() const noexcept;
 [[nodiscard]] std::optional<PhysicalFrameRevision> Wait(std::stop_token, void* source, Snapshot);

private:
 bool failed_ = false;
 std::uint64_t notification_generation_ = 0U;
 std::uint64_t last_revision_ = 0U;
 mutable std::mutex mutex_;
 std::condition_variable_any ready_;
};
}  // namespace mmltk::backend::media::live
