/*
 * jmorecfg.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1997, Thomas G. Lane.
 * Modified 1997-2009 by Guido Vollbeding.
 * Lossless JPEG Modifications:
 * Copyright (C) 1999, Ken Murchison.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2009, 2011, 2014-2015, 2018, 2020, 2022, 2026,
 *           D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains additional configuration options that customize the
 * JPEG software for special applications or support machine-dependent
 * optimizations.  Most users will not need to touch this file.
 */

#include <stdint.h>


#define MAX_COMPONENTS  10      /* maximum number of image components */





typedef unsigned char JSAMPLE;
#define GETJSAMPLE(value)  ((int)(value))

#define MAXJSAMPLE       255
#define CENTERJSAMPLE    128



typedef short J12SAMPLE;

#define MAXJ12SAMPLE     4095
#define CENTERJ12SAMPLE  2048



typedef unsigned short J16SAMPLE;

#define MAXJ16SAMPLE     65535
#define CENTERJ16SAMPLE  32768



typedef short JCOEF;



typedef unsigned char JOCTET;
#define GETJOCTET(value)  (value)




typedef uint8_t UINT8;


typedef uint16_t UINT16;


typedef int16_t INT16;


typedef int32_t INT32;


typedef unsigned int JDIMENSION;

#define JPEG_MAX_DIMENSION  65500L  /* a tad under 64K to prevent overflows */



#define METHODDEF(type)         static type
#define LOCAL(type)             static type
#define GLOBAL(type)            type
#define EXTERN(type)            extern type



#define JMETHOD(type, methodname, arglist)  type (*methodname) arglist



#undef FAR
#define FAR



#ifndef HAVE_BOOLEAN
typedef int boolean;
#endif
#ifndef FALSE                   /* in case these macros already exist */
#define FALSE   0               /* values of boolean */
#endif
#ifndef TRUE
#define TRUE    1
#endif



#ifdef JPEG_INTERNALS
#define JPEG_INTERNAL_OPTIONS
#endif

#ifdef JPEG_INTERNAL_OPTIONS




#define DCT_ISLOW_SUPPORTED     /* accurate integer method */
#define DCT_IFAST_SUPPORTED     /* less accurate int method [legacy feature] */
#define DCT_FLOAT_SUPPORTED     /* floating-point method [legacy feature] */


#define C_MULTISCAN_FILES_SUPPORTED /* Multiple-scan JPEG files? */
#define C_PROGRESSIVE_SUPPORTED     /* Progressive JPEG?  (Requires
                                       C_MULTISCAN_FILES_SUPPORTED and
                                       ENTROPY_OPT_SUPPORTED) */
#define C_LOSSLESS_SUPPORTED        /* Lossless JPEG? */
#define ENTROPY_OPT_SUPPORTED       /* Optimization of entropy coding parms? */
#define INPUT_SMOOTHING_SUPPORTED   /* Input image smoothing option? */


#define D_MULTISCAN_FILES_SUPPORTED /* Multiple-scan JPEG files? */
#define D_PROGRESSIVE_SUPPORTED     /* Progressive JPEG?  (Requires
                                       D_MULTISCAN_FILES_SUPPORTED) */
#define D_LOSSLESS_SUPPORTED        /* Lossless JPEG?  (Requires
                                       D_MULTISCAN_FILES_SUPPORTED) */
#define SAVE_MARKERS_SUPPORTED      /* jpeg_save_markers() needed? */
#define BLOCK_SMOOTHING_SUPPORTED   /* Block smoothing? (Progressive only) */
#define IDCT_SCALING_SUPPORTED      /* Output rescaling via IDCT?  (Requires
                                       DCT_ISLOW_SUPPORTED) */
#define UPSAMPLE_MERGING_SUPPORTED  /* Fast path for sloppy upsampling? */
#define QUANT_1PASS_SUPPORTED       /* 1-pass color quantization? */
#define QUANT_2PASS_SUPPORTED       /* 2-pass color quantization? */




