/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkSLProgramKind_DEFINED)
#define SkSLProgramKind_DEFINED

#include <cinttypes>

namespace SkSL {

enum class ProgramKind : int8_t {
    kFragment,
    kVertex,
    kCompute,
    kGraphiteFragment,
    kGraphiteVertex,
    kRuntimeColorFilter,        
    kRuntimeShader,             
    kRuntimeBlender,            
    kPrivateRuntimeColorFilter, 
    kPrivateRuntimeShader,      
    kPrivateRuntimeBlender,     
    kMeshVertex,                
    kMeshFragment,              
};

} 

#endif
