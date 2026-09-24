#pragma once
#include <ATen/core/ATen_fwd.h>
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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
void build_instance_colors_async(const std::int32_t* labels, std::size_t count, int num_classes, std::uint8_t* colors_rgb, cudaStream_t stream);
}  // namespace mmltk::backend::models::rfdetr
