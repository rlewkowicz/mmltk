// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_EMULATEMULTIDRAWSHADERBUILTINS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_EMULATEMULTIDRAWSHADERBUILTINS_H_

#include <GLSLANG/ShaderLang.h>
#include <vector>

#include "common/angleutils.h"
#include "compiler/translator/HashNames.h"

namespace sh
{
struct ShaderVariable;
class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool EmulateGLDrawID(TCompiler *compiler,
                                   TIntermBlock *root,
                                   TSymbolTable *symbolTable);

[[nodiscard]] bool EmulateGLBaseVertexBaseInstance(TCompiler *compiler,
                                                   TIntermBlock *root,
                                                   TSymbolTable *symbolTable,
                                                   bool addBaseVertexToVertexID);

}  

#endif
