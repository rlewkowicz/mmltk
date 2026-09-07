// Copyright 2024 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REMOVEUNUSEDFRAMEBUFFERFETCH_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REMOVEUNUSEDFRAMEBUFFERFETCH_H_

#include "common/angleutils.h"

namespace sh
{

class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool RemoveUnusedFramebufferFetch(TCompiler *compiler,
                                                TIntermBlock *root,
                                                TSymbolTable *symbolTable);

}  

#endif
