#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cstdint>
#include <algorithm>

namespace fastloader::rfdetr {

__constant__ unsigned char d_font5x7[10][7] = {
    {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}, // 0
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70}, // 1
    {0x70, 0x88, 0x08, 0x30, 0x40, 0x80, 0xF8}, // 2
    {0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70}, // 3
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10}, // 4
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70}, // 5
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}, // 6
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40}, // 7
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70}, // 8
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}  // 9
};

__device__ void hsv2rgb_device(float h,
                               float s,
                               float v,
                               uint8_t& r,
                               uint8_t& g,
                               uint8_t& b) {
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rf, gf, bf;
    if (h >= 0.0f && h < 60.0f) {
        rf = c;
        gf = x;
        bf = 0.0f;
    } else if (h < 120.0f) {
        rf = x;
        gf = c;
        bf = 0.0f;
    } else if (h < 180.0f) {
        rf = 0.0f;
        gf = c;
        bf = x;
    } else if (h < 240.0f) {
        rf = 0.0f;
        gf = x;
        bf = c;
    } else if (h < 300.0f) {
        rf = x;
        gf = 0.0f;
        bf = c;
    } else {
        rf = c;
        gf = 0.0f;
        bf = x;
    }
    r = static_cast<uint8_t>((rf + m) * 255.0f);
    g = static_cast<uint8_t>((gf + m) * 255.0f);
    b = static_cast<uint8_t>((bf + m) * 255.0f);
}

__global__ void build_instance_colors_from_labels_kernel(
    const int* labels,
    int count,
    int num_classes,
    uint8_t* colors_rgb
) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= count) {
        return;
    }

    const int safe_class_count = max(1, num_classes);
    int label = labels[index];
    if (label < 0 || label >= safe_class_count) {
        label = 0;
    }

    int rank = 0;
    for (int previous = 0; previous < index; ++previous) {
        int previous_label = labels[previous];
        if (previous_label < 0 || previous_label >= safe_class_count) {
            previous_label = 0;
        }
        if (previous_label == label) {
            ++rank;
        }
    }

    const float hue_step = 360.0f / static_cast<float>(safe_class_count);
    const float base_h = static_cast<float>(label) * hue_step;

    float h = base_h;
    float s = 1.0f;
    const float v = 1.0f;
    if (rank > 0) {
        const int ring = (rank - 1) / 10 + 1;
        const int sv_idx = ((rank - 1) % 10) / 2;
        const int sign = ((rank - 1) % 2 == 0) ? 1 : -1;
        s = fmaxf(0.0f, 1.0f - (static_cast<float>(sv_idx) * 0.05f));
        h = fmodf(base_h + (static_cast<float>(sign) * static_cast<float>(ring) * 3.6f) + 360.0f, 360.0f);
    }

    const int color_offset = index * 3;
    hsv2rgb_device(h, s, v, colors_rgb[color_offset], colors_rgb[color_offset + 1], colors_rgb[color_offset + 2]);
}

__device__ bool pixel_hits_box_edge(int x,
                                    int y,
                                    int x1,
                                    int y1,
                                    int x2,
                                    int y2,
                                    int box_thickness) {
    if (x < x1 - box_thickness || x > x2 + box_thickness ||
        y < y1 - box_thickness || y > y2 + box_thickness) {
        return false;
    }
    return x < x1 || x > x2 || y < y1 || y > y2;
}

__device__ bool pixel_hits_label_digit(int x,
                                       int y,
                                       int x1,
                                       int y1,
                                       int label) {
    if (label < 0) {
        return false;
    }

    const int tx = x - x1;
    const int ty = y - (y1 - 16);
    if (ty < 0 || ty >= 14 || tx < 0 || tx >= 24) {
        return false;
    }

    int digits[2];
    int num_digits = 0;
    if (label == 0) {
        digits[0] = 0;
        num_digits = 1;
    } else {
        int temp = label;
        while (temp > 0 && num_digits < 2) {
            digits[num_digits++] = temp % 10;
            temp /= 10;
        }
    }

    for (int d = 0; d < num_digits; ++d) {
        const int char_idx = num_digits - 1 - d;
        const int digit = digits[d];
        const int char_start_x = char_idx * 12;
        if (tx < char_start_x || tx >= char_start_x + 10) {
            continue;
        }

        const int font_x = (tx - char_start_x) / 2;
        const int font_y = ty / 2;
        if (font_x < 5 && font_y < 7 && (d_font5x7[digit][font_y] & (0x80 >> font_x))) {
            return true;
        }
    }
    return false;
}

