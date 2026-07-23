// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/message_loop.h"

#include <algorithm>

#include "base/logging.h"
#include "base/message_pump_default.h"
#include "base/string_util.h"
#include "base/thread_local.h"
#include "mozilla/Atomics.h"
#include "mozilla/MaybeLeakRefPtr.h"
#include "mozilla/Mutex.h"
#include "mozilla/TargetShutdownTaskSet.h"
#include "nsIEventTarget.h"
#include "nsITargetShutdownTask.h"
#include "nsThreadUtils.h"

#if defined(XP_UNIX) && !0
#  include "base/message_pump_libevent.h"
#endif
#if defined(XP_LINUX) || 0 || 0 || \
    0 || 0
#if defined(MOZ_WIDGET_GTK)
#    include "base/message_pump_glib.h"
#endif
#endif
#include "nsISerialEventTarget.h"

#include "mozilla/ipc/MessagePump.h"
#include "nsThreadUtils.h"

using base::Time;
using base::TimeDelta;
using base::TimeTicks;

using mozilla::Runnable;

static base::ThreadLocalPointer<MessageLoop>& get_tls_ptr() {
  static base::ThreadLocalPointer<MessageLoop> tls_ptr;
  return tls_ptr;
}




class MessageLoop::EventTarget : public nsISerialEventTarget,
                                 public nsITargetShutdownTask,
                                 public MessageLoop::DestructionObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIEVENTTARGET_FULL

  void TargetShutdown() override {
    TargetShutdownTaskSet::TasksArray shutdownTasks;
    {
      mozilla::MutexAutoLock lock(mMutex);
      if (mShutdownTasksRun) {
        return;
      }
      shutdownTasks = mShutdownTasks.Extract();
    }
    for (const auto& task : shutdownTasks) {
      task->TargetShutdown();
    }
  }

  explicit EventTarget(MessageLoop* aLoop)
      : mMutex("MessageLoop::EventTarget"), mLoop(aLoop) {
    aLoop->AddDestructionObserver(this);
  }

 private:
  virtual ~EventTarget() {
    if (mLoop) {
      mLoop->RemoveDestructionObserver(this);
    }
  }

  void WillDestroyCurrentMessageLoop() override {
    {
      mozilla::MutexAutoLock lock(mMutex);
      mLoop->RemoveDestructionObserver(this);
      mLoop = nullptr;
    }

    TargetShutdown();
  }

  mozilla::Mutex mMutex;
  bool mShutdownTasksRun MOZ_GUARDED_BY(mMutex) = false;
  TargetShutdownTaskSet mShutdownTasks MOZ_GUARDED_BY(mMutex);
  MessageLoop* mLoop MOZ_GUARDED_BY(mMutex);
};

NS_IMPL_ISUPPORTS(MessageLoop::EventTarget, nsIEventTarget,
                  nsISerialEventTarget)

NS_IMETHODIMP_(bool)
MessageLoop::EventTarget::IsOnCurrentThreadInfallible() {
  mozilla::MutexAutoLock lock(mMutex);
  return mLoop == MessageLoop::current();
}

NS_IMETHODIMP
MessageLoop::EventTarget::IsOnCurrentThread(bool* aResult) {
  *aResult = IsOnCurrentThreadInfallible();
  return NS_OK;
}

NS_IMETHODIMP
MessageLoop::EventTarget::DispatchFromScript(nsIRunnable* aEvent,
                                             DispatchFlags aFlags) {
  return Dispatch(do_AddRef(aEvent), aFlags);
}

NS_IMETHODIMP
MessageLoop::EventTarget::Dispatch(already_AddRefed<nsIRunnable> aEvent,
                                   DispatchFlags aFlags) {
  mozilla::MaybeLeakRefPtr<nsIRunnable> event(std::move(aEvent),
                                              aFlags & NS_DISPATCH_FALLIBLE);

  mozilla::MutexAutoLock lock(mMutex);
  if (!mLoop) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  mLoop->PostTask(event.forget());
  return NS_OK;
}

