/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_Navigator_h
#define mozilla_dom_Navigator_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/Fetch.h"
#include "mozilla/dom/NavigatorBinding.h"
#include "mozilla/dom/Nullable.h"
#include "nsHashKeys.h"
#include "nsInterfaceHashtable.h"
#include "nsPIDOMWindowInlines.h"  // FIXME: Stop including inline definitions!
#include "nsString.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

class nsPluginArray;
class nsMimeTypeArray;
class nsPIDOMWindowInner;
class nsIDOMNavigatorSystemMessages;
class nsIPrincipal;
class nsIURI;

namespace mozilla {
class ErrorResult;

namespace dom {
class AudioSession;
class BodyExtractorBase;
class systemMessageCallback;
class ArrayBufferOrArrayBufferViewOrBlobOrFormDataOrUSVStringOrURLSearchParams;
class ServiceWorkerContainer;
class Clipboard;
class LockManager;
class HTMLMediaElement;
class AudioContext;
}  
namespace webgpu {
class Instance;
}  
}  


namespace mozilla::dom {

class Permissions;

class Promise;

nsTArray<uint32_t> SanitizeVibratePattern(const nsTArray<uint32_t>& aPattern);

namespace network {
class Connection;
}  

class LegacyMozTCPSocket;
class StorageManager;
class MediaSession;
class UserActivation;

class Navigator final : public nsISupports, public nsWrapperCache {
 public:
  explicit Navigator(nsPIDOMWindowInner* aInnerWindow);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(Navigator)

  void Invalidate();
  nsPIDOMWindowInner* GetWindow() const { return mWindow; }

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  void GetProduct(nsAString& aProduct);
  void GetLanguage(nsAString& aLanguage);
  void GetAppName(nsAString& aAppName) const;
  void GetAppVersion(nsAString& aAppName, CallerType aCallerType,
                     ErrorResult& aRv) const;
  void GetPlatform(nsAString& aPlatform, CallerType aCallerType,
                   ErrorResult& aRv) const;
  void GetUserAgent(nsAString& aUserAgent, CallerType aCallerType,
                    ErrorResult& aRv) const;
  bool OnLine();
  nsMimeTypeArray* GetMimeTypes(ErrorResult& aRv);
  nsPluginArray* GetPlugins(ErrorResult& aRv);
  bool PdfViewerEnabled();
  Permissions* GetPermissions(ErrorResult& aRv);
  void GetDoNotTrack(nsAString& aResult);
  bool GlobalPrivacyControl();

  static nsresult GetPlatform(nsAString& aPlatform, Document* aCallerDoc,
                              bool aUsePrefOverriddenValue);

  static nsresult GetAppVersion(nsAString& aAppVersion, Document* aCallerDoc,
                                bool aUsePrefOverriddenValue);

  static nsresult GetUserAgent(nsPIDOMWindowInner* aWindow,
                               Document* aCallerDoc,
                               Maybe<bool> aShouldResistFingerprinting,
                               nsAString& aUserAgent);

  void ClearLanguageCache();

  void ClearPlatformCache();

  void ClearUserAgentCache();

  bool Vibrate(uint32_t aDuration);
  bool Vibrate(const nsTArray<uint32_t>& aDuration);
  void SetVibrationPermission(bool aPermitted, bool aPersistent);
  uint32_t MaxTouchPoints(CallerType aCallerType);
  void GetAppCodeName(nsAString& aAppCodeName, ErrorResult& aRv);
  void GetOscpu(nsAString& aOscpu, CallerType aCallerType,
                ErrorResult& aRv) const;
  void GetVendorSub(nsAString& aVendorSub);
  void GetVendor(nsAString& aVendor);
  void GetProductSub(nsAString& aProductSub);
  bool CookieEnabled();
  void GetBuildID(nsAString& aBuildID, CallerType aCallerType,
                  ErrorResult& aRv) const;
  bool JavaEnabled() { return false; }
  uint64_t HardwareConcurrency();
  bool TaintEnabled() { return false; }

  already_AddRefed<LegacyMozTCPSocket> MozTCPSocket();
  network::Connection* GetConnection(ErrorResult& aRv);
  bool SendBeacon(const nsAString& aUrl, const Nullable<fetch::BodyInit>& aData,
                  ErrorResult& aRv);

  already_AddRefed<ServiceWorkerContainer> ServiceWorker();

  dom::Clipboard* Clipboard();
  webgpu::Instance* Gpu();
  dom::LockManager* Locks();

  static bool Webdriver();

  void GetLanguages(nsTArray<nsString>& aLanguages);

  StorageManager* Storage();

  static void GetAcceptLanguages(nsTArray<nsString>& aLanguages,
                                 const nsCString* aLanguageOverride);

  dom::MediaSession* MediaSession();
  dom::AudioSession* AudioSession();

  nsPIDOMWindowInner* GetParentObject() const { return GetWindow(); }

  JSObject* WrapObject(JSContext* cx,
                       JS::Handle<JSObject*> aGivenProto) override;

  static already_AddRefed<nsPIDOMWindowInner> GetWindowFromGlobal(
      JSObject* aGlobal);

  bool HasCreatedMediaSession() const;

  AutoplayPolicy GetAutoplayPolicy(AutoplayPolicyMediaType aType);
  AutoplayPolicy GetAutoplayPolicy(HTMLMediaElement& aElement);
  AutoplayPolicy GetAutoplayPolicy(AudioContext& aContext);

  already_AddRefed<dom::UserActivation> UserActivation();

  bool TestTrialGatedAttribute() const { return true; }

 private:
  ~Navigator();

  enum BeaconType { eBeaconTypeBlob, eBeaconTypeArrayBuffer, eBeaconTypeOther };

  bool SendBeaconInternal(const nsAString& aUrl, BodyExtractorBase* aBody,
                          BeaconType aType, ErrorResult& aRv);

  nsIDocShell* GetDocShell() const {
    return mWindow ? mWindow->GetDocShell() : nullptr;
  }

  RefPtr<nsPluginArray> mPlugins;
  RefPtr<Permissions> mPermissions;
  RefPtr<network::Connection> mConnection;
  RefPtr<dom::Clipboard> mClipboard;
  RefPtr<ServiceWorkerContainer> mServiceWorkerContainer;
  nsCOMPtr<nsPIDOMWindowInner> mWindow;
  nsTArray<uint32_t> mRequestedVibrationPattern;
  RefPtr<StorageManager> mStorageManager;
  RefPtr<dom::MediaSession> mMediaSession;
  RefPtr<dom::AudioSession> mAudioSession;
  RefPtr<webgpu::Instance> mWebGpu;
  RefPtr<LockManager> mLocks;
  RefPtr<dom::UserActivation> mUserActivation;
};

}  

#endif  // mozilla_dom_Navigator_h
