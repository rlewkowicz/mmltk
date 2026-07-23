/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/CaptivePortalService.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Services.h"
#include "mozilla/Preferences.h"
#include "nsIObserverService.h"
#include "nsServiceManagerUtils.h"
#include "nsXULAppAPI.h"
#include "xpcpublic.h"
#include "xpcprivate.h"

static const char kOpenCaptivePortalLoginEvent[] = "captive-portal-login";
static const char kAbortCaptivePortalLoginEvent[] =
    "captive-portal-login-abort";
static const char kCaptivePortalLoginSuccessEvent[] =
    "captive-portal-login-success";

namespace mozilla {
namespace net {

static LazyLogModule gCaptivePortalLog("CaptivePortalService");
#undef LOG
#define LOG(args) MOZ_LOG(gCaptivePortalLog, mozilla::LogLevel::Debug, args)

NS_IMPL_ISUPPORTS(CaptivePortalService, nsICaptivePortalService, nsIObserver,
                  nsISupportsWeakReference, nsITimerCallback, nsINamed)

static StaticRefPtr<CaptivePortalService> gCPService;

already_AddRefed<nsICaptivePortalService> CaptivePortalService::GetSingleton() {
  if (gCPService) {
    return do_AddRef(gCPService);
  }

  gCPService = new CaptivePortalService();
  ClearOnShutdown(&gCPService);
  return do_AddRef(gCPService);
}

CaptivePortalService::CaptivePortalService() {
  mLastChecked = TimeStamp::Now();
}

CaptivePortalService::~CaptivePortalService() {
  LOG(("CaptivePortalService::~CaptivePortalService isParentProcess:%d\n",
       XRE_GetProcessType() == GeckoProcessType_Default));
}

nsresult CaptivePortalService::PerformCheck() {
  return NS_OK;
}

nsresult CaptivePortalService::RearmTimer() {
  LOG(("CaptivePortalService::RearmTimer\n"));
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  if (mTimer) {
    mTimer->Cancel();
  }

  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    mTimer = nullptr;
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
  }

  if (mState == NOT_CAPTIVE) {
    return NS_OK;
  }

  if (!mTimer) {
    mTimer = NS_NewTimer();
  }

  if (mTimer && mDelay > 0) {
    LOG(("CaptivePortalService - Reloading timer with delay %u\n", mDelay));
    return mTimer->InitWithCallback(this, mDelay, nsITimer::TYPE_ONE_SHOT);
  }

  return NS_OK;
}

nsresult CaptivePortalService::Initialize() {
  if (mInitialized) {
    return NS_OK;
  }
  mInitialized = true;

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    return NS_OK;
  }

  nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
  if (observerService) {
    observerService->AddObserver(this, kOpenCaptivePortalLoginEvent, true);
    observerService->AddObserver(this, kAbortCaptivePortalLoginEvent, true);
    observerService->AddObserver(this, kCaptivePortalLoginSuccessEvent, true);
    observerService->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, true);
  }

  LOG(("Initialized CaptivePortalService\n"));
  return NS_OK;
}

nsresult CaptivePortalService::Start() {
  if (!mInitialized) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  if (false &&
      !Preferences::GetBool("network.captive-portal-service.testMode", false)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    return NS_OK;
  }

  if (mStarted) {
    return NS_OK;
  }

  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
  }

  MOZ_ASSERT(mState == UNKNOWN, "Initial state should be UNKNOWN");
  mStarted = true;
  mEverBeenCaptive = false;

  Preferences::GetUint("network.captive-portal-service.minInterval",
                       &mMinInterval);
  Preferences::GetUint("network.captive-portal-service.maxInterval",
                       &mMaxInterval);
  Preferences::GetFloat("network.captive-portal-service.backoffFactor",
                        &mBackoffFactor);

  LOG(("CaptivePortalService::Start min:%u max:%u backoff:%.2f\n", mMinInterval,
       mMaxInterval, mBackoffFactor));

  mSlackCount = 0;
  mDelay = mMinInterval;

  return NS_OK;
}

