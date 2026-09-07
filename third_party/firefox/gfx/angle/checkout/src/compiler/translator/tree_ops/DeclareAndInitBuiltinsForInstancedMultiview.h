// Copyright 2017 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_DECLAREANDINITBUILTINSFORINSTANCEDMULTIVIEW_H_)
#define COMPILER_TRANSLATOR_TREEOPS_DECLAREANDINITBUILTINSFORINSTANCEDMULTIVIEW_H_

#include "GLSLANG/ShaderLang.h"
#include "angle_gl.h"
#include "common/angleutils.h"

namespace sh
{

class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool DeclareAndInitBuiltinsForInstancedMultiview(
    TCompiler *compiler,
    TIntermBlock *root,
    unsigned numberOfViews,
    GLenum shaderType,
    const ShCompileOptions &compileOptions,
    ShShaderOutput shaderOutput,
    TSymbolTable *symbolTable);

}  

#endif
