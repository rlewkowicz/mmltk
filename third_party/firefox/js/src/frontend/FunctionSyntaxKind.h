/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FunctionSyntaxKind_h
#define frontend_FunctionSyntaxKind_h

#include <stdint.h>  // uint8_t

namespace js {
namespace frontend {

enum class FunctionSyntaxKind : uint8_t {
  Expression,

  Statement,

  Arrow,

  Method,
  FieldInitializer,

  StaticClassBlock,

  ClassConstructor,
  DerivedClassConstructor,
  Getter,
  Setter,
};

} 
} 

#endif /* frontend_FunctionSyntaxKind_h */