nsresult CaptivePortalService::Stop() {
  LOG(("CaptivePortalService::Stop\n"));

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    return NS_OK;
  }

  if (!mStarted) {
    return NS_OK;
  }

  if (mTimer) {
    mTimer->Cancel();
  }
  mTimer = nullptr;
  mRequestInProgress = false;
  mStarted = false;
  mEverBeenCaptive = false;
  mState = UNKNOWN;
  return NS_OK;
}

void CaptivePortalService::SetStateInChild(int32_t aState) {
  MOZ_ASSERT(XRE_GetProcessType() != GeckoProcessType_Default);

  mState = aState;
  mLastChecked = TimeStamp::Now();
}


NS_IMETHODIMP
CaptivePortalService::GetState(int32_t* aState) {
  *aState = mState;
  return NS_OK;
}

NS_IMETHODIMP
CaptivePortalService::RecheckCaptivePortal() {
  LOG(("CaptivePortalService::RecheckCaptivePortal\n"));

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    return NS_OK;
  }

  mSlackCount = 0;
  mDelay = mMinInterval;

  mLastChecked = TimeStamp::Now();
  return NS_OK;
}

NS_IMETHODIMP
CaptivePortalService::GetLastChecked(uint64_t* aLastChecked) {
  double duration = (TimeStamp::Now() - mLastChecked).ToMilliseconds();
  *aLastChecked = static_cast<uint64_t>(duration);
  return NS_OK;
}

NS_IMETHODIMP
CaptivePortalService::Notify(nsITimer* aTimer) {
  LOG(("CaptivePortalService::Notify\n"));
  MOZ_ASSERT(aTimer == mTimer);
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);

  PerformCheck();

  mSlackCount++;
  if (mSlackCount % 10 == 0) {
    mDelay = mDelay * mBackoffFactor;
  }
  if (mDelay > mMaxInterval) {
    mDelay = mMaxInterval;
  }

  RearmTimer();

  return NS_OK;
}


NS_IMETHODIMP
CaptivePortalService::GetName(nsACString& aName) {
  aName.AssignLiteral("CaptivePortalService");
  return NS_OK;
}

NS_IMETHODIMP
CaptivePortalService::Observe(nsISupports* aSubject, const char* aTopic,
                              const char16_t* aData) {
  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    return NS_OK;
  }

  LOG(("CaptivePortalService::Observe() topic=%s\n", aTopic));
  if (!strcmp(aTopic, kOpenCaptivePortalLoginEvent)) {
    StateTransition(LOCKED_PORTAL);
    mLastChecked = TimeStamp::Now();
    mEverBeenCaptive = true;
  } else if (!strcmp(aTopic, kCaptivePortalLoginSuccessEvent)) {
    StateTransition(UNLOCKED_PORTAL);
    mLastChecked = TimeStamp::Now();
    mSlackCount = 0;
    mDelay = mMinInterval;

  } else if (!strcmp(aTopic, kAbortCaptivePortalLoginEvent)) {
    StateTransition(UNKNOWN);
    mLastChecked = TimeStamp::Now();
    mSlackCount = 0;
  } else if (!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    Stop();
    return NS_OK;
  }

  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  if (observerService) {
    nsCOMPtr<nsICaptivePortalService> cps(this);
    observerService->NotifyObservers(cps, NS_IPC_CAPTIVE_PORTAL_SET_STATE,
                                     nullptr);
  }

  return NS_OK;
}

void CaptivePortalService::NotifyConnectivityAvailable(bool aCaptive) {
  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  if (observerService) {
    nsCOMPtr<nsICaptivePortalService> cps(this);
    observerService->NotifyObservers(cps, NS_CAPTIVE_PORTAL_CONNECTIVITY,
                                     aCaptive ? u"captive" : u"clear");
  }
}

void CaptivePortalService::StateTransition(int32_t aNewState) {
  int32_t oldState = mState;
  mState = aNewState;

  if ((oldState == UNKNOWN && mState == NOT_CAPTIVE) ||
      (oldState == LOCKED_PORTAL && mState == UNLOCKED_PORTAL)) {
    nsCOMPtr<nsIObserverService> observerService =
        services::GetObserverService();
    if (observerService) {
      nsCOMPtr<nsICaptivePortalService> cps(this);
      observerService->NotifyObservers(
          cps, NS_CAPTIVE_PORTAL_CONNECTIVITY_CHANGED, nullptr);
    }
  }
}

}  
}  
