/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef VariableUsageHelpers_h_
#define VariableUsageHelpers_h_

#include "plugin.h"

std::vector<const Stmt *> getUsageAsRvalue(const ValueDecl *ValueDeclaration,
                                           const FunctionDecl *FuncDecl);

enum class EscapesFunctionError {
  ConstructorDeclNotFound = 1,
  FunctionDeclNotFound,
  FunctionIsBuiltin,
  FunctionIsVariadic,
  ExprNotInCall,
  NoParamForArg,
  ArgAndParamNotPointers
};

std::error_code make_error_code(EscapesFunctionError);

ErrorOr<std::tuple<const Stmt *, const Decl *>>
escapesFunction(const Expr *Arg, const FunctionDecl *FuncDecl,
                const Expr *const *Arguments, unsigned NumArgs);

ErrorOr<std::tuple<const Stmt *, const Decl *>>
escapesFunction(const Expr *Arg, const CallExpr *Call);

ErrorOr<std::tuple<const Stmt *, const Decl *>>
escapesFunction(const Expr *Arg, const CXXConstructExpr *Construct);

ErrorOr<std::tuple<const Stmt *, const Decl *>>
escapesFunction(const Expr *Arg, const CXXOperatorCallExpr *OpCall);

#endif
