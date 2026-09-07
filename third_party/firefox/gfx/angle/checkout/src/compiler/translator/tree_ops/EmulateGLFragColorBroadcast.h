// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_EMULATEGLFRAGCOLORBROADCAST_H_)
#define COMPILER_TRANSLATOR_TREEOPS_EMULATEGLFRAGCOLORBROADCAST_H_

#include <vector>

#include "common/angleutils.h"

namespace sh
{
struct ShaderVariable;
class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool EmulateGLFragColorBroadcast(TCompiler *compiler,
                                               TIntermBlock *root,
                                               int maxDrawBuffers,
                                               int maxDualSourceDrawBuffers,
                                               TSymbolTable *symbolTable,
                                               int shaderVersion);
}  

#endif
