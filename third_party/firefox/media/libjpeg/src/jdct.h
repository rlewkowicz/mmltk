/*
 * jdct.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2015, 2022, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This include file contains common declarations for the forward and
 * inverse DCT modules.  These declarations are private to the DCT managers
 * (jcdctmgr.c, jddctmgr.c) and the individual DCT algorithms.
 * The individual DCT algorithms are kept in separate files to ease
 * machine-dependent tuning (e.g., assembly coding).
 */

#include "jsamplecomp.h"



#if BITS_IN_JSAMPLE == 8
#ifndef WITH_SIMD
typedef int DCTELEM;            
typedef unsigned int UDCTELEM;
typedef unsigned long long UDCTELEM2;
#else
typedef short DCTELEM;          
typedef unsigned short UDCTELEM;
typedef unsigned int UDCTELEM2;
#endif
#else
typedef JLONG DCTELEM;          
typedef unsigned long long UDCTELEM2;
#endif





typedef MULTIPLIER ISLOW_MULT_TYPE;  
#if BITS_IN_JSAMPLE == 8
typedef MULTIPLIER IFAST_MULT_TYPE;  
#define IFAST_SCALE_BITS  2          /* fractional bits in scale factors */
#else
typedef JLONG IFAST_MULT_TYPE;       
#define IFAST_SCALE_BITS  13         /* fractional bits in scale factors */
#endif
typedef FAST_FLOAT FLOAT_MULT_TYPE;  



#define IDCT_range_limit(cinfo) \
  ((_JSAMPLE *)((cinfo)->sample_range_limit) + _CENTERJSAMPLE)

#define RANGE_MASK  (_MAXJSAMPLE * 4 + 3) /* 2 bits wider than legal samples */



EXTERN(void) _jpeg_fdct_islow(DCTELEM *data);
EXTERN(void) _jpeg_fdct_ifast(DCTELEM *data);
EXTERN(void) jpeg_fdct_float(FAST_FLOAT *data);

EXTERN(void) _jpeg_idct_islow(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_ifast(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_float(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_7x7(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_6x6(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_5x5(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_4x4(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_3x3(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_2x2(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_1x1(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_9x9(j_decompress_ptr cinfo,
                            jpeg_component_info *compptr, JCOEFPTR coef_block,
                            _JSAMPARRAY output_buf, JDIMENSION output_col);
EXTERN(void) _jpeg_idct_10x10(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_11x11(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_12x12(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_13x13(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_14x14(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_15x15(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);
EXTERN(void) _jpeg_idct_16x16(j_decompress_ptr cinfo,
                              jpeg_component_info *compptr,
                              JCOEFPTR coef_block, _JSAMPARRAY output_buf,
                              JDIMENSION output_col);



#define ONE          ((JLONG)1)
#define CONST_SCALE  (ONE << CONST_BITS)


#define FIX(x)  ((JLONG)((x) * CONST_SCALE + 0.5))


#define DESCALE(x, n)  RIGHT_SHIFT((x) + (ONE << ((n) - 1)), n)


#ifdef SHORTxSHORT_32           /* may work if 'int' is 32 bits */
#define MULTIPLY16C16(var, const)  (((INT16)(var)) * ((INT16)(const)))
#endif
#ifdef SHORTxLCONST_32          /* known to work with Microsoft C 6.0 */
#define MULTIPLY16C16(var, const)  (((INT16)(var)) * ((JLONG)(const)))
#endif

#ifndef MULTIPLY16C16           /* default definition */
#define MULTIPLY16C16(var, const)  ((var) * (const))
#endif


#ifdef SHORTxSHORT_32           /* may work if 'int' is 32 bits */
#define MULTIPLY16V16(var1, var2)  (((INT16)(var1)) * ((INT16)(var2)))
#endif

#ifndef MULTIPLY16V16           /* default definition */
#define MULTIPLY16V16(var1, var2)  ((var1) * (var2))
#endif
