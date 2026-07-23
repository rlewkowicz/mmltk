/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorVsyncScheduler.h"

#include <stdio.h>        // for fprintf, stdout
#include <stdint.h>       // for uint64_t
#include "base/task.h"    // for CancelableTask, etc
#include "base/thread.h"  // for Thread
#include "gfxPlatform.h"  // for gfxPlatform
#if defined(MOZ_WIDGET_GTK)
#  include "gfxPlatformGtk.h"  // for gfxPlatform
#endif
#include "mozilla/AutoRestore.h"  // for AutoRestore
#include "mozilla/DebugOnly.h"    // for DebugOnly
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/gfx/2D.h"     // for DrawTarget
#include "mozilla/gfx/Point.h"  // for IntSize
#include "mozilla/gfx/Rect.h"   // for IntSize
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/CompositorVsyncSchedulerOwner.h"
#include "mozilla/mozalloc.h"  // for operator new, etc
#include "nsCOMPtr.h"          // for already_AddRefed
#include "nsDebug.h"           // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"   // for MOZ_COUNT_CTOR, etc
#include "nsIWidget.h"         // for nsIWidget
#include "nsThreadUtils.h"     // for NS_IsMainThread
#include "mozilla/VsyncDispatcher.h"
#if 0 || defined(MOZ_WIDGET_GTK)
#  include "VsyncSource.h"
#endif
#include "mozilla/widget/CompositorWidget.h"
namespace mozilla {

namespace layers {

using namespace mozilla::gfx;

CompositorVsyncScheduler::Observer::Observer(CompositorVsyncScheduler* aOwner)
    : mMutex("CompositorVsyncScheduler.Observer.Mutex"), mOwner(aOwner) {}

CompositorVsyncScheduler::Observer::~Observer() { MOZ_ASSERT(!mOwner); }

void CompositorVsyncScheduler::Observer::NotifyVsync(const VsyncEvent& aVsync) {
  MutexAutoLock lock(mMutex);
  if (!mOwner) {
    return;
  }
  mOwner->NotifyVsync(aVsync);
}

void CompositorVsyncScheduler::Observer::Destroy() {
  MutexAutoLock lock(mMutex);
  mOwner = nullptr;
}

CompositorVsyncScheduler::CompositorVsyncScheduler(
    CompositorVsyncSchedulerOwner* aVsyncSchedulerOwner,
    widget::CompositorWidget* aWidget)
    : mVsyncSchedulerOwner(aVsyncSchedulerOwner),
      mLastComposeTime(SampleTime::FromNow()),
      mLastVsyncTime(TimeStamp::Now()),
      mLastVsyncOutputTime(TimeStamp::Now()),
      mIsObservingVsync(false),
      mRendersDelayedByVsyncReasons(wr::RenderReasons::NONE),
      mVsyncNotificationsSkipped(0),
      mWidget(aWidget),
      mCurrentCompositeTaskMonitor("CurrentCompositeTaskMonitor"),
      mCurrentCompositeTask(nullptr),
      mCurrentCompositeTaskReasons(wr::RenderReasons::NONE) {
  mVsyncObserver = new Observer(this);

  mAsapScheduling =
      StaticPrefs::layers_offmainthreadcomposition_frame_rate() == 0 ||
      gfxPlatform::IsInLayoutAsapMode();
}

CompositorVsyncScheduler::~CompositorVsyncScheduler() {
  MOZ_ASSERT(!mIsObservingVsync);
  MOZ_ASSERT(!mVsyncObserver);
  mVsyncSchedulerOwner = nullptr;
}

void CompositorVsyncScheduler::Destroy() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  if (!mVsyncObserver) {
    return;
  }
  UnobserveVsync();
  mVsyncObserver->Destroy();
  mVsyncObserver = nullptr;

  mCompositeRequestedAt = TimeStamp();
  CancelCurrentCompositeTask();
}

void CompositorVsyncScheduler::PostCompositeTask(const VsyncEvent& aVsyncEvent,
                                                 wr::RenderReasons aReasons) {
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  mCurrentCompositeTaskReasons = mCurrentCompositeTaskReasons | aReasons;
  if (mCurrentCompositeTask == nullptr && CompositorThread()) {
    RefPtr<CancelableRunnable> task =
        NewCancelableRunnableMethod<VsyncEvent, wr::RenderReasons>(
            "layers::CompositorVsyncScheduler::Composite", this,
            &CompositorVsyncScheduler::Composite, aVsyncEvent, aReasons);
    mCurrentCompositeTask = task;
    CompositorThread()->Dispatch(task.forget());
  }
}

void CompositorVsyncScheduler::ScheduleComposition(wr::RenderReasons aReasons) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mVsyncObserver) {
    return;
  }

  TimeStamp vsyncTime = TimeStamp::Now();
  TimeStamp outputTime = vsyncTime + mVsyncSchedulerOwner->GetVsyncInterval();
  VsyncEvent vsyncEvent(VsyncId(), vsyncTime, outputTime);

  if (mAsapScheduling) {
    PostCompositeTask(vsyncEvent, aReasons);
  } else {
    if (!mCompositeRequestedAt) {
      mCompositeRequestedAt = TimeStamp::Now();
    }
    if (!mIsObservingVsync && mCompositeRequestedAt) {
      ObserveVsync();
      PostCompositeTask(vsyncEvent,
                        aReasons | wr::RenderReasons::START_OBSERVING_VSYNC);
    } else {
      mRendersDelayedByVsyncReasons = aReasons;
    }
  }
}

