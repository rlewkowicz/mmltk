/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_GenericFactory_h
#define mozilla_GenericFactory_h

#include "nsIFactory.h"

namespace mozilla {

class GenericFactory final : public nsIFactory {
  ~GenericFactory() = default;

 public:
  typedef nsresult (*ConstructorProcPtr)(const nsIID& aIID, void** aResult);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIFACTORY

  explicit GenericFactory(ConstructorProcPtr aCtor) : mCtor(aCtor) {
    NS_ASSERTION(mCtor, "GenericFactory with no constructor");
  }

 private:
  ConstructorProcPtr mCtor;
};

}  

#endif  // mozilla_GenericFactory_h
