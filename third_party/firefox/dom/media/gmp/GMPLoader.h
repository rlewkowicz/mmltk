/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMP_LOADER_H_
#define GMP_LOADER_H_

#include <stdint.h>

#include "gmp-entrypoints.h"
#include "mozilla/UniquePtr.h"
#include "nsString.h"
#include "prlink.h"


namespace mozilla::gmp {

class SandboxStarter {
 public:
  virtual ~SandboxStarter() = default;
  virtual bool Start(const char* aLibPath) = 0;
};

class GMPAdapter {
 public:
  virtual ~GMPAdapter() = default;
  virtual void SetAdaptee(PRLibrary* aLib) = 0;

  virtual GMPErr GMPInit(const GMPPlatformAPI* aPlatformAPI) = 0;
  virtual GMPErr GMPGetAPI(const char* aAPIName, void* aHostAPI,
                           void** aPluginAPI, const nsACString& aKeySystem) = 0;
  virtual void GMPShutdown() = 0;
};

class GMPLoader {
 public:
  GMPLoader();

  bool Load(const char* aUTF8LibPath, uint32_t aLibPathLen,
            const GMPPlatformAPI* aPlatformAPI, GMPAdapter* aAdapter = nullptr);

  GMPErr GetAPI(const char* aAPIName, void* aHostAPI, void** aPluginAPI,
                const nsACString& aKeySystem);

  void Shutdown();

  bool CanSandbox() const;

 private:
  UniquePtr<SandboxStarter> mSandboxStarter;
  UniquePtr<GMPAdapter> mAdapter;
};

}  

#endif  // GMP_LOADER_H_
