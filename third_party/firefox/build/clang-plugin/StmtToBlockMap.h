/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StmtToBlockMap_h_
#define StmtToBlockMap_h_

#include "Utils.h"

// This method is copied from clang-tidy's ExprSequence.cpp.
inline SmallVector<const Stmt *, 1> getParentStmts(const Stmt *S,
                                                   ASTContext *Context) {
  SmallVector<const Stmt *, 1> Result;

  auto Parents = Context->getParents(*S);

  SmallVector<clang::DynTypedNode, 1> NodesToProcess(Parents.begin(),
                                                     Parents.end());

  while (!NodesToProcess.empty()) {
    clang::DynTypedNode Node = NodesToProcess.back();
    NodesToProcess.pop_back();

    if (const auto *S = Node.get<Stmt>()) {
      Result.push_back(S);
    } else {
      Parents = Context->getParents(Node);
      NodesToProcess.append(Parents.begin(), Parents.end());
    }
  }

  return Result;
}

// This class is a modified version of the class from clang-tidy's
class StmtToBlockMap {
public:
  StmtToBlockMap(const CFG *TheCFG, ASTContext *TheContext)
      : Context(TheContext) {
    for (const auto *B : *TheCFG) {
      for (size_t I = 0; I < B->size(); ++I) {
        if (auto S = (*B)[I].getAs<CFGStmt>()) {
          Map[S->getStmt()] = std::make_pair(B, I);
        }
      }
    }
  }

  const CFGBlock *blockContainingStmt(const Stmt *S,
                                      size_t *Index = nullptr) const {
    while (!Map.count(S)) {
      SmallVector<const Stmt *, 1> Parents = getParentStmts(S, Context);
      if (Parents.empty())
        return nullptr;
      S = Parents[0];
    }

    const auto &E = Map.lookup(S);
    if (Index)
      *Index = E.second;
    return E.first;
  }

private:
  ASTContext *Context;

  llvm::DenseMap<const Stmt *, std::pair<const CFGBlock *, size_t>> Map;
};

#endif // StmtToBlockMap_h_
