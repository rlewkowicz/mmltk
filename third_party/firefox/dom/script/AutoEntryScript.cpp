/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/AutoEntryScript.h"

#include <stdint.h>

#include "jsapi.h"
#include "mozilla/Assertions.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsIDocShell.h"
#include "nsJSUtils.h"
#include "nsPIDOMWindow.h"
#include "nsPIDOMWindowInlines.h"
#include "nsString.h"
#include "xpcpublic.h"

namespace mozilla::dom {

namespace {
#ifdef DEBUG
void AssertIfNotSafeToRunScript() {
  if (nsContentUtils::IsSafeToRunScript()) {
    return;
  }

  if (AutoAllowLegacyScriptExecution::IsAllowed()) {
    return;
  }

  MOZ_ASSERT(false, "is it safe to run script?");
}
#endif

}  

AutoEntryScript::AutoEntryScript(nsIGlobalObject* aGlobalObject,
                                 const char* aReason, bool aIsMainThread)
    : AutoJSAPI(aGlobalObject, aIsMainThread, eEntryScript),
      mWebIDLCallerPrincipal(nullptr)
      ,
      mCallerOverride(cx()),
      mJSThreadExecution(aGlobalObject, aIsMainThread) {
  MOZ_ASSERT(aGlobalObject);

  if (aIsMainThread) {
#ifdef DEBUG
    AssertIfNotSafeToRunScript();
#endif
    mScriptActivity.emplace(true);
  }
}

AutoEntryScript::AutoEntryScript(JSObject* aObject, const char* aReason,
                                 bool aIsMainThread)
    : AutoEntryScript(xpc::NativeGlobal(aObject), aReason, aIsMainThread) {
}

AutoEntryScript::~AutoEntryScript() = default;

}  
