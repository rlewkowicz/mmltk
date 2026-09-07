/*
 * copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_LOG_H
#define AVUTIL_LOG_H

#include <stdarg.h>
#include "avutil.h"
#include "attributes.h"

typedef struct AVClass {
    const char* class_name;

    const char* (*item_name)(void* ctx);

    const struct AVOption *option;


    int version;

    int log_level_offset_offset;

    int parent_log_context_offset;

    void* (*child_next)(void *obj, void *prev);

    const struct AVClass* (*child_class_next)(const struct AVClass *prev);
} AVClass;


#define AV_LOG_QUIET    -8

#define AV_LOG_PANIC     0

#define AV_LOG_FATAL     8

#define AV_LOG_ERROR    16

#define AV_LOG_WARNING  24

#define AV_LOG_INFO     32
#define AV_LOG_VERBOSE  40

#define AV_LOG_DEBUG    48

void av_log(void *avcl, int level, const char *fmt, ...) av_printf_format(3, 4);

void av_vlog(void *avcl, int level, const char *fmt, va_list);
int av_log_get_level(void);
void av_log_set_level(int);
void av_log_set_callback(void (*)(void*, int, const char*, va_list));
void av_log_default_callback(void* ptr, int level, const char* fmt, va_list vl);
const char* av_default_item_name(void* ctx);


#ifdef DEBUG
#    define av_dlog(pctx, ...) av_log(pctx, AV_LOG_DEBUG, __VA_ARGS__)
#else
#    define av_dlog(pctx, ...)
#endif

#define AV_LOG_SKIP_REPEATED 1
void av_log_set_flags(int arg);

#endif /* AVUTIL_LOG_H */
