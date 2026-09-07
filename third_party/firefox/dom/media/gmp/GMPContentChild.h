/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMPContentChild_h_
#define GMPContentChild_h_

#include "mozilla/gmp/PGMPContentChild.h"


namespace mozilla::gmp {

class GMPChild;

class GMPContentChild final : public PGMPContentChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(GMPContentChild, final)

  explicit GMPContentChild(GMPChild* aChild) : mGMPChild(aChild) {}

  MessageLoop* GMPMessageLoop();

  mozilla::ipc::IPCResult RecvPGMPVideoDecoderConstructor(
      PGMPVideoDecoderChild* aActor) override;
  mozilla::ipc::IPCResult RecvPGMPVideoEncoderConstructor(
      PGMPVideoEncoderChild* aActor) override;
  mozilla::ipc::IPCResult RecvPChromiumCDMConstructor(
      PChromiumCDMChild* aActor, const nsACString& aKeySystem) override;

  already_AddRefed<PGMPVideoDecoderChild> AllocPGMPVideoDecoderChild();

  already_AddRefed<PGMPVideoEncoderChild> AllocPGMPVideoEncoderChild();

  already_AddRefed<PChromiumCDMChild> AllocPChromiumCDMChild(
      const nsACString& aKeySystem);


  void ActorDestroy(ActorDestroyReason aWhy) override;
  void ProcessingError(Result aCode, const char* aReason) override;

  void CloseActive();
  bool IsUsed();

  GMPChild* mGMPChild;

 private:
  ~GMPContentChild() = default;
};

}  

#endif  // GMPContentChild_h_
