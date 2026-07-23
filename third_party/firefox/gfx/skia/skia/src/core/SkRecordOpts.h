/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkRecordOpts_DEFINED)
#define SkRecordOpts_DEFINED

class SkRecord;

void SkRecordOptimize(SkRecord*);

void SkRecordNoopSaveRestores(SkRecord*);

#if !defined(SK_BUILD_FOR_ANDROID_FRAMEWORK)
void SkRecordNoopSaveLayerDrawRestores(SkRecord*);
#endif

void SkRecordMergeSvgOpacityAndFilterLayers(SkRecord*);

#endif
