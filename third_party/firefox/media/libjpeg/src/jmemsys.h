/*
 * jmemsys.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1992-1997, Thomas G. Lane.
 * It was modified by The libjpeg-turbo Project to include only code and
 * information relevant to libjpeg-turbo.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This include file defines the interface between the system-independent
 * and system-dependent portions of the JPEG memory manager.  No other
 * modules need include it.  (The system-independent portion is jmemmgr.c;
 * there are several different versions of the system-dependent portion.)
 *
 * This file works as-is for the system-dependent memory managers supplied
 * in the IJG distribution.  You may need to modify it if you write a
 * custom memory manager.  If system-dependent changes are needed in
 * this file, the best method is to #ifdef them based on a configuration
 * symbol supplied in jconfig.h.
 */



EXTERN(void *) jpeg_get_small(j_common_ptr cinfo, size_t sizeofobject);
EXTERN(void) jpeg_free_small(j_common_ptr cinfo, void *object,
                             size_t sizeofobject);


EXTERN(void *) jpeg_get_large(j_common_ptr cinfo, size_t sizeofobject);
EXTERN(void) jpeg_free_large(j_common_ptr cinfo, void *object,
                             size_t sizeofobject);


#ifndef MAX_ALLOC_CHUNK         /* may be overridden in jconfig.h */
#define MAX_ALLOC_CHUNK  1000000000L
#endif


EXTERN(size_t) jpeg_mem_available(j_common_ptr cinfo, size_t min_bytes_needed,
                                  size_t max_bytes_needed,
                                  size_t already_allocated);



#define TEMP_NAME_LENGTH   64   /* max length of a temporary file's name */


typedef struct backing_store_struct *backing_store_ptr;

typedef struct backing_store_struct {
  void (*read_backing_store) (j_common_ptr cinfo, backing_store_ptr info,
                              void *buffer_address, long file_offset,
                              long byte_count);
  void (*write_backing_store) (j_common_ptr cinfo, backing_store_ptr info,
                               void *buffer_address, long file_offset,
                               long byte_count);
  void (*close_backing_store) (j_common_ptr cinfo, backing_store_ptr info);

  FILE *temp_file;              
  char temp_name[TEMP_NAME_LENGTH]; 
} backing_store_info;



EXTERN(void) jpeg_open_backing_store(j_common_ptr cinfo,
                                     backing_store_ptr info,
                                     long total_bytes_needed);



EXTERN(long) jpeg_mem_init(j_common_ptr cinfo);
EXTERN(void) jpeg_mem_term(j_common_ptr cinfo);
