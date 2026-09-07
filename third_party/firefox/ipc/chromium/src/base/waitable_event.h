// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_WAITABLE_EVENT_H_)
#define BASE_WAITABLE_EVENT_H_

#include "base/basictypes.h"


#if defined(XP_UNIX)
#  include <list>
#  include <utility>
#  include "nsISupportsImpl.h"
#endif

#include "base/message_loop.h"

namespace base {

static const int kNoTimeout = -1;

class TimeDelta;

class WaitableEvent {
 public:
  WaitableEvent(bool manual_reset, bool initially_signaled);

  ~WaitableEvent();

  void Reset();

  void Signal();

  bool IsSignaled();

  bool Wait();

  bool TimedWait(const TimeDelta& max_time);


  static size_t WaitMany(WaitableEvent** waitables, size_t count);


  class Waiter {
   public:
    virtual bool Fire(WaitableEvent* signaling_event) = 0;

    virtual bool Compare(void* tag) = 0;
  };

 private:
  friend class WaitableEventWatcher;

  struct WaitableEventKernel final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WaitableEventKernel)
    WaitableEventKernel(bool manual_reset, bool initially_signaled)
        : lock_("WaitableEventKernel"),
          manual_reset_(manual_reset),
          signaled_(initially_signaled) {}

    bool Dequeue(Waiter* waiter, void* tag);

    mozilla::Mutex lock_;
    const bool manual_reset_;
    bool signaled_;
    std::list<Waiter*> waiters_;

   protected:
    ~WaitableEventKernel() {}
  };

  RefPtr<WaitableEventKernel> kernel_;

  bool SignalAll();
  bool SignalOne();
  void Enqueue(Waiter* waiter);

  typedef std::pair<WaitableEvent*, size_t> WaiterAndIndex;
  static size_t EnqueueMany(WaiterAndIndex* waitables, size_t count,
                            Waiter* waiter);

  DISALLOW_COPY_AND_ASSIGN(WaitableEvent);
};

}  

#endif
