/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkAlphaType_DEFINED)
#define SkAlphaType_DEFINED

enum SkAlphaType : int {
    kUnknown_SkAlphaType,                          
    kOpaque_SkAlphaType,                           
    kPremul_SkAlphaType,                           
    kUnpremul_SkAlphaType,                         
    kLastEnum_SkAlphaType = kUnpremul_SkAlphaType, 
};

static inline bool SkAlphaTypeIsOpaque(SkAlphaType at) {
    return kOpaque_SkAlphaType == at;
}

#endif
