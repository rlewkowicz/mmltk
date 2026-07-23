/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForOfLoopControl_h
#define frontend_ForOfLoopControl_h

#include "mozilla/Maybe.h"  // mozilla::Maybe

#include <stdint.h>  // int32_t, uint32_t

#include "frontend/BytecodeControlStructures.h"  // NestableControl, LoopControl
#include "frontend/IteratorKind.h"               // IteratorKind
#include "frontend/SelfHostedIter.h"             // SelfHostedIter
#include "frontend/TryEmitter.h"                 // TryEmitter
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
#  include "frontend/UsingEmitter.h"  // ForOfDisposalEmitter
#endif
#include "vm/CompletionKind.h"  // CompletionKind

namespace js {
namespace frontend {

struct BytecodeEmitter;
class BytecodeOffset;
class EmitterScope;

class ForOfLoopControl : public LoopControl {
  int32_t iterDepth_;

  mozilla::Maybe<TryEmitter> tryCatch_;

  uint32_t numYieldsAtBeginCodeNeedingIterClose_;

  SelfHostedIter selfHostedIter_;

  IteratorKind iterKind_;

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  mozilla::Maybe<ForOfDisposalEmitter> forOfDisposalEmitter_;
#endif

 public:
  ForOfLoopControl(BytecodeEmitter* bce, int32_t iterDepth,
                   SelfHostedIter selfHostedIter, IteratorKind iterKind);

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  [[nodiscard]] bool prepareForForOfLoopIteration(
      BytecodeEmitter* bce, const EmitterScope* headLexicalEmitterScope,
      bool hasAwaitUsing);
#endif

  [[nodiscard]] bool emitBeginCodeNeedingIteratorClose(BytecodeEmitter* bce);
  [[nodiscard]] bool emitEndCodeNeedingIteratorClose(BytecodeEmitter* bce);

  [[nodiscard]] bool emitIteratorCloseInInnermostScopeWithTryNote(
      BytecodeEmitter* bce, CompletionKind completionKind);
  [[nodiscard]] bool emitIteratorCloseInScope(BytecodeEmitter* bce,
                                              EmitterScope& currentScope,
                                              CompletionKind completionKind);

  [[nodiscard]] bool emitPrepareForNonLocalJumpFromScope(
      BytecodeEmitter* bce, EmitterScope& currentScope, bool isTarget,
      BytecodeOffset* tryNoteStart);
};
template <>
inline bool NestableControl::is<ForOfLoopControl>() const {
  return kind_ == StatementKind::ForOfLoop;
}

} 
} 

#endif /* frontend_ForOfLoopControl_h */
