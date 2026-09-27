#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include "src/frameworks/gpu/image/image_buffer.h"
namespace mmltk::backend::imaging::raster {
// One reusable pinned image and one asynchronous file write. The borrowed
// device image is copied completely before Write returns. Flush publishes the
// completed path only after atomic rename; failed writes preserve prior files.
class RenderedImageWriter final {
public:
 using PngEncoder = std::function<int(const char*, int, int, int, const void*, int)>;
 explicit RenderedImageWriter(mmltk::frameworks::gpu::DeviceContext, PngEncoder = {});
 ~RenderedImageWriter();
 void Write(mmltk::frameworks::gpu::BorrowedImageProductReadView, const std::filesystem::path&);
 [[nodiscard]] std::filesystem::path Flush();

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::imaging::raster
