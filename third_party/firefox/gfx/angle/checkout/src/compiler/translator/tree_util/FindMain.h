// Copyright 2017 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(COMPILER_TRANSLATOR_TREEUTIL_FINDMAIN_H_)
#define COMPILER_TRANSLATOR_TREEUTIL_FINDMAIN_H_

#include <cstddef>

namespace sh
{
class TIntermBlock;
class TIntermFunctionDefinition;
class TIntermFunctionPrototype;

size_t FindMainIndex(TIntermBlock *root);
TIntermFunctionDefinition *FindMain(TIntermBlock *root);
TIntermFunctionPrototype *FindMainPrototype(TIntermBlock *root);
TIntermBlock *FindMainBody(TIntermBlock *root);
}  

#endif
