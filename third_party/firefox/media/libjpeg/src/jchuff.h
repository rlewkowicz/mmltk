/*
 * jchuff.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1997, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2022, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains declarations for Huffman entropy encoding routines
 * that are shared between the sequential encoder (jchuff.c) and the
 * progressive encoder (jcphuff.c).  No other modules need to see these.
 */


typedef unsigned short UJCOEF;


typedef struct {
  unsigned int ehufco[256];     
  char ehufsi[256];             
} c_derived_tbl;

EXTERN(void) jpeg_make_c_derived_tbl(j_compress_ptr cinfo, boolean isDC,
                                     int tblno, c_derived_tbl **pdtbl);

EXTERN(void) jpeg_gen_optimal_table(j_compress_ptr cinfo, JHUFF_TBL *htbl,
                                    long freq[]);
