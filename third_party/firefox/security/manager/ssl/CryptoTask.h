/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_CryptoTask_h
#define mozilla_CryptoTask_h

#include "nsThreadUtils.h"

namespace mozilla {

class CryptoTask : public Runnable {
 public:
  nsresult Dispatch();

 protected:
  CryptoTask() : Runnable("CryptoTask"), mRv(NS_ERROR_NOT_INITIALIZED) {}

  virtual ~CryptoTask() = default;

  virtual nsresult CalculateResult() = 0;

  virtual void CallCallback(nsresult rv) = 0;

 private:
  NS_IMETHOD Run() final;

  nsresult mRv;
};

}  

#endif  // mozilla_CryptoTask_h
