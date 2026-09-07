/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QuotaPrefs.h"

#include "mozilla/StaticPrefs_dom.h"
#include "prenv.h"

#define STATIC_PREF(b1, b2, b3, b4) \
  StaticPrefs::b1##_##b2##_##b3##_##b4##_DoNotUseDirectly

namespace mozilla::dom::quota {

bool QuotaPrefs::LazyOriginInitializationEnabled() {
  return IncrementalOriginInitializationEnabled() ||
         STATIC_PREF(dom, quotaManager, temporaryStorage,
                     lazyOriginInitialization)();
}

bool QuotaPrefs::TriggerOriginInitializationInBackgroundEnabled() {
  return IncrementalOriginInitializationEnabled() ||
         STATIC_PREF(dom, quotaManager, temporaryStorage,
                     triggerOriginInitializationInBackground)();
}

bool QuotaPrefs::IncrementalOriginInitializationEnabled() {
  if (STATIC_PREF(dom, quotaManager, temporaryStorage,
                  incrementalOriginInitialization)()) {
    return true;
  }

  const char* env = PR_GetEnv("MOZ_ENABLE_INC_ORIGIN_INIT");
  return (env && *env == '1');
}

}  
