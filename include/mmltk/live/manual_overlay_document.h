#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::live {

struct ManualOverlayBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

struct ManualOverlayMaskRegion {
    std::uint32_t capture_x = 0;
    std::uint32_t capture_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ManualOverlayPoint {
    int x = 0;
    int y = 0;
};

struct ManualOverlayEdge {
    std::uint32_t source_index = 0;
    std::uint32_t target_index = 0;
};

struct ManualOverlayInstance {
    std::string instance_id;
    bool enabled = true;
    ManualOverlayBox box{};
    ManualOverlayMaskRegion mask_region{};
    std::vector<std::uint8_t> mask;
    std::vector<ManualOverlayPoint> polyline_points;
    bool polyline_closed = false;
    std::vector<ManualOverlayPoint> points;
    std::vector<ManualOverlayEdge> skeleton_edges;
    std::size_t category_index = 0;
};

struct ManualOverlayDocumentSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::vector<ManualOverlayInstance> instances;
    std::optional<std::size_t> selected_instance;
};

class ManualOverlayDocument {
public:
    ManualOverlayDocument();

    void publish_snapshot(ManualOverlayDocumentSnapshot snapshot);
    void clear(std::uint32_t capture_width, std::uint32_t capture_height);

    [[nodiscard]] std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot() const;
    [[nodiscard]] std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot_if_changed(
        std::uint64_t last_seen_generation) const;

private:
    std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot_;
};

} // namespace mmltk::live
