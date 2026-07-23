/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkAnnotation_DEFINED)
#define SkAnnotation_DEFINED

#include "include/core/SkTypes.h"

class SkData;
struct SkPoint;
struct SkRect;
class SkCanvas;

SK_API void SkAnnotateRectWithURL(SkCanvas*, const SkRect&, SkData*);

SK_API void SkAnnotateNamedDestination(SkCanvas*, const SkPoint&, SkData*);

SK_API void SkAnnotateLinkToDestination(SkCanvas*, const SkRect&, SkData*);

#endif
