/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_ipc_glue_UtilityProcessParent_h_)
#define _include_ipc_glue_UtilityProcessParent_h_
#include "mozilla/ipc/PUtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessHost.h"
#include "mozilla/dom/MemoryReportRequest.h"

#include "mozilla/RefPtr.h"

namespace mozilla {

namespace ipc {

class UtilityProcessHost;

class UtilityProcessParent final : public PUtilityProcessParent {
  typedef mozilla::dom::MemoryReportRequestHost MemoryReportRequestHost;

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityProcessParent, override);
  friend class UtilityProcessHost;

  explicit UtilityProcessParent(UtilityProcessHost* aHost);

  mozilla::ipc::IPCResult RecvAddMemoryReport(const MemoryReport& aReport);

  bool SendRequestMemoryReport(const uint32_t& aGeneration,
                               const bool& aAnonymize,
                               const bool& aMinimizeMemoryUsage,
                               const Maybe<ipc::FileDescriptor>& aDMDFile);


  mozilla::ipc::IPCResult RecvInitCompleted();

  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  UtilityProcessHost* mHost;
  UniquePtr<MemoryReportRequestHost> mMemoryReportRequest{};

  ~UtilityProcessParent();

  static void Destroy(RefPtr<UtilityProcessParent> aParent);
};

}  

}  

#endif
