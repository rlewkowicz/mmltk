/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TaskController.h"
#include "IdleTaskRunner.h"
#include "nsIIdleRunnable.h"
#include "nsIRunnable.h"
#include "nsThreadUtils.h"
#include <algorithm>
#include "mozilla/AppShutdown.h"
#include "mozilla/EventQueue.h"
#include "mozilla/Hal.h"
#include "mozilla/InputTaskManager.h"
#include "mozilla/VsyncTaskManager.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/SchedulerGroup.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_memory.h"
#include "nsIThreadInternal.h"
#include "nsThread.h"
#include "prenv.h"
#include "prsystem.h"

namespace mozilla {

StaticAutoPtr<TaskController> TaskController::sSingleton;

std::atomic<uint64_t> Task::sCurrentTaskSeqNo = 0;

const int32_t kMinimumPoolThreadCount = 2;
const int32_t kMaximumPoolThreadCount = 8;

struct PoolThread {
  const size_t mIndex;
  PRThread* mThread = nullptr;

  CondVar mThreadCV;
  RefPtr<Task> mCurrentTask;

  uint32_t mEffectiveTaskPriority = 0;

  PoolThread(size_t aIndex, Mutex& aGraphMutex)
      : mIndex(aIndex), mThreadCV(aGraphMutex, "PoolThread::mThreadCV") {}
};

int32_t TaskController::GetPoolThreadCount() {
  if (PR_GetEnv("MOZ_TASKCONTROLLER_THREADCOUNT")) {
    return strtol(PR_GetEnv("MOZ_TASKCONTROLLER_THREADCOUNT"), nullptr, 0);
  }

  int32_t numCores = 0;
  {
    numCores = std::max<int32_t>(1, PR_GetNumberOfProcessors());
  }

  return std::clamp<int32_t>(numCores, kMinimumPoolThreadCount,
                             kMaximumPoolThreadCount);
}

Task::TaskResult TaskController::RunTask(Task* aTask) { return aTask->Run(); }

bool TaskManager::
    UpdateCachesForCurrentIterationAndReportPriorityModifierChanged(
        const MutexAutoLock& aProofOfLock, IterationType aIterationType) {
  mCurrentSuspended = IsSuspended(aProofOfLock);

  if (aIterationType == IterationType::EVENT_LOOP_TURN && !mCurrentSuspended) {
    int32_t oldModifier = mCurrentPriorityModifier;
    mCurrentPriorityModifier =
        GetPriorityModifierForEventLoopTurn(aProofOfLock);

    if (mCurrentPriorityModifier != oldModifier) {
      return true;
    }
  }
  return false;
}

Task* Task::GetHighestPriorityDependency() {
  Task* currentTask = this;

  while (!currentTask->mDependencies.empty()) {
    auto iter = currentTask->mDependencies.begin();

    while (iter != currentTask->mDependencies.end()) {
      if ((*iter)->mCompleted) {
        auto oldIter = iter;
        iter++;
        currentTask->mDependencies.erase(oldIter);
        continue;
      }

      currentTask = iter->get();
      break;
    }
  }

  return currentTask == this ? nullptr : currentTask;
}

#if defined(MOZ_MEMORY)
static StaticRefPtr<IdleTaskRunner> sIdleMemoryCleanupRunner;
static StaticRefPtr<nsITimer> sIdleMemoryCleanupWantsLater;
static bool sIdleMemoryCleanupWantsLaterScheduled = false;

static const char kEnableLazyPurgePref[] = "memory.lazypurge.enable";
static const char kMaxPurgeDelayPref[] = "memory.lazypurge.maximum_delay";
static const char kMinPurgeBudgetPref[] =
    "memory.lazypurge.minimum_idle_budget";
static const char kMinPurgeReuseGracePref[] =
    "memory.lazypurge.reuse_grace_period";
#endif

void TaskController::Initialize() {
  MOZ_ASSERT(!sSingleton);
  sSingleton = new TaskController();
}

void ThreadFuncPoolThread(void* aData) {
  auto* thread = static_cast<PoolThread*>(aData);
  TaskController::Get()->RunPoolThread(thread);
}

TaskController::TaskController()
    : mGraphMutex("TaskController::mGraphMutex"),
      mMainThreadCV(mGraphMutex, "TaskController::mMainThreadCV"),
#if defined(MOZ_MEMORY)
      mIsLazyPurgeEnabled(false),
#endif
      mRunOutOfMTTasksCounter(0) {
  InputTaskManager::Init();
  VsyncTaskManager::Init();
  mMTProcessingRunnable = NS_NewRunnableFunction(
      "TaskController::ExecutePendingMTTasks()",
      []() { TaskController::Get()->ProcessPendingMTTask(); });
  mMTBlockingProcessingRunnable = NS_NewRunnableFunction(
      "TaskController::ExecutePendingMTTasks()",
      []() { TaskController::Get()->ProcessPendingMTTask(true); });
}

void TaskController::InitializeThreadPool() {
  mPoolInitializationMutex.AssertCurrentThreadOwns();
  MOZ_ASSERT(!mThreadPoolInitialized);
  mThreadPoolInitialized = true;

  int32_t poolSize = GetPoolThreadCount();
  for (int32_t i = 0; i < poolSize; i++) {
    auto thread = MakeUnique<PoolThread>(i, mGraphMutex);
    thread->mThread =
        PR_CreateThread(PR_USER_THREAD, ThreadFuncPoolThread, thread.get(),
                        PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD,
                        PR_JOINABLE_THREAD, nsIThreadManager::LargeStackSize());
    MOZ_RELEASE_ASSERT(thread->mThread,
                       "Failed to create TaskController pool thread");
    mPoolThreads.emplace_back(std::move(thread));
  }

  mIdleThreadCount = mPoolThreads.size();
}

size_t TaskController::GetThreadStackSize() {
  return nsIThreadManager::LargeStackSize();
}

void TaskController::SetPerformanceCounterState(
    PerformanceCounterState* aPerformanceCounterState) {
  mPerformanceCounterState = aPerformanceCounterState;
}

void TaskController::Shutdown() {
  InputTaskManager::Cleanup();
  VsyncTaskManager::Cleanup();
  if (sSingleton) {
    sSingleton->ShutdownThreadPoolInternal();
    sSingleton = nullptr;
  }
  MOZ_ASSERT(!sSingleton);

#if defined(MOZ_MEMORY)
  if (sIdleMemoryCleanupRunner) {
    sIdleMemoryCleanupRunner->Cancel();
    sIdleMemoryCleanupRunner = nullptr;
  }
  if (sIdleMemoryCleanupWantsLater) {
    sIdleMemoryCleanupWantsLater->Cancel();
    sIdleMemoryCleanupWantsLater = nullptr;
    sIdleMemoryCleanupWantsLaterScheduled = false;
  }
#endif
}

void TaskController::ShutdownThreadPoolInternal() {
  {
    MutexAutoLock lock(mGraphMutex);
    mShuttingDown = true;
    for (auto& thread : mPoolThreads) {
      thread->mThreadCV.NotifyAll();
    }
  }
  for (auto& thread : mPoolThreads) {
    PR_JoinThread(thread->mThread);
  }

  MOZ_ASSERT(mIdleThreadCount == mPoolThreads.size());
}

void TaskController::RunPoolThread(PoolThread* aThread) {
  IOInterposer::RegisterCurrentThread();

  nsAutoCString threadName;
  threadName.AppendLiteral("TaskController #");
  threadName.AppendInt(static_cast<int64_t>(aThread->mIndex));

  MutexAutoLock lock(mGraphMutex);
  while (!mShuttingDown) {
    if (!aThread->mCurrentTask) {
      aThread->mThreadCV.Wait();
      continue;
    }

    Task* task = aThread->mCurrentTask;
    bool taskCompleted = false;

    {
      MutexAutoUnlock unlock(mGraphMutex);
      taskCompleted = RunTask(task) == Task::TaskResult::Complete;
    }

    task->mInProgress = false;

    if (!taskCompleted) {
      auto insertion = mThreadableTasks.insert(aThread->mCurrentTask);
      MOZ_ASSERT(insertion.second);
      task->mIterator = insertion.first;
    } else {
      task->mCompleted = true;
#if defined(DEBUG)
      task->mIsInGraph = false;
#endif
      task->mDependencies.clear();
      mMayHaveMainThreadTask = true;
      EnsureMainThreadTasksScheduled();

      MaybeInterruptTask(GetHighestPriorityMTTask(), lock);
    }

    RefPtr<Task> lastTask = aThread->mCurrentTask.forget();
    mIdleThreadCount++;
    MOZ_ASSERT(mIdleThreadCount <= mPoolThreads.size());

    DispatchThreadableTasks(lock);

    {
      MutexAutoUnlock unlock(mGraphMutex);
      lastTask = nullptr;
    }
  }

  MOZ_ASSERT(mThreadableTasks.empty());

  IOInterposer::UnregisterCurrentThread();
}

void TaskController::AddTask(already_AddRefed<Task> aTask) {
  RefPtr<Task> task(aTask);

  if (task->GetKind() == Task::Kind::OffMainThreadOnly) {
    MutexAutoLock lock(mPoolInitializationMutex);
    if (!mThreadPoolInitialized) {
      InitializeThreadPool();
    }
  }

  MutexAutoLock lock(mGraphMutex);

  if (TaskManager* manager = task->GetManager()) {
    if (manager->mTaskCount == 0) {
      mTaskManagers.insert(manager);
    }
    manager->DidQueueTask();

    task->mPriorityModifier = manager->mCurrentPriorityModifier;
  }

#if defined(DEBUG)
  task->mIsInGraph = true;

  for (const RefPtr<Task>& otherTask : task->mDependencies) {
    MOZ_ASSERT(!otherTask->mTaskManager ||
               otherTask->mTaskManager == task->mTaskManager);
  }
#endif

  LogTask::LogDispatch(task);

  std::pair<std::set<RefPtr<Task>, Task::PriorityCompare>::iterator, bool>
      insertion;
  switch (task->GetKind()) {
    case Task::Kind::MainThreadOnly:
      if (task->GetPriority() >=
              static_cast<uint32_t>(EventQueuePriority::Normal) &&
          !mMainThreadTasks.empty()) {
        insertion = std::pair(
            mMainThreadTasks.insert(--mMainThreadTasks.end(), std::move(task)),
            true);
      } else {
        insertion = mMainThreadTasks.insert(std::move(task));
      }
      break;
    case Task::Kind::OffMainThreadOnly:
      insertion = mThreadableTasks.insert(std::move(task));
      break;
  }
  (*insertion.first)->mIterator = insertion.first;
  MOZ_ASSERT(insertion.second);

  MaybeInterruptTask(*insertion.first, lock);
}

void TaskController::DispatchThreadableTasks(
    const MutexAutoLock& aProofOfLock) {
  while (MaybeDispatchOneThreadableTask(aProofOfLock)) {
  }
}

bool TaskController::MaybeDispatchOneThreadableTask(
    const MutexAutoLock& aProofOfLock) {
  if (mThreadableTasks.empty() || mIdleThreadCount == 0) {
    return false;
  }

  auto [task, effetivePriority] = TakeThreadableTaskToRun(aProofOfLock);
  if (!task) {
    return false;
  }

  PoolThread* thread = SelectThread(aProofOfLock);

  MOZ_ASSERT(!thread->mCurrentTask);
  MOZ_ASSERT(mIdleThreadCount != 0);
  thread->mCurrentTask = task;
  thread->mEffectiveTaskPriority = effetivePriority;
  thread->mThreadCV.Notify();
  task->mInProgress = true;
  mIdleThreadCount--;

  return true;
}

TaskController::TaskToRun TaskController::TakeThreadableTaskToRun(
    const MutexAutoLock& aProofOfLock) {
  MOZ_ASSERT(!mThreadableTasks.empty());

  for (const RefPtr<Task>& rootTask : mThreadableTasks) {
    MOZ_ASSERT(!rootTask->mTaskManager);

    Task* task = rootTask;
    while (Task* nextTask = task->GetHighestPriorityDependency()) {
      task = nextTask;
    }

    if (task->GetKind() != Task::Kind::MainThreadOnly && !task->mInProgress) {
      TaskToRun taskToRun{task, rootTask->GetPriority()};
      mThreadableTasks.erase(task->mIterator);
      task->mIterator = mThreadableTasks.end();
      return taskToRun;
    }
  }

  return TaskToRun();
}

PoolThread* TaskController::SelectThread(const MutexAutoLock& aProofOfLock) {
  MOZ_ASSERT(mIdleThreadCount != 0);

  for (auto& thread : mPoolThreads) {
    if (!thread->mCurrentTask) {
      return thread.get();
    }
  }

  MOZ_CRASH("Couldn't find idle thread");
}

void TaskController::WaitForTaskOrMessage() {
  MutexAutoLock lock(mGraphMutex);
  while (!mMayHaveMainThreadTask) {
    mMainThreadCV.Wait();
  }
}

void TaskController::ExecuteNextTaskOnlyMainThread() {
  MOZ_ASSERT(NS_IsMainThread());
  MutexAutoLock lock(mGraphMutex);
  ExecuteNextTaskOnlyMainThreadInternal(lock);
}

void TaskController::ProcessPendingMTTask(bool aMayWait) {
  MOZ_ASSERT(NS_IsMainThread());
  MutexAutoLock lock(mGraphMutex);

  for (;;) {

    mMTTaskRunnableProcessedTask = ExecuteNextTaskOnlyMainThreadInternal(lock);

    if (mMTTaskRunnableProcessedTask || !aMayWait) {
      break;
    }

    {
      mMainThreadCV.Wait();
    }

  }

  if (mMayHaveMainThreadTask) {
    EnsureMainThreadTasksScheduled();
  }
}

void TaskController::ReprioritizeTask(Task* aTask, uint32_t aPriority) {
  MutexAutoLock lock(mGraphMutex);
  std::set<RefPtr<Task>, Task::PriorityCompare>* queue = &mMainThreadTasks;
  if (aTask->GetKind() == Task::Kind::OffMainThreadOnly) {
    queue = &mThreadableTasks;
  }

  MOZ_ASSERT(aTask->mIterator != queue->end());
  queue->erase(aTask->mIterator);

  aTask->mPriority = aPriority;

  auto insertion = queue->insert(aTask);
  MOZ_ASSERT(insertion.second);
  aTask->mIterator = insertion.first;

  MaybeInterruptTask(aTask, lock);
}

class RunnableTask : public Task {
 public:
  RunnableTask(already_AddRefed<nsIRunnable> aRunnable, int32_t aPriority,
               Kind aKind)
      : Task(aKind, aPriority), mRunnable(aRunnable) {}