NS_IMETHODIMP
MessageLoop::EventTarget::DelayedDispatch(already_AddRefed<nsIRunnable> aEvent,
                                          uint32_t aDelayMs) {
  mozilla::MutexAutoLock lock(mMutex);
  if (!mLoop || mShutdownTasksRun) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  mLoop->PostDelayedTask(std::move(aEvent), aDelayMs);
  return NS_OK;
}

NS_IMETHODIMP
MessageLoop::EventTarget::RegisterShutdownTask(nsITargetShutdownTask* aTask) {
  mozilla::MutexAutoLock lock(mMutex);
  if (!mLoop || mShutdownTasksRun) {
    return NS_ERROR_UNEXPECTED;
  }
  mShutdownTasks.AddTask(aTask);
  return NS_OK;
}

NS_IMETHODIMP
MessageLoop::EventTarget::UnregisterShutdownTask(nsITargetShutdownTask* aTask) {
  mozilla::MutexAutoLock lock(mMutex);
  if (!mLoop) {
    return NS_ERROR_UNEXPECTED;
  }
  return mShutdownTasks.RemoveTask(aTask);
}

nsIEventTarget::FeatureFlags MessageLoop::EventTarget::GetFeatures() {
  return FeatureFlags::SUPPORTS_SHUTDOWN_TASKS;
}


MessageLoop* MessageLoop::current() { return get_tls_ptr().Get(); }

void MessageLoop::set_current(MessageLoop* loop) { get_tls_ptr().Set(loop); }

static mozilla::Atomic<int32_t> message_loop_id_seq(0);

MessageLoop::MessageLoop(Type type, nsISerialEventTarget* aEventTarget)
    : type_(type),
      id_(++message_loop_id_seq),
      nestable_tasks_allowed_(true),
      exception_restoration_(false),
      incoming_queue_lock_("MessageLoop Incoming Queue Lock"),
      state_(nullptr),
      run_depth_base_(1),
      shutting_down_(false),
      transient_hang_timeout_(0),
      permanent_hang_timeout_(0),
      next_sequence_num_(0) {
  DCHECK(!current()) << "should only have one message loop per thread";
  get_tls_ptr().Set(this);

  mEventTarget = new EventTarget(this);

  switch (type_) {
    case TYPE_MOZILLA_PARENT:
      MOZ_RELEASE_ASSERT(!aEventTarget);
      pump_ = new mozilla::ipc::MessagePump(aEventTarget);
      return;
    case TYPE_MOZILLA_CHILD:
      MOZ_RELEASE_ASSERT(!aEventTarget);
      pump_ = new mozilla::ipc::MessagePumpForChildProcess();
      run_depth_base_ = 2;
      return;
    case TYPE_MOZILLA_NONMAINTHREAD:
      pump_ = new mozilla::ipc::MessagePumpForNonMainThreads(aEventTarget);
      return;
    default:
      break;
  }

  if (type_ == TYPE_UI) {
#if defined(XP_LINUX) || 0 || 0 || \
      0 || 0
    pump_ = new base::MessagePumpForUI();
#endif
  } else if (type_ == TYPE_IO) {
    pump_ = new base::MessagePumpLibevent();
  } else {
    pump_ = new base::MessagePumpDefault();
  }

  if (nsISerialEventTarget* thread = pump_->GetXPCOMThread()) {
    MOZ_ALWAYS_SUCCEEDS(thread->RegisterShutdownTask(mEventTarget));
  } else {
    mozilla::SerialEventTargetGuard::Set(mEventTarget);
  }
}

MessageLoop::~MessageLoop() {
  DCHECK(this == current());

  FOR_EACH_OBSERVER(DestructionObserver, destruction_observers_,
                    WillDestroyCurrentMessageLoop());

  DCHECK(!state_);

  bool did_work;
  for (int i = 0; i < 100; ++i) {
    DeletePendingTasks();
    ReloadWorkQueue();
    did_work = DeletePendingTasks();
    if (!did_work) break;
  }
  DCHECK(!did_work);

  get_tls_ptr().Set(nullptr);
}

void MessageLoop::AddDestructionObserver(DestructionObserver* obs) {
  DCHECK(this == current());
  destruction_observers_.AddObserver(obs);
}

