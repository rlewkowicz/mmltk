/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MOZILLA_GFX_LAYERS_SYNCOBJECT_H)
#define MOZILLA_GFX_LAYERS_SYNCOBJECT_H

#include "mozilla/gfx/FileHandleWrapper.h"
#include "mozilla/RefCounted.h"

struct ID3D11Device;

namespace mozilla {
namespace layers {

typedef uintptr_t SyncHandle;

class SyncObjectHost : public RefCounted<SyncObjectHost> {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(SyncObjectHost)
  virtual ~SyncObjectHost() = default;

  virtual bool Init() = 0;

  virtual SyncHandle GetSyncHandle() = 0;

  virtual bool Synchronize(bool aFallible = false) = 0;

 protected:
  SyncObjectHost() = default;
};

class SyncObjectClient : public external::AtomicRefCounted<SyncObjectClient> {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(SyncObjectClient)
  virtual ~SyncObjectClient() = default;

  static already_AddRefed<SyncObjectClient>
  CreateSyncObjectClientForContentDevice(SyncHandle aHandle);

  enum class SyncType {
    D3D11,
  };

  virtual SyncType GetSyncType() = 0;

  virtual bool Synchronize(bool aFallible = false) = 0;

  virtual bool IsSyncObjectValid() = 0;

  virtual void EnsureInitialized() = 0;

 protected:
  SyncObjectClient() = default;
};

}  
}  

#endif
