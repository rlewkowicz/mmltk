/*
 * jdmerge.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2020, 2022, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 */

#define JPEG_INTERNALS
#include "jpeglib.h"
#include "jsamplecomp.h"

#ifdef UPSAMPLE_MERGING_SUPPORTED



typedef struct {
  struct jpeg_upsampler pub;    

  void (*upmethod) (j_decompress_ptr cinfo, _JSAMPIMAGE input_buf,
                    JDIMENSION in_row_group_ctr, _JSAMPARRAY output_buf);

  int *Cr_r_tab;                
  int *Cb_b_tab;                
  JLONG *Cr_g_tab;              
  JLONG *Cb_g_tab;              

  _JSAMPROW spare_row;
  boolean spare_full;           

  JDIMENSION out_row_width;     
  JDIMENSION rows_to_go;        
} my_merged_upsampler;

typedef my_merged_upsampler *my_merged_upsample_ptr;

#endif /* UPSAMPLE_MERGING_SUPPORTED */
