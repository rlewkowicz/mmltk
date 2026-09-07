/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WebBrowserPersistDocumentParent_h_
#define WebBrowserPersistDocumentParent_h_

#include "mozilla/Maybe.h"
#include "mozilla/PWebBrowserPersistDocumentParent.h"
#include "nsCOMPtr.h"
#include "nsIWebBrowserPersistDocument.h"


namespace mozilla {

class WebBrowserPersistRemoteDocument;

class WebBrowserPersistDocumentParent final
    : public PWebBrowserPersistDocumentParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(WebBrowserPersistDocumentParent, override)

  WebBrowserPersistDocumentParent();

  void SetOnReady(nsIWebBrowserPersistDocumentReceiver* aOnReady);

  using Attrs = WebBrowserPersistDocumentAttrs;

  mozilla::ipc::IPCResult RecvAttributes(const Attrs& aAttrs,
                                         const Maybe<IPCStream>& aPostStream);
  mozilla::ipc::IPCResult RecvInitFailure(const nsresult& aFailure);

  PWebBrowserPersistResourcesParent* AllocPWebBrowserPersistResourcesParent();
  bool DeallocPWebBrowserPersistResourcesParent(
      PWebBrowserPersistResourcesParent* aActor);

  PWebBrowserPersistSerializeParent* AllocPWebBrowserPersistSerializeParent(
      const WebBrowserPersistURIMap& aMap,
      const nsACString& aRequestedContentType, const uint32_t& aEncoderFlags,
      const uint32_t& aWrapColumn);
  bool DeallocPWebBrowserPersistSerializeParent(
      PWebBrowserPersistSerializeParent* aActor);

  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  virtual ~WebBrowserPersistDocumentParent();

  nsCOMPtr<nsIWebBrowserPersistDocumentReceiver> mOnReady;
  WebBrowserPersistRemoteDocument* mReflection;
};

}  

#endif  // WebBrowserPersistDocumentParent_h_
