// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_MESSAGE_PUMP_H_)
#define BASE_MESSAGE_PUMP_H_

#include "nsISupportsImpl.h"

class nsISerialEventTarget;

namespace base {

class TimeTicks;

class MessagePump {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MessagePump)

  class Delegate {
   public:
    virtual ~Delegate() {}

    virtual bool DoWork() = 0;

    virtual bool DoDelayedWork(TimeTicks* next_delayed_work_time) = 0;

    virtual bool DoIdleWork() = 0;
  };

  virtual void Run(Delegate* delegate) = 0;

  virtual void Quit() = 0;

  virtual void ScheduleWork() = 0;

  virtual void ScheduleWorkForNestedLoop() { ScheduleWork(); };

  virtual void ScheduleDelayedWork(const TimeTicks& delayed_work_time) = 0;

  virtual nsISerialEventTarget* GetXPCOMThread() { return nullptr; }

 protected:
  virtual ~MessagePump() {};
};

}  

#endif
