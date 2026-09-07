/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
 * Copyright (C) 2018 Advanced Micro Devices, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef MENUMS_H
#define MENUMS_H

#include "util/macros.h"

typedef enum
{
   API_OPENGL_COMPAT,      
   API_OPENGLES,
   API_OPENGLES2,
   API_OPENGL_CORE,
   API_OPENGL_LAST = API_OPENGL_CORE
} gl_api;

typedef enum
{
   TEXTURE_2D_MULTISAMPLE_INDEX,
   TEXTURE_2D_MULTISAMPLE_ARRAY_INDEX,
   TEXTURE_CUBE_ARRAY_INDEX,
   TEXTURE_BUFFER_INDEX,
   TEXTURE_2D_ARRAY_INDEX,
   TEXTURE_1D_ARRAY_INDEX,
   TEXTURE_EXTERNAL_INDEX,
   TEXTURE_CUBE_INDEX,
   TEXTURE_3D_INDEX,
   TEXTURE_RECT_INDEX,
   TEXTURE_2D_INDEX,
   TEXTURE_1D_INDEX,
   NUM_TEXTURE_TARGETS
} gl_texture_index;

enum PACKED gl_logicop_mode {
   COLOR_LOGICOP_CLEAR = 0,
   COLOR_LOGICOP_NOR = 1,
   COLOR_LOGICOP_AND_INVERTED = 2,
   COLOR_LOGICOP_COPY_INVERTED = 3,
   COLOR_LOGICOP_AND_REVERSE = 4,
   COLOR_LOGICOP_INVERT = 5,
   COLOR_LOGICOP_XOR = 6,
   COLOR_LOGICOP_NAND = 7,
   COLOR_LOGICOP_AND = 8,
   COLOR_LOGICOP_EQUIV = 9,
   COLOR_LOGICOP_NOOP = 10,
   COLOR_LOGICOP_OR_INVERTED = 11,
   COLOR_LOGICOP_COPY = 12,
   COLOR_LOGICOP_OR_REVERSE = 13,
   COLOR_LOGICOP_OR = 14,
   COLOR_LOGICOP_SET = 15
};

typedef enum
{
   BUFFER_FRONT_LEFT,
   BUFFER_BACK_LEFT,
   BUFFER_FRONT_RIGHT,
   BUFFER_BACK_RIGHT,
   BUFFER_DEPTH,
   BUFFER_STENCIL,
   BUFFER_ACCUM,
   BUFFER_AUX0,
   BUFFER_COLOR0,
   BUFFER_COLOR1,
   BUFFER_COLOR2,
   BUFFER_COLOR3,
   BUFFER_COLOR4,
   BUFFER_COLOR5,
   BUFFER_COLOR6,
   BUFFER_COLOR7,
   BUFFER_COUNT,
   BUFFER_NONE = -1,
} gl_buffer_index;

typedef enum
{
   MAP_USER,
   MAP_INTERNAL,
   MAP_COUNT
} gl_map_buffer_index;


enum mesa_debug_source
{
   MESA_DEBUG_SOURCE_API,
   MESA_DEBUG_SOURCE_WINDOW_SYSTEM,
   MESA_DEBUG_SOURCE_SHADER_COMPILER,
   MESA_DEBUG_SOURCE_THIRD_PARTY,
   MESA_DEBUG_SOURCE_APPLICATION,
   MESA_DEBUG_SOURCE_OTHER,
   MESA_DEBUG_SOURCE_COUNT
};

enum mesa_debug_type
{
   MESA_DEBUG_TYPE_ERROR,
   MESA_DEBUG_TYPE_DEPRECATED,
   MESA_DEBUG_TYPE_UNDEFINED,
   MESA_DEBUG_TYPE_PORTABILITY,
   MESA_DEBUG_TYPE_PERFORMANCE,
   MESA_DEBUG_TYPE_OTHER,
   MESA_DEBUG_TYPE_MARKER,
   MESA_DEBUG_TYPE_PUSH_GROUP,
   MESA_DEBUG_TYPE_POP_GROUP,
   MESA_DEBUG_TYPE_COUNT
};

enum mesa_debug_severity
{
   MESA_DEBUG_SEVERITY_LOW,
   MESA_DEBUG_SEVERITY_MEDIUM,
   MESA_DEBUG_SEVERITY_HIGH,
   MESA_DEBUG_SEVERITY_NOTIFICATION,
   MESA_DEBUG_SEVERITY_COUNT
};


#endif
