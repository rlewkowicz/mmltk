/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WebBrowserPersistRemoteDocument_h_
#define WebBrowserPersistRemoteDocument_h_

#include "mozilla/PWebBrowserPersistDocumentParent.h"
#include "nsCOMPtr.h"
#include "nsIInputStream.h"
#include "nsIWebBrowserPersistDocument.h"

class nsIPrincipal;


namespace mozilla {

class WebBrowserPersistDocumentParent;

class WebBrowserPersistRemoteDocument final
    : public nsIWebBrowserPersistDocument {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIWEBBROWSERPERSISTDOCUMENT

 private:
  using Attrs = WebBrowserPersistDocumentAttrs;
  WebBrowserPersistDocumentParent* mActor;
  Attrs mAttrs;
  RefPtr<dom::SessionHistoryEntry> mSHEntry;
  nsCOMPtr<nsICookieJarSettings> mCookieJarSettings;
  nsCOMPtr<nsIInputStream> mPostData;
  nsCOMPtr<nsIPrincipal> mPrincipal;

  friend class WebBrowserPersistDocumentParent;
  WebBrowserPersistRemoteDocument(WebBrowserPersistDocumentParent* aActor,
                                  const Attrs& aAttrs,
                                  nsIInputStream* aPostData);
  ~WebBrowserPersistRemoteDocument();

  void ActorDestroy(void);
};

}  

#endif  // WebBrowserPersistRemoteDocument_h_
