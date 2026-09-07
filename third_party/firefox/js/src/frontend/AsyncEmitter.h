/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_AsyncEmitter_h
#define frontend_AsyncEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

#include "frontend/TryEmitter.h"  // TryEmitter

namespace js {
namespace frontend {

struct BytecodeEmitter;


class MOZ_STACK_CLASS AsyncEmitter {
 private:
  BytecodeEmitter* bce_;

  mozilla::Maybe<TryEmitter> rejectTryCatch_;

#ifdef DEBUG

  enum class State {
    Start,

    Parameters,

    ModulePrologue,

    PostParams,

    Body,

    End,
  };

  State state_ = State::Start;
#endif

  [[nodiscard]] bool emitRejectCatch();
  [[nodiscard]] bool emitFinalYield();

 public:
  explicit AsyncEmitter(BytecodeEmitter* bce) : bce_(bce) {};

  [[nodiscard]] bool prepareForParamsWithoutExpressionOrDestructuring();
  [[nodiscard]] bool prepareForParamsWithExpressionOrDestructuring();
  [[nodiscard]] bool prepareForModule();
  [[nodiscard]] bool emitParamsEpilogue();
  [[nodiscard]] bool prepareForBody();
  [[nodiscard]] bool emitEndFunction();
  [[nodiscard]] bool emitEndModule();
};

} 
} 

#endif /* frontend_AsyncEmitter_h */
