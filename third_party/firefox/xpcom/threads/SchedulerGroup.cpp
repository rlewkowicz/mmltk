/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/SchedulerGroup.h"
#include "nsThreadUtils.h"

namespace mozilla {

nsresult SchedulerGroup::Dispatch(already_AddRefed<nsIRunnable> aRunnable,
                                  nsIEventTarget::DispatchFlags aFlags) {
  if (NS_IsMainThread()) {
    return NS_DispatchToCurrentThread(std::move(aRunnable));
  }
  return NS_DispatchToMainThread(std::move(aRunnable), aFlags);
}

}  
