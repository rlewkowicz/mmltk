/*
 * jdsample.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1996, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2022, 2026, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 */

#define JPEG_INTERNALS
#include "jpeglib.h"
#include "jsamplecomp.h"


typedef void (*upsample1_ptr) (j_decompress_ptr cinfo,
                               jpeg_component_info *compptr,
                               _JSAMPARRAY input_data,
                               _JSAMPARRAY *output_data_ptr);


typedef struct {
  struct jpeg_upsampler pub;    

  _JSAMPARRAY color_buf[MAX_COMPONENTS];

  upsample1_ptr methods[MAX_COMPONENTS];

  int next_row_out;             
  JDIMENSION rows_to_go;        

  int rowgroup_height[MAX_COMPONENTS];

  UINT8 h_expand[MAX_COMPONENTS];
  UINT8 v_expand[MAX_COMPONENTS];
} my_upsampler;

typedef my_upsampler *my_upsample_ptr;
