/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_CForEmitter_h
#define frontend_CForEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stdint.h>  // uint32_t

#include "frontend/BytecodeControlStructures.h"  // LoopControl
#include "frontend/TDZCheckCache.h"              // TDZCheckCache

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

class MOZ_STACK_CLASS CForEmitter {
 public:
  enum class Cond { Missing, Present };
  enum class Update { Missing, Present };

 private:
  BytecodeEmitter* bce_;

  Cond cond_ = Cond::Missing;
  Update update_ = Update::Missing;

  mozilla::Maybe<LoopControl> loopInfo_;

  const EmitterScope* headLexicalEmitterScopeForLet_;

  mozilla::Maybe<TDZCheckCache> tdzCache_;

#ifdef DEBUG
  enum class State {
    Start,

    Init,

    Cond,

    Body,

    Update,

    End
  };
  State state_ = State::Start;
#endif

 public:
  CForEmitter(BytecodeEmitter* bce,
              const EmitterScope* headLexicalEmitterScopeForLet);

  [[nodiscard]] bool emitInit(const mozilla::Maybe<uint32_t>& initPos);
  [[nodiscard]] bool emitCond(const mozilla::Maybe<uint32_t>& condPos);
  [[nodiscard]] bool emitBody(Cond cond);
  [[nodiscard]] bool emitUpdate(Update update,
                                const mozilla::Maybe<uint32_t>& updatePos);
  [[nodiscard]] bool emitEnd(uint32_t forPos);
};

} 
} 

#endif /* frontend_CForEmitter_h */