void MessageLoop::RemoveDestructionObserver(DestructionObserver* obs) {
  DCHECK(this == current());
  destruction_observers_.RemoveObserver(obs);
}

void MessageLoop::Run() {
  AutoRunState save_state(this);
  RunHandler();
}

void MessageLoop::RunHandler() {

  RunInternal();
}


void MessageLoop::RunInternal() {
  DCHECK(this == current());
  pump_->Run(this);
}


bool MessageLoop::ProcessNextDelayedNonNestableTask() {
  if (state_->run_depth > run_depth_base_) return false;

  if (deferred_non_nestable_work_queue_.empty()) return false;

  nsCOMPtr<nsIRunnable> task =
      std::move(deferred_non_nestable_work_queue_.front().task);
  deferred_non_nestable_work_queue_.pop();

  RunTask(task.forget());
  return true;
}


void MessageLoop::Quit() {
  DCHECK(current() == this);
  if (state_) {
    state_->quit_received = true;
  } else {
    NOTREACHED() << "Must be inside Run to call Quit";
  }
}

void MessageLoop::PostTask(already_AddRefed<nsIRunnable> task) {
  PostTask_Helper(std::move(task), 0);
}

void MessageLoop::PostDelayedTask(already_AddRefed<nsIRunnable> task,
                                  int delay_ms) {
  PostTask_Helper(std::move(task), delay_ms);
}

void MessageLoop::PostIdleTask(already_AddRefed<nsIRunnable> task) {
  DCHECK(current() == this);
  MOZ_ASSERT(NS_IsMainThread());

  PendingTask pending_task(std::move(task), false);
  mozilla::LogRunnable::LogDispatch(pending_task.task.get());
  deferred_non_nestable_work_queue_.push(std::move(pending_task));
}

void MessageLoop::PostTask_Helper(already_AddRefed<nsIRunnable> task,
                                  int delay_ms) {
  if (nsISerialEventTarget* target = pump_->GetXPCOMThread()) {
    nsresult rv;
    if (delay_ms) {
      rv = target->DelayedDispatch(std::move(task), delay_ms);
    } else {
      rv = target->Dispatch(std::move(task), NS_DISPATCH_NORMAL);
    }
    MOZ_ALWAYS_SUCCEEDS(rv);
    return;
  }

  MOZ_ASSERT(!shutting_down_);

  PendingTask pending_task(std::move(task), true);

  if (delay_ms > 0) {
    pending_task.delayed_run_time =
        TimeTicks::Now() + TimeDelta::FromMilliseconds(delay_ms);
  } else {
    DCHECK(delay_ms == 0) << "delay should not be negative";
  }


  RefPtr<base::MessagePump> pump;
  {
    mozilla::MutexAutoLock locked(incoming_queue_lock_);
    mozilla::LogRunnable::LogDispatch(pending_task.task.get());
    incoming_queue_.push(std::move(pending_task));
    pump = pump_;
  }

  pump->ScheduleWork();
}

void MessageLoop::SetNestableTasksAllowed(bool allowed) {
  if (nestable_tasks_allowed_ != allowed) {
    nestable_tasks_allowed_ = allowed;
    if (!nestable_tasks_allowed_) return;
    pump_->ScheduleWorkForNestedLoop();
  }
}

void MessageLoop::ScheduleWork() {
  pump_->ScheduleWork();
}

bool MessageLoop::NestableTasksAllowed() const {
  return nestable_tasks_allowed_;
}


void MessageLoop::RunTask(already_AddRefed<nsIRunnable> aTask) {
  DCHECK(nestable_tasks_allowed_);
  nestable_tasks_allowed_ = false;

  nsCOMPtr<nsIRunnable> task = aTask;

  {
    mozilla::LogRunnable::Run log(task.get());
    task->Run();
    task = nullptr;
  }

  nestable_tasks_allowed_ = true;
}

bool MessageLoop::DeferOrRunPendingTask(PendingTask&& pending_task) {
  if (pending_task.nestable || state_->run_depth <= run_depth_base_) {
    RunTask(pending_task.task.forget());
    return true;
  }

  mozilla::LogRunnable::LogDispatch(pending_task.task.get());
  deferred_non_nestable_work_queue_.push(std::move(pending_task));
  return false;
}