#define RGB_RED         0       /* Offset of Red in an RGB scanline element */
#define RGB_GREEN       1       /* Offset of Green */
#define RGB_BLUE        2       /* Offset of Blue */
#define RGB_PIXELSIZE   3       /* JSAMPLEs per RGB scanline element */

#define JPEG_NUMCS  17

#define EXT_RGB_RED         0
#define EXT_RGB_GREEN       1
#define EXT_RGB_BLUE        2
#define EXT_RGB_PIXELSIZE   3

#define EXT_RGBX_RED        0
#define EXT_RGBX_GREEN      1
#define EXT_RGBX_BLUE       2
#define EXT_RGBX_PIXELSIZE  4

#define EXT_BGR_RED         2
#define EXT_BGR_GREEN       1
#define EXT_BGR_BLUE        0
#define EXT_BGR_PIXELSIZE   3

#define EXT_BGRX_RED        2
#define EXT_BGRX_GREEN      1
#define EXT_BGRX_BLUE       0
#define EXT_BGRX_PIXELSIZE  4

#define EXT_XBGR_RED        3
#define EXT_XBGR_GREEN      2
#define EXT_XBGR_BLUE       1
#define EXT_XBGR_PIXELSIZE  4

#define EXT_XRGB_RED        1
#define EXT_XRGB_GREEN      2
#define EXT_XRGB_BLUE       3
#define EXT_XRGB_PIXELSIZE  4

static const int rgb_red[JPEG_NUMCS] = {
  -1, -1, RGB_RED, -1, -1, -1, EXT_RGB_RED, EXT_RGBX_RED,
  EXT_BGR_RED, EXT_BGRX_RED, EXT_XBGR_RED, EXT_XRGB_RED,
  EXT_RGBX_RED, EXT_BGRX_RED, EXT_XBGR_RED, EXT_XRGB_RED,
  -1
};

static const int rgb_green[JPEG_NUMCS] = {
  -1, -1, RGB_GREEN, -1, -1, -1, EXT_RGB_GREEN, EXT_RGBX_GREEN,
  EXT_BGR_GREEN, EXT_BGRX_GREEN, EXT_XBGR_GREEN, EXT_XRGB_GREEN,
  EXT_RGBX_GREEN, EXT_BGRX_GREEN, EXT_XBGR_GREEN, EXT_XRGB_GREEN,
  -1
};

static const int rgb_blue[JPEG_NUMCS] = {
  -1, -1, RGB_BLUE, -1, -1, -1, EXT_RGB_BLUE, EXT_RGBX_BLUE,
  EXT_BGR_BLUE, EXT_BGRX_BLUE, EXT_XBGR_BLUE, EXT_XRGB_BLUE,
  EXT_RGBX_BLUE, EXT_BGRX_BLUE, EXT_XBGR_BLUE, EXT_XRGB_BLUE,
  -1
};

static const int rgb_pixelsize[JPEG_NUMCS] = {
  -1, -1, RGB_PIXELSIZE, -1, -1, -1, EXT_RGB_PIXELSIZE, EXT_RGBX_PIXELSIZE,
  EXT_BGR_PIXELSIZE, EXT_BGRX_PIXELSIZE, EXT_XBGR_PIXELSIZE, EXT_XRGB_PIXELSIZE,
  EXT_RGBX_PIXELSIZE, EXT_BGRX_PIXELSIZE, EXT_XBGR_PIXELSIZE, EXT_XRGB_PIXELSIZE,
  -1
};



#ifndef MULTIPLIER
#ifndef WITH_SIMD
#define MULTIPLIER  int         /* type for fastest integer multiply */
#else
#define MULTIPLIER  short       /* prefer 16-bit with SIMD for parellelism */
#endif
#endif



#ifndef FAST_FLOAT
#define FAST_FLOAT  float
#endif

#endif /* JPEG_INTERNAL_OPTIONS */
