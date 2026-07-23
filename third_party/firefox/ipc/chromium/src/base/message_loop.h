// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_MESSAGE_LOOP_H_)
#define BASE_MESSAGE_LOOP_H_

#include <queue>
#include <string>

#include "base/message_pump.h"
#include "base/observer_list.h"

#include "mozilla/Mutex.h"

#  include "base/message_pump_libevent.h"

#include "nsCOMPtr.h"
#include "nsIRunnable.h"
#include "nsThreadUtils.h"

class nsISerialEventTarget;

namespace mozilla {
namespace ipc {

class DoWorkRunnable;

} 
} 

class MessageLoop : public base::MessagePump::Delegate {
  friend class mozilla::ipc::DoWorkRunnable;

 public:
  class DestructionObserver {
   public:
    virtual ~DestructionObserver() = default;
    virtual void WillDestroyCurrentMessageLoop() = 0;
  };

  void AddDestructionObserver(DestructionObserver* destruction_observer);

  void RemoveDestructionObserver(DestructionObserver* destruction_observer);


  bool IsAcceptingTasks() const { return !shutting_down_; }

  void PostTask(already_AddRefed<nsIRunnable> task);

  void PostDelayedTask(already_AddRefed<nsIRunnable> task, int delay_ms);

  void PostIdleTask(already_AddRefed<nsIRunnable> task);

  void Run();

  void Quit();

  class QuitTask : public mozilla::Runnable {
   public:
    QuitTask() : mozilla::Runnable("QuitTask") {}
    NS_IMETHOD Run() override {
      MessageLoop::current()->Quit();
      return NS_OK;
    }
  };

  nsISerialEventTarget* SerialEventTarget();

  enum Type {
    TYPE_DEFAULT,
    TYPE_UI,
    TYPE_IO,
    TYPE_MOZILLA_CHILD,
    TYPE_MOZILLA_PARENT,
    TYPE_MOZILLA_NONMAINTHREAD,
    TYPE_MOZILLA_NONMAINUITHREAD,
    TYPE_MOZILLA_ANDROID_UI
  };

  explicit MessageLoop(Type type = TYPE_DEFAULT,
                       nsISerialEventTarget* aEventTarget = nullptr);
  ~MessageLoop();

  Type type() const { return type_; }

  int32_t id() const { return id_; }

  void set_thread_name(const std::string& aThreadName) {
    DCHECK(thread_name_.empty()) << "Should not rename this thread!";
    thread_name_ = aThreadName;
  }
  const std::string& thread_name() const { return thread_name_; }

  static MessageLoop* current();

  static void set_current(MessageLoop* loop);

  void SetNestableTasksAllowed(bool allowed);
  void ScheduleWork();
  bool NestableTasksAllowed() const;

  void set_exception_restoration(bool restore) {
    exception_restoration_ = restore;
  }


  void set_hang_timeouts(uint32_t transient_timeout_ms,
                         uint32_t permanent_timeout_ms) {
    transient_hang_timeout_ = transient_timeout_ms;
    permanent_hang_timeout_ = permanent_timeout_ms;
  }
  uint32_t transient_hang_timeout() const { return transient_hang_timeout_; }
  uint32_t permanent_hang_timeout() const { return permanent_hang_timeout_; }

 protected:
  struct RunState {
    int run_depth;

    bool quit_received;

  };

  class AutoRunState : RunState {
   public:
    explicit AutoRunState(MessageLoop* loop);
    ~AutoRunState();

   private:
    MessageLoop* loop_;
    RunState* previous_state_;
  };

  struct PendingTask {
    nsCOMPtr<nsIRunnable> task;        
    base::TimeTicks delayed_run_time;  
    int sequence_num;                  
    bool nestable;                     

    PendingTask(already_AddRefed<nsIRunnable> aTask, bool aNestable)
        : task(aTask), sequence_num(0), nestable(aNestable) {}

    PendingTask(PendingTask&& aOther)
        : task(std::move(aOther.task)),
          delayed_run_time(aOther.delayed_run_time),
          sequence_num(aOther.sequence_num),
          nestable(aOther.nestable) {}

