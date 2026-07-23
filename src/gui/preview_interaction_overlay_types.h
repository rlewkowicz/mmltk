#pragma once

#include "annotation_core.h"

#include <cstdint>
#include <vector>

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

}  
