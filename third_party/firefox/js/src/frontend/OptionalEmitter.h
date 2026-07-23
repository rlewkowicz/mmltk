/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_OptionalEmitter_h
#define frontend_OptionalEmitter_h

#include "mozilla/Attributes.h"

#include "frontend/JumpList.h"
#include "frontend/TDZCheckCache.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

class MOZ_RAII OptionalEmitter {
 public:
  OptionalEmitter(BytecodeEmitter* bce, int32_t initialDepth);

 private:
  BytecodeEmitter* bce_;

  TDZCheckCache tdzCache_;

  JumpList jumpShortCircuit_;

  JumpList jumpFinish_;

  int32_t initialDepth_;

#ifdef DEBUG
  enum class State {
    Start,

    ShortCircuit,

    ShortCircuitForCall,

    JumpEnd
  };

  State state_ = State::Start;
#endif

 public:
  enum class Kind {
    Reference,
    Other
  };

  [[nodiscard]] bool emitJumpShortCircuit();
  [[nodiscard]] bool emitJumpShortCircuitForCall();

  [[nodiscard]] bool emitOptionalJumpTarget(JSOp op, Kind kind = Kind::Other);
};

} 
} 

#endif /* frontend_OptionalEmitter_h */
