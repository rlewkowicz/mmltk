#pragma once

#include <torch/torch.h>
#include <string>
#include <filesystem>
#include <optional>

namespace fastloader::rfdetr {

struct RenderSampleOptions {
    std::filesystem::path output_path;
    int num_classes = 6;
    float box_thickness = 1.0f;
    int label_size = 12;
    float mask_alpha = 0.5f;
};

// Submits the image to be drawn on GPU, copies to CPU, and queues a background
// write so validation can keep running. Image is expected to be [3, H, W] in CHW
// format, masks in [N, H, W] (bool or float), boxes in [N, 4] format (XYXY),
// and labels in [N] with 0-based class indices.
void draw_eval_sample_async_gpu(
    const torch::Tensor& image,
    const torch::Tensor& boxes,
    const torch::Tensor& labels,
    const torch::Tensor& masks,
    const RenderSampleOptions& options
);

// Blocks until every queued eval-sample image has been written to disk.
void flush_eval_sample_writes();

} // namespace fastloader::rfdetr
