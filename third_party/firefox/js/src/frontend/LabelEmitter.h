/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_LabelEmitter_h
#define frontend_LabelEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // Maybe

#include "frontend/BytecodeControlStructures.h"  // LabelControl

namespace js {
namespace frontend {

struct BytecodeEmitter;
class TaggedParserAtomIndex;

class MOZ_STACK_CLASS LabelEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<LabelControl> controlInfo_;

#ifdef DEBUG
  enum class State {
    Start,

    Label,

    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit LabelEmitter(BytecodeEmitter* bce) : bce_(bce) {}

  void emitLabel(TaggedParserAtomIndex name);
  [[nodiscard]] bool emitEnd();
};

} 
} 

#endif /* frontend_LabelEmitter_h */
