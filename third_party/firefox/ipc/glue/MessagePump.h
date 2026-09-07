/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(IPC_GLUE_MESSAGEPUMP_H_)
#define IPC_GLUE_MESSAGEPUMP_H_

#include "base/message_pump_default.h"

#include "base/time.h"
#include "mozilla/Mutex.h"
#include "nsCOMPtr.h"
#include "nsIThreadInternal.h"

class nsIEventTarget;
class nsITimer;

namespace mozilla {
namespace ipc {

class DoWorkRunnable;

class MessagePump : public base::MessagePumpDefault {
  friend class DoWorkRunnable;

 public:
  explicit MessagePump(nsISerialEventTarget* aEventTarget);

  virtual void Run(base::MessagePump::Delegate* aDelegate) override;

  virtual void ScheduleWork() override;

  virtual void ScheduleWorkForNestedLoop() override;

  virtual void ScheduleDelayedWork(
      const base::TimeTicks& aDelayedWorkTime) override;

  virtual nsISerialEventTarget* GetXPCOMThread() override;

 protected:
  virtual ~MessagePump();

 private:
  void DoDelayedWork(base::MessagePump::Delegate* aDelegate);

 protected:
  nsISerialEventTarget* mEventTarget;

  nsCOMPtr<nsITimer> mDelayedWorkTimer;

 private:
  RefPtr<DoWorkRunnable> mDoWorkEvent;
};

class MessagePumpForChildProcess final : public MessagePump {
 public:
  MessagePumpForChildProcess() : MessagePump(nullptr), mFirstRun(true) {}

  virtual void Run(base::MessagePump::Delegate* aDelegate) override;

 private:
  ~MessagePumpForChildProcess() = default;

  bool mFirstRun;
};

class MessagePumpForNonMainThreads final : public MessagePump {
 public:
  explicit MessagePumpForNonMainThreads(nsISerialEventTarget* aEventTarget)
      : MessagePump(aEventTarget) {}

  virtual void Run(base::MessagePump::Delegate* aDelegate) override;

 private:
  ~MessagePumpForNonMainThreads() = default;
};



} 
} 

#endif
