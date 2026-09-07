/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SwitchEmitter_h
#define frontend_SwitchEmitter_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stddef.h>  // size_t
#include <stdint.h>  // int32_t, uint32_t

#include "frontend/BytecodeControlStructures.h"  // BreakableControl
#include "frontend/EmitterScope.h"               // EmitterScope
#include "frontend/JumpList.h"                   // JumpList, JumpTarget
#include "frontend/TDZCheckCache.h"              // TDZCheckCache
#include "js/AllocPolicy.h"                      // SystemAllocPolicy
#include "js/Value.h"                            // JSVAL_INT_MAX, JSVAL_INT_MIN
#include "js/Vector.h"                           // Vector
#include "vm/Scope.h"                            // LexicalScope

namespace js {
namespace frontend {

struct BytecodeEmitter;

class MOZ_STACK_CLASS SwitchEmitter {

 public:
  enum class Kind { Table, Cond };

  class MOZ_STACK_CLASS TableGenerator {
    BytecodeEmitter* bce_;

    using BitArray = ExternalBitArray<size_t>;
    mozilla::Maybe<js::Vector<size_t, 128, SystemAllocPolicy>> intmap_;

    int32_t intmapBitLength_ = 0;

    uint32_t tableLength_ = 0;

    int32_t low_ = JSVAL_INT_MAX, high_ = JSVAL_INT_MIN;

    bool valid_ = true;

#ifdef DEBUG
    bool finished_ = false;
#endif

   public:
    explicit TableGenerator(BytecodeEmitter* bce) : bce_(bce) {}

    void setInvalid() { valid_ = false; }
    [[nodiscard]] bool isValid() const { return valid_; }
    [[nodiscard]] bool isInvalid() const { return !valid_; }

    [[nodiscard]] bool addNumber(int32_t caseValue);

    void finish(uint32_t caseCount);

   private:
    friend SwitchEmitter;


    int32_t low() const {
      MOZ_ASSERT(finished_);
      return low_;
    }

    int32_t high() const {
      MOZ_ASSERT(finished_);
      return high_;
    }

    uint32_t toCaseIndex(int32_t caseValue) const;

    uint32_t tableLength() const;
  };

 private:
  BytecodeEmitter* bce_;

  Kind kind_ = Kind::Cond;

  bool hasDefault_ = false;

  uint32_t caseCount_ = 0;

  uint32_t caseIndex_ = 0;

  BytecodeOffset top_;

  BytecodeOffset lastCaseOffset_;

  JumpTarget defaultJumpTargetOffset_;

  JumpList condSwitchDefaultOffset_;

  mozilla::Maybe<TDZCheckCache> tdzCacheLexical_;
  mozilla::Maybe<EmitterScope> emitterScope_;

  mozilla::Maybe<TDZCheckCache> tdzCacheCaseAndBody_;

  mozilla::Maybe<BreakableControl> controlInfo_;

  uint32_t switchPos_ = 0;

  js::Vector<BytecodeOffset, 32, SystemAllocPolicy> caseOffsets_;

 protected:
  enum class State {
    Start,

    Discriminant,

    CaseCount,

    Lexical,

    Cond,

    Table,

    CaseValue,

    Case,

    CaseBody,

    DefaultBody,

    End
  };
  State state_ = State::Start;

 public:
  explicit SwitchEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool emitDiscriminant(uint32_t switchPos);

  [[nodiscard]] bool validateCaseCount(uint32_t caseCount);

  [[nodiscard]] bool emitLexical(LexicalScope::ParserData* bindings);

  [[nodiscard]] bool emitCond();
  [[nodiscard]] bool emitTable(const TableGenerator& tableGen);

  [[nodiscard]] bool prepareForCaseValue();
  [[nodiscard]] bool emitCaseJump();

  [[nodiscard]] bool emitCaseBody();
  [[nodiscard]] bool emitCaseBody(int32_t caseValue,
                                  const TableGenerator& tableGen);
  [[nodiscard]] bool emitDefaultBody();
  [[nodiscard]] bool emitEnd();

 private:
  [[nodiscard]] bool emitCaseOrDefaultJump(uint32_t caseIndex, bool isDefault);
  [[nodiscard]] bool emitImplicitDefault();
};

class MOZ_STACK_CLASS InternalSwitchEmitter : public SwitchEmitter {
 public:
  explicit InternalSwitchEmitter(BytecodeEmitter* bce);
};

} 
} 

#endif /* frontend_SwitchEmitter_h */
