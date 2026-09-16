#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "src/backend/imaging/annotation/manual_mask_mapping.h"
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace mmltk::backend::media::live {
enum class SemanticRenderer : std::uint8_t {
    Iced = 0U,
    NativeImageView = 1U,
    NativeTileAtlas = 2U,
};
[[nodiscard]] constexpr bool semantic_renderer_valid(const SemanticRenderer renderer) noexcept {
    return renderer == SemanticRenderer::Iced || renderer == SemanticRenderer::NativeImageView || renderer == SemanticRenderer::NativeTileAtlas;
}
struct LiveInteractionAck final {
    std::uint64_t interaction = 0U;
    std::uint64_t sequence = 0U;
    [[nodiscard]] bool valid() const noexcept { return interaction != 0U && sequence != 0U; }
    bool operator==(const LiveInteractionAck&) const noexcept = default;
};
struct ManualOverlayBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    bool operator==(const ManualOverlayBox&) const noexcept = default;
};
struct ManualOverlayMaskRegion {
    std::uint32_t capture_x = 0;
    std::uint32_t capture_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const ManualOverlayMaskRegion&) const noexcept = default;
};
struct ManualOverlayMaskRun {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    bool operator==(const ManualOverlayMaskRun&) const noexcept = default;
};
using ManualOverlayDeferredMaskMapping = mmltk::backend::imaging::annotation::ManualMaskMapping;
struct ManualOverlayDeferredMaskProjection {
    ManualOverlayDeferredMaskMapping mapping{};
    ManualOverlayMaskRegion region{};
    std::uint32_t run_count = 0;
    std::size_t run_value_count = 0;
};
[[nodiscard]] bool manual_overlay_mask_region_contained(const ManualOverlayMaskRegion& region, std::uint32_t target_width,
                                                        std::uint32_t target_height) noexcept;
[[nodiscard]] std::optional<ManualOverlayDeferredMaskProjection> validate_manual_overlay_deferred_mask(const ManualOverlayDeferredMaskMapping& mapping,
                                                                                                       const ManualOverlayMaskRegion& region,
                                                                                                       std::span<const ManualOverlayMaskRun> runs,
                                                                                                       std::uint32_t target_width,
                                                                                                       std::uint32_t target_height) noexcept;
struct ManualOverlayPoint {
    int x = 0;
    int y = 0;
    bool operator==(const ManualOverlayPoint&) const noexcept = default;
};
struct ManualOverlayEdge {
    std::uint32_t source_index = 0;
    std::uint32_t target_index = 0;
    bool operator==(const ManualOverlayEdge&) const noexcept = default;
};
struct ManualOverlayBrushPreview {
    int capture_x = 0;
    int capture_y = 0;
    int radius = 1;
    bool erase = false;
    bool operator==(const ManualOverlayBrushPreview&) const noexcept = default;
};
struct ManualOverlayStyle {
    std::uint8_t r = 255U;
    std::uint8_t g = 255U;
    std::uint8_t b = 255U;
    std::uint8_t alpha = 255U;
    int line_thickness = 2;
    int point_radius = 3;
    bool draw_handles = false;
    int handle_radius = 4;
    bool operator==(const ManualOverlayStyle&) const noexcept = default;
};
struct ManualOverlayInstance {
    std::string instance_id;
    bool enabled = true;
    ManualOverlayBox box{};
    ManualOverlayMaskRegion mask_region{};
    std::vector<std::uint8_t> mask;
    std::vector<ManualOverlayMaskRun> mask_runs;
    std::optional<ManualOverlayDeferredMaskMapping> deferred_mask_mapping;
    std::vector<ManualOverlayPoint> polyline_points;
    bool polyline_closed = false;
    std::vector<ManualOverlayPoint> points;
    std::vector<ManualOverlayEdge> skeleton_edges;
    std::size_t category_index = 0;
    std::optional<ManualOverlayStyle> style;
    bool operator==(const ManualOverlayInstance&) const = default;
};
struct ManualOverlayDocumentSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    LiveInteractionAck interaction_ack{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::vector<ManualOverlayInstance> instances;
    std::vector<ManualOverlayInstance> interaction_instances;
    std::optional<std::size_t> selected_instance;
    std::optional<ManualOverlayBrushPreview> brush_preview;
    SemanticRenderer renderer_mode = SemanticRenderer::NativeImageView;
    [[nodiscard]] bool same_content(const ManualOverlayDocumentSnapshot& other) const {
        return document_generation == other.document_generation && session_revision == other.session_revision && interaction_ack == other.interaction_ack &&
               capture_width == other.capture_width && capture_height == other.capture_height && instances == other.instances &&
               interaction_instances == other.interaction_instances && selected_instance == other.selected_instance && brush_preview == other.brush_preview &&
               renderer_mode == other.renderer_mode;
    }
};
class ManualOverlayDocument {
   public:
    ManualOverlayDocument();
    void publish_snapshot(ManualOverlayDocumentSnapshot snapshot);
    void publish_snapshot(std::shared_ptr<ManualOverlayDocumentSnapshot> snapshot);
    void clear(std::uint32_t capture_width, std::uint32_t capture_height);
    [[nodiscard]] std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot() const;
    [[nodiscard]] std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot_if_changed(std::uint64_t last_seen_generation) const;

   private:
    std::atomic<std::shared_ptr<const ManualOverlayDocumentSnapshot>> snapshot_;
};
}  // namespace mmltk::backend::media::live
