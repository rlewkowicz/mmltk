#pragma once
#include "src/backend/media/live/live_frame_id.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <cuda_runtime_api.h>
namespace mmltk::backend::media::live {
// Completed host projection of one region. Spans are borrowed from the owning
// product, retain canonical joint values, and contain only the initialized prefix.
struct LiveAnalysisRegion final {
 mmltk::backend::ml::runtime::AnalysisRegion source_region{};
 mmltk::backend::data::catalog::ClassReferenceDomain class_domain{};
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog;
 std::size_t count = 0;
 std::uint32_t keypoint_capacity = 0;
 bool masks_available = false, keypoints_available = false;
 std::span<const std::array<float, 4>> boxes_xyxy;
 std::span<const std::int32_t> class_references;
 std::span<const float> confidences;
 std::span<const std::array<std::uint8_t, 3>> colors_rgb;
 std::span<const std::uint8_t> masks;
 std::span<const mmltk::backend::ml::runtime::AnalysisKeypoint> keypoints;
 std::span<const std::int32_t> keypoint_counts;
};
// A compositor slot owns pinned readback; the controller owns an independent
// reusable host copy. Owners settle queued reads before reset/reuse/destruction.
// Neither form retains provider storage or its count custody after completion.
class LiveAnalysisProduct final {
public:
 enum class Storage : std::uint8_t { Host, Pinned };
 using Allocate = std::shared_ptr<void> (*)(std::size_t, Storage);
 using DeviceCopy = cudaError_t (*)(void*, const void*, std::size_t, std::uintptr_t) noexcept;
 explicit LiveAnalysisProduct(Storage storage = Storage::Host, Allocate allocation = nullptr);
 ~LiveAnalysisProduct();
 LiveAnalysisProduct(const LiveAnalysisProduct&) = delete;
 LiveAnalysisProduct& operator=(const LiveAnalysisProduct&) = delete;
 void Reset(LiveFrameId = {}) noexcept;
 void PrepareDevice(LiveFrameId, std::uint32_t width, std::uint32_t height, std::span<const mmltk::backend::ml::runtime::AnalysisAnnotationStorage>);
 [[nodiscard]] cudaError_t EnqueueDeviceCopy(std::uintptr_t stream, DeviceCopy = nullptr) noexcept;
 // Called on the existing physical completion boundary, before analyzer release.
 void CompleteDeviceCopy();
 // Failure/Stop may discard a product after the same stream has settled.
 void SettleDeviceCopy() noexcept;
 void CopyFrom(const LiveAnalysisProduct&);
 [[nodiscard]] LiveFrameId frame() const noexcept;
 [[nodiscard]] bool ready() const noexcept;
 [[nodiscard]] bool analyzed() const noexcept;
 [[nodiscard]] std::span<const LiveAnalysisRegion> regions() const noexcept;
 [[nodiscard]] std::size_t capacity_bytes() const noexcept;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::media::live
