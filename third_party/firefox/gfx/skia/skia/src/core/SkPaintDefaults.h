/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkPaintDefaults_DEFINED)
#define SkPaintDefaults_DEFINED

#include "include/core/SkFontTypes.h"


#if !defined(SkPaintDefaults_TextSize)
    #define SkPaintDefaults_TextSize        SkIntToScalar(12)
#endif

#if !defined(SkPaintDefaults_Hinting)
    #define SkPaintDefaults_Hinting         SkFontHinting::kNormal
#endif

#if !defined(SkPaintDefaults_MiterLimit)
    #define SkPaintDefaults_MiterLimit      SkIntToScalar(4)
#endif

#endif
