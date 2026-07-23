/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBTRANSPORT_API_WEBTRANSPORTSENDSTREAM_H_
#define DOM_WEBTRANSPORT_API_WEBTRANSPORTSENDSTREAM_H_

#include "mozilla/dom/WritableStream.h"

namespace mozilla::ipc {
class DataPipeSender;
}

namespace mozilla::dom {

class WebTransport;

class WebTransportSendStream final : public WritableStream {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(WebTransportSendStream,
                                           WritableStream)

  WebTransportSendStream(nsIGlobalObject* aGlobal, WebTransport* aTransport);

  static already_AddRefed<WebTransportSendStream> Create(
      WebTransport* aWebTransport, nsIGlobalObject* aGlobal, uint64_t aStreamId,
      mozilla::ipc::DataPipeSender* aSender, Maybe<int64_t> aSendOrder,
      ErrorResult& aRv);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  Nullable<int64_t> GetSendOrder() { return mSendOrder; }

  void SetSendOrder(Nullable<int64_t> aSendOrder);

  already_AddRefed<Promise> GetStats();

 private:
  ~WebTransportSendStream() override { mozilla::DropJSObjects(this); };

  RefPtr<WebTransport> mTransport;
  uint64_t mStreamId;
  Nullable<int64_t> mSendOrder;
};
}  

#endif
