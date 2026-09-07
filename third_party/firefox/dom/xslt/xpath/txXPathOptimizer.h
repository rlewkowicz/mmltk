/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef txXPathOptimizer_h_
#define txXPathOptimizer_h_

#include "txCore.h"

class Expr;

class txXPathOptimizer {
 public:
  void optimize(Expr* aInExpr, Expr** aOutExpr);

 private:
  void optimizeStep(Expr* aInExpr, Expr** aOutExpr);
  void optimizePath(Expr* aInExpr, Expr** aOutExpr);
  void optimizeUnion(Expr* aInExpr, Expr** aOutExpr);
};

#endif
