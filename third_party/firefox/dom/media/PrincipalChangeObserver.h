/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_PRINCIPALCHANGEOBSERVER_H_
#define MOZILLA_PRINCIPALCHANGEOBSERVER_H_

namespace mozilla::dom {

template <typename T>
class PrincipalChangeObserver {
 public:
  virtual void PrincipalChanged(T* aArg) = 0;
};

}  

#endif /* MOZILLA_PRINCIPALCHANGEOBSERVER_H_ */
