#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mmltk::backend::media::live {

inline constexpr std::array<std::array<std::uint8_t, 3>, 8> kManualOverlayCategoryPalette{{
    {{240, 196, 68}},
    {{88, 188, 255}},
    {{255, 128, 88}},
    {{96, 214, 146}},
    {{214, 112, 255}},
    {{255, 96, 152}},
    {{180, 214, 92}},
    {{255, 168, 64}},
}};

[[nodiscard]] constexpr std::array<std::uint8_t, 3> manual_overlay_category_color(const std::size_t index) noexcept {
    return kManualOverlayCategoryPalette[index % kManualOverlayCategoryPalette.size()];
}

}  // namespace mmltk::backend::media::live
