/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIObserverService.h"
#include "xpcpublic.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_alerts.h"
#include "nsServiceManagerUtils.h"
#include "nsXULAlerts.h"

#include "nsAlertsService.h"

#include "nsToolkitCompsCID.h"
#include "nsComponentManagerUtils.h"

#if defined(MOZ_PLACES)
#  include "nsIFaviconService.h"
#endif


using namespace mozilla;

NS_IMPL_ISUPPORTS(nsAlertsService, nsIAlertsService, nsIAlertsDoNotDisturb,
                  nsIObserver)

nsAlertsService::nsAlertsService() : mBackend(nullptr) {
  mBackend = do_GetService(NS_SYSTEMALERTSERVICE_CONTRACTID);
}

nsresult nsAlertsService::Init() {
  if (nsCOMPtr<nsIObserverService> obsServ =
          mozilla::services::GetObserverService()) {
    (void)NS_WARN_IF(
        NS_FAILED(obsServ->AddObserver(this, "last-pb-context-exited", false)));
  }

  RunOnShutdown([self = RefPtr{this}]() { self->Teardown(); });

  return NS_OK;
}

nsAlertsService::~nsAlertsService() = default;

bool nsAlertsService::ShouldShowAlert() {
  bool result = true;


  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(GetDNDBackend());
  if (alertsDND) {
    bool suppressForScreenSharing = false;
    nsresult rv =
        alertsDND->GetSuppressForScreenSharing(&suppressForScreenSharing);
    if (NS_SUCCEEDED(rv)) {
      result &= !suppressForScreenSharing;
    }
  }

  return result;
}

NS_IMETHODIMP nsAlertsService::ShowAlert(nsIAlertNotification* aAlert,
                                         nsIObserver* aAlertListener) {
  NS_ENSURE_ARG(aAlert);

  nsAutoString cookie;
  nsresult rv = aAlert->GetCookie(cookie);
  NS_ENSURE_SUCCESS(rv, rv);

  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    return NS_OK;
  }

  if (StaticPrefs::alerts_useSystemBackend()) {
    if (!mBackend) {
      return NS_ERROR_NOT_AVAILABLE;
    }
    return mBackend->ShowAlert(aAlert, aAlertListener);
  }

  if (!ShouldShowAlert()) {
    if (aAlertListener) {
      aAlertListener->Observe(nullptr, "alertfinished", cookie.get());
    }
    return NS_OK;
  }

  nsCOMPtr<nsIAlertsService> xulBackend(nsXULAlerts::GetInstance());
  NS_ENSURE_TRUE(xulBackend, NS_ERROR_FAILURE);
  return xulBackend->ShowAlert(aAlert, aAlertListener);
}

NS_IMETHODIMP nsAlertsService::CloseAlert(const nsAString& aAlertName,
                                          bool aContextClosed) {
  if (!StaticPrefs::alerts_useSystemBackend()) {
    nsCOMPtr<nsIAlertsService> xulBackend(nsXULAlerts::GetInstance());
    NS_ENSURE_TRUE(xulBackend, NS_ERROR_FAILURE);
    return xulBackend->CloseAlert(aAlertName, aContextClosed);
  }
  if (!mBackend) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return mBackend->CloseAlert(aAlertName, aContextClosed);
}

NS_IMETHODIMP nsAlertsService::GetHistory(nsTArray<nsString>& aResult) {
  if (!mBackend) {
    return NS_OK;
  }

  return mBackend->GetHistory(aResult);
}

NS_IMETHODIMP nsAlertsService::GetManualDoNotDisturb(bool* aRetVal) {
  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(GetDNDBackend());
  NS_ENSURE_TRUE(alertsDND, NS_ERROR_NOT_IMPLEMENTED);
  return alertsDND->GetManualDoNotDisturb(aRetVal);
}

NS_IMETHODIMP nsAlertsService::SetManualDoNotDisturb(bool aDoNotDisturb) {
  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(GetDNDBackend());
  NS_ENSURE_TRUE(alertsDND, NS_ERROR_NOT_IMPLEMENTED);

  return alertsDND->SetManualDoNotDisturb(aDoNotDisturb);
}

NS_IMETHODIMP nsAlertsService::GetSuppressForScreenSharing(bool* aRetVal) {
  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(GetDNDBackend());
  NS_ENSURE_TRUE(alertsDND, NS_ERROR_NOT_IMPLEMENTED);
  return alertsDND->GetSuppressForScreenSharing(aRetVal);
}

NS_IMETHODIMP nsAlertsService::SetSuppressForScreenSharing(bool aSuppress) {
  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(GetDNDBackend());
  NS_ENSURE_TRUE(alertsDND, NS_ERROR_NOT_IMPLEMENTED);
  return alertsDND->SetSuppressForScreenSharing(aSuppress);
}

already_AddRefed<nsIAlertsDoNotDisturb> nsAlertsService::GetDNDBackend() {
  nsCOMPtr<nsIAlertsService> backend;
  if (StaticPrefs::alerts_useSystemBackend()) {
    backend = mBackend;
  }
  if (!backend) {
    backend = nsXULAlerts::GetInstance();
  }

  nsCOMPtr<nsIAlertsDoNotDisturb> alertsDND(do_QueryInterface(backend));
  return alertsDND.forget();
}

NS_IMETHODIMP nsAlertsService::Observe(nsISupports* aSubject,
                                       const char* aTopic,
                                       const char16_t* aData) {
  nsDependentCString topic(aTopic);
  if (topic == "last-pb-context-exited"_ns) {
    return PbmTeardown();
  }
  return NS_OK;
}

NS_IMETHODIMP nsAlertsService::Teardown() {
  nsCOMPtr<nsIAlertsService> backend;
  if (StaticPrefs::alerts_useSystemBackend()) {
    backend = mBackend;
  }
  if (!backend) {
    return NS_OK;
  }
  return backend->Teardown();
}

NS_IMETHODIMP nsAlertsService::PbmTeardown() {
  nsCOMPtr<nsIAlertsService> backend;
  if (StaticPrefs::alerts_useSystemBackend()) {
    backend = mBackend;
  }
  if (!backend) {
    backend = nsXULAlerts::GetInstance();
  }
  return backend->PbmTeardown();
}

NS_IMETHODIMP nsAlertsService::IsFullscreen(bool* aRetVal) {
  *aRetVal = false;
  if (mBackend) {
    return mBackend->IsFullscreen(aRetVal);
  }
  return NS_OK;
}
