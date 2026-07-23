/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsSyncLoadService_h_
#define nsSyncLoadService_h_

#include "mozilla/AlreadyAddRefed.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIReferrerInfo.h"
#include "nscore.h"

class nsICookieJarSettings;
class nsIInputStream;
class nsILoadGroup;
class nsIStreamListener;
class nsIURI;
class nsIPrincipal;
class nsIChannel;

namespace mozilla::dom {
class Document;
}  

class nsSyncLoadService {
 public:
  static nsresult LoadDocument(
      nsIURI* aURI, nsContentPolicyType aContentPolicyType,
      mozilla::dom::Document* aLoaderDoc, nsIPrincipal* aLoaderPrincipal,
      nsSecurityFlags aSecurityFlags, nsILoadGroup* aLoadGroup,
      nsICookieJarSettings* aCookieJarSettings, bool aForceToXML,
      mozilla::dom::ReferrerPolicy aReferrerPolicy,
      mozilla::dom::Document** aResult);

  static nsresult PushSyncStreamToListener(already_AddRefed<nsIInputStream> aIn,
                                           nsIStreamListener* aListener,
                                           nsIChannel* aChannel);
};

#endif  // nsSyncLoadService_h_
