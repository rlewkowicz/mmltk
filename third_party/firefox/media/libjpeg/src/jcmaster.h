/*
 * jcmaster.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1995, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2016, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains master control structure for the JPEG compressor.
 */


typedef enum {
  main_pass,                    
  huff_opt_pass,                
  output_pass                   
} c_pass_type;

typedef struct {
  struct jpeg_comp_master pub;  

  c_pass_type pass_type;        

  int pass_number;              
  int total_passes;             

  int scan_number;              

  const char *jpeg_version;

} my_comp_master;

typedef my_comp_master *my_master_ptr;
