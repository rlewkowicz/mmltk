/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RDDProcessImpl.h"

#include "mozilla/GeckoArgs.h"


namespace mozilla {

using namespace ipc;

RDDProcessImpl::~RDDProcessImpl() = default;

bool RDDProcessImpl::Init(int aArgc, char* aArgv[]) {
  Maybe<const char*> parentBuildID =
      geckoargs::sParentBuildID.Get(aArgc, aArgv);
  if (parentBuildID.isNothing()) {
    return false;
  }

  if (!ProcessChild::InitPrefs(aArgc, aArgv)) {
    return false;
  }

  return mRDD->Init(TakeInitialEndpoint(), *parentBuildID);
}

void RDDProcessImpl::CleanUp() { NS_ShutdownXPCOM(nullptr); }

}  