// Colors blending kernel
__global__ void draw_masks_and_boxes_kernel(
    uint8_t* image_out,
    int width, int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    float mask_alpha,
    int box_thickness
) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= width || y >= height) return;
    
    int pixel_idx = (y * width + x) * 3;
    float r = image_out[pixel_idx];
    float g = image_out[pixel_idx + 1];
    float b = image_out[pixel_idx + 2];
    
    // Draw masks
    for (int i = 0; i < num_instances; ++i) {
        if (masks[i * width * height + y * width + x]) {
            r = r * (1.0f - mask_alpha) + colors[i * 3] * mask_alpha;
            g = g * (1.0f - mask_alpha) + colors[i * 3 + 1] * mask_alpha;
            b = b * (1.0f - mask_alpha) + colors[i * 3 + 2] * mask_alpha;
        }
    }
    
    // Draw edges for bounding boxes
    for (int i = 0; i < num_instances; ++i) {
        int x1 = static_cast<int>(boxes[i * 4 + 0]);
        int y1 = static_cast<int>(boxes[i * 4 + 1]);
        int x2 = static_cast<int>(boxes[i * 4 + 2]);
        int y2 = static_cast<int>(boxes[i * 4 + 3]);
        
        const bool is_edge = pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness);
        
        if (is_edge) {
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
        }
        
        if (pixel_hits_label_digit(x, y, x1, y1, labels[i])) {
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
        }
    }
    
    // clamp and write back
    image_out[pixel_idx] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
    image_out[pixel_idx + 1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    image_out[pixel_idx + 2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
}

__global__ void draw_boxes_labels_bgr_pitched_kernel(
    uint8_t* image_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    int box_thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    uint8_t* pixel = image_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 3U;
    float b = pixel[0];
    float g = pixel[1];
    float r = pixel[2];

    for (int i = 0; i < num_instances; ++i) {
        const int x1 = static_cast<int>(boxes[i * 4 + 0]);
        const int y1 = static_cast<int>(boxes[i * 4 + 1]);
        const int x2 = static_cast<int>(boxes[i * 4 + 2]);
        const int y2 = static_cast<int>(boxes[i * 4 + 3]);
        const bool is_edge = pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness);
        const bool is_label = pixel_hits_label_digit(x, y, x1, y1, labels[i]);
        if (!is_edge && !is_label) {
            continue;
        }

        r = colors[i * 3];
        g = colors[i * 3 + 1];
        b = colors[i * 3 + 2];
    }

    pixel[0] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
    pixel[1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    pixel[2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
}

__global__ void draw_masks_boxes_labels_bgr_pitched_kernel(
    uint8_t* image_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    float mask_alpha,
    int box_thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    uint8_t* pixel = image_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 3U;
    float b = pixel[0];
    float g = pixel[1];
    float r = pixel[2];

    for (int i = 0; i < num_instances; ++i) {
        if (masks[i * width * height + y * width + x]) {
            r = r * (1.0f - mask_alpha) + colors[i * 3] * mask_alpha;
            g = g * (1.0f - mask_alpha) + colors[i * 3 + 1] * mask_alpha;
            b = b * (1.0f - mask_alpha) + colors[i * 3 + 2] * mask_alpha;
        }
    }

    for (int i = 0; i < num_instances; ++i) {
        const int x1 = static_cast<int>(boxes[i * 4 + 0]);
        const int y1 = static_cast<int>(boxes[i * 4 + 1]);
        const int x2 = static_cast<int>(boxes[i * 4 + 2]);
        const int y2 = static_cast<int>(boxes[i * 4 + 3]);
        const bool is_edge = pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness);
        const bool is_label = pixel_hits_label_digit(x, y, x1, y1, labels[i]);
        if (!is_edge && !is_label) {
            continue;
        }

        r = colors[i * 3];
        g = colors[i * 3 + 1];
        b = colors[i * 3 + 2];
    }

    pixel[0] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
    pixel[1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    pixel[2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
}

void launch_draw_masks_boxes(
    uint8_t* image_out,
    int width, int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    float mask_alpha,
    int box_thickness,
    cudaStream_t stream
) {
    if (width <= 0 || height <= 0) return;
    
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    
    draw_masks_and_boxes_kernel<<<grid, block, 0, stream>>>(
        image_out, width, height, masks, boxes, colors, labels, 
        num_instances, mask_alpha, box_thickness
    );
}

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
) {
    if (image_out == nullptr || boxes == nullptr || colors == nullptr || labels == nullptr ||
        width <= 0 || height <= 0 || num_instances <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    draw_boxes_labels_bgr_pitched_kernel<<<grid, block, 0, stream>>>(
        image_out,
        pitch_bytes,
        width,
        height,
        boxes,
        colors,
        labels,
        num_instances,
        std::max(1, box_thickness));
}

void launch_build_instance_colors_from_zero_based_labels(
    const int* labels,
    std::size_t count,
    int num_classes,
    std::uint8_t* colors_rgb,
    cudaStream_t stream
) {
    if (labels == nullptr || colors_rgb == nullptr || count == 0U) {
        return;
    }

    const int safe_count = static_cast<int>(count);
    const int threads = 128;
    const int blocks = (safe_count + threads - 1) / threads;
    build_instance_colors_from_labels_kernel<<<blocks, threads, 0, stream>>>(
        labels,
        safe_count,
        num_classes,
        colors_rgb);
}

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
) {
    if (image_out == nullptr || masks == nullptr || boxes == nullptr || colors == nullptr || labels == nullptr ||
        width <= 0 || height <= 0 || num_instances <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    draw_masks_boxes_labels_bgr_pitched_kernel<<<grid, block, 0, stream>>>(
        image_out,
        pitch_bytes,
        width,
        height,
        masks,
        boxes,
        colors,
        labels,
        num_instances,
        mask_alpha,
        std::max(1, box_thickness));
}

} // namespace fastloader::rfdetr
