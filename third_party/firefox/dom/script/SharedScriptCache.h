/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SharedScriptCache_h
#define mozilla_dom_SharedScriptCache_h

#include "PLDHashTable.h"            // PLDHashEntryHdr
#include "js/TypeDecls.h"            // JSContext, JS::MutableHandle, JS::Value
#include "js/loader/LoadedScript.h"  // JS::loader::LoadedScript
#include "js/loader/ScriptFetchOptions.h"    // JS::loader::ScriptFetchOptions
#include "js/loader/ScriptKind.h"            // JS::loader::ScriptKind
#include "js/loader/ScriptLoadRequest.h"     // JS::loader::ScriptLoadRequest
#include "mozilla/CORSMode.h"                // mozilla::CORSMode
#include "mozilla/MemoryReporting.h"         // MallocSizeOf
#include "mozilla/Mutex.h"                   // Mutex, GUARDED_BY, MutexAutoLock
#include "mozilla/RefPtr.h"                  // RefPtr
#include "mozilla/SharedSubResourceCache.h"  // SharedSubResourceCache, SharedSubResourceCacheLoadingValueBase, SubResourceNetworkMetadataHolder
#include "mozilla/ThreadSafety.h"            // MOZ_GUARDED_BY
#include "mozilla/WeakPtr.h"                 // SupportsWeakPtr
#include "mozilla/dom/CacheExpirationTime.h"  // CacheExpirationTime
#include "nsIMemoryReporter.h"  // nsIMemoryReporter, NS_DECL_NSIMEMORYREPORTER
#include "nsIObserver.h"        // nsIObserver
#include "nsIPrincipal.h"       // nsIPrincipal
#include "nsISupports.h"        // nsISupports, NS_DECL_ISUPPORTS
#include "nsStringFwd.h"        // nsACString

namespace mozilla {
namespace dom {

class ScriptLoader;
class ScriptLoadData;

class ScriptHashKey : public PLDHashEntryHdr {
 public:
  using KeyType = const ScriptHashKey&;
  using KeyTypePointer = const ScriptHashKey*;

  explicit ScriptHashKey(const ScriptHashKey& aKey)
      : PLDHashEntryHdr(),
        mURI(aKey.mURI),
        mPartitionPrincipal(aKey.mPartitionPrincipal),
        mLoaderPrincipal(aKey.mLoaderPrincipal),
        mKind(aKey.mKind),
        mCORSMode(aKey.mCORSMode),
        mReferrerPolicy(aKey.mReferrerPolicy),
        mHintCharset(aKey.mHintCharset) {
    MOZ_COUNT_CTOR(ScriptHashKey);
  }

  explicit ScriptHashKey(const ScriptHashKey* aKey) : ScriptHashKey(*aKey) {}

  ScriptHashKey(ScriptHashKey&& aKey)
      : PLDHashEntryHdr(),
        mURI(std::move(aKey.mURI)),
        mPartitionPrincipal(std::move(aKey.mPartitionPrincipal)),
        mLoaderPrincipal(std::move(aKey.mLoaderPrincipal)),
        mKind(std::move(aKey.mKind)),
        mCORSMode(std::move(aKey.mCORSMode)),
        mReferrerPolicy(std::move(aKey.mReferrerPolicy)),
        mHintCharset(std::move(aKey.mHintCharset)) {
    MOZ_COUNT_CTOR(ScriptHashKey);
  }

  ScriptHashKey(ScriptLoader* aLoader,
                const JS::loader::ScriptLoadRequest* aRequest,
                mozilla::dom::ReferrerPolicy aReferrerPolicy,
                const JS::loader::ScriptFetchOptions* aFetchOptions,
                const nsCOMPtr<nsIURI> aURI);
  explicit ScriptHashKey(const ScriptLoadData& aLoadData);

  static Maybe<ScriptHashKey> FromStringsForLookup(
      const nsACString& aKey, const nsACString& aURI,
      const nsACString& aHintCharset);

 private:
  ScriptHashKey(nsIURI* aURI, nsIPrincipal* aPartitionPrincipal,
                JS::loader::ScriptKind aKind, CORSMode aCORSMode,
                mozilla::dom::ReferrerPolicy aReferrerPolicy,
                const nsString& aHintCharset)
      : PLDHashEntryHdr(),
        mURI(aURI),
        mPartitionPrincipal(aPartitionPrincipal),
        mLoaderPrincipal(nullptr),
        mKind(aKind),
        mCORSMode(aCORSMode),
        mReferrerPolicy(aReferrerPolicy),
        mHintCharset(aHintCharset) {
    MOZ_COUNT_CTOR(ScriptHashKey);
  }

 public:
  MOZ_COUNTED_DTOR(ScriptHashKey)

  const ScriptHashKey& GetKey() const { return *this; }
  const ScriptHashKey* GetKeyPointer() const { return this; }

  bool KeyEquals(const ScriptHashKey* aKey) const { return KeyEquals(*aKey); }

  bool KeyEquals(const ScriptHashKey&) const;

  static const ScriptHashKey* KeyToPointer(const ScriptHashKey& aKey) {
    return &aKey;
  }
  static PLDHashNumber HashKey(const ScriptHashKey* aKey) {
    return nsURIHashKey::HashKey(aKey->mURI);
  }

