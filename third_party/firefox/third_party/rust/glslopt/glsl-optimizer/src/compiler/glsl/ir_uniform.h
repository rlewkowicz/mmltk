/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef IR_UNIFORM_H
#define IR_UNIFORM_H


#include <stdbool.h>
#include "util/macros.h"
#include "program/prog_parameter.h"  /* For union gl_constant_value. */

#define INACTIVE_UNIFORM_EXPLICIT_LOCATION ((gl_uniform_storage *) -1)

#ifdef __cplusplus
extern "C" {
#endif

enum PACKED gl_uniform_driver_format {
   uniform_native = 0,          
   uniform_int_float,           
};

struct gl_uniform_driver_storage {
   uint8_t element_stride;

   uint8_t vector_stride;

   enum gl_uniform_driver_format format;

   void *data;
};

struct gl_opaque_uniform_index {
   uint8_t index;

   bool active;
};

struct gl_uniform_storage {
   char *name;
   const struct glsl_type *type;

   unsigned array_elements;

   struct gl_opaque_uniform_index opaque[MESA_SHADER_STAGES];

   unsigned active_shader_mask;

   unsigned num_driver_storage;
   struct gl_uniform_driver_storage *driver_storage;

   union gl_constant_value *storage;


   int block_index;

   int offset;

   int matrix_stride;

   int array_stride;

   bool row_major;


   bool hidden;

   bool builtin;

   bool is_shader_storage;

   int atomic_buffer_index;

   unsigned remap_location;

   unsigned num_compatible_subroutines;

   unsigned top_level_array_size;

   unsigned top_level_array_stride;

   bool is_bindless;
};

#ifdef __cplusplus
}
#endif

#endif /* IR_UNIFORM_H */
