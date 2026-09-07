/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ChannelInfo_h
#define mozilla_dom_ChannelInfo_h

#include "nsCOMPtr.h"
#include "nsITransportSecurityInfo.h"
#include "nsString.h"

class nsIChannel;
class nsIGlobalObject;
class nsIURI;

namespace mozilla {
namespace dom {

class Document;

class ChannelInfo final {
 public:
  ChannelInfo() : mInited(false) {}

  ChannelInfo(const ChannelInfo& aRHS) = default;

  ChannelInfo& operator=(const ChannelInfo& aRHS) = default;

  void InitFromDocument(Document* aDoc);
  void InitFromChannel(nsIChannel* aChannel);
  void InitFromChromeGlobal(nsIGlobalObject* aGlobal);
  void InitFromTransportSecurityInfo(nsITransportSecurityInfo* aSecurityInfo);

  nsresult ResurrectInfoOnChannel(nsIChannel* aChannel);

  bool IsInitialized() const { return mInited; }

  already_AddRefed<nsITransportSecurityInfo> SecurityInfo() const;

 private:
  void SetSecurityInfo(nsITransportSecurityInfo* aSecurityInfo);

  nsCOMPtr<nsITransportSecurityInfo> mSecurityInfo;
  bool mInited;
};

}  
}  

#endif  // mozilla_dom_ChannelInfo_h
