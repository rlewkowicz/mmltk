/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_CompositorVsyncScheduler_h
#define mozilla_layers_CompositorVsyncScheduler_h

#include <stdint.h>  // for uint64_t

#include "mozilla/Monitor.h"    // for Monitor
#include "mozilla/RefPtr.h"     // for RefPtr
#include "mozilla/TimeStamp.h"  // for TimeStamp
#include "mozilla/gfx/Point.h"  // for IntSize
#include "mozilla/layers/SampleTime.h"
#include "mozilla/webrender/webrender_ffi.h"
#include "mozilla/VsyncDispatcher.h"
#include "mozilla/widget/CompositorWidget.h"
#include "nsISupportsImpl.h"

namespace mozilla {

class CancelableRunnable;
class Runnable;

namespace gfx {
class DrawTarget;
}  

namespace layers {

class CompositorVsyncSchedulerOwner;

class CompositorVsyncScheduler {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CompositorVsyncScheduler)

 public:
  CompositorVsyncScheduler(CompositorVsyncSchedulerOwner* aVsyncSchedulerOwner,
                           widget::CompositorWidget* aWidget);

  void NotifyVsync(const VsyncEvent& aVsync);

  void Destroy();

  void ScheduleComposition(wr::RenderReasons aReasons);

  wr::RenderReasons CancelCurrentCompositeTask();

  bool NeedsComposite();

  void ForceComposeToTarget(wr::RenderReasons aReasons,
                            gfx::DrawTarget* aTarget,
                            const gfx::IntRect* aRect);

  bool FlushPendingComposite();

  const SampleTime& GetLastComposeTime() const;

  const TimeStamp& GetLastVsyncTime() const;
  const TimeStamp& GetLastVsyncOutputTime() const;
  const VsyncId& GetLastVsyncId() const;

  void UpdateLastComposeTime();

 private:
  virtual ~CompositorVsyncScheduler();

  void PostCompositeTask(const VsyncEvent& aVsyncEvent,
                         wr::RenderReasons aReasons);

  void Composite(const VsyncEvent& aVsyncEvent, wr::RenderReasons aReasons);

  void ObserveVsync();
  void UnobserveVsync();

  class Observer final : public VsyncObserver {
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CompositorVsyncScheduler::Observer,
                                          override)

   public:
    explicit Observer(CompositorVsyncScheduler* aOwner);
    void NotifyVsync(const VsyncEvent& aVsync) override;
    void Destroy();

   private:
    virtual ~Observer();

    Mutex mMutex;
    CompositorVsyncScheduler* mOwner;
  };

  CompositorVsyncSchedulerOwner* mVsyncSchedulerOwner;
  SampleTime mLastComposeTime;
  TimeStamp mLastVsyncTime;
  TimeStamp mLastVsyncOutputTime;
  VsyncId mLastVsyncId;

  bool mAsapScheduling;
  bool mIsObservingVsync;
  wr::RenderReasons mRendersDelayedByVsyncReasons;
  TimeStamp mCompositeRequestedAt;
  int32_t mVsyncNotificationsSkipped;
  widget::CompositorWidget* mWidget;
  RefPtr<CompositorVsyncScheduler::Observer> mVsyncObserver;

  mozilla::Monitor mCurrentCompositeTaskMonitor;
  RefPtr<CancelableRunnable> mCurrentCompositeTask
      MOZ_GUARDED_BY(mCurrentCompositeTaskMonitor);
  wr::RenderReasons mCurrentCompositeTaskReasons
      MOZ_GUARDED_BY(mCurrentCompositeTaskMonitor);

};
}  
}  

#endif  // mozilla_layers_CompositorVsyncScheduler_h
