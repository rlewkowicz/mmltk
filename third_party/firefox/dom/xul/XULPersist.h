/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_XULPersist_h
#define mozilla_dom_XULPersist_h

#include "nsStubDocumentObserver.h"

#include "nsCOMPtr.h"
class nsIXULStore;

template <typename T>
class nsCOMArray;

namespace mozilla::dom {

class XULPersist final : public nsStubDocumentObserver {
 public:
  NS_DECL_ISUPPORTS

  explicit XULPersist(Document* aDocument);
  void Init();
  void DropDocumentReference();

  NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED

 protected:
  void Persist(mozilla::dom::Element* aElement, nsAtom* aAttribute);

 private:
  ~XULPersist();
  nsresult ApplyPersistentAttributes();
  nsresult ApplyPersistentAttributesToElements(const nsAString& aID,
                                               const nsAString& aDocURI,
                                               nsCOMArray<Element>& aElements);

  nsCOMPtr<nsIXULStore> mLocalStore;

  Document* MOZ_NON_OWNING_REF mDocument;
};

}  

#endif  // mozilla_dom_XULPersist_h
