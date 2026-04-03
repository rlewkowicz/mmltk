#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cstdint>
#include <algorithm>

namespace mmltk::rfdetr {

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

__global__ void draw_analysis_overlay_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    uint8_t mask_alpha,
    int box_thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    uint8_t* pixel = overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    uint8_t a = 0U;

    if (masks != nullptr) {
        for (int i = 0; i < num_instances; ++i) {
            if (!masks[i * width * height + y * width + x]) {
                continue;
            }
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
            a = mask_alpha;
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
        a = 255U;
    }

    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = a;
}

__global__ void composite_rgba_over_bgr_pitched_kernel(
    uint8_t* base_bgr,
    std::size_t base_pitch_bytes,
    const uint8_t* overlay_rgba,
    std::size_t overlay_pitch_bytes,
    int width,
    int height
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const uint8_t* overlay_pixel =
        overlay_rgba + static_cast<std::size_t>(y) * overlay_pitch_bytes + static_cast<std::size_t>(x) * 4U;
    const uint8_t alpha_u8 = overlay_pixel[3];
    if (alpha_u8 == 0U) {
        return;
    }

    uint8_t* base_pixel =
        base_bgr + static_cast<std::size_t>(y) * base_pitch_bytes + static_cast<std::size_t>(x) * 3U;
    if (alpha_u8 == 255U) {
        base_pixel[0] = overlay_pixel[2];
        base_pixel[1] = overlay_pixel[1];
        base_pixel[2] = overlay_pixel[0];
        return;
    }

    const float alpha = static_cast<float>(alpha_u8) / 255.0f;
    const float inv_alpha = 1.0f - alpha;
    const float b = static_cast<float>(overlay_pixel[2]) * alpha + static_cast<float>(base_pixel[0]) * inv_alpha;
    const float g = static_cast<float>(overlay_pixel[1]) * alpha + static_cast<float>(base_pixel[1]) * inv_alpha;
    const float r = static_cast<float>(overlay_pixel[0]) * alpha + static_cast<float>(base_pixel[2]) * inv_alpha;
    base_pixel[0] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
    base_pixel[1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    base_pixel[2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
}

__global__ void draw_manual_mask_rgba_pitched_kernel(
    uint8_t* overlay_region,
    std::size_t pitch_bytes,
    int width,
    int height,
    const uint8_t* mask,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t alpha
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    if (mask[y * width + x] == 0U) {
        return;
    }

    uint8_t* pixel = overlay_region + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = alpha;
}

__global__ void draw_box_outline_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1,
    int box_y1,
    int box_x2,
    int box_y2,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    int thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height) {
        return;
    }
    if (!pixel_hits_box_edge(x, y, box_x1, box_y1, box_x2 - 1, box_y2 - 1, thickness)) {
        return;
    }

    uint8_t* pixel = overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 255U;
}

__global__ void draw_selection_handles_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1,
    int box_y1,
    int box_x2,
    int box_y2,
    int handle_radius
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height) {
        return;
    }

    const int corners_x[4] = {box_x1, box_x2 - 1, box_x1, box_x2 - 1};
    const int corners_y[4] = {box_y1, box_y1, box_y2 - 1, box_y2 - 1};
    for (int i = 0; i < 4; ++i) {
        if (x < corners_x[i] - handle_radius || x > corners_x[i] + handle_radius ||
            y < corners_y[i] - handle_radius || y > corners_y[i] + handle_radius) {
            continue;
        }
        uint8_t* pixel =
            overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
        pixel[0] = 255U;
        pixel[1] = 220U;
        pixel[2] = 96U;
        pixel[3] = 240U;
        return;
    }
}

__device__ float point_distance_sq(float px,
                                   float py,
                                   float qx,
                                   float qy) {
    const float dx = px - qx;
    const float dy = py - qy;
    return dx * dx + dy * dy;
}

__device__ float point_to_segment_distance_sq(float px,
                                              float py,
                                              float ax,
                                              float ay,
                                              float bx,
                                              float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = px - ax;
    const float apy = py - ay;
    const float ab_len_sq = abx * abx + aby * aby;
    if (ab_len_sq <= 0.0f) {
        return point_distance_sq(px, py, ax, ay);
    }
    const float t = fminf(1.0f, fmaxf(0.0f, (apx * abx + apy * aby) / ab_len_sq));
    return point_distance_sq(px, py, ax + abx * t, ay + aby * t);
}

__global__ void draw_polyline_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    int closed,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    int thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height || points_xy == nullptr || point_count < 2) {
        return;
    }

    const int segment_count = closed != 0 ? point_count : point_count - 1;
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = fmaxf(1.0f, static_cast<float>(thickness * thickness));
    for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
        const int start_index = segment_index * 2;
        const int end_point_index = ((segment_index + 1) % point_count) * 2;
        const float ax = static_cast<float>(points_xy[start_index + 0]);
        const float ay = static_cast<float>(points_xy[start_index + 1]);
        const float bx = static_cast<float>(points_xy[end_point_index + 0]);
        const float by = static_cast<float>(points_xy[end_point_index + 1]);
        if (point_to_segment_distance_sq(px, py, ax, ay, bx, by) > max_distance_sq) {
            continue;
        }
        uint8_t* pixel =
            overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = 255U;
        return;
    }
}

__global__ void draw_points_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    int radius,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t alpha
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height || points_xy == nullptr || point_count <= 0) {
        return;
    }

    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = static_cast<float>(radius * radius);
    for (int point_index = 0; point_index < point_count; ++point_index) {
        const int xy_index = point_index * 2;
        const float qx = static_cast<float>(points_xy[xy_index + 0]);
        const float qy = static_cast<float>(points_xy[xy_index + 1]);
        if (point_distance_sq(px, py, qx, qy) > max_distance_sq) {
            continue;
        }
        uint8_t* pixel =
            overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = alpha;
        return;
    }
}

