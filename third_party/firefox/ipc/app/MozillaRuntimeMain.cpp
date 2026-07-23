/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsXULAppAPI.h"
#include "XREChildData.h"
#include "mozilla/Bootstrap.h"
#include "mozilla/ProcessType.h"

using namespace mozilla;

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return 3;
  }
  SetGeckoProcessType(argv[--argc]);
  SetGeckoChildID(argv[--argc]);

  auto bootstrapResult = GetBootstrap();
  if (bootstrapResult.isErr()) {
    return 2;
  }

  Bootstrap::UniquePtr bootstrap = bootstrapResult.unwrap();

#if defined(MOZ_ENABLE_FORKSERVER)
  if (GetGeckoProcessType() == GeckoProcessType_ForkServer) {
    bootstrap->NS_LogInit();

    if (bootstrap->XRE_ForkServer(&argc, &argv)) {
      bootstrap->NS_LogTerm();
      return 0;
    }
  }
#endif

#ifdef HAS_DLL_BLOCKLIST
  uint32_t initFlags = eDllBlocklistInitFlagIsChildProcess;
  SetDllBlocklistProcessTypeFlags(initFlags, GetGeckoProcessType());
  DllBlocklist_Initialize(initFlags);
#endif

  XREChildData childData;


  nsresult rv = bootstrap->XRE_InitChildProcess(argc, argv, &childData);

#if defined(DEBUG) && defined(HAS_DLL_BLOCKLIST)
  DllBlocklist_Shutdown();
#endif

  return NS_FAILED(rv) ? 1 : 0;
}
