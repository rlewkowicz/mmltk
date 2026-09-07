/*
* Copyright 2019 Google LLC
*
* Use of this source code is governed by a BSD-style license that can be
* found in the LICENSE file.
*/

#if !defined(SkClipStackUtils_DEFINED)
#define SkClipStackUtils_DEFINED

#include "include/core/SkTypes.h"

class SkClipStack;
class SkPath;

SkPath SkClipStack_AsPath(const SkClipStack&);

#endif
