/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DefaultEmitter_h
#define frontend_DefaultEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // Maybe

#include "frontend/IfEmitter.h"  // IfEmitter

namespace js {
namespace frontend {

struct BytecodeEmitter;

class MOZ_STACK_CLASS DefaultEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<IfEmitter> ifUndefined_;

#ifdef DEBUG
  enum class State {
    Start,

    Default,

    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit DefaultEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool prepareForDefault();
  [[nodiscard]] bool emitEnd();
};

} 
} 

#endif /* frontend_LabelEmitter_h */