__global__ void draw_skeleton_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    const std::uint32_t* edge_indices,
    int edge_count,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    int thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height || points_xy == nullptr || edge_indices == nullptr ||
        point_count <= 0 || edge_count <= 0) {
        return;
    }

    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float max_distance_sq = fmaxf(1.0f, static_cast<float>(thickness * thickness));
    for (int edge_index = 0; edge_index < edge_count; ++edge_index) {
        const int pair_index = edge_index * 2;
        const std::uint32_t source_index = edge_indices[pair_index + 0];
        const std::uint32_t target_index = edge_indices[pair_index + 1];
        if (source_index >= static_cast<std::uint32_t>(point_count) ||
            target_index >= static_cast<std::uint32_t>(point_count)) {
            continue;
        }
        const int source_xy_index = static_cast<int>(source_index) * 2;
        const int target_xy_index = static_cast<int>(target_index) * 2;
        const float ax = static_cast<float>(points_xy[source_xy_index + 0]);
        const float ay = static_cast<float>(points_xy[source_xy_index + 1]);
        const float bx = static_cast<float>(points_xy[target_xy_index + 0]);
        const float by = static_cast<float>(points_xy[target_xy_index + 1]);
        if (point_to_segment_distance_sq(px, py, ax, ay, bx, by) > max_distance_sq) {
            continue;
        }
        uint8_t* pixel =
            overlay_out + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 4U;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = 255U;
        return;
    }
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

void launch_draw_analysis_overlay_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int width,
    int height,
    const bool* masks,
    const float* boxes,
    const std::uint8_t* colors,
    const int* labels,
    int num_instances,
    const std::uint8_t mask_alpha,
    int box_thickness,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || boxes == nullptr || colors == nullptr || labels == nullptr ||
        width <= 0 || height <= 0 || num_instances <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    draw_analysis_overlay_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
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

void launch_composite_rgba_over_bgr_pitched(
    std::uint8_t* base_bgr,
    std::size_t base_pitch_bytes,
    const std::uint8_t* overlay_rgba,
    std::size_t overlay_pitch_bytes,
    int width,
    int height,
    cudaStream_t stream
) {
    if (base_bgr == nullptr || overlay_rgba == nullptr || width <= 0 || height <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    composite_rgba_over_bgr_pitched_kernel<<<grid, block, 0, stream>>>(
        base_bgr,
        base_pitch_bytes,
        overlay_rgba,
        overlay_pitch_bytes,
        width,
        height);
}

void launch_draw_manual_mask_rgba_pitched(
    std::uint8_t* overlay_region,
    std::size_t pitch_bytes,
    int width,
    int height,
    const std::uint8_t* mask,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::uint8_t alpha,
    cudaStream_t stream
) {
    if (overlay_region == nullptr || mask == nullptr || width <= 0 || height <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    draw_manual_mask_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_region,
        pitch_bytes,
        width,
        height,
        mask,
        r,
        g,
        b,
        alpha);
}

void launch_draw_box_outline_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1,
    int box_y1,
    int box_x2,
    int box_y2,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    int thickness,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || image_width <= 0 || image_height <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((image_width + block.x - 1) / block.x, (image_height + block.y - 1) / block.y);
    draw_box_outline_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
        pitch_bytes,
        image_width,
        image_height,
        box_x1,
        box_y1,
        box_x2,
        box_y2,
        r,
        g,
        b,
        std::max(1, thickness));
}

void launch_draw_selection_handles_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1,
    int box_y1,
    int box_x2,
    int box_y2,
    int handle_radius,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || image_width <= 0 || image_height <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((image_width + block.x - 1) / block.x, (image_height + block.y - 1) / block.y);
    draw_selection_handles_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
        pitch_bytes,
        image_width,
        image_height,
        box_x1,
        box_y1,
        box_x2,
        box_y2,
        std::max(1, handle_radius));
}

void launch_draw_polyline_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    bool closed,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    int thickness,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || points_xy == nullptr || image_width <= 0 || image_height <= 0 || point_count < 2) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((image_width + block.x - 1) / block.x, (image_height + block.y - 1) / block.y);
    draw_polyline_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
        pitch_bytes,
        image_width,
        image_height,
        points_xy,
        point_count,
        closed ? 1 : 0,
        r,
        g,
        b,
        std::max(1, thickness));
}

void launch_draw_points_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    int radius,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::uint8_t alpha,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || points_xy == nullptr || image_width <= 0 || image_height <= 0 || point_count <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((image_width + block.x - 1) / block.x, (image_height + block.y - 1) / block.y);
    draw_points_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
        pitch_bytes,
        image_width,
        image_height,
        points_xy,
        point_count,
        std::max(1, radius),
        r,
        g,
        b,
        alpha);
}

void launch_draw_skeleton_rgba_pitched(
    std::uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    const int* points_xy,
    int point_count,
    const std::uint32_t* edge_indices,
    int edge_count,
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    int thickness,
    cudaStream_t stream
) {
    if (overlay_out == nullptr || points_xy == nullptr || edge_indices == nullptr ||
        image_width <= 0 || image_height <= 0 || point_count <= 0 || edge_count <= 0) {
        return;
    }

    dim3 block(16, 16);
    dim3 grid((image_width + block.x - 1) / block.x, (image_height + block.y - 1) / block.y);
    draw_skeleton_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_out,
        pitch_bytes,
        image_width,
        image_height,
        points_xy,
        point_count,
        edge_indices,
        edge_count,
        r,
        g,
        b,
        std::max(1, thickness));
}

} // namespace mmltk::rfdetr