  virtual TaskResult Run() override {
    mRunnable->Run();
    mRunnable = nullptr;
    return TaskResult::Complete;
  }

  void SetIdleDeadline(TimeStamp aDeadline) override {
    nsCOMPtr<nsIIdleRunnable> idleRunnable = do_QueryInterface(mRunnable);
    if (idleRunnable) {
      idleRunnable->SetDeadline(aDeadline);
    }
  }

  virtual bool GetName(nsACString& aName) override {
    return false;
  }

 private:
  RefPtr<nsIRunnable> mRunnable;
};

void TaskController::DispatchRunnable(already_AddRefed<nsIRunnable> aRunnable,
                                      uint32_t aPriority,
                                      TaskManager* aManager) {
  RefPtr task = MakeRefPtr<RunnableTask>(std::move(aRunnable), aPriority,
                                         Task::Kind::MainThreadOnly);

  task->SetManager(aManager);
  TaskController::Get()->AddTask(task.forget());
}

nsIRunnable* TaskController::GetRunnableForMTTask(bool aReallyWait) {
  MutexAutoLock lock(mGraphMutex);

  while (mMainThreadTasks.empty()) {
    if (!aReallyWait) {
      return nullptr;
    }

    mMainThreadCV.Wait();
  }

  return aReallyWait ? mMTBlockingProcessingRunnable : mMTProcessingRunnable;
}

bool TaskController::HasMainThreadPendingTasks() {
  MOZ_ASSERT(NS_IsMainThread());
  auto resetIdleState = MakeScopeExit([&idleManager = mIdleTaskManager] {
    if (idleManager) {
      idleManager->State().ClearCachedIdleDeadline();
    }
  });

  for (bool considerIdle : {false, true}) {
    if (considerIdle && !mIdleTaskManager) {
      continue;
    }

    MutexAutoLock lock(mGraphMutex);

    if (considerIdle) {
      mIdleTaskManager->State().ForgetPendingTaskGuarantee();
      {
        MutexAutoUnlock unlock(mGraphMutex);
        mIdleTaskManager->State().CachePeekedIdleDeadline(unlock);
      }
    }

    if (mMainThreadTasks.empty()) {
      return false;
    }

    uint64_t totalSuspended = 0;
    for (TaskManager* manager : mTaskManagers) {
      DebugOnly<bool> modifierChanged =
          manager
              ->UpdateCachesForCurrentIterationAndReportPriorityModifierChanged(
                  lock, TaskManager::IterationType::NOT_EVENT_LOOP_TURN);
      MOZ_ASSERT(!modifierChanged);

      MOZ_ASSERT(manager != mIdleTaskManager || manager->mCurrentSuspended ||
                     considerIdle,
                 "Why are idle tasks not suspended here?");

      if (manager->mCurrentSuspended) {
        totalSuspended += manager->mTaskCount;
      }
    }

    if (mMainThreadTasks.size() > totalSuspended) {
      if (mIdleTaskManager && mIdleTaskManager->mTaskCount &&
          !mIdleTaskManager->mCurrentSuspended) {
        MOZ_ASSERT(considerIdle, "Why is mIdleTaskManager not suspended?");
        if (mMainThreadTasks.size() - mIdleTaskManager->mTaskCount <=
            totalSuspended) {
          mIdleTaskManager->State().EnforcePendingTaskGuarantee();
        }
      }
      return true;
    }
  }
  return false;
}

uint64_t TaskController::PendingMainthreadTaskCountIncludingSuspended() {
  MutexAutoLock lock(mGraphMutex);
  return mMainThreadTasks.size();
}

#if defined(MOZ_MEMORY)
void TaskController::UpdateIdleMemoryCleanupPrefs() {
  mIsLazyPurgeEnabled = StaticPrefs::memory_lazypurge_enable();
  moz_enable_deferred_purge(mIsLazyPurgeEnabled);
}

static void PrefChangeCallback(const char* aPrefName, void* aNull) {
  MOZ_ASSERT((0 == strcmp(aPrefName, kEnableLazyPurgePref)) ||
             (0 == strcmp(aPrefName, kMaxPurgeDelayPref)) ||
             (0 == strcmp(aPrefName, kMinPurgeBudgetPref)) ||
             (0 == strcmp(aPrefName, kMinPurgeReuseGracePref)));

  TaskController::Get()->UpdateIdleMemoryCleanupPrefs();
}

void TaskController::SetupIdleMemoryCleanup() {
  Preferences::RegisterCallback(PrefChangeCallback, kEnableLazyPurgePref);
  Preferences::RegisterCallback(PrefChangeCallback, kMaxPurgeDelayPref);
  Preferences::RegisterCallback(PrefChangeCallback, kMinPurgeBudgetPref);
  Preferences::RegisterCallback(PrefChangeCallback, kMinPurgeReuseGracePref);
  TaskController::Get()->UpdateIdleMemoryCleanupPrefs();
}

bool RunIdleMemoryCleanup(TimeStamp aDeadline, uint32_t aWantsLaterDelay);

void CheckIdleMemoryCleanupNeeded(nsITimer* aTimer, void* aClosure);

void CancelIdleMemoryCleanupTimerAndRunner() {
  if (sIdleMemoryCleanupRunner) {
    sIdleMemoryCleanupRunner->Cancel();
    sIdleMemoryCleanupRunner = nullptr;
  }
  if (sIdleMemoryCleanupWantsLaterScheduled) {
    MOZ_ASSERT(sIdleMemoryCleanupWantsLater);
    sIdleMemoryCleanupWantsLater->Cancel();
    sIdleMemoryCleanupWantsLaterScheduled = false;
  }
}

void ScheduleWantsLaterTimer(uint32_t aWantsLaterDelay) {
  if (sIdleMemoryCleanupRunner) {
    sIdleMemoryCleanupRunner->Cancel();
    sIdleMemoryCleanupRunner = nullptr;
  }
  nsresult timerInitOK = NS_OK;
  if (!sIdleMemoryCleanupWantsLater) {
    auto res = NS_NewTimerWithFuncCallback(
        CheckIdleMemoryCleanupNeeded, (void*)"IdleMemoryCleanupWantsLaterCheck",
        aWantsLaterDelay, nsITimer::TYPE_ONE_SHOT_LOW_PRIORITY,
        "IdleMemoryCleanupWantsLaterCheck"_ns);
    if (res.isOk()) {
      sIdleMemoryCleanupWantsLater = res.unwrap().forget();
    } else {
      timerInitOK = res.unwrapErr();
    }
  } else {
    if (sIdleMemoryCleanupWantsLaterScheduled) {
      sIdleMemoryCleanupWantsLater->Cancel();
    }
    timerInitOK = sIdleMemoryCleanupWantsLater->InitWithNamedFuncCallback(
        CheckIdleMemoryCleanupNeeded, (void*)"IdleMemoryCleanupWantsLaterCheck",
        aWantsLaterDelay, nsITimer::TYPE_ONE_SHOT_LOW_PRIORITY,
        "IdleMemoryCleanupWantsLaterCheck"_ns);
  }
  if (NS_SUCCEEDED(timerInitOK)) {
    sIdleMemoryCleanupWantsLaterScheduled = true;
  } else {
    MOZ_ASSERT_UNREACHABLE(
        "ScheduleWantsLaterTimer could not create the timer.");
    jemalloc_free_dirty_pages();
  }
}

void ScheduleIdleMemoryCleanup(uint32_t aWantsLaterDelay) {
  TimeDuration maxPurgeDelay = TimeDuration::FromMilliseconds(
      StaticPrefs::memory_lazypurge_maximum_delay());
  TimeDuration minPurgeBudget = TimeDuration::FromMilliseconds(
      StaticPrefs::memory_lazypurge_minimum_idle_budget());

  CancelIdleMemoryCleanupTimerAndRunner();
  sIdleMemoryCleanupRunner = IdleTaskRunner::Create(
      [aWantsLaterDelay](TimeStamp aDeadline) {
        return RunIdleMemoryCleanup(aDeadline, aWantsLaterDelay);
      },
      "TaskController::IdlePurgeRunner"_ns, TimeDuration(), maxPurgeDelay,
      minPurgeBudget, true, [] { return AppShutdown::IsShutdownImpending(); });
}
}  

