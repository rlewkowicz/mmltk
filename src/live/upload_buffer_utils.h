#pragma once

#include "live/live_helpers.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmltk::live {

template <typename T>
inline void ensure_pinned_upload_capacity(const std::size_t value_count,
                                          PinnedUploadBuffer<T>* buffer,
                                          const char* context) {
    if (value_count == 0U || buffer == nullptr) {
        return;
    }
    buffer->ensure_capacity(value_count, context);
}

template <typename T>
inline void ensure_device_upload_capacity(const std::size_t value_count,
                                          DeviceUploadBuffer<T>* buffer,
                                          const char* context) {
    if (value_count == 0U || buffer == nullptr) {
        return;
    }
    buffer->ensure_capacity(value_count, context);
}

template <typename Point>
inline std::size_t pack_points_xy(const std::vector<Point>& points, int* values_out) {
    if (values_out == nullptr) {
        return 0U;
    }
    std::size_t write_index = 0U;
    for (const Point& point : points) {
        values_out[write_index++] = point.x;
        values_out[write_index++] = point.y;
    }
    return write_index;
}

template <typename Edge>
inline std::size_t pack_edge_indices(const std::vector<Edge>& edges, std::uint32_t* values_out) {
    if (values_out == nullptr) {
        return 0U;
    }
    std::size_t write_index = 0U;
    for (const Edge& edge : edges) {
        values_out[write_index++] = edge.source_index;
        values_out[write_index++] = edge.target_index;
    }
    return write_index;
}

} // namespace mmltk::live
