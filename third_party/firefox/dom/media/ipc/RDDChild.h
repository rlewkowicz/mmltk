/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_dom_media_ipc_RDDChild_h_
#define _include_dom_media_ipc_RDDChild_h_
#include "mozilla/PRDDChild.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/GPUProcessListener.h"
#include "mozilla/gfx/gfxVarReceiver.h"

namespace mozilla {


namespace dom {
class MemoryReportRequestHost;
}  

class RDDProcessHost;

class RDDChild final : public PRDDChild,
                       public gfx::gfxVarReceiver,
                       public gfx::GPUProcessListener {
  typedef mozilla::dom::MemoryReportRequestHost MemoryReportRequestHost;

 public:
  NS_INLINE_DECL_REFCOUNTING(RDDChild, final)

  explicit RDDChild(RDDProcessHost* aHost);

  bool Init();

  void OnCompositorUnexpectedShutdown() override;
  void OnVarChanged(const nsTArray<GfxVarUpdate>& aVar) override;

  void ActorDestroy(ActorDestroyReason aWhy) override;

  mozilla::ipc::IPCResult RecvAddMemoryReport(const MemoryReport& aReport);
  mozilla::ipc::IPCResult RecvUpdateMediaCodecsSupported(
      const media::MediaCodecsSupported& aSupported);
  bool SendRequestMemoryReport(const uint32_t& aGeneration,
                               const bool& aAnonymize,
                               const bool& aMinimizeMemoryUsage,
                               const Maybe<ipc::FileDescriptor>& aDMDFile);

  static void Destroy(RefPtr<RDDChild>&& aChild);

 private:
  ~RDDChild();

  RDDProcessHost* mHost;
  UniquePtr<MemoryReportRequestHost> mMemoryReportRequest;
};

}  

#endif  // _include_dom_media_ipc_RDDChild_h_