namespace mozilla {
void CheckIdleMemoryCleanupNeeded(nsITimer*, void*) {
  uint32_t reuseGracePeriod =
      StaticPrefs::memory_lazypurge_reuse_grace_period();

  uint32_t wantsLaterDelay = reuseGracePeriod * 2;

  MOZ_ASSERT(!sIdleMemoryCleanupRunner ||
             !sIdleMemoryCleanupWantsLaterScheduled);
  auto result =
      moz_may_purge_now( true, reuseGracePeriod, Nothing());
  switch (result) {
    case may_purge_now_result_t::Done:
      if (sIdleMemoryCleanupRunner || sIdleMemoryCleanupWantsLaterScheduled) {
        CancelIdleMemoryCleanupTimerAndRunner();
      }
      break;
    case may_purge_now_result_t::WantsLater:
      ScheduleWantsLaterTimer(wantsLaterDelay);
      break;
    case may_purge_now_result_t::NeedsMore:
      if (!sIdleMemoryCleanupRunner) {
        ScheduleIdleMemoryCleanup(wantsLaterDelay);
      } else {
        MOZ_ASSERT(!sIdleMemoryCleanupWantsLaterScheduled);
      }
      break;
  }
}
}  

namespace mozilla {

bool RunIdleMemoryCleanup(TimeStamp aDeadline, uint32_t aWantsLaterDelay) {
  MOZ_ASSERT(!sIdleMemoryCleanupWantsLaterScheduled);

  uint32_t reuseGracePeriod =
      StaticPrefs::memory_lazypurge_reuse_grace_period();

  may_purge_now_result_t result;
  do {
    result = moz_may_purge_now(
         false, reuseGracePeriod, Some([aDeadline] {
          return aDeadline.IsNull() || TimeStamp::Now() <= aDeadline;
        }));
  } while ((result == may_purge_now_result_t::NeedsMore) &&
           (aDeadline.IsNull() || TimeStamp::Now() <= aDeadline));

  switch (result) {
    case may_purge_now_result_t::Done:
      CancelIdleMemoryCleanupTimerAndRunner();
      break;
    case may_purge_now_result_t::WantsLater:
      ScheduleWantsLaterTimer(aWantsLaterDelay);
      break;
    case may_purge_now_result_t::NeedsMore:
      break;
  }


  return true;
};

void TaskController::MayScheduleIdleMemoryCleanup() {
  if (PendingMainthreadTaskCountIncludingSuspended() > 0) {
    return;
  }
  if (!mIsLazyPurgeEnabled) {
    return;
  }

  if (AppShutdown::IsShutdownImpending()) {
    CancelIdleMemoryCleanupTimerAndRunner();
    return;
  }

  CheckIdleMemoryCleanupNeeded(nullptr, (void*)"MayScheduleIdleMemoryCleanup");
}

void TaskController::RequestIdleMemoryCleanup(StaticString aReason) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mIsLazyPurgeEnabled) {
    jemalloc_free_dirty_pages();
    return;
  }
  if (AppShutdown::IsShutdownImpending()) {
    return;
  }
  CheckIdleMemoryCleanupNeeded(nullptr, (void*)aReason.get());
}
#endif

