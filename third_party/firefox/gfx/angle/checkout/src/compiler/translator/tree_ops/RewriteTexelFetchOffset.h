// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REWRITE_TEXELFETCHOFFSET_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REWRITE_TEXELFETCHOFFSET_H_

#include "common/angleutils.h"

namespace sh
{

class TCompiler;
class TIntermNode;
class TSymbolTable;

[[nodiscard]] bool RewriteTexelFetchOffset(TCompiler *compiler,
                                           TIntermNode *root,
                                           const TSymbolTable &symbolTable,
                                           int shaderVersion);

}  

#endif
