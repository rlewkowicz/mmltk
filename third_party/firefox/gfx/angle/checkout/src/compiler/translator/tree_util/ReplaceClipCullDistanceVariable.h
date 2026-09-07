// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEUTIL_REPLACECLIPCULLDISTANCEVARIABLE_H_)
#define COMPILER_TRANSLATOR_TREEUTIL_REPLACECLIPCULLDISTANCEVARIABLE_H_

#include "GLSLANG/ShaderLang.h"
#include "common/angleutils.h"

namespace sh
{

struct InterfaceBlock;
class TCompiler;
class TIntermBlock;
class TSymbolTable;
class TIntermTyped;

[[nodiscard]] bool ReplaceClipDistanceAssignments(TCompiler *compiler,
                                                  TIntermBlock *root,
                                                  TSymbolTable *symbolTable,
                                                  const GLenum shaderType,
                                                  const TIntermTyped *clipDistanceEnableFlags);

[[nodiscard]] bool ReplaceCullDistanceAssignments(TCompiler *compiler,
                                                  TIntermBlock *root,
                                                  TSymbolTable *symbolTable,
                                                  const GLenum shaderType);

[[nodiscard]] bool ZeroDisabledClipDistanceAssignments(TCompiler *compiler,
                                                       TIntermBlock *root,
                                                       TSymbolTable *symbolTable,
                                                       const GLenum shaderType,
                                                       const TIntermTyped *clipDistanceEnableFlags);
}  

#endif
