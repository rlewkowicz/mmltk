/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SERVICEWORKERS_SERVICEWORKERSHUTDOWNSTATE_H_
#define DOM_SERVICEWORKERS_SERVICEWORKERSHUTDOWNSTATE_H_

#include "ipc/EnumSerializer.h"
#include "mozilla/dom/ServiceWorkerOpArgs.h"

namespace mozilla::dom {

class ServiceWorkerShutdownState {
 public:
  enum class Progress {
    ParentProcessMainThread,
    ParentProcessIpdlBackgroundThread,
    ContentProcessWorkerLauncherThread,
    ContentProcessMainThread,
    ShutdownCompleted,
    EndGuard_,
  };

  ServiceWorkerShutdownState();

  ~ServiceWorkerShutdownState();

  const char* GetProgressString() const;

  void SetProgress(Progress aProgress);

 private:
  Progress mProgress;
};

void MaybeReportServiceWorkerShutdownProgress(const ServiceWorkerOpArgs& aArgs,
                                              bool aShutdownCompleted = false);

}  

namespace IPC {

using Progress = mozilla::dom::ServiceWorkerShutdownState::Progress;

template <>
struct ParamTraits<Progress>
    : public ContiguousEnumSerializer<
          Progress, Progress::ParentProcessMainThread, Progress::EndGuard_> {};

}  

#endif  // DOM_SERVICEWORKERS_SERVICEWORKERSHUTDOWNSTATE_H_
