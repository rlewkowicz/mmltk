/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPLoader.h"

#include "GMPLog.h"
#include "gmp-entrypoints.h"
#include "prenv.h"
#include "prerror.h"
#include "prlink.h"


namespace mozilla::gmp {
class PassThroughGMPAdapter : public GMPAdapter {
 public:
  ~PassThroughGMPAdapter() override {
    GMPShutdown();
  }

  void SetAdaptee(PRLibrary* aLib) override { mLib = aLib; }

  GMPErr GMPInit(const GMPPlatformAPI* aPlatformAPI) override {
    if (NS_WARN_IF(!mLib)) {
      MOZ_CRASH("Missing library!");
      return GMPGenericErr;
    }
    GMPInitFunc initFunc =
        reinterpret_cast<GMPInitFunc>(PR_FindFunctionSymbol(mLib, "GMPInit"));
    if (!initFunc) {
      MOZ_CRASH("Missing init method!");
      return GMPNotImplementedErr;
    }
    return initFunc(aPlatformAPI);
  }

  GMPErr GMPGetAPI(const char* aAPIName, void* aHostAPI, void** aPluginAPI,
                   const nsACString& ) override {
    if (!mLib) {
      return GMPGenericErr;
    }
    GMPGetAPIFunc getapiFunc = reinterpret_cast<GMPGetAPIFunc>(
        PR_FindFunctionSymbol(mLib, "GMPGetAPI"));
    if (!getapiFunc) {
      return GMPNotImplementedErr;
    }
    return getapiFunc(aAPIName, aHostAPI, aPluginAPI);
  }

  void GMPShutdown() override {
    if (mLib) {
      GMPShutdownFunc shutdownFunc = reinterpret_cast<GMPShutdownFunc>(
          PR_FindFunctionSymbol(mLib, "GMPShutdown"));
      if (shutdownFunc) {
        shutdownFunc();
      }
      PR_UnloadLibrary(mLib);
      mLib = nullptr;
    }
  }

 private:
  PRLibrary* mLib = nullptr;
};


bool GMPLoader::Load(const char* aUTF8LibPath, uint32_t aUTF8LibPathLen,
                     const GMPPlatformAPI* aPlatformAPI, GMPAdapter* aAdapter) {
  PRLibSpec libSpec;

  if (!getenv("MOZ_DISABLE_GMP_SANDBOX") && mSandboxStarter &&
      !mSandboxStarter->Start(aUTF8LibPath)) {
    MOZ_CRASH("Cannot start sandbox!");
    return false;
  }

  libSpec.value.pathname = aUTF8LibPath;
  libSpec.type = PR_LibSpec_Pathname;
  PRLibrary* lib = PR_LoadLibraryWithFlags(libSpec, 0);
  if (!lib) {
    MOZ_CRASH_UNSAFE_PRINTF("Cannot load plugin as library %d %d",
                            PR_GetError(), PR_GetOSError());
    return false;
  }

  mAdapter.reset((!aAdapter) ? new PassThroughGMPAdapter() : aAdapter);
  mAdapter->SetAdaptee(lib);

  if (mAdapter->GMPInit(aPlatformAPI) != GMPNoErr) {
    MOZ_CRASH("Cannot initialize plugin adapter!");
    return false;
  }

  return true;
}

GMPErr GMPLoader::GetAPI(const char* aAPIName, void* aHostAPI,
                         void** aPluginAPI, const nsACString& aKeySystem) {
  return mAdapter->GMPGetAPI(aAPIName, aHostAPI, aPluginAPI, aKeySystem);
}

void GMPLoader::Shutdown() {
  if (mAdapter) {
    mAdapter->GMPShutdown();
  }
}



static UniquePtr<SandboxStarter> MakeSandboxStarter() {
  return nullptr;
}

GMPLoader::GMPLoader() : mSandboxStarter(MakeSandboxStarter()) {}

bool GMPLoader::CanSandbox() const { return !!mSandboxStarter; }

}  
