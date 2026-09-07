/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_ActioChild_h
#define mozilla_dom_cache_ActioChild_h

#include "mozilla/dom/SafeRefPtr.h"

namespace mozilla::dom::cache {

class CacheWorkerRef;

class ActorChild {
 public:
  virtual void StartDestroy() = 0;
  virtual void NoteDeletedActor() {  }

 protected:
  ActorChild() = default;
  ~ActorChild() = default;
};

class CacheActorChild : public ActorChild {
 public:
  void SetWorkerRef(SafeRefPtr<CacheWorkerRef> aWorkerRef);
  const SafeRefPtr<CacheWorkerRef>& GetWorkerRefPtr() const;
  void RemoveWorkerRef();

 protected:
  CacheActorChild() = default;
  ~CacheActorChild();

 private:
  SafeRefPtr<CacheWorkerRef> mWorkerRef;
};

}  

#endif  // mozilla_dom_cache_ActioChild_h
