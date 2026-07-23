// Copyright 2011 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#include "src/dsp/dsp.h"

#if defined(WEBP_USE_SSE41)
#include <smmintrin.h>

#include <assert.h>
#include <string.h>

#include "src/webp/types.h"
#include "src/dsp/cpu.h"
#include "src/dsp/yuv.h"
#include "src/webp/decode.h"

#if defined(FANCY_UPSAMPLING)

#if !defined(WEBP_REDUCE_CSP)


#define GET_M(ij, in, out) do {                                                \
  const __m128i tmp0 = _mm_avg_epu8(k, (in));            \
  const __m128i tmp1 = _mm_and_si128((ij), st);              \
  const __m128i tmp2 = _mm_xor_si128(k, (in));                     \
  const __m128i tmp3 = _mm_or_si128(tmp1, tmp2);  \
  const __m128i tmp4 = _mm_and_si128(tmp3, one);    \
  (out) = _mm_sub_epi8(tmp0, tmp4);     \
} while (0)

#define PACK_AND_STORE(a, b, da, db, out) do {                                 \
  const __m128i t_a = _mm_avg_epu8(a, da);   \
  const __m128i t_b = _mm_avg_epu8(b, db);   \
  const __m128i t_1 = _mm_unpacklo_epi8(t_a, t_b);                             \
  const __m128i t_2 = _mm_unpackhi_epi8(t_a, t_b);                             \
  _mm_store_si128(((__m128i*)(out)) + 0, t_1);                                 \
  _mm_store_si128(((__m128i*)(out)) + 1, t_2);                                 \
} while (0)

#define UPSAMPLE_32PIXELS(r1, r2, out) do {                                    \
  const __m128i one = _mm_set1_epi8(1);                                        \
  const __m128i a = _mm_loadu_si128((const __m128i*)&(r1)[0]);                 \
  const __m128i b = _mm_loadu_si128((const __m128i*)&(r1)[1]);                 \
  const __m128i c = _mm_loadu_si128((const __m128i*)&(r2)[0]);                 \
  const __m128i d = _mm_loadu_si128((const __m128i*)&(r2)[1]);                 \
                                                                               \
  const __m128i s = _mm_avg_epu8(a, d);               \
  const __m128i t = _mm_avg_epu8(b, c);               \
  const __m128i st = _mm_xor_si128(s, t);                        \
                                                                               \
  const __m128i ad = _mm_xor_si128(a, d);                        \
  const __m128i bc = _mm_xor_si128(b, c);                        \
                                                                               \
  const __m128i t1 = _mm_or_si128(ad, bc);                  \
  const __m128i t2 = _mm_or_si128(t1, st);          \
  const __m128i t3 = _mm_and_si128(t2, one);    \
  const __m128i t4 = _mm_avg_epu8(s, t);                                       \
  const __m128i k = _mm_sub_epi8(t4, t3);         \
  __m128i diag1, diag2;                                                        \
                                                                               \
  GET_M(bc, t, diag1);                      \
  GET_M(ad, s, diag2);                      \
                                                                               \
                                                \
  PACK_AND_STORE(a, b, diag1, diag2, (out) +      0);           \
  PACK_AND_STORE(c, d, diag2, diag1, (out) + 2 * 32);        \
} while (0)

static void Upsample32Pixels_SSE41(const uint8_t* WEBP_RESTRICT const r1,
                                   const uint8_t* WEBP_RESTRICT const r2,
                                   uint8_t* WEBP_RESTRICT const out) {
  UPSAMPLE_32PIXELS(r1, r2, out);
}

#define UPSAMPLE_LAST_BLOCK(tb, bb, num_pixels, out) {                         \
  uint8_t r1[17], r2[17];                                                      \
  memcpy(r1, (tb), (num_pixels));                                              \
  memcpy(r2, (bb), (num_pixels));                                              \
                                                      \
  memset(r1 + (num_pixels), r1[(num_pixels) - 1], 17 - (num_pixels));          \
  memset(r2 + (num_pixels), r2[(num_pixels) - 1], 17 - (num_pixels));          \
       \
  Upsample32Pixels_SSE41(r1, r2, out);                                         \
}

