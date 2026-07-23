/*
 * Copyright (C) 2014, 2021, 2024-2025, D. R. Commander.
 * Copyright (C) 2014, Olle Liljenzin.
 * Copyright (C) 2020, Arm Limited.
 *
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 */


#if (defined(__GNUC__) && (defined(__arm__) || defined(__aarch64__))) || \
    defined(_M_ARM) || defined(_M_ARM64) || defined(_M_ARM64EC)
#if !defined(__thumb__) || defined(__thumb2__)
#define USE_CLZ_INTRINSIC
#endif
#endif

#ifdef USE_CLZ_INTRINSIC
#if defined(_MSC_VER) && !defined(__clang__)
#define JPEG_NBITS_NONZERO(x)  (32 - _CountLeadingZeros(x))
#else
#define JPEG_NBITS_NONZERO(x)  (32 - __builtin_clz(x))
#endif
#define JPEG_NBITS(x)          (x ? JPEG_NBITS_NONZERO(x) : 0)
#else
extern const unsigned char jpeg_nbits_table[65536];
#define JPEG_NBITS(x)          (jpeg_nbits_table[x])
#define JPEG_NBITS_NONZERO(x)  JPEG_NBITS(x)
#endif
