#pragma once

#include <cmath>
#include <cstdint>

namespace mmltk::rfdetr::draw_color {

[[nodiscard]] __host__ __device__ __forceinline__ int safe_class_count(const int num_classes) {
    return num_classes < 1 ? 1 : num_classes;
}

[[nodiscard]] __host__ __device__ __forceinline__ int normalize_label(const int label, const int safe_count) {
    return (label < 0 || label >= safe_count) ? 0 : label;
}

__host__ __device__ __forceinline__ void hsv_to_rgb(const float h, const float s, const float v, std::uint8_t& r,
                                                    std::uint8_t& g, std::uint8_t& b) {
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rf = 0.0f;
    float gf = 0.0f;
    float bf = 0.0f;

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

    r = static_cast<std::uint8_t>((rf + m) * 255.0f);
    g = static_cast<std::uint8_t>((gf + m) * 255.0f);
    b = static_cast<std::uint8_t>((bf + m) * 255.0f);
}

__host__ __device__ __forceinline__ void instance_color_from_rank(const int label, const int rank, const int safe_count,
                                                                  std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    const int normalized_label = normalize_label(label, safe_count);
    const float hue_step = 360.0f / static_cast<float>(safe_count);
    const float base_h = static_cast<float>(normalized_label) * hue_step;

    float h = base_h;
    float s = 1.0f;
    const float v = 1.0f;
    if (rank > 0) {
        const int ring = (rank - 1) / 10 + 1;
        const int sv_idx = ((rank - 1) % 10) / 2;
        const int sign = ((rank - 1) % 2 == 0) ? 1 : -1;
        s = 1.0f - (static_cast<float>(sv_idx) * 0.05f);
        if (s < 0.0f) {
            s = 0.0f;
        }
        h = fmodf(base_h + (static_cast<float>(sign) * static_cast<float>(ring) * 3.6f) + 360.0f, 360.0f);
    }

    hsv_to_rgb(h, s, v, r, g, b);
}

}  