void CompositorVsyncScheduler::NotifyVsync(const VsyncEvent& aVsync) {
#if defined(DEBUG)
#if defined(MOZ_WAYLAND)
  if (!XRE_IsParentProcess() ||
      !gfxPlatformGtk::GetPlatform()->IsWaylandDisplay())
#endif
  {
    MOZ_ASSERT_IF(XRE_IsParentProcess(),
                  !CompositorThreadHolder::IsInCompositorThread());
    MOZ_ASSERT(!NS_IsMainThread());
  }

  MOZ_ASSERT_IF(XRE_GetProcessType() == GeckoProcessType_GPU,
                CompositorThreadHolder::IsInCompositorThread());
#endif

  PostCompositeTask(aVsync, wr::RenderReasons::VSYNC);
}

wr::RenderReasons CompositorVsyncScheduler::CancelCurrentCompositeTask() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread() ||
             NS_IsMainThread());
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  wr::RenderReasons canceledTaskRenderReasons = mCurrentCompositeTaskReasons;
  mCurrentCompositeTaskReasons = wr::RenderReasons::NONE;
  if (mCurrentCompositeTask) {
    mCurrentCompositeTask->Cancel();
    mCurrentCompositeTask = nullptr;
  }

  return canceledTaskRenderReasons;
}

void CompositorVsyncScheduler::Composite(const VsyncEvent& aVsyncEvent,
                                         wr::RenderReasons aReasons) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MOZ_ASSERT(mVsyncSchedulerOwner);

  {  
    MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
    aReasons =
        aReasons | mCurrentCompositeTaskReasons | mRendersDelayedByVsyncReasons;
    mCurrentCompositeTaskReasons = wr::RenderReasons::NONE;
    mRendersDelayedByVsyncReasons = wr::RenderReasons::NONE;
    mCurrentCompositeTask = nullptr;
  }

  mLastVsyncTime = aVsyncEvent.mTime;
  mLastVsyncOutputTime = aVsyncEvent.mOutputTime;
  mLastVsyncId = aVsyncEvent.mId;

  if (!mAsapScheduling) {
    if (aVsyncEvent.mTime < mLastComposeTime.Time()) {
      return;
    }

    if (mVsyncSchedulerOwner->IsPendingComposite()) {
      mVsyncSchedulerOwner->FinishPendingComposite();
      return;
    }
  }

  if (mCompositeRequestedAt || mAsapScheduling) {
    mCompositeRequestedAt = TimeStamp();
    mLastComposeTime = SampleTime::FromVsync(aVsyncEvent.mTime);

    mVsyncSchedulerOwner->CompositeToTarget(aVsyncEvent.mId, aReasons, nullptr,
                                            nullptr);

    mVsyncNotificationsSkipped = 0;

  } else if (mVsyncNotificationsSkipped++ >
             StaticPrefs::gfx_vsync_compositor_unobserve_count_AtStartup()) {
    UnobserveVsync();
  }
}

void CompositorVsyncScheduler::ForceComposeToTarget(wr::RenderReasons aReasons,
                                                    gfx::DrawTarget* aTarget,
                                                    const IntRect* aRect) {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  mVsyncNotificationsSkipped = 0;

  mLastComposeTime = SampleTime::FromNow();
  MOZ_ASSERT(mVsyncSchedulerOwner);
  mVsyncSchedulerOwner->CompositeToTarget(VsyncId(), aReasons, aTarget, aRect);
}

bool CompositorVsyncScheduler::NeedsComposite() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return (bool)mCompositeRequestedAt;
}

bool CompositorVsyncScheduler::FlushPendingComposite() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (mCompositeRequestedAt) {
    wr::RenderReasons reasons = CancelCurrentCompositeTask();
    ForceComposeToTarget(reasons, nullptr, nullptr);
    return true;
  }
  return false;
}

void CompositorVsyncScheduler::ObserveVsync() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(mVsyncObserver);
  mIsObservingVsync = true;
}

void CompositorVsyncScheduler::UnobserveVsync() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(nullptr);
  mIsObservingVsync = false;
}

const SampleTime& CompositorVsyncScheduler::GetLastComposeTime() const {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return mLastComposeTime;
}

const TimeStamp& CompositorVsyncScheduler::GetLastVsyncTime() const {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return mLastVsyncTime;
}

const TimeStamp& CompositorVsyncScheduler::GetLastVsyncOutputTime() const {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return mLastVsyncOutputTime;
}

const VsyncId& CompositorVsyncScheduler::GetLastVsyncId() const {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return mLastVsyncId;
}

void CompositorVsyncScheduler::UpdateLastComposeTime() {
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mLastComposeTime = SampleTime::FromNow();
}

}  
}  
