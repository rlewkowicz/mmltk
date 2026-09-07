/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_HWCONTEXT_DRM_H
#define AVUTIL_HWCONTEXT_DRM_H

#include <stddef.h>
#include <stdint.h>


enum {
  AV_DRM_MAX_PLANES = 4
};

typedef struct AVDRMObjectDescriptor {
  int fd;
  size_t size;
  uint64_t format_modifier;
} AVDRMObjectDescriptor;

typedef struct AVDRMPlaneDescriptor {
  int object_index;
  ptrdiff_t offset;
  ptrdiff_t pitch;
} AVDRMPlaneDescriptor;

typedef struct AVDRMLayerDescriptor {
  uint32_t format;
  int nb_planes;
  AVDRMPlaneDescriptor planes[AV_DRM_MAX_PLANES];
} AVDRMLayerDescriptor;

typedef struct AVDRMFrameDescriptor {
  int nb_objects;
  AVDRMObjectDescriptor objects[AV_DRM_MAX_PLANES];
  int nb_layers;
  AVDRMLayerDescriptor layers[AV_DRM_MAX_PLANES];
} AVDRMFrameDescriptor;

typedef struct AVDRMDeviceContext {
  int fd;
} AVDRMDeviceContext;

#endif /* AVUTIL_HWCONTEXT_DRM_H */
