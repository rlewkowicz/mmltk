/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSReauthenticator.h"

#include "OSKeyStore.h"
#include "nsNetCID.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/Logging.h"
#include "mozilla/Maybe.h"
#include "mozilla/Preferences.h"
#include "nsComponentManagerUtils.h"
#include "nsIBaseWindow.h"
#include "nsIDocShell.h"
#include "nsISupportsUtils.h"
#include "nsIWidget.h"
#include "nsPIDOMWindow.h"
#include "nsPIDOMWindowInlines.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "mozilla/ipc/IPCTypes.h"

NS_IMPL_ISUPPORTS(OSReauthenticator, nsIOSReauthenticator)

extern mozilla::LazyLogModule gCredentialManagerSecretLog;

using mozilla::LogLevel;
using mozilla::Maybe;
using mozilla::Preferences;
using mozilla::WindowsHandle;
using mozilla::dom::Promise;

#define PREF_BLANK_PASSWORD "security.osreauthenticator.blank_password"
#define PREF_PASSWORD_LAST_CHANGED_LO \
  "security.osreauthenticator.password_last_changed_lo"
#define PREF_PASSWORD_LAST_CHANGED_HI \
  "security.osreauthenticator.password_last_changed_hi"


static nsresult ReauthenticateUser(const nsAString& prompt,
                                   const nsAString& caption,
                                   const WindowsHandle& hwndParent,
                                    bool& reauthenticated,
                                    bool& isBlankPassword,
                                    int64_t& prefLastChanged,
                                    bool& isAutoAdminLogonEnabled,
                                    bool& isRequireSignonEnabled) {
  reauthenticated = false;
  return NS_OK;
}

static void BackgroundReauthenticateUser(RefPtr<Promise>& aPromise,
                                         const nsAString& aMessageText,
                                         const nsAString& aCaptionText,
                                         const WindowsHandle& hwndParent,
                                         bool isBlankPassword,
                                         int64_t prefLastChanged) {
  nsAutoCString recovery;
  bool reauthenticated;
  bool isAutoAdminLogonEnabled;
  bool isRequireSignonEnabled;
  nsresult rv = ReauthenticateUser(
      aMessageText, aCaptionText, hwndParent, reauthenticated, isBlankPassword,
      prefLastChanged, isAutoAdminLogonEnabled, isRequireSignonEnabled);

  nsTArray<int32_t> prefLastChangedUpdates;

  nsTArray<int32_t> results;
  results.AppendElement(reauthenticated);
  results.AppendElement(isBlankPassword);
  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUserResolve",
      [rv, results = std::move(results),
       prefLastChangedUpdates = std::move(prefLastChangedUpdates),
       aPromise = std::move(aPromise)]() {
        if (NS_FAILED(rv)) {
          aPromise->MaybeReject(rv);
        } else {
          aPromise->MaybeResolve(results);
        }

        nsresult rv = Preferences::SetBool(PREF_BLANK_PASSWORD, results[1]);
        if (NS_FAILED(rv)) {
          return;
        }
        if (prefLastChangedUpdates.Length() > 1) {
          rv = Preferences::SetInt(PREF_PASSWORD_LAST_CHANGED_HI,
                                   prefLastChangedUpdates[0]);
          if (NS_FAILED(rv)) {
            return;
          }
          Preferences::SetInt(PREF_PASSWORD_LAST_CHANGED_LO,
                              prefLastChangedUpdates[1]);
        }
      }));
  NS_DispatchToMainThread(runnable.forget());
}

NS_IMETHODIMP
OSReauthenticator::AsyncReauthenticateUser(const nsAString& aMessageText,
                                           const nsAString& aCaptionText,
                                           mozIDOMWindow* aParentWindow,
                                           JSContext* aCx,
                                           Promise** promiseOut) {
  NS_ENSURE_ARG_POINTER(aCx);

  RefPtr<Promise> promiseHandle;
  nsresult rv = GetPromise(aCx, promiseHandle);
  if (NS_FAILED(rv)) {
    return rv;
  }

  WindowsHandle hwndParent = 0;
  if (aParentWindow) {
    nsPIDOMWindowInner* win = nsPIDOMWindowInner::From(aParentWindow);
    nsIDocShell* docShell = win->GetDocShell();
    if (docShell) {
      nsCOMPtr<nsIBaseWindow> baseWindow = do_QueryInterface(docShell);
      if (baseWindow) {
        nsCOMPtr<nsIWidget> widget;
        baseWindow->GetMainWidget(getter_AddRefs(widget));
        if (widget) {
          hwndParent = reinterpret_cast<WindowsHandle>(
              widget->GetNativeData(NS_NATIVE_WINDOW));
        }
      }
    }
  }

  int64_t prefLastChanged = 0;
  bool isBlankPassword = false;

  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUser",
      [promiseHandle, aMessageText = nsAutoString(aMessageText),
       aCaptionText = nsAutoString(aCaptionText), hwndParent, isBlankPassword,
       prefLastChanged]() mutable {
        BackgroundReauthenticateUser(promiseHandle, aMessageText, aCaptionText,
                                     hwndParent, isBlankPassword,
                                     prefLastChanged);
      }));

  nsCOMPtr<nsIEventTarget> target(
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID));
  if (!target) {
    return NS_ERROR_FAILURE;
  }
  rv = target->Dispatch(runnable, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  promiseHandle.forget(promiseOut);
  return NS_OK;
}