#define CONVERT2RGB_32(FUNC, XSTEP, top_y, bottom_y,                           \
                       top_dst, bottom_dst, cur_x) do {                        \
  FUNC##32_SSE41((top_y) + (cur_x), r_u, r_v, (top_dst) + (cur_x) * (XSTEP));  \
  if ((bottom_y) != NULL) {                                                    \
    FUNC##32_SSE41((bottom_y) + (cur_x), r_u + 64, r_v + 64,                   \
                  (bottom_dst) + (cur_x) * (XSTEP));                           \
  }                                                                            \
} while (0)

#define SSE4_UPSAMPLE_FUNC(FUNC_NAME, FUNC, XSTEP)                             \
static void FUNC_NAME(const uint8_t* WEBP_RESTRICT top_y,                      \
                      const uint8_t* WEBP_RESTRICT bottom_y,                   \
                      const uint8_t* WEBP_RESTRICT top_u,                      \
                      const uint8_t* WEBP_RESTRICT top_v,                      \
                      const uint8_t* WEBP_RESTRICT cur_u,                      \
                      const uint8_t* WEBP_RESTRICT cur_v,                      \
                      uint8_t* WEBP_RESTRICT top_dst,                          \
                      uint8_t* WEBP_RESTRICT bottom_dst, int len) {            \
  int uv_pos, pos;                                                             \
                      \
  uint8_t uv_buf[14 * 32 + 15] = { 0 };                                        \
  uint8_t* const r_u = (uint8_t*)((uintptr_t)(uv_buf + 15) & ~(uintptr_t)15);  \
  uint8_t* const r_v = r_u + 32;                                               \
                                                                               \
  assert(top_y != NULL);                                                       \
  {                                  \
    const int u_diag = ((top_u[0] + cur_u[0]) >> 1) + 1;                       \
    const int v_diag = ((top_v[0] + cur_v[0]) >> 1) + 1;                       \
    const int u0_t = (top_u[0] + u_diag) >> 1;                                 \
    const int v0_t = (top_v[0] + v_diag) >> 1;                                 \
    FUNC(top_y[0], u0_t, v0_t, top_dst);                                       \
    if (bottom_y != NULL) {                                                    \
      const int u0_b = (cur_u[0] + u_diag) >> 1;                               \
      const int v0_b = (cur_v[0] + v_diag) >> 1;                               \
      FUNC(bottom_y[0], u0_b, v0_b, bottom_dst);                               \
    }                                                                          \
  }                                                                            \
    \
  for (pos = 1, uv_pos = 0; pos + 32 + 1 <= len; pos += 32, uv_pos += 16) {    \
    UPSAMPLE_32PIXELS(top_u + uv_pos, cur_u + uv_pos, r_u);                    \
    UPSAMPLE_32PIXELS(top_v + uv_pos, cur_v + uv_pos, r_v);                    \
    CONVERT2RGB_32(FUNC, XSTEP, top_y, bottom_y, top_dst, bottom_dst, pos);    \
  }                                                                            \
  if (len > 1) {                                                               \
    const int left_over = ((len + 1) >> 1) - (pos >> 1);                       \
    uint8_t* const tmp_top_dst = r_u + 4 * 32;                                 \
    uint8_t* const tmp_bottom_dst = tmp_top_dst + 4 * 32;                      \
    uint8_t* const tmp_top = tmp_bottom_dst + 4 * 32;                          \
    uint8_t* const tmp_bottom = (bottom_y == NULL) ? NULL : tmp_top + 32;      \
    assert(left_over > 0);                                                     \
    UPSAMPLE_LAST_BLOCK(top_u + uv_pos, cur_u + uv_pos, left_over, r_u);       \
    UPSAMPLE_LAST_BLOCK(top_v + uv_pos, cur_v + uv_pos, left_over, r_v);       \
    memcpy(tmp_top, top_y + pos, len - pos);                                   \
    if (bottom_y != NULL) memcpy(tmp_bottom, bottom_y + pos, len - pos);       \
    CONVERT2RGB_32(FUNC, XSTEP, tmp_top, tmp_bottom, tmp_top_dst,              \
         tmp_bottom_dst, 0);                                                   \
    memcpy(top_dst + pos * (XSTEP), tmp_top_dst, (len - pos) * (XSTEP));       \
    if (bottom_y != NULL) {                                                    \
      memcpy(bottom_dst + pos * (XSTEP), tmp_bottom_dst,                       \
             (len - pos) * (XSTEP));                                           \
    }                                                                          \
  }                                                                            \
}

