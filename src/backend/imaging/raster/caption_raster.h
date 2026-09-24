#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include "native_caption_metrics.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/image_buffer.h"
namespace mmltk::backend::imaging::raster {
struct NamedCaption final {
 std::uint32_t name = 0U;
 int x = 0, y = 0;
 std::array<std::uint8_t, 3> background{};
 // Numeric ASCII suffixes reuse the atlas; borrowed until Draw returns.
 std::string_view suffix{};
};
// One bundled face, fixed native size, and catalog per resource. Caller settles
// its stream before the next Draw, Prepare, or destruction. Catalog uploads
// settle once in Prepare; draws reuse bounded storage on the caller stream.
class CaptionRaster final {
public:
 explicit CaptionRaster(mmltk::frameworks::gpu::DeviceContext);
 ~CaptionRaster();
 CaptionRaster(const CaptionRaster&) = delete;
 CaptionRaster& operator=(const CaptionRaster&) = delete;
 void Prepare(std::span<const std::string> names);
 void Draw(mmltk::frameworks::gpu::ImagePlaneView, std::span<const NamedCaption>, std::uintptr_t stream) const;
 static void Composite(mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t stream);
private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}
