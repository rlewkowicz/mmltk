/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PermissionObserver.h"

#include "PermissionStatusSink.h"
#include "PermissionUtils.h"
#include "mozilla/Services.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/WindowGlobalChild.h"
#include "nsIObserverService.h"
#include "nsIPermission.h"
#include "nsISupportsPrimitives.h"
#include "nsPIDOMWindowInlines.h"

namespace mozilla::dom {

namespace {
PermissionObserver* gInstance = nullptr;

}  

NS_IMPL_ISUPPORTS(PermissionObserver, nsIObserver, nsISupportsWeakReference)

PermissionObserver::PermissionObserver() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!gInstance);
}

PermissionObserver::~PermissionObserver() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mSinks.IsEmpty());
  MOZ_ASSERT(gInstance == this);

  gInstance = nullptr;
}

already_AddRefed<PermissionObserver> PermissionObserver::GetInstance() {
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<PermissionObserver> instance = gInstance;
  if (!instance) {
    instance = new PermissionObserver();

    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (NS_WARN_IF(!obs)) {
      return nullptr;
    }

    nsresult rv = obs->AddObserver(instance, "perm-changed", true);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return nullptr;
    }

    rv = obs->AddObserver(instance, "perm-changed-notify-only", true);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return nullptr;
    }

    rv = obs->AddObserver(instance, "browser-perm-changed", true);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return nullptr;
    }

    gInstance = instance;
  }

  return instance.forget();
}

void PermissionObserver::AddSink(PermissionStatusSink* aSink) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSink);
  MOZ_ASSERT(!mSinks.Contains(aSink));

  mSinks.AppendElement(aSink);
}

void PermissionObserver::RemoveSink(PermissionStatusSink* aSink) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSink);
  MOZ_ASSERT(mSinks.Contains(aSink));

  mSinks.RemoveElement(aSink);
}

NS_IMETHODIMP
PermissionObserver::Observe(nsISupports* aSubject, const char* aTopic,
                            const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!strcmp(aTopic, "perm-changed") ||
             !strcmp(aTopic, "perm-changed-notify-only") ||
             !strcmp(aTopic, "browser-perm-changed"));

  if (mSinks.IsEmpty()) {
    return NS_OK;
  }

  nsCOMPtr<nsIPermission> perm = nullptr;
  nsCOMPtr<nsPIDOMWindowInner> innerWindow = nullptr;
  nsAutoCString type;
  bool isBrowserPerm = !strcmp(aTopic, "browser-perm-changed");

  if (isBrowserPerm && aData && !NS_strcmp(aData, u"cleared")) {
    uint64_t clearedBrowserId = 0;
    nsCOMPtr<nsISupportsPRUint64> wrapper = do_QueryInterface(aSubject);
    if (wrapper) {
      wrapper->GetData(&clearedBrowserId);
    }

    for (PermissionStatusSink* sink : mSinks) {
      if (!clearedBrowserId ||
          sink->MaybeAffectedByBrowserIdOnMainThread(clearedBrowserId)) {
        sink->PermissionChangedOnMainThread();
      }
    }
    return NS_OK;
  }

  if (!strcmp(aTopic, "perm-changed") || isBrowserPerm) {
    perm = do_QueryInterface(aSubject);
    if (!perm) {
      return NS_OK;
    }
    perm->GetType(type);
  } else if (!strcmp(aTopic, "perm-changed-notify-only")) {
    innerWindow = do_QueryInterface(aSubject);
    if (!innerWindow) {
      return NS_OK;
    }
    type = NS_ConvertUTF16toUTF8(aData);
  }

  Maybe<PermissionName> permission = TypeToPermissionName(type);
  if (permission) {
    for (PermissionStatusSink* sink : mSinks) {
      if (sink->Name() != permission.value()) {
        continue;
      }
      if (perm) {
        if (isBrowserPerm) {
          if (sink->MaybeUpdatedByBrowserPermOnMainThread(perm)) {
            sink->PermissionChangedOnMainThread();
          }
        } else if (sink->MaybeUpdatedByOnMainThread(perm)) {
          sink->PermissionChangedOnMainThread();
        }
      }
      if (innerWindow &&
          sink->MaybeUpdatedByNotifyOnlyOnMainThread(innerWindow)) {
        sink->PermissionChangedOnMainThread();
      }
    }
  }

  return NS_OK;
}

void PermissionObserver::NotifySystemPermissionChanged(PermissionName aName,
                                                       PermissionState aState) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!gInstance) {
    return;
  }
  for (PermissionStatusSink* sink : gInstance->mSinks) {
    if (sink->Name() == aName) {
      sink->SystemPermissionChangedOnMainThread(aState);
    }
  }
}

}  
