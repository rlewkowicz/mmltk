#pragma once

#include <cstdint>

namespace mmltk::live {

struct LiveCaptureRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline bool operator==(const LiveCaptureRegion& lhs, const LiveCaptureRegion& rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

inline bool operator!=(const LiveCaptureRegion& lhs, const LiveCaptureRegion& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace mmltk::live
