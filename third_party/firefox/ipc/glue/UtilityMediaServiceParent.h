/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_ipc_glue_UtilityMediaServiceParent_h_
#define _include_ipc_glue_UtilityMediaServiceParent_h_

#include "mozilla/PRemoteMediaManagerParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/PUtilityMediaServiceParent.h"

#include "mozilla/ipc/UtilityProcessTypes.h"

#include "nsThreadManager.h"

namespace mozilla::ipc {

class UtilityMediaServiceParent final : public PUtilityMediaServiceParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityMediaServiceParent, override);

  explicit UtilityMediaServiceParent(
      nsTArray<mozilla::gfx::GfxVarUpdate>&& aUpdates);

  static void GenericPreloadForSandbox();
  static void WMFPreloadForSandbox();

  void Start(Endpoint<PUtilityMediaServiceParent>&& aEndpoint);

  mozilla::ipc::IPCResult RecvNewContentRemoteMediaManager(
      Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
      const ContentParentId& aParentId);

  IPCResult RecvUpdateVar(const nsTArray<mozilla::gfx::GfxVarUpdate>& aUpdate);

 private:
  ~UtilityMediaServiceParent();

  const UtilityProcessKind mKind;
  TimeStamp mUtilityMediaServiceParentStart;
};

}  

#endif  // _include_ipc_glue_UtilityMediaServiceParent_h_
