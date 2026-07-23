// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REMOVEINACTIVEVARIABLES_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REMOVEINACTIVEVARIABLES_H_

#include "common/angleutils.h"

namespace sh
{

struct InterfaceBlock;
struct ShaderVariable;
class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool RemoveInactiveInterfaceVariables(
    TCompiler *compiler,
    TIntermBlock *root,
    TSymbolTable *symbolTable,
    const std::vector<sh::ShaderVariable> &attributes,
    const std::vector<sh::ShaderVariable> &inputVaryings,
    const std::vector<sh::ShaderVariable> &outputVariables,
    const std::vector<sh::ShaderVariable> &uniforms,
    const std::vector<sh::InterfaceBlock> &interfaceBlocks,
    bool removeFragmentOutputs);

}  

#endif
