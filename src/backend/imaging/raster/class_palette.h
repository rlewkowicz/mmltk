#pragma once
#include <cmath>
#include <cstdint>
#if defined(__CUDACC__)
#define MMLTK_RASTER_COLOR_INLINE __host__ __device__ __forceinline__
#else
#define MMLTK_RASTER_COLOR_INLINE inline
#endif
namespace mmltk::backend::imaging::raster::color {
[[nodiscard]] MMLTK_RASTER_COLOR_INLINE int safe_class_count(const int num_classes) { return num_classes < 1 ? 1 : num_classes; }
[[nodiscard]] MMLTK_RASTER_COLOR_INLINE int normalize_label(const int label, const int safe_count) { return (label < 0 || label >= safe_count) ? 0 : label; }
MMLTK_RASTER_COLOR_INLINE void hsv_to_rgb(const float h, const float s, const float v, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
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
MMLTK_RASTER_COLOR_INLINE void class_hsv(const int label, const int safe_count, float& h, float& s, float& v) {
 const int normalized_label = normalize_label(label, safe_count);
 const float hue_step = 360.0f / static_cast<float>(safe_count);
 h = static_cast<float>(normalized_label) * hue_step;
 // A cycle uses two alternating tones and, for odd catalogs, a third
 // closing tone. Both S and V differ across every edge, including C-1/0.
 const int tone = safe_count > 12 ? ((safe_count % 2 != 0 && normalized_label == safe_count - 1) ? 2 : normalized_label % 2) : 0;
 s = tone == 0 ? 1.0F : (tone == 1 ? 0.55F : 0.78F);
 v = tone == 0 ? 1.0F : (tone == 1 ? 0.72F : 0.86F);
}
MMLTK_RASTER_COLOR_INLINE void class_color(const int label, const int safe_count, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
 float h, s, v;
 class_hsv(label, safe_count, h, s, v);
 hsv_to_rgb(h, s, v, r, g, b);
}
}  // namespace mmltk::backend::imaging::raster::color
#undef MMLTK_RASTER_COLOR_INLINE
