#include "still_image_preview.h"

#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk/rfdetr/predict.h"
#include "rfdetr/torch_cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <torch/torch.h>

#include "stb_image.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmltk::gui {

namespace {

std::string cuda_error_message(cudaError_t status, const char* label) {
    return std::string(label) + " failed: " + cudaGetErrorString(status);
}

void require_cuda_ok(cudaError_t status, const char* label) {
    if (status != cudaSuccess) {
        throw std::runtime_error(cuda_error_message(status, label));
    }
}

void rgb_to_bgr_in_place(std::vector<std::uint8_t>& pixels) {
    for (std::size_t offset = 0; offset + 2U < pixels.size(); offset += 3U) {
        std::swap(pixels[offset], pixels[offset + 2U]);
    }
}

void decode_encoded_mask_to_linear(const mmltk::rfdetr::EncodedMask& mask,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint8_t* destination) {
    if (destination == nullptr) {
        throw std::runtime_error("decode_encoded_mask_to_linear requires a destination buffer");
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::fill(destination, destination + pixel_count, std::uint8_t{0});
    if (mask.width != width || mask.height != height) {
        return;
    }

    for (const auto& run : mask.runs) {
        const std::size_t start = std::min<std::size_t>(run.first, pixel_count);
        const std::size_t end = std::min<std::size_t>(start + run.second, pixel_count);
        std::fill(destination + start, destination + end, std::uint8_t{1});
    }
}

} // namespace

StillImagePreview render_single_image_prediction_preview(
    const mmltk::rfdetr::PredictImageInput& input,
    const mmltk::rfdetr::PredictionRecord& record,
    int num_classes,
    int device_id) {
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load preview image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("invalid preview image dimensions: " + input.image_path.string());
    }

    StillImagePreview preview;
    preview.source_name = record.source_name.empty() ? input.image_path.string() : record.source_name;
    preview.width = static_cast<std::uint32_t>(raw_width);
    preview.height = static_cast<std::uint32_t>(raw_height);
    const std::size_t image_bytes =
        static_cast<std::size_t>(raw_width) * static_cast<std::size_t>(raw_height) * 3U;
    preview.pixels_bgr.assign(raw_pixels, raw_pixels + image_bytes);
    stbi_image_free(raw_pixels);
    rgb_to_bgr_in_place(preview.pixels_bgr);

    std::vector<float> boxes_host;
    std::vector<int> labels_host;
    std::vector<std::uint8_t> masks_host;
    std::vector<const mmltk::rfdetr::Prediction*> kept_detections;
    boxes_host.reserve(record.detections.size() * 4U);
    labels_host.reserve(record.detections.size());
    kept_detections.reserve(record.detections.size());
    bool has_any_masks = false;
    for (const auto& detection : record.detections) {
        if (detection.category_id <= 0) {
            continue;
        }

        const float x1 = std::clamp(detection.bbox_xyxy[0], 0.0f, static_cast<float>(raw_width));
        const float y1 = std::clamp(detection.bbox_xyxy[1], 0.0f, static_cast<float>(raw_height));
        const float x2 = std::clamp(detection.bbox_xyxy[2], 0.0f, static_cast<float>(raw_width));
        const float y2 = std::clamp(detection.bbox_xyxy[3], 0.0f, static_cast<float>(raw_height));
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        boxes_host.push_back(x1);
        boxes_host.push_back(y1);
        boxes_host.push_back(x2);
        boxes_host.push_back(y2);
        labels_host.push_back(detection.category_id - 1);
        kept_detections.push_back(&detection);
        if (detection.has_mask) {
            has_any_masks = true;
        }
    }

    if (labels_host.empty()) {
        return preview;
    }

    const std::size_t pixels_per_mask =
        static_cast<std::size_t>(raw_width) * static_cast<std::size_t>(raw_height);
    if (has_any_masks) {
        masks_host.assign(labels_host.size() * pixels_per_mask, 0U);
        for (std::size_t kept_index = 0; kept_index < kept_detections.size(); ++kept_index) {
            const auto& detection = *kept_detections[kept_index];
            if (detection.has_mask) {
                decode_encoded_mask_to_linear(
                    detection.mask,
                    static_cast<std::uint32_t>(raw_width),
                    static_cast<std::uint32_t>(raw_height),
                    masks_host.data() + kept_index * pixels_per_mask);
            }
        }
    }

