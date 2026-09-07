/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMPChild_h_
#define GMPChild_h_

#include "GMPLoader.h"
#include "GMPStorageChild.h"
#include "GMPTimerChild.h"
#include "gmp-entrypoints.h"
#include "mozilla/gmp/PGMPChild.h"
#include "prlink.h"

namespace mozilla {

namespace gmp {

class GMPContentChild;

class GMPChild : public PGMPChild {
  friend class PGMPChild;

 public:
  NS_INLINE_DECL_REFCOUNTING(GMPChild, override)

  GMPChild();

  bool Init(const nsAString& aPluginPath, const char* aParentBuildID,
            mozilla::ipc::UntypedEndpoint&& aEndpoint);
  void Shutdown();
  MessageLoop* GMPMessageLoop();

  GMPTimerChild* GetGMPTimers();
  GMPStorageChild* GetGMPStorage();


 private:
  friend class GMPContentChild;

  virtual ~GMPChild();

  bool GetUTF8LibPath(nsACString& aOutLibPath);

  bool GetPluginName(nsACString& aPluginName) const;

  mozilla::ipc::IPCResult RecvProvideStorageId(const nsCString& aStorageId);

  mozilla::ipc::IPCResult RecvStartPlugin(const nsString& aAdapter);
  mozilla::ipc::IPCResult RecvPreloadLibs(const nsCString& aLibs);

  void GMPContentChildActorDestroy(GMPContentChild* aGMPContentChild);

  mozilla::ipc::IPCResult RecvCrashPluginNow();
  mozilla::ipc::IPCResult RecvCloseActive();

  mozilla::ipc::IPCResult RecvInitGMPContentChild(
      Endpoint<PGMPContentChild>&& aEndpoint);

  mozilla::ipc::IPCResult RecvPreferenceUpdate(const Pref& aPref);

  mozilla::ipc::IPCResult RecvShutdown(ShutdownResolver&& aResolver);


  void ActorDestroy(ActorDestroyReason aWhy) override;
  void ProcessingError(Result aCode, const char* aReason) override;

  GMPErr GetAPI(const char* aAPIName, void* aHostAPI, void** aPluginAPI,
                const nsACString& aKeySystem = ""_ns);

  nsTArray<std::pair<nsCString, nsCString>> MakeCDMHostVerificationPaths(
      const nsACString& aPluginLibPath);

  nsTArray<RefPtr<GMPContentChild>> mGMPContentChildren;

  RefPtr<GMPTimerChild> mTimerChild;
  RefPtr<GMPStorageChild> mStorage;

  MessageLoop* mGMPMessageLoop;
  nsString mPluginPath;
  nsCString mStorageId;
  UniquePtr<GMPLoader> mGMPLoader;
  nsTArray<void*> mLibHandles;
};

}  
}  

#endif  // GMPChild_h_
