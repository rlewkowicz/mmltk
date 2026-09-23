#pragma once
#include <ATen/core/ATen_fwd.h>
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include "src/frameworks/gpu/image_buffer.h"
namespace mmltk::backend::models::rfdetr {
struct RenderSampleOptions final {
 std::filesystem::path output_path;
 int num_classes = 6;
 float box_thickness = 1.0F;
 int label_size = 12;
 float mask_alpha = 0.5F;
};
class EvaluationSampleWriter final {
public:
 EvaluationSampleWriter();
 ~EvaluationSampleWriter();
 EvaluationSampleWriter(const EvaluationSampleWriter&) = delete;
 EvaluationSampleWriter& operator=(const EvaluationSampleWriter&) = delete;
 void Draw(const at::Tensor& image, const at::Tensor& boxes, const at::Tensor& labels, const at::Tensor& masks, const RenderSampleOptions& options);
 void Flush();

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
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
void build_instance_colors_async(const std::int32_t* labels, std::size_t count, int num_classes, std::uint8_t* colors_rgb, cudaStream_t stream);
}  // namespace mmltk::backend::models::rfdetr
