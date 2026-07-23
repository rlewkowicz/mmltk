/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "UtilityProcessImpl.h"

#include "mozilla/GeckoArgs.h"




namespace mozilla::ipc {

UtilityProcessImpl::~UtilityProcessImpl() = default;


bool UtilityProcessImpl::Init(int aArgc, char* aArgv[]) {
  Maybe<uint64_t> utilityProcessKind = geckoargs::sUtilityProcessKind.Get(aArgc, aArgv);
  if (utilityProcessKind.isNothing()) {
    return false;
  }

  if (*utilityProcessKind >= UtilityProcessKind::COUNT) {
    return false;
  }


  Maybe<const char*> parentBuildID =
      geckoargs::sParentBuildID.Get(aArgc, aArgv);
  if (parentBuildID.isNothing()) {
    return false;
  }

  if (!ProcessChild::InitPrefs(aArgc, aArgv)) {
    return false;
  }

#if defined(MOZ_MEMORY) && defined(DEBUG)
  jemalloc_stats_t stats;
  jemalloc_stats(&stats);
  MOZ_ASSERT(stats.opt_randomize_small,
             "Utility process should randomize small allocations");
#endif

  return mUtility->Init(TakeInitialEndpoint(), nsCString(*parentBuildID),
                        UtilityProcessKind(*utilityProcessKind));
}

void UtilityProcessImpl::CleanUp() { NS_ShutdownXPCOM(nullptr); }

}  