bool TaskController::ExecuteNextTaskOnlyMainThreadInternal(
    const MutexAutoLock& aProofOfLock) MOZ_REQUIRES(mGraphMutex) {
  MOZ_ASSERT(NS_IsMainThread());
  mGraphMutex.AssertCurrentThreadOwns();
  bool taskRan = false;
  do {
    taskRan = DoExecuteNextTaskOnlyMainThreadInternal(aProofOfLock);
    if (taskRan) {
      if (mIdleTaskManager && mIdleTaskManager->mTaskCount &&
          mIdleTaskManager->IsSuspended(aProofOfLock)) {
        uint32_t activeTasks = mMainThreadTasks.size();
        for (TaskManager* manager : mTaskManagers) {
          if (manager->IsSuspended(aProofOfLock)) {
            activeTasks -= manager->mTaskCount;
          } else {
            break;
          }
        }

        if (!activeTasks) {
          MutexAutoUnlock unlock(mGraphMutex);
          mIdleTaskManager->State().RequestIdleDeadlineIfNeeded(unlock);
        }
      }
      break;
    }

    if (!mIdleTaskManager) {
      break;
    }

    if (mIdleTaskManager->mTaskCount) {
      MutexAutoUnlock unlock(mGraphMutex);
      mIdleTaskManager->State().UpdateCachedIdleDeadline(unlock);
    } else {
      MutexAutoUnlock unlock(mGraphMutex);
      mIdleTaskManager->State().RanOutOfTasks(unlock);
    }

    taskRan = DoExecuteNextTaskOnlyMainThreadInternal(aProofOfLock);
  } while (false);

  if (mIdleTaskManager) {
    mIdleTaskManager->State().ForgetPendingTaskGuarantee();

    if (mMainThreadTasks.empty()) {
      ++mRunOutOfMTTasksCounter;

      MutexAutoUnlock unlock(mGraphMutex);
      mIdleTaskManager->State().RanOutOfTasks(unlock);
    }
  }

  return taskRan;
}

