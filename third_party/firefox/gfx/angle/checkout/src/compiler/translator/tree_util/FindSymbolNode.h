// Copyright 2015 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEUTIL_FINDSYMBOLNODE_H_)
#define COMPILER_TRANSLATOR_TREEUTIL_FINDSYMBOLNODE_H_

namespace sh
{

class ImmutableString;
class TIntermNode;
class TIntermSymbol;

const TIntermSymbol *FindSymbolNode(TIntermNode *root, const ImmutableString &symbolName);

}  

#endif