    std::vector<std::uint8_t> colors_host;
    mmltk::rfdetr::build_instance_colors_from_zero_based_labels(
        labels_host.data(),
        labels_host.size(),
        num_classes,
        &colors_host);

    const torch::Device device(torch::kCUDA, static_cast<torch::DeviceIndex>(device_id));
    c10::cuda::CUDAGuard guard(device);
    const c10::cuda::CUDAStream stream = mmltk::rfdetr::get_high_priority_cuda_stream(device_id);

    torch::Tensor image_device;
    torch::Tensor boxes_device;
    torch::Tensor colors_device;
    torch::Tensor labels_device;
    bool* masks_device = nullptr;
    {
        c10::cuda::CUDAStreamGuard stream_guard(stream);
        image_device = torch::empty(
            {raw_height, raw_width, 3},
            torch::TensorOptions().dtype(torch::kUInt8).device(device));
        require_cuda_ok(cudaMemcpyAsync(image_device.data_ptr<std::uint8_t>(),
                                        preview.pixels_bgr.data(),
                                        image_bytes,
                                        cudaMemcpyHostToDevice,
                                        stream.stream()),
                        "cudaMemcpyAsync preview image host->device");

        boxes_device = torch::tensor(
            boxes_host,
            torch::TensorOptions().dtype(torch::kFloat32).device(device));
        colors_device = torch::tensor(
            colors_host,
            torch::TensorOptions().dtype(torch::kUInt8).device(device));
        labels_device = torch::tensor(
            labels_host,
            torch::TensorOptions().dtype(torch::kInt32).device(device));
        if (!masks_host.empty()) {
            require_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&masks_device),
                                       masks_host.size() * sizeof(bool)),
                            "cudaMalloc preview masks_device");
            require_cuda_ok(cudaMemcpyAsync(masks_device,
                                            masks_host.data(),
                                            masks_host.size() * sizeof(bool),
                                            cudaMemcpyHostToDevice,
                                            stream.stream()),
                            "cudaMemcpyAsync preview masks host->device");
            mmltk::rfdetr::launch_draw_masks_boxes_labels_bgr_pitched(
                image_device.data_ptr<std::uint8_t>(),
                static_cast<std::size_t>(raw_width) * 3U,
                raw_width,
                raw_height,
                masks_device,
                boxes_device.data_ptr<float>(),
                colors_device.data_ptr<std::uint8_t>(),
                labels_device.data_ptr<int>(),
                static_cast<int>(labels_host.size()),
                0.45f,
                2,
                stream.stream());
            require_cuda_ok(cudaPeekAtLastError(), "launch_draw_masks_boxes_labels_bgr_pitched");
        } else {
            mmltk::rfdetr::launch_draw_boxes_labels_bgr_pitched(
                image_device.data_ptr<std::uint8_t>(),
                static_cast<std::size_t>(raw_width) * 3U,
                raw_width,
                raw_height,
                boxes_device.data_ptr<float>(),
                colors_device.data_ptr<std::uint8_t>(),
                labels_device.data_ptr<int>(),
                static_cast<int>(labels_host.size()),
                2,
                stream.stream());
            require_cuda_ok(cudaPeekAtLastError(), "launch_draw_boxes_labels_bgr_pitched");
        }

        require_cuda_ok(cudaMemcpyAsync(preview.pixels_bgr.data(),
                                        image_device.data_ptr<std::uint8_t>(),
                                        image_bytes,
                                        cudaMemcpyDeviceToHost,
                                        stream.stream()),
                        "cudaMemcpyAsync preview image device->host");
    }
    require_cuda_ok(cudaStreamSynchronize(stream.stream()), "cudaStreamSynchronize preview draw");
    if (masks_device != nullptr) {
        require_cuda_ok(cudaFree(masks_device), "cudaFree preview masks_device");
    }
    return preview;
}

} // namespace mmltk::gui
