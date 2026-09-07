/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GMPParent_h_)
#define GMPParent_h_

#include "GMPNativeTypes.h"
#include "GMPProcessParent.h"
#include "GMPServiceParent.h"
#include "GMPStorageParent.h"
#include "GMPTimerParent.h"
#include "GMPVideoDecoderParent.h"
#include "GMPVideoEncoderParent.h"
#include "mozilla/Atomics.h"
#include "mozilla/MozPromise.h"
#include "mozilla/gmp/PGMPParent.h"
#include "nsCOMPtr.h"
#include "nsIFile.h"
#include "nsISupports.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nscore.h"

namespace mozilla::gmp {

class GMPCapability {
 public:
  explicit GMPCapability() = default;
  GMPCapability(GMPCapability&& aOther)
      : mAPIName(std::move(aOther.mAPIName)),
        mAPITags(std::move(aOther.mAPITags)) {}
  explicit GMPCapability(const nsACString& aAPIName) : mAPIName(aAPIName) {}
  explicit GMPCapability(const GMPCapability& aOther) = default;
  nsCString mAPIName;
  CopyableTArray<nsCString> mAPITags;

  static bool Supports(const nsTArray<GMPCapability>& aCapabilities,
                       const nsACString& aAPI,
                       const nsTArray<nsCString>& aTags);

  static bool Supports(const nsTArray<GMPCapability>& aCapabilities,
                       const nsACString& aAPI, const nsCString& aTag);
};

enum class GMPState : uint32_t {
  NotLoaded,
  Loaded,
  Unloading,
  Closing,
  Closed
};

class GMPContentParent;

class GMPParent final : public PGMPParent {
  friend class PGMPParent;

 public:
  static constexpr GeckoProcessType PROCESS_TYPE = GeckoProcessType_GMPlugin;

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(GMPParent, final)

  GMPParent();


  RefPtr<GenericPromise> Init(GeckoMediaPluginServiceParent* aService,
                              nsIFile* aPluginDir);
  void CloneFrom(const GMPParent* aOther);

  void Crash();

  nsresult LoadProcess();

  void CloseIfUnused();

  void CloseActive(bool aDieWhenUnloaded);

  void MarkForDeletion();
  bool IsMarkedForDeletion();

  void Shutdown();

  void DeleteProcess();

  GMPState State() const;
  nsCOMPtr<nsISerialEventTarget> GMPEventTarget();

  void OnPreferenceChange(const mozilla::dom::Pref& aPref);


  void SetNodeId(const nsACString& aNodeId);
  const nsACString& GetNodeId() const { return mNodeId; }

  const nsCString& GetDisplayName() const;
  const nsCString& GetVersion() const;
  uint32_t GetPluginId() const;
  GMPPluginType GetPluginType() const { return mPluginType; }
  nsString GetPluginBaseName() const;

  bool CanBeSharedCrossNodeIds() const;

  bool CanBeUsedFrom(const nsACString& aNodeId) const;

  already_AddRefed<nsIFile> GetDirectory() {
    return nsCOMPtr<nsIFile>(mDirectory).forget();
  }

  void ChildTerminated();

  bool OpenPGMPContent();

  void GetGMPContentParent(
      UniquePtr<MozPromiseHolder<GetGMPContentParentPromise>>&& aPromiseHolder);
  already_AddRefed<GMPContentParent> ForgetGMPContentParent();

  bool EnsureProcessLoaded(base::ProcessId* aID);

  void IncrementGMPContentChildCount();

  const nsTArray<GMPCapability>& GetCapabilities() const {
    return mCapabilities;
  }

 private:
  ~GMPParent();
  void UpdatePluginType();

  RefPtr<GeckoMediaPluginServiceParent> mService;
  bool EnsureProcessLoaded();
  RefPtr<GenericPromise> ReadGMPMetaData();
  RefPtr<GenericPromise> ReadGMPInfoFile(nsIFile* aFile);
  RefPtr<GenericPromise> ParseChromiumManifest(
      const nsAString& aJSON);  
  RefPtr<GenericPromise> ReadChromiumManifestFile(
      nsIFile* aFile);  
  void ActorDestroy(ActorDestroyReason aWhy) override;

  already_AddRefed<PGMPStorageParent> AllocPGMPStorageParent();
  already_AddRefed<PGMPTimerParent> AllocPGMPTimerParent();

  mozilla::ipc::IPCResult RecvPGMPContentChildDestroyed();


  bool IsUsed() {
    return mGMPContentChildCount > 0 || !mGetContentParentPromises.IsEmpty();
  }

  void ResolveGetContentParentPromises();
  void RejectGetContentParentPromises();

  Atomic<GMPState> mState;
  nsCOMPtr<nsIFile> mDirectory;  
  nsString mName;  
  nsCString mDisplayName;  
  nsCString mDescription;  
  nsCString mVersion;
  nsCString mLibs;
  nsString mAdapter;
  const uint32_t mPluginId;
  GMPPluginType mPluginType = GMPPluginType::Unknown;
  nsTArray<GMPCapability> mCapabilities;
  GMPProcessParent* mProcess;
  bool mDeleteProcessOnlyOnUnload;
  bool mAbnormalShutdownInProgress;
  bool mIsBlockingDeletion;

  bool mCanDecrypt;

  nsCString mNodeId;
  RefPtr<GMPContentParent> mGMPContentParent;
  nsTArray<UniquePtr<MozPromiseHolder<GetGMPContentParentPromise>>>
      mGetContentParentPromises;
  uint32_t mGMPContentChildCount;

  int mChildPid;

#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  uint32_t mChildLaunchArch;
#endif
  const nsCOMPtr<nsISerialEventTarget> mMainThread;
};

}  

#endif