bool TaskController::DoExecuteNextTaskOnlyMainThreadInternal(
    const MutexAutoLock& aProofOfLock) MOZ_REQUIRES(mGraphMutex) {
  mGraphMutex.AssertCurrentThreadOwns();

  nsCOMPtr<nsIThread> mainIThread;
  NS_GetMainThread(getter_AddRefs(mainIThread));

  nsThread* mainThread = static_cast<nsThread*>(mainIThread.get());
  if (mainThread) {
    mainThread->SetRunningEventDelay(TimeDuration(), TimeStamp());
  }

  uint32_t totalSuspended = 0;
  for (TaskManager* manager : mTaskManagers) {
    bool modifierChanged =
        manager
            ->UpdateCachesForCurrentIterationAndReportPriorityModifierChanged(
                aProofOfLock, TaskManager::IterationType::EVENT_LOOP_TURN);
    if (modifierChanged) {
      ProcessUpdatedPriorityModifier(manager);
    }
    if (manager->mCurrentSuspended) {
      totalSuspended += manager->mTaskCount;
    }
  }

  MOZ_ASSERT(mMainThreadTasks.size() >= totalSuspended);

  if (mMainThreadTasks.size() > totalSuspended) {
    for (auto iter = mMainThreadTasks.begin(); iter != mMainThreadTasks.end();
         iter++) {
      Task* task = iter->get();

      if (task->mTaskManager && task->mTaskManager->mCurrentSuspended) {
        continue;
      }

      task = GetFinalDependency(task);

      if (task->GetKind() == Task::Kind::OffMainThreadOnly ||
          task->mInProgress ||
          (task->mTaskManager && task->mTaskManager->mCurrentSuspended)) {
        continue;
      }

      mCurrentTasksMT.push(task);
      mMainThreadTasks.erase(task->mIterator);
      task->mIterator = mMainThreadTasks.end();
      task->mInProgress = true;
      TaskManager* manager = task->GetManager();
      bool result = false;

      {
        MutexAutoUnlock unlock(mGraphMutex);
        if (manager) {
          manager->WillRunTask();
          if (manager != mIdleTaskManager) {
            mIdleTaskManager->State().FlagNotIdle();
          } else {
            TimeStamp idleDeadline =
                mIdleTaskManager->State().GetCachedIdleDeadline();
            MOZ_ASSERT(
                idleDeadline,
                "How can we not have a deadline if our manager is enabled?");
            task->SetIdleDeadline(idleDeadline);
          }
        }
        if (mIdleTaskManager) {
          mIdleTaskManager->State().ClearCachedIdleDeadline();
        }

        TimeStamp now = TimeStamp::Now();

        if (mainThread) {
          if (task->GetPriority() < uint32_t(EventQueuePriority::InputHigh) ||
              task->mInsertionTime.IsNull()) {
            mainThread->SetRunningEventDelay(TimeDuration(), now);
          } else {
            mainThread->SetRunningEventDelay(now - task->mInsertionTime, now);
          }
        }

        nsAutoCString name;
        PerformanceCounterState::Snapshot snapshot =
            mPerformanceCounterState->RunnableWillRun(
                now, manager == mIdleTaskManager);

        {
          LogTask::Run log(task);
          result = RunTask(task) == Task::TaskResult::Complete;
        }

        if (manager) {
          manager->DidRunTask();
        }

        mPerformanceCounterState->RunnableDidRun(name, std::move(snapshot));
      }

      if (manager && result && manager->mTaskCount == 0) {
        mTaskManagers.erase(manager);
      }

      task->mInProgress = false;

      if (!result) {
        auto insertion =
            mMainThreadTasks.insert(std::move(mCurrentTasksMT.top()));
        MOZ_ASSERT(insertion.second);
        task->mIterator = insertion.first;
        if (manager) {
          manager->WillRunTask();
        }
      } else {
        task->mCompleted = true;
#if defined(DEBUG)
        task->mIsInGraph = false;
#endif
        task->mDependencies.clear();

        DispatchThreadableTasks(aProofOfLock);
      }

      mCurrentTasksMT.pop();
      return true;
    }
  }

  mMayHaveMainThreadTask = false;
  if (mIdleTaskManager) {
    mIdleTaskManager->State().ClearCachedIdleDeadline();
  }
  return false;
}