    PendingTask(const PendingTask& aOther) = default;
    PendingTask& operator=(const PendingTask& aOther) {
      task = aOther.task;
      delayed_run_time = aOther.delayed_run_time;
      sequence_num = aOther.sequence_num;
      nestable = aOther.nestable;
      return *this;
    }

    bool operator<(const PendingTask& other) const;
  };

  typedef std::queue<PendingTask> TaskQueue;
  typedef std::priority_queue<PendingTask> DelayedTaskQueue;

  base::MessagePumpLibevent* pump_libevent() {
    return static_cast<base::MessagePumpLibevent*>(pump_.get());
  }

  void RunHandler();

  void RunInternal();

  bool ProcessNextDelayedNonNestableTask();

  bool QueueOrRunTask(already_AddRefed<nsIRunnable> new_task);

  void RunTask(already_AddRefed<nsIRunnable> task);

  bool DeferOrRunPendingTask(PendingTask&& pending_task);

  void AddToDelayedWorkQueue(const PendingTask& pending_task);

  void ReloadWorkQueue();

  bool DeletePendingTasks();

  void PostTask_Helper(already_AddRefed<nsIRunnable> task, int delay_ms);

  virtual bool DoWork() override;
  virtual bool DoDelayedWork(base::TimeTicks* next_delayed_work_time) override;
  virtual bool DoIdleWork() override;

  Type type_;
  int32_t id_;

  TaskQueue work_queue_;

  DelayedTaskQueue delayed_work_queue_;

  TaskQueue deferred_non_nestable_work_queue_;

  RefPtr<base::MessagePump> pump_;

  base::ObserverList<DestructionObserver> destruction_observers_;

  bool nestable_tasks_allowed_;

  bool exception_restoration_;

  std::string thread_name_;

  TaskQueue incoming_queue_ MOZ_GUARDED_BY(incoming_queue_lock_);
  mozilla::Mutex incoming_queue_lock_;

  RunState* state_;
  int run_depth_base_;
  bool shutting_down_;


  uint32_t transient_hang_timeout_;
  uint32_t permanent_hang_timeout_;

  int next_sequence_num_;

  class EventTarget;
  RefPtr<EventTarget> mEventTarget;

  DISALLOW_COPY_AND_ASSIGN(MessageLoop);
};

class MessageLoopForUI : public MessageLoop {
 public:
  explicit MessageLoopForUI(Type aType = TYPE_UI) : MessageLoop(aType) {}

  static MessageLoopForUI* current() {
    MessageLoop* loop = MessageLoop::current();
    if (!loop) return nullptr;
    Type type = loop->type();
    DCHECK(type == MessageLoop::TYPE_UI ||
           type == MessageLoop::TYPE_MOZILLA_PARENT ||
           type == MessageLoop::TYPE_MOZILLA_CHILD);
    return static_cast<MessageLoopForUI*>(loop);
  }

};

COMPILE_ASSERT(sizeof(MessageLoop) == sizeof(MessageLoopForUI),
               MessageLoopForUI_should_not_have_extra_member_variables);

class MessageLoopForIO : public MessageLoop {
 public:
  MessageLoopForIO() : MessageLoop(TYPE_IO) {}

  static MessageLoopForIO* current() {
    MessageLoop* loop = MessageLoop::current();
    DCHECK_EQ(MessageLoop::TYPE_IO, loop->type());
    return static_cast<MessageLoopForIO*>(loop);
  }

  typedef base::MessagePumpLibevent::Watcher Watcher;
  typedef base::MessagePumpLibevent::FileDescriptorWatcher
      FileDescriptorWatcher;

  enum Mode {
    WATCH_READ = base::MessagePumpLibevent::WATCH_READ,
    WATCH_WRITE = base::MessagePumpLibevent::WATCH_WRITE,
    WATCH_READ_WRITE = base::MessagePumpLibevent::WATCH_READ_WRITE
  };

  bool WatchFileDescriptor(int fd, bool persistent, Mode mode,
                           FileDescriptorWatcher* controller,
                           Watcher* delegate);
};

COMPILE_ASSERT(sizeof(MessageLoop) == sizeof(MessageLoopForIO),
               MessageLoopForIO_should_not_have_extra_member_variables);

#endif