void MessageLoop::AddToDelayedWorkQueue(const PendingTask& pending_task) {
  PendingTask new_pending_task(pending_task);
  new_pending_task.sequence_num = next_sequence_num_++;
  mozilla::LogRunnable::LogDispatch(new_pending_task.task.get());
  delayed_work_queue_.push(std::move(new_pending_task));
}

void MessageLoop::ReloadWorkQueue() {
  if (!work_queue_.empty())
    return;  

  {
    mozilla::MutexAutoLock lock(incoming_queue_lock_);
    if (incoming_queue_.empty()) return;
    std::swap(incoming_queue_, work_queue_);
    DCHECK(incoming_queue_.empty());
  }
}

bool MessageLoop::DeletePendingTasks() {
  MOZ_ASSERT(work_queue_.empty());
  bool did_work = !deferred_non_nestable_work_queue_.empty();
  while (!deferred_non_nestable_work_queue_.empty()) {
    deferred_non_nestable_work_queue_.pop();
  }
  did_work |= !delayed_work_queue_.empty();
  while (!delayed_work_queue_.empty()) {
    delayed_work_queue_.pop();
  }
  return did_work;
}

bool MessageLoop::DoWork() {
  if (!nestable_tasks_allowed_) {
    return false;
  }

  for (;;) {
    ReloadWorkQueue();
    if (work_queue_.empty()) break;

    do {
      PendingTask pending_task = std::move(work_queue_.front());
      work_queue_.pop();
      if (!pending_task.delayed_run_time.is_null()) {
        AddToDelayedWorkQueue(pending_task);
        if (delayed_work_queue_.top().task == pending_task.task)
          pump_->ScheduleDelayedWork(pending_task.delayed_run_time);
      } else {
        if (DeferOrRunPendingTask(std::move(pending_task))) return true;
      }
    } while (!work_queue_.empty());
  }

  return false;
}

bool MessageLoop::DoDelayedWork(TimeTicks* next_delayed_work_time) {
  if (!nestable_tasks_allowed_ || delayed_work_queue_.empty()) {
    *next_delayed_work_time = TimeTicks();
    return false;
  }

  if (delayed_work_queue_.top().delayed_run_time > TimeTicks::Now()) {
    *next_delayed_work_time = delayed_work_queue_.top().delayed_run_time;
    return false;
  }

  PendingTask pending_task = delayed_work_queue_.top();
  delayed_work_queue_.pop();

  if (!delayed_work_queue_.empty())
    *next_delayed_work_time = delayed_work_queue_.top().delayed_run_time;

  return DeferOrRunPendingTask(std::move(pending_task));
}

bool MessageLoop::DoIdleWork() {
  if (ProcessNextDelayedNonNestableTask()) return true;

  if (state_->quit_received) pump_->Quit();

  return false;
}


MessageLoop::AutoRunState::AutoRunState(MessageLoop* loop) : loop_(loop) {
  MOZ_ASSERT(!loop_->shutting_down_);

  previous_state_ = loop_->state_;
  if (previous_state_) {
    run_depth = previous_state_->run_depth + 1;
  } else {
    run_depth = 1;
  }
  loop_->state_ = this;

  quit_received = false;
}

MessageLoop::AutoRunState::~AutoRunState() {
  loop_->state_ = previous_state_;

  loop_->shutting_down_ = !previous_state_;
}


bool MessageLoop::PendingTask::operator<(const PendingTask& other) const {

  if (delayed_run_time < other.delayed_run_time) return false;

  if (delayed_run_time > other.delayed_run_time) return true;

  return (sequence_num - other.sequence_num) > 0;
}


nsISerialEventTarget* MessageLoop::SerialEventTarget() { return mEventTarget; }





bool MessageLoopForIO::WatchFileDescriptor(int fd, bool persistent, Mode mode,
                                           FileDescriptorWatcher* controller,
                                           Watcher* delegate) {
  return pump_libevent()->WatchFileDescriptor(
      fd, persistent, static_cast<base::MessagePumpLibevent::Mode>(mode),
      controller, delegate);
}
