/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "GraphRunner.h"

#include "GraphDriver.h"
#include "MediaTrackGraph.h"
#include "MediaTrackGraphImpl.h"
#include "audio_thread_priority.h"
#include "mozilla/dom/WorkletThread.h"
#include "nsISupportsImpl.h"
#include "nsISupportsPriority.h"
#include "prthread.h"

namespace mozilla {

GraphRunner::GraphRunner(MediaTrackGraphImpl* aGraph,
                         already_AddRefed<nsIThread> aThread)
    : Runnable("GraphRunner"),
      mMonitor("GraphRunner::mMonitor"),
      mGraph(aGraph),
      mThreadState(ThreadState::Wait),
      mThread(aThread) {
  mThread->Dispatch(do_AddRef(this));
}

GraphRunner::~GraphRunner() {
  MOZ_ASSERT(mThreadState == ThreadState::Shutdown);
}

already_AddRefed<GraphRunner> GraphRunner::Create(MediaTrackGraphImpl* aGraph) {
  nsCOMPtr<nsIThread> thread;
  nsIThreadManager::ThreadCreationOptions options = {
      .stackSize = mozilla::dom::WorkletThread::StackSize()};
  if (NS_WARN_IF(NS_FAILED(NS_NewNamedThread(
          "GraphRunner", getter_AddRefs(thread), nullptr, options)))) {
    return nullptr;
  }
  nsCOMPtr<nsISupportsPriority> supportsPriority = do_QueryInterface(thread);
  MOZ_ASSERT(supportsPriority);
  MOZ_ALWAYS_SUCCEEDS(
      supportsPriority->SetPriority(nsISupportsPriority::PRIORITY_HIGHEST));

  return do_AddRef(new GraphRunner(aGraph, thread.forget()));
}

void GraphRunner::Shutdown() {
  {
    MonitorAutoLock lock(mMonitor);
    MOZ_ASSERT(mThreadState == ThreadState::Wait);
    mThreadState = ThreadState::Shutdown;
    mMonitor.Notify();
  }
  mThread->Shutdown();
}

auto GraphRunner::OneIteration(GraphTime aStateTime,
                               MixerCallbackReceiver* aMixerReceiver)
    -> IterationResult {

  MonitorAutoLock lock(mMonitor);
  MOZ_ASSERT(mThreadState == ThreadState::Wait);
  mIterationState = Some(IterationState(aStateTime, aMixerReceiver));

#if defined(DEBUG)
  if (const auto* audioDriver =
          mGraph->CurrentDriver()->AsAudioCallbackDriver()) {
    mAudioDriverThreadId = audioDriver->ThreadId();
  } else if (const auto* clockDriver =
                 mGraph->CurrentDriver()->AsSystemClockDriver()) {
    mClockDriverThread = clockDriver->Thread();
  } else {
    MOZ_CRASH("Unknown GraphDriver");
  }
#endif
  mThreadState = ThreadState::Run;
  mMonitor.Notify();
  do {
    mMonitor.Wait();
  } while (mThreadState == ThreadState::Run);

#if defined(DEBUG)
  mAudioDriverThreadId = std::thread::id();
  mClockDriverThread = nullptr;
#endif

  mIterationState = Nothing();

  IterationResult result = std::move(mIterationResult);
  mIterationResult = IterationResult();
  return result;
}


NS_IMETHODIMP GraphRunner::Run() {
#if !defined(XP_LINUX)
  atp_handle* handle =
      atp_promote_current_thread_to_real_time(0, mGraph->GraphRate());
#endif


  nsCOMPtr<nsIThreadInternal> threadInternal = do_QueryInterface(mThread);
  threadInternal->SetObserver(mGraph);

  MonitorAutoLock lock(mMonitor);
  while (true) {
    while (mThreadState == ThreadState::Wait) {
      mMonitor.Wait();  
    }
    if (mThreadState == ThreadState::Shutdown) {
      break;
    }
    MOZ_DIAGNOSTIC_ASSERT(mIterationState.isSome());
    mIterationResult = mGraph->OneIterationImpl(
        mIterationState->StateTime(), mIterationState->MixerReceiver());
    mThreadState = ThreadState::Wait;
    mMonitor.Notify();
  }

#if !defined(XP_LINUX)
  if (handle) {
    atp_demote_current_thread_from_real_time(handle);
  }
#endif

  return NS_OK;
}

bool GraphRunner::OnThread() const { return mThread->IsOnCurrentThread(); }

#if defined(DEBUG)
bool GraphRunner::InDriverIteration(const GraphDriver* aDriver) const {
  if (!OnThread()) {
    return false;
  }

  if (const auto* audioDriver = aDriver->AsAudioCallbackDriver()) {
    return audioDriver->ThreadId() == mAudioDriverThreadId;
  }

  if (const auto* clockDriver = aDriver->AsSystemClockDriver()) {
    return clockDriver->Thread() == mClockDriverThread;
  }

  MOZ_CRASH("Unknown driver");
}
#endif

}  
