/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_LexicalScopeEmitter_h
#define frontend_LexicalScopeEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // Maybe

#include "frontend/EmitterScope.h"   // EmitterScope
#include "frontend/TDZCheckCache.h"  // TDZCheckCache
#include "vm/Scope.h"                // ScopeKind, LexicalScope

namespace js {
namespace frontend {

struct BytecodeEmitter;

class MOZ_STACK_CLASS LexicalScopeEmitter {
  BytecodeEmitter* bce_;

  mozilla::Maybe<TDZCheckCache> tdzCache_;
  mozilla::Maybe<EmitterScope> emitterScope_;

#ifdef DEBUG
  enum class State {
    Start,

    Scope,

    End,
  };
  State state_ = State::Start;
#endif

 public:
  explicit LexicalScopeEmitter(BytecodeEmitter* bce);

  const EmitterScope& emitterScope() const { return *emitterScope_; }

  [[nodiscard]] bool emitScope(ScopeKind kind,
                               LexicalScope::ParserData* bindings
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                               ,
                               BlockKind blockKind = BlockKind::Other
#endif
  );
  [[nodiscard]] bool emitEmptyScope();

  [[nodiscard]] bool emitEnd();
};

} 
} 

#endif /* frontend_LexicalScopeEmitter_h */
