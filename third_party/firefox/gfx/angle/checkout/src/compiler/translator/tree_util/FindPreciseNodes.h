// Copyright 2021 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEUTIL_FINDPRECISENODES_H_)
#define COMPILER_TRANSLATOR_TREEUTIL_FINDPRECISENODES_H_

#include "common/angleutils.h"

namespace sh
{

class TCompiler;
class TIntermBlock;

void FindPreciseNodes(TCompiler *compiler, TIntermBlock *root);

}  

#endif