  nsIPrincipal* LoaderPrincipal() const { return mLoaderPrincipal; }
  nsIPrincipal* PartitionPrincipal() const { return mPartitionPrincipal; }

  nsIURI* URI() const { return mURI; }

  enum { ALLOW_MEMMOVE = true };

  void ToStringForLookup(nsACString& aResult);

 protected:

  const nsCOMPtr<nsIURI> mURI;

  const nsCOMPtr<nsIPrincipal> mPartitionPrincipal;

  const nsCOMPtr<nsIPrincipal> mLoaderPrincipal;

  const JS::loader::ScriptKind mKind;
  const CORSMode mCORSMode;
  const mozilla::dom::ReferrerPolicy mReferrerPolicy;

  nsString mHintCharset;
};

class ScriptLoadData final
    : public SupportsWeakPtr,
      public nsISupports,
      public SharedSubResourceCacheLoadingValueBase<ScriptLoadData> {
 protected:
  ~ScriptLoadData() = default;

 public:
  ScriptLoadData(ScriptLoader* aLoader, JS::loader::ScriptLoadRequest* aRequest,
                 CacheExpirationTime aExpirationTime,
                 JS::loader::LoadedScript* aLoadedScript);

  NS_DECL_ISUPPORTS

  bool IsLoading() const override { return false; }
  bool IsCancelled() const override { return false; }
  bool IsSyncLoad() const override { return true; }

  SubResourceNetworkMetadataHolder* GetNetworkMetadata() const override {
    return mNetworkMetadata.get();
  }

  void StartLoading() override {}
  void SetLoadCompleted() override {}
  void OnCoalescedTo(const ScriptLoadData& aExistingLoad) override {}
  void Cancel() override {}

  void DidCancelLoad() {}

  bool ShouldDefer() const { return false; }

  JS::loader::LoadedScript* ValueForCache() const {
    return mLoadedScript.get();
  }

  const CacheExpirationTime& ExpirationTime() const { return mExpirationTime; }

  ScriptLoader& Loader() { return *mLoader; }

  const ScriptHashKey& CacheKey() const { return mKey; }

 private:
  CacheExpirationTime mExpirationTime = CacheExpirationTime::Never();
  ScriptLoader* mLoader;
  ScriptHashKey mKey;
  RefPtr<JS::loader::LoadedScript> mLoadedScript;
  RefPtr<SubResourceNetworkMetadataHolder> mNetworkMetadata;
};

struct SharedScriptCacheTraits {
  using Loader = ScriptLoader;
  using Key = ScriptHashKey;
  using Value = JS::loader::LoadedScript;
  using LoadingValue = ScriptLoadData;

  static ScriptHashKey KeyFromLoadingValue(const LoadingValue& aValue) {
    return ScriptHashKey(aValue);
  }
};

class SharedScriptCache final
    : public SharedSubResourceCache<SharedScriptCacheTraits, SharedScriptCache>,
      public nsIMemoryReporter,
      public nsIObserver {
 public:
  using Base =
      SharedSubResourceCache<SharedScriptCacheTraits, SharedScriptCache>;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIMEMORYREPORTER

  SharedScriptCache();
  void Init();

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) override {
    return Base::DoObserve(aSubject, aTopic, aData);
  }

  bool MaybeScheduleUpdateDiskCache();
  void UpdateDiskCache();

  void EncodeAndCompress();
  void SaveToDiskCache();

  void InvalidateInProcess();

  static void LoadCompleted(SharedScriptCache*, ScriptLoadData&);
  using Base::LoadCompleted;
  static void Clear(const Maybe<bool>& aChrome = Nothing(),
                    const Maybe<nsCOMPtr<nsIPrincipal>>& aPrincipal = Nothing(),
                    const Maybe<nsCString>& aSchemelessSite = Nothing(),
                    const Maybe<OriginAttributesPattern>& aPattern = Nothing(),
                    const Maybe<nsCString>& aURL = Nothing());

  static void Invalidate();

  static bool GetCachedScriptSource(JSContext* aCx, const nsACString& aKey,
                                    const nsACString& aURI,
                                    const nsACString& aHintCharset,
                                    JS::MutableHandle<JS::Value> aRetval);

  static void PrepareForLastCC();

 protected:
  ~SharedScriptCache();

  bool ShouldIgnoreMemoryPressure() override;

  void ClearInProcessForMemoryPressure() override;

 private:
  void SetDiskCacheTimer();
  void ClearDiskCacheTimer();
  void OnDiskCacheTimer();

  class EncodeItem {
   public:
    EncodeItem(JS::Stencil* aStencil, JS::TranscodeBuffer&& aSRI,
               JS::loader::LoadedScript* aLoadedScript)
        : mStencil(aStencil),
          mSRI(std::move(aSRI)),
          mLoadedScript(aLoadedScript) {}

    RefPtr<JS::Stencil> mStencil;
    JS::TranscodeBuffer mSRI;
    Vector<uint8_t> mCompressed;

    RefPtr<JS::loader::LoadedScript> mLoadedScript;
  };

  bool mRetryDiskCacheTimer = false;

  Mutex mEncodeMutex{"SharedScriptCache::mEncodeMutex"};
  Vector<EncodeItem> mEncodeItems MOZ_GUARDED_BY(mEncodeMutex);

  nsCOMPtr<nsITimer> mDiskCacheTimer;
};

}  
}  

#endif  // mozilla_dom_SharedScriptCache_h
