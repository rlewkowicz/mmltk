/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForOfEmitter_h
#define frontend_ForOfEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stdint.h>  // int32_t

#include "frontend/ForOfLoopControl.h"  // ForOfLoopControl
#include "frontend/IteratorKind.h"      // IteratorKind
#include "frontend/SelfHostedIter.h"    // SelfHostedIter
#include "frontend/TDZCheckCache.h"     // TDZCheckCache

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

class MOZ_STACK_CLASS ForOfEmitter {
  BytecodeEmitter* bce_;

#ifdef DEBUG
  int32_t loopDepth_ = 0;
#endif

  SelfHostedIter selfHostedIter_;
  IteratorKind iterKind_;

  mozilla::Maybe<ForOfLoopControl> loopInfo_;

  const EmitterScope* headLexicalEmitterScope_;

  mozilla::Maybe<TDZCheckCache> tdzCacheForIteratedValue_;

#ifdef DEBUG
  enum class State {
    Start,

    Iterated,

    Initialize,

    Body,

    End
  };
  State state_ = State::Start;
#endif

 public:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  enum class HeadUsingDeclarationKind { None, Sync, Async };

 private:
  HeadUsingDeclarationKind usingDeclarationInHead_ =
      HeadUsingDeclarationKind::None;

 public:
#endif

  ForOfEmitter(BytecodeEmitter* bce,
               const EmitterScope* headLexicalEmitterScope,
               SelfHostedIter selfHostedIter, IteratorKind iterKind
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
               ,
               HeadUsingDeclarationKind usingDeclarationInHead
#endif
  );

  [[nodiscard]] bool emitIterated();
  [[nodiscard]] bool emitInitialize(uint32_t forPos);
  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitEnd(uint32_t iteratedPos);
};

} 
} 

#endif /* frontend_ForOfEmitter_h */
