/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EnterpriseRoots.h"

#include "PKCS11ModuleDB.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Logging.h"
#include "mozpkix/Result.h"
#include "nsCRT.h"
#include "nsThreadUtils.h"




extern mozilla::LazyLogModule gPIPNSSLog;

using namespace mozilla;
using namespace psm;

void EnterpriseCert::CopyBytes(nsTArray<uint8_t>& dest) const {
  dest.Assign(mDER);
}

pkix::Result EnterpriseCert::GetInput(pkix::Input& input) const {
  return input.Init(mDER.Elements(), mDER.Length());
}

bool EnterpriseCert::GetIsRoot() const { return mIsRoot; }

bool EnterpriseCert::IsKnownRoot(UniqueSECMODModule& rootsModule) {
  if (!rootsModule) {
    return false;
  }

  SECItem certItem = {siBuffer, mDER.Elements(),
                      static_cast<unsigned int>(mDER.Length())};
  AutoSECMODListReadLock lock;
  for (int i = 0; i < rootsModule->slotCount; i++) {
    PK11SlotInfo* slot = rootsModule->slots[i];
    if (PK11_FindEncodedCertInSlot(slot, &certItem, nullptr) !=
        CK_INVALID_HANDLE) {
      return true;
    }
  }
  return false;
}




nsresult GatherEnterpriseCerts(nsTArray<EnterpriseCert>& certs) {
  MOZ_ASSERT(!NS_IsMainThread());
  if (NS_IsMainThread()) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  certs.Clear();
  UniqueSECMODModule rootsModule(SECMOD_FindModule(kRootModuleName.get()));
  return NS_OK;
}
