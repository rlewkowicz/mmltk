#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include <cstddef>
#include <cstdint>
#include <span>
namespace mmltk::backend::imaging::resample::detail {
struct TorchResizeStorage {
 std::size_t supplied_bytes = 0, opaque_bytes = 0;
};
void require_torch_resize();
[[nodiscard]] TorchResizeStorage torch_resize_storage(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, bool antialias);
// All tensor views and opaque temporaries retire before return. The supplied
// planes are tightly packed, unit-sRGB float; no RGB8 output is produced.
void torch_resize(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, bool antialias, std::span<std::byte>, RgbResizeWork*);
}  // namespace mmltk::backend::imaging::resample::detail