SSE4_UPSAMPLE_FUNC(UpsampleRgbLinePair_SSE41,  VP8YuvToRgb,  3)
SSE4_UPSAMPLE_FUNC(UpsampleBgrLinePair_SSE41,  VP8YuvToBgr,  3)

#undef GET_M
#undef PACK_AND_STORE
#undef UPSAMPLE_32PIXELS
#undef UPSAMPLE_LAST_BLOCK
#undef CONVERT2RGB
#undef CONVERT2RGB_32
#undef SSE4_UPSAMPLE_FUNC

#endif


extern WebPUpsampleLinePairFunc WebPUpsamplers[];

extern void WebPInitUpsamplersSSE41(void);

WEBP_TSAN_IGNORE_FUNCTION void WebPInitUpsamplersSSE41(void) {
#if !defined(WEBP_REDUCE_CSP)
  WebPUpsamplers[MODE_RGB]  = UpsampleRgbLinePair_SSE41;
  WebPUpsamplers[MODE_BGR]  = UpsampleBgrLinePair_SSE41;
#endif
}

#endif


extern WebPYUV444Converter WebPYUV444Converters[];
extern void WebPInitYUV444ConvertersSSE41(void);

#define YUV444_FUNC(FUNC_NAME, CALL, CALL_C, XSTEP)                            \
extern void CALL_C(const uint8_t* WEBP_RESTRICT y,                             \
                   const uint8_t* WEBP_RESTRICT u,                             \
                   const uint8_t* WEBP_RESTRICT v,                             \
                   uint8_t* WEBP_RESTRICT dst, int len);                       \
static void FUNC_NAME(const uint8_t* WEBP_RESTRICT y,                          \
                      const uint8_t* WEBP_RESTRICT u,                          \
                      const uint8_t* WEBP_RESTRICT v,                          \
                      uint8_t* WEBP_RESTRICT dst, int len) {                   \
  int i;                                                                       \
  const int max_len = len & ~31;                                               \
  for (i = 0; i < max_len; i += 32) {                                          \
    CALL(y + i, u + i, v + i, dst + i * (XSTEP));                              \
  }                                                                            \
  if (i < len) {                                               \
    CALL_C(y + i, u + i, v + i, dst + i * (XSTEP), len - i);                   \
  }                                                                            \
}

#if !defined(WEBP_REDUCE_CSP)
YUV444_FUNC(Yuv444ToRgb_SSE41, VP8YuvToRgb32_SSE41, WebPYuv444ToRgb_C, 3)
YUV444_FUNC(Yuv444ToBgr_SSE41, VP8YuvToBgr32_SSE41, WebPYuv444ToBgr_C, 3)
#endif

WEBP_TSAN_IGNORE_FUNCTION void WebPInitYUV444ConvertersSSE41(void) {
#if !defined(WEBP_REDUCE_CSP)
  WebPYUV444Converters[MODE_RGB]       = Yuv444ToRgb_SSE41;
  WebPYUV444Converters[MODE_BGR]       = Yuv444ToBgr_SSE41;
#endif
}

#else

WEBP_DSP_INIT_STUB(WebPInitYUV444ConvertersSSE41)

#endif

#if !(defined(FANCY_UPSAMPLING) && defined(WEBP_USE_SSE41))
WEBP_DSP_INIT_STUB(WebPInitUpsamplersSSE41)
#endif
