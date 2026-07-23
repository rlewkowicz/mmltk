/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ReadIntoRequest_h
#define mozilla_dom_ReadIntoRequest_h

#include "js/TypeDecls.h"
#include "js/Value.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/LinkedList.h"
#include "mozilla/RefPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupportsImpl.h"

namespace mozilla::dom {

struct ReadIntoRequest : public nsISupports,
                         public LinkedListElement<RefPtr<ReadIntoRequest>> {
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(ReadIntoRequest)

  virtual void ChunkSteps(JSContext* aCx, JS::Handle<JS::Value> aChunk,
                          ErrorResult& aRv) = 0;

  MOZ_CAN_RUN_SCRIPT
  virtual void CloseSteps(JSContext* aCx, JS::Handle<JS::Value> aChunk,
                          ErrorResult& aRv) = 0;

  virtual void ErrorSteps(JSContext* aCx, JS::Handle<JS::Value> e,
                          ErrorResult& aRv) = 0;

 protected:
  virtual ~ReadIntoRequest() = default;
};

}  

#endif
