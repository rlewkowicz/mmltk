/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Observer_h
#define mozilla_Observer_h

#include "nsTObserverArray.h"

namespace mozilla {

template <class T>
class Observer {
 public:
  virtual ~Observer() = default;
  virtual void Notify(const T& aParam) = 0;
};

template <class T>
class ObserverList {
 public:
  void AddObserver(Observer<T>* aObserver) {
    mObservers.AppendElementUnlessExists(aObserver);
  }

  bool RemoveObserver(Observer<T>* aObserver) {
    return mObservers.RemoveElement(aObserver);
  }

  uint32_t Length() { return mObservers.Length(); }

  void Broadcast(const T& aParam) {
    for (Observer<T>* obs : mObservers.ForwardRange()) {
      obs->Notify(aParam);
    }
  }

 protected:
  nsTObserverArray<Observer<T>*> mObservers;
};

}  

#endif  // mozilla_Observer_h
