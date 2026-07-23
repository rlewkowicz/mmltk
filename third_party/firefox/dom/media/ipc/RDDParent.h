/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_dom_media_ipc_RDDParent_h_)
#define _include_dom_media_ipc_RDDParent_h_
#include "mozilla/PRDDParent.h"
#include "mozilla/RefPtr.h"
#include "mozilla/ipc/AsyncBlockers.h"


namespace mozilla {

class TimeStamp;
class RDDParent final : public PRDDParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(RDDParent, final)

  RDDParent();

  static RDDParent* GetSingleton();

  ipc::AsyncBlockers& AsyncShutdownService() { return mShutdownBlockers; }

  bool Init(mozilla::ipc::UntypedEndpoint&& aEndpoint,
            const char* aParentBuildID);

  mozilla::ipc::IPCResult RecvInit(nsTArray<GfxVarUpdate>&& vars,
                                   const Maybe<ipc::FileDescriptor>& aBrokerFd);
  mozilla::ipc::IPCResult RecvNewContentRemoteMediaManager(
      Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
      const ContentParentId& aParentId);
  mozilla::ipc::IPCResult RecvInitVideoBridge(
      Endpoint<PVideoBridgeChild>&& aEndpoint,
      const bool& aCreateHardwareDevice,
      const ContentDeviceData& aContentDeviceData);
  mozilla::ipc::IPCResult RecvRequestMemoryReport(
      const uint32_t& generation, const bool& anonymize,
      const bool& minimizeMemoryUsage,
      const Maybe<ipc::FileDescriptor>& DMDFile,
      const RequestMemoryReportResolver& aResolver);
  mozilla::ipc::IPCResult RecvPreferenceUpdate(const Pref& pref);
  mozilla::ipc::IPCResult RecvUpdateVar(const nsTArray<GfxVarUpdate>& var);

  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  ~RDDParent();

  const TimeStamp mLaunchTime;
  ipc::AsyncBlockers mShutdownBlockers;
};

}  

#endif