Task* TaskController::GetFinalDependency(Task* aTask) {
  Task* nextTask;

  while ((nextTask = aTask->GetHighestPriorityDependency())) {
    aTask = nextTask;
  }

  return aTask;
}

void TaskController::MaybeInterruptTask(Task* aTask,
                                        const MutexAutoLock& aProofOfLock) {
  mGraphMutex.AssertCurrentThreadOwns();

  if (!aTask) {
    return;
  }

  if (!aTask->mDependencies.empty()) {
    Task* firstDependency = aTask->mDependencies.begin()->get();
    if (aTask->GetPriority() <= firstDependency->GetPriority() &&
        !firstDependency->mCompleted &&
        aTask->GetKind() == firstDependency->GetKind()) {
      return;
    }
  }

  Task* finalDependency = GetFinalDependency(aTask);

  if (finalDependency->mInProgress) {
    return;
  }

  if (aTask->GetKind() == Task::Kind::MainThreadOnly) {
    mMayHaveMainThreadTask = true;

    EnsureMainThreadTasksScheduled();

    if (mCurrentTasksMT.empty()) {
      return;
    }

    if (finalDependency->GetKind() == Task::Kind::OffMainThreadOnly) {
      return;
    }

    if (mCurrentTasksMT.top()->GetPriority() < aTask->GetPriority()) {
      mCurrentTasksMT.top()->RequestInterrupt(aTask->GetPriority());
    }
  } else {
    if (mIdleThreadCount != 0) {
      DispatchThreadableTasks(aProofOfLock);

      return;
    }

    Task* lowestPriorityTask = nullptr;
    for (auto& thread : mPoolThreads) {
      MOZ_ASSERT(thread->mCurrentTask);
      if (!lowestPriorityTask) {
        lowestPriorityTask = thread->mCurrentTask.get();
        continue;
      }

      if (lowestPriorityTask->GetPriority() > thread->mEffectiveTaskPriority) {
        lowestPriorityTask = thread->mCurrentTask.get();
      }
    }

    if (lowestPriorityTask->GetPriority() < aTask->GetPriority()) {
      lowestPriorityTask->RequestInterrupt(aTask->GetPriority());
    }

  }
}

