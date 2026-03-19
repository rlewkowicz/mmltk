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
        
        bool is_edge = false;
        if (x >= x1 - box_thickness && x <= x2 + box_thickness && y >= y1 - box_thickness && y <= y2 + box_thickness) {
            if (x < x1 || x > x2 || y < y1 || y > y2) {
                is_edge = true;
            }
        }
        
        if (is_edge) {
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
        }
        
        // Draw text: simple 12px scaling (x2 scaling from 5x7)
        int lbl = labels[i];
        if (lbl >= 0) {
            // Draw digit starting at top-left of box (outside if possible, or inside)
            int tx = x - x1;
            int ty = y - (y1 - 16); // position text above box
            if (ty >= 0 && ty < 14 && tx >= 0 && tx < 10) {
                // handle multi-digit labels (e.g., up to 99)
                int digits[2];
                int num_digits = 0;
                if (lbl == 0) {
                    digits[0] = 0;
                    num_digits = 1;
                } else {
                    int temp = lbl;
                    while (temp > 0 && num_digits < 2) {
                        digits[num_digits++] = temp % 10;
                        temp /= 10;
                    }
                }
                
                for (int d = 0; d < num_digits; ++d) {
                    // draw right to left
                    int char_idx = num_digits - 1 - d;
                    int d_val = digits[d];
                    
                    int char_start_x = char_idx * 12; // 10px width + 2px spacing
                    if (tx >= char_start_x && tx < char_start_x + 10) {
                        int font_x = (tx - char_start_x) / 2;
                        int font_y = ty / 2;
                        if (font_x < 5 && font_y < 7) {
                            if (d_font5x7[d_val][font_y] & (0x80 >> font_x)) {
                                r = colors[i * 3];
                                g = colors[i * 3 + 1];
                                b = colors[i * 3 + 2];
                            }
                        }
                    }
                }
            }
        }
    }
    
    // clamp and write back
    image_out[pixel_idx] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
    image_out[pixel_idx + 1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    image_out[pixel_idx + 2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
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

} // namespace fastloader::rfdetr
