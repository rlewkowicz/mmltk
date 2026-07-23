/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsDOMCaretPosition_h
#define nsDOMCaretPosition_h

#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsINode.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {
class DOMRect;
}  

class nsDOMCaretPosition : public nsISupports, public nsWrapperCache {
  using DOMRect = mozilla::dom::DOMRect;

 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(nsDOMCaretPosition)

  nsDOMCaretPosition(nsINode* aNode, uint32_t aOffset);

  uint32_t Offset() const { return mOffset; }

  nsINode* GetOffsetNode() const;

  already_AddRefed<DOMRect> GetClientRect() const;

  void SetAnonymousContentNode(nsINode* aNode) {
    mAnonymousContentNode = aNode;
  }

  nsISupports* GetParentObject() const { return GetOffsetNode(); }

  JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) final;

 protected:
  virtual ~nsDOMCaretPosition();

  uint32_t mOffset;
  nsCOMPtr<nsINode> mOffsetNode;
  nsCOMPtr<nsINode> mAnonymousContentNode;
};
#endif
