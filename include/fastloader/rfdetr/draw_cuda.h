#pragma once

#include <cuda_runtime_api.h>
#include <torch/torch.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <filesystem>
#include <optional>
#include <vector>

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

// Builds the same per-instance class-gradient colors used by the eval sample renderer.
// Labels are expected to be 0-based class indices.
void build_instance_colors_from_zero_based_labels(const int* labels,
                                                  std::size_t count,
                                                  int num_classes,
                                                  std::vector<std::uint8_t>* colors_rgb);

// Builds the same per-instance class-gradient colors directly on GPU.
// Labels are expected to be 0-based class indices on device memory.
void launch_build_instance_colors_from_zero_based_labels(
    const int* labels,
    std::size_t count,
    int num_classes,
    std::uint8_t* colors_rgb,
    cudaStream_t stream
);

// Draws detection boxes and numeric class labels into a pitched BGR image on GPU.
// The image buffer stays device-resident and no CPU round-trip is performed.
void launch_draw_boxes_labels_bgr_pitched(
    std::uint8_t* image_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const float* boxes,
    const std::uint8_t* colors,
    const int* labels,
    int num_instances,
    int box_thickness,
    cudaStream_t stream
);

// Draws masks, boxes, and numeric class labels into a pitched BGR image on GPU.
// Masks are expected to be a contiguous [num_instances,height,width] bool tensor.
void launch_draw_masks_boxes_labels_bgr_pitched(
    std::uint8_t* image_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const bool* masks,
    const float* boxes,
    const std::uint8_t* colors,
    const int* labels,
    int num_instances,
    float mask_alpha,
    int box_thickness,
    cudaStream_t stream
);

// Blocks until every queued eval-sample image has been written to disk.
void flush_eval_sample_writes();

} // namespace fastloader::rfdetr
