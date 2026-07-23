/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef GFX_SOFTWARE_VSYNC_SOURCE_H
#define GFX_SOFTWARE_VSYNC_SOURCE_H

#include "mozilla/DataMutex.h"
#include "mozilla/Monitor.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "base/thread.h"
#include "nsISupportsImpl.h"
#include "VsyncSource.h"

namespace mozilla::gfx {

class SoftwareVsyncSource : public VsyncSource {
 public:
  explicit SoftwareVsyncSource(const TimeDuration& aInitialVsyncRate);
  virtual ~SoftwareVsyncSource();

  void EnableVsync() override;
  void DisableVsync() override;
  bool IsVsyncEnabled() override;
  bool IsInSoftwareVsyncThread();
  void NotifyVsync(const TimeStamp& aVsyncTimestamp,
                   const TimeStamp& aOutputTimestamp) override;
  TimeDuration GetVsyncRate() override;
  void ScheduleNextVsync(TimeStamp aVsyncTimestamp);
  void Shutdown() override;

  void SetVsyncRate(const TimeDuration& aNewRate);

 protected:
  base::Thread* mVsyncThread;
  RefPtr<CancelableRunnable> mCurrentVsyncTask;  
  bool mVsyncEnabled;                            

 private:
  DataMutex<TimeDuration> mVsyncRate;  
};

}  

#endif /* GFX_SOFTWARE_VSYNC_SOURCE_H */
