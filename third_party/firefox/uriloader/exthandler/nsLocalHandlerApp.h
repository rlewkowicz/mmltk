/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(_nsLocalHandlerAppImpl_h_)
#define _nsLocalHandlerAppImpl_h_

#include "nsString.h"
#include "nsIMIMEInfo.h"
#include "nsIFile.h"
#include "nsTArray.h"

#include <functional>

class nsLocalHandlerApp : public nsILocalHandlerApp {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIHANDLERAPP
  NS_DECL_NSILOCALHANDLERAPP

  nsLocalHandlerApp() = default;

  nsLocalHandlerApp(const char16_t* aName, nsIFile* aExecutable)
      : mName(aName), mExecutable(aExecutable) {}

  nsLocalHandlerApp(const nsAString& aName, nsIFile* aExecutable)
      : mName(aName), mExecutable(aExecutable) {}

 protected:
  virtual ~nsLocalHandlerApp() = default;

  virtual std::function<nsresult(nsString&)>
  GetPrettyNameOnNonMainThreadCallback();

  nsString mName;
  nsString mDetailedDescription;
  nsTArray<nsString> mParameters;
  nsCOMPtr<nsIFile> mExecutable;

  nsresult LaunchWithIProcess(const nsCString& aArg);
};

typedef nsLocalHandlerApp PlatformLocalHandlerApp_t;

#endif
