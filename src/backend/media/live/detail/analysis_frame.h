#pragma once
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/media/live/live_frame_id.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
namespace mmltk::backend::media::live {
namespace runtime = mmltk::backend::ml::runtime;
class LiveAnalysisFrame final {
   public:
    inline LiveAnalysisFrame(std::uint32_t slot, LiveFrameId frame, std::span<runtime::AnalysisAnnotationStorage> annotations,
                             runtime::AnalysisResult result) noexcept
        : slot_(slot), frame_(frame), annotations_(annotations), result_(std::move(result)) {}
    LiveAnalysisFrame(const LiveAnalysisFrame&) = delete;
    LiveAnalysisFrame& operator=(const LiveAnalysisFrame&) = delete;
    inline LiveAnalysisFrame(LiveAnalysisFrame&&) noexcept = default;
    LiveAnalysisFrame& operator=(LiveAnalysisFrame&&) = delete;
    [[nodiscard]] inline std::uint32_t slot() const noexcept { return slot_; }
    [[nodiscard]] inline LiveFrameId frame() const noexcept { return frame_; }
    [[nodiscard]] inline std::span<const runtime::AnalysisAnnotationStorage> annotations() const noexcept { return annotations_; }
    [[nodiscard]] inline const runtime::AnalysisResult& result() const noexcept { return result_; }
    [[nodiscard]] inline runtime::AnalysisResult release_result() && noexcept { return std::move(result_); }

   private:
    std::uint32_t slot_ = 0U;
    LiveFrameId frame_{};
    std::span<runtime::AnalysisAnnotationStorage> annotations_{};
    runtime::AnalysisResult result_{};
};
struct LiveAnalysisOverlayProjection final {
    LiveFrameId frame{};
    std::uint32_t slot = 0U;
    std::span<const runtime::AnalysisAnnotationStorage> annotations{};
    runtime::AnalysisCompletion completion{};
    [[nodiscard]] inline bool valid() const noexcept { return frame.valid() && !annotations.empty() && completion.valid(); }
};
}  // namespace mmltk::backend::media::live
