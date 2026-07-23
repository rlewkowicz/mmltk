/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsStubMutationObserver_h_
#define nsStubMutationObserver_h_

#include "nsIMutationObserver.h"
#include "nsTHashMap.h"

class nsStubMutationObserver : public nsIMutationObserver {
 public:
  NS_DECL_NSIMUTATIONOBSERVER
};

class MutationObserverWrapper;

class nsMultiMutationObserver : public nsIMutationObserver {
 public:
  void AddMutationObserverToNode(nsINode* aNode);

  void RemoveMutationObserverFromNode(nsINode* aNode);

  bool ContainsNode(const nsINode* aNode) const;

 private:
  friend class MutationObserverWrapper;
  nsTHashMap<nsINode*, MutationObserverWrapper*> mWrapperForNode;
};

class nsStubMultiMutationObserver : public nsMultiMutationObserver {
 public:
  NS_DECL_NSIMUTATIONOBSERVER
};

#endif /* !defined(nsStubMutationObserver_h_) */