Task* TaskController::GetHighestPriorityMTTask() {
  mGraphMutex.AssertCurrentThreadOwns();

  if (!mMainThreadTasks.empty()) {
    return mMainThreadTasks.begin()->get();
  }
  return nullptr;
}

void TaskController::EnsureMainThreadTasksScheduled() {
  if (mObserver) {
    mObserver->OnDispatchedEvent();
  }
  if (mExternalCondVar) {
    mExternalCondVar->Notify();
  }
  mMainThreadCV.Notify();
}

void TaskController::ProcessUpdatedPriorityModifier(TaskManager* aManager) {
  mGraphMutex.AssertCurrentThreadOwns();

  MOZ_ASSERT(NS_IsMainThread());

  int32_t modifier = aManager->mCurrentPriorityModifier;

  PrioritySortedTasks managerTasks;
  auto cur = mMainThreadTasks.begin();
  while (cur != mMainThreadTasks.end()) {
    auto next = std::next(cur);
    if (cur->get()->mTaskManager == aManager) {
      auto task = mMainThreadTasks.extract(cur);
      task.value()->mPriorityModifier = modifier;
      managerTasks.insert(std::move(task));
    }
    cur = std::move(next);
  }
  mMainThreadTasks.merge(std::move(managerTasks));
}

}  
