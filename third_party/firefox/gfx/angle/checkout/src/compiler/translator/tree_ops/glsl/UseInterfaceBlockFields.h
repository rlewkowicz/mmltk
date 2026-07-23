// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(COMPILER_TRANSLATOR_TREEOPS_GLSL_USEINTERFACEBLOCKFIELDS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_GLSL_USEINTERFACEBLOCKFIELDS_H_

#include <GLSLANG/ShaderLang.h>
#include "common/angleutils.h"
#include "common/debug.h"

namespace sh
{

class TCompiler;
class TIntermBlock;
class TSymbolTable;

using InterfaceBlockList = std::vector<sh::InterfaceBlock>;

#if defined(ANGLE_ENABLE_GLSL)
[[nodiscard]] bool UseInterfaceBlockFields(TCompiler *compiler,
                                           TIntermBlock *root,
                                           const InterfaceBlockList &blocks,
                                           const TSymbolTable &symbolTable);
#else
[[nodiscard]] ANGLE_INLINE bool UseInterfaceBlockFields(TCompiler *compiler,
                                                        TIntermBlock *root,
                                                        const InterfaceBlockList &blocks,
                                                        const TSymbolTable &symbolTable)
{
    UNREACHABLE();
    return false;
}
#endif

}  

#endif
