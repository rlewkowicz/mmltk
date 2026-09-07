/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_ipc_glue_UtilityProcessChild_h_)
#define _include_ipc_glue_UtilityProcessChild_h_
#include "mozilla/ipc/PUtilityProcessChild.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/ipc/UtilityMediaServiceParent.h"

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
#  include "mozilla/psm/PKCS11ModuleChild.h"
#endif

#include "mozilla/PRemoteMediaManagerParent.h"
#include "mozilla/ipc/AsyncBlockers.h"
#include "mozilla/dom/JSOracleChild.h"

namespace mozilla::dom {
class PJSOracleChild;
}  

namespace mozilla::ipc {

class UtilityProcessHost;

class UtilityProcessChild final : public PUtilityProcessChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityProcessChild, override);

  UtilityProcessChild();

  static RefPtr<UtilityProcessChild> GetSingleton();
  static RefPtr<UtilityProcessChild> Get();

  UtilityProcessKind mKind{};

  bool Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
            const nsCString& aParentBuildID, UtilityProcessKind aUtilityProcessKind);

  mozilla::ipc::IPCResult RecvInit(
      const Maybe<ipc::FileDescriptor>& aBrokerFd);
  mozilla::ipc::IPCResult RecvPreferenceUpdate(const Pref& pref);

  mozilla::ipc::IPCResult RecvRequestMemoryReport(
      const uint32_t& generation, const bool& anonymize,
      const bool& minimizeMemoryUsage,
      const Maybe<ipc::FileDescriptor>& DMDFile,
      const RequestMemoryReportResolver& aResolver);

  mozilla::ipc::IPCResult RecvStartUtilityMediaService(
      Endpoint<PUtilityMediaServiceParent>&& aEndpoint,
      nsTArray<gfx::GfxVarUpdate>&& aUpdates);

  mozilla::ipc::IPCResult RecvStartJSOracleService(
      Endpoint<dom::PJSOracleChild>&& aEndpoint);


  AsyncBlockers& AsyncShutdownService() { return mShutdownBlockers; }

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  IPCResult RecvStartPKCS11ModuleService(
      Endpoint<PPKCS11ModuleChild>&& aEndpoint, nsCString&& aProfilePath);
#endif

  void ActorDestroy(ActorDestroyReason aWhy) override;


  RefPtr<UtilityMediaServiceParent> GetMediaService() const {
    return mUtilityMediaServiceInstance;
  }

 protected:
  friend class UtilityProcessImpl;
  ~UtilityProcessChild();

 private:
  TimeStamp mChildStartTime;
  RefPtr<UtilityMediaServiceParent> mUtilityMediaServiceInstance{};
  RefPtr<dom::JSOracleChild> mJSOracleInstance{};
#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  RefPtr<psm::PKCS11ModuleChild> mPKCS11ModuleInstance;
#endif

  AsyncBlockers mShutdownBlockers;
};

}  

#endif
