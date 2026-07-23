/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_NoExecute_h
#define debugger_NoExecute_h

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Attributes.h"  // for MOZ_RAII

#include "NamespaceImports.h"  // for HandleScript
#include "js/Promise.h"        // for JS::AutoDebuggerJobQueueInterruption

namespace js {

class Debugger;
class LeaveDebuggeeNoExecute;

class MOZ_RAII EnterDebuggeeNoExecute {
  friend class LeaveDebuggeeNoExecute;

  Debugger& dbg_;
  EnterDebuggeeNoExecute** stack_;
  EnterDebuggeeNoExecute* prev_;

  LeaveDebuggeeNoExecute* unlocked_;

  bool reported_;

 public:
  explicit EnterDebuggeeNoExecute(
      JSContext* cx, Debugger& dbg,
      const JS::AutoDebuggerJobQueueInterruption& adjqiProof);

  ~EnterDebuggeeNoExecute() {
    MOZ_ASSERT(*stack_ == this);
    *stack_ = prev_;
  }

  Debugger& debugger() const { return dbg_; }

#ifdef DEBUG
  static bool isLockedInStack(JSContext* cx, Debugger& dbg);
#endif

  static EnterDebuggeeNoExecute* findInStack(JSContext* cx);

  static bool reportIfFoundInStack(JSContext* cx, HandleScript script);
};

class MOZ_RAII LeaveDebuggeeNoExecute {
  EnterDebuggeeNoExecute* prevLocked_;

 public:
  explicit LeaveDebuggeeNoExecute(JSContext* cx)
      : prevLocked_(EnterDebuggeeNoExecute::findInStack(cx)) {
    if (prevLocked_) {
      MOZ_ASSERT(!prevLocked_->unlocked_);
      prevLocked_->unlocked_ = this;
    }
  }

  ~LeaveDebuggeeNoExecute() {
    if (prevLocked_) {
      MOZ_ASSERT(prevLocked_->unlocked_ == this);
      prevLocked_->unlocked_ = nullptr;
    }
  }
};

} 

#endif /* debugger_NoExecute_h */
