/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBTRANSPORT_API_WEBTRANSPORTRECEIVESTREAM_H_
#define DOM_WEBTRANSPORT_API_WEBTRANSPORTRECEIVESTREAM_H_

#include "mozilla/dom/ReadableStream.h"

namespace mozilla::ipc {
class DataPipeReceiver;
}

namespace mozilla::dom {

class WebTransport;

class WebTransportReceiveStream final : public ReadableStream {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(WebTransportReceiveStream,
                                           ReadableStream)

  WebTransportReceiveStream(nsIGlobalObject* aGlobal, WebTransport* aTransport);

  static already_AddRefed<WebTransportReceiveStream> Create(
      WebTransport* aWebTransport, nsIGlobalObject* aGlobal, uint64_t aStreamId,
      mozilla::ipc::DataPipeReceiver* receiver, ErrorResult& aRv);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<Promise> GetStats();

 private:
  ~WebTransportReceiveStream() override { mozilla::DropJSObjects(this); }

  RefPtr<WebTransport> mTransport;
};
}  

#endif
