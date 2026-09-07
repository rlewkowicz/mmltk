/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Eval_h
#define builtin_Eval_h

#include "NamespaceImports.h"

#include "js/TypeDecls.h"

namespace js {

[[nodiscard]] extern bool IndirectEval(JSContext* cx, unsigned argc, Value* vp);

[[nodiscard]] extern bool DirectEval(JSContext* cx, HandleValue v,
                                     MutableHandleValue vp);

extern bool IsAnyBuiltinEval(JSFunction* fun);

}  

#endif /* builtin_Eval_h */
