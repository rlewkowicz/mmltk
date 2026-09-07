/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_NavigationTransition_h_
#define mozilla_dom_NavigationTransition_h_

#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupports.h"
#include "nsWrapperCache.h"

class nsIGlobalObject;

namespace mozilla::dom {

class NavigationHistoryEntry;
enum class NavigationType : uint8_t;
class Promise;

class NavigationTransition final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(NavigationTransition)

  NavigationTransition(nsIGlobalObject* aGlobalObject,
                       NavigationType aNavigationType,
                       NavigationHistoryEntry* aFrom, Promise* aCommitted,
                       Promise* aFinished);

  enum NavigationType NavigationType() const;
  NavigationHistoryEntry* From() const;

  Promise* Committed() const;

  Promise* Finished() const;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;
  nsIGlobalObject* GetParentObject() const;

 private:
  ~NavigationTransition() = default;

  nsCOMPtr<nsIGlobalObject> mGlobalObject;

  enum NavigationType mNavigationType;

  RefPtr<NavigationHistoryEntry> mFrom;

  RefPtr<Promise> mCommitted;

  RefPtr<Promise> mFinished;
};

}  

#endif  // mozilla_dom_NavigationTransition_h_
