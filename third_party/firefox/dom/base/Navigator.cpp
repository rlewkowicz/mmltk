/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Navigator.h"

#include "base/basictypes.h"
#include "mozilla/Components.h"
#include "mozilla/ContentBlockingNotifier.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/dom/BodyExtractor.h"
#include "mozilla/dom/FetchBinding.h"
#include "mozilla/dom/File.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsContentPolicyUtils.h"
#include "nsContentUtils.h"
#include "nsIClassOfService.h"
#include "nsIContentPolicy.h"
#include "nsIHttpProtocolHandler.h"
#include "nsISupportsPriority.h"
#include "nsIXULAppInfo.h"
#include "nsMimeTypeArray.h"
#include "nsPluginArray.h"
#include "nsUnicharUtils.h"
#include "Connection.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Hal.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_privacy.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/StorageAccess.h"
#include "mozilla/dom/AudioSession.h"
#include "mozilla/dom/Clipboard.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Event.h"  // for Event
#include "mozilla/dom/LockManager.h"
#include "mozilla/dom/MediaSession.h"
#include "mozilla/dom/Permissions.h"
#include "mozilla/dom/ServiceWorkerContainer.h"
#include "mozilla/dom/StorageManager.h"
#include "mozilla/dom/TCPSocket.h"
#include "mozilla/dom/URLSearchParams.h"
#include "mozilla/dom/UserActivation.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "mozilla/dom/workerinternals/RuntimeService.h"
#include "nsComponentManagerUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsICookieManager.h"
#include "nsICookieService.h"
#include "nsIHttpChannel.h"
#include "nsIPermissionManager.h"
#include "nsMimeTypes.h"
#include "nsNetUtil.h"
#include "nsRFPService.h"
#include "nsStringStream.h"
#include "BrowserChild.h"
#include "ReferrerInfo.h"
#include "WidgetUtils.h"
#include "mozilla/PermissionDelegateHandler.h"
#include "mozilla/dom/FormData.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRunnable.h"
#include "mozilla/ipc/URIUtils.h"
#include "nsIDocShell.h"
#include "nsIScriptError.h"
#include "nsIUploadChannel2.h"
#include "nsJSUtils.h"
#include "nsStreamUtils.h"


#include "AutoplayPolicy.h"
#include "mozilla/dom/AudioContext.h"
#include "mozilla/dom/HTMLMediaElement.h"
#include "mozilla/intl/LocaleService.h"
#include "mozilla/webgpu/Instance.h"

namespace mozilla::dom {

static const nsLiteralCString kVibrationPermissionType = "vibration"_ns;

Navigator::Navigator(nsPIDOMWindowInner* aWindow) : mWindow(aWindow) {}

Navigator::~Navigator() { Invalidate(); }

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Navigator)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(Navigator)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Navigator)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(Navigator)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(Navigator)
  tmp->Invalidate();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mWindow)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(Navigator)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPlugins)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPermissions)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mConnection)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mStorageManager)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mServiceWorkerContainer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mMediaSession)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAudioSession)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mWebGpu)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mLocks)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mUserActivation)

  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mWindow)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mClipboard)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

void Navigator::Invalidate() {

  mPlugins = nullptr;

  mPermissions = nullptr;

  if (mStorageManager) {
    mStorageManager->Shutdown();
    mStorageManager = nullptr;
  }

  if (mConnection) {
    mConnection->Shutdown();
    mConnection = nullptr;
  }

  mServiceWorkerContainer = nullptr;

  if (mMediaSession) {
    mMediaSession->Shutdown();
    mMediaSession = nullptr;
  }

  mAudioSession = nullptr;

  mWebGpu = nullptr;

  if (mLocks) {
    mLocks->Shutdown();
    mLocks = nullptr;
  }

  mUserActivation = nullptr;

  mClipboard = nullptr;
}

void Navigator::GetUserAgent(nsAString& aUserAgent, CallerType aCallerType,
                             ErrorResult& aRv) const {
  nsCOMPtr<nsPIDOMWindowInner> window;

  if (mWindow) {
    window = mWindow;
    nsIDocShell* docshell = window->GetDocShell();
    nsString customUserAgent;
    if (docshell) {
      docshell->GetBrowsingContext()->GetCustomUserAgent(customUserAgent);

      if (!customUserAgent.IsEmpty()) {
        aUserAgent = std::move(customUserAgent);
        return;
      }
    }
  }

  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();
  nsresult rv = GetUserAgent(
      mWindow, doc, aCallerType == CallerType::System ? Some(false) : Nothing(),
      aUserAgent);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
  }
}

void Navigator::GetAppCodeName(nsAString& aAppCodeName, ErrorResult& aRv) {
  nsresult rv;

  nsCOMPtr<nsIHttpProtocolHandler> service(
      do_GetService(NS_NETWORK_PROTOCOL_CONTRACTID_PREFIX "http", &rv));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  nsAutoCString appName;
  rv = service->GetAppName(appName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  CopyASCIItoUTF16(appName, aAppCodeName);
}

void Navigator::GetAppVersion(nsAString& aAppVersion, CallerType aCallerType,
                              ErrorResult& aRv) const {
  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();

  nsresult rv = GetAppVersion(
      aAppVersion, doc,
       aCallerType != CallerType::System);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
  }
}

void Navigator::GetAppName(nsAString& aAppName) const {
  aAppName.AssignLiteral("Netscape");
}

void Navigator::GetAcceptLanguages(nsTArray<nsString>& aLanguages,
                                   const nsCString* aLanguageOverride) {
  MOZ_ASSERT(NS_IsMainThread());

  aLanguages.Clear();

  nsAutoCString acceptLang;
  if (aLanguageOverride) {
    acceptLang.Assign(aLanguageOverride->get());
  } else {
    intl::LocaleService::GetInstance()->GetAcceptLanguages(acceptLang);
  }

  for (nsDependentCSubstring lang :
       nsCCharSeparatedTokenizer(acceptLang, ',').ToRange()) {
    if (lang.Length() > 2 && lang[2] == '_') {
      lang.Replace(2, 1, '-');
    }

    if (lang.Length() > 2) {
      int32_t pos = 0;
      bool first = true;
      for (const nsACString& code :
           nsCCharSeparatedTokenizer(lang, '-').ToRange()) {
        if (code.Length() == 2 && !first) {
          nsAutoCString upper(code);
          ToUpperCase(upper);
          lang.Replace(pos, code.Length(), upper);
        }

        pos += code.Length() + 1;  
        first = false;
      }
    }

    aLanguages.AppendElement(NS_ConvertUTF8toUTF16(lang));
  }
  if (aLanguages.Length() == 0) {
    nsTArray<nsCString> locales;
    mozilla::intl::LocaleService::GetInstance()->GetWebExposedLocales(locales);
    aLanguages.AppendElement(NS_ConvertUTF8toUTF16(locales[0]));
  }
}

void Navigator::GetLanguage(nsAString& aLanguage) {
  nsTArray<nsString> languages;
  GetLanguages(languages);
  MOZ_ASSERT(languages.Length() >= 1);
  aLanguage.Assign(languages[0]);
}

void Navigator::GetLanguages(nsTArray<nsString>& aLanguages) {
  BrowsingContext* bc = mWindow ? mWindow->GetBrowsingContext() : nullptr;
  if (bc) {
    const nsCString& languageOverride = bc->Top()->GetLanguageOverride();

    if (!languageOverride.IsEmpty()) {
      GetAcceptLanguages(aLanguages, &languageOverride);

      return;
    }
  }

  GetAcceptLanguages(aLanguages, nullptr);

}

void Navigator::GetPlatform(nsAString& aPlatform, CallerType aCallerType,
                            ErrorResult& aRv) const {
  if (mWindow) {
    BrowsingContext* bc = mWindow->GetBrowsingContext();
    nsString customPlatform;
    if (bc) {
      bc->GetCustomPlatform(customPlatform);

      if (!customPlatform.IsEmpty()) {
        aPlatform = std::move(customPlatform);
        return;
      }
    }
  }

  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();

  nsresult rv = GetPlatform(
      aPlatform, doc,
       aCallerType != CallerType::System);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
  }
}

void Navigator::GetOscpu(nsAString& aOSCPU, CallerType aCallerType,
                         ErrorResult& aRv) const {
  if (aCallerType != CallerType::System) {
    if (nsContentUtils::ShouldResistFingerprinting(GetDocShell(),
                                                   RFPTarget::NavigatorOscpu)) {
      aOSCPU.AssignLiteral(SPOOFED_OSCPU);
      return;
    }

    nsAutoString override;
    nsresult rv = Preferences::GetString("general.oscpu.override", override);
    if (NS_SUCCEEDED(rv)) {
      aOSCPU = std::move(override);
      return;
    }
  }

  nsresult rv;
  nsCOMPtr<nsIHttpProtocolHandler> service(
      do_GetService(NS_NETWORK_PROTOCOL_CONTRACTID_PREFIX "http", &rv));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  nsAutoCString oscpu;
  rv = service->GetOscpu(oscpu);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  CopyASCIItoUTF16(oscpu, aOSCPU);
}

void Navigator::GetVendor(nsAString& aVendor) { aVendor.Truncate(); }

void Navigator::GetVendorSub(nsAString& aVendorSub) { aVendorSub.Truncate(); }

void Navigator::GetProduct(nsAString& aProduct) {
  aProduct.AssignLiteral("Gecko");
}

void Navigator::GetProductSub(nsAString& aProductSub) {
  aProductSub.AssignLiteral(LEGACY_UA_GECKO_TRAIL);
}

nsMimeTypeArray* Navigator::GetMimeTypes(ErrorResult& aRv) {
  auto* plugins = GetPlugins(aRv);
  if (!plugins) {
    return nullptr;
  }

  return plugins->MimeTypeArray();
}

nsPluginArray* Navigator::GetPlugins(ErrorResult& aRv) {
  if (!mPlugins) {
    if (!mWindow) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return nullptr;
    }
    mPlugins = MakeRefPtr<nsPluginArray>(mWindow);
  }

  return mPlugins;
}

bool Navigator::PdfViewerEnabled() {
  return false;
}

Permissions* Navigator::GetPermissions(ErrorResult& aRv) {
  if (!mWindow) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  if (!mPermissions) {
    mPermissions = new Permissions(mWindow->AsGlobal());
  }

  return mPermissions;
}

StorageManager* Navigator::Storage() {
  MOZ_ASSERT(mWindow);

  if (!mStorageManager) {
    mStorageManager = new StorageManager(mWindow->AsGlobal());
  }

  return mStorageManager;
}

bool Navigator::CookieEnabled() {
  if (!mWindow || !mWindow->GetDocShell()) {
    return nsICookieManager::GetCookieBehavior(false) !=
           nsICookieService::BEHAVIOR_REJECT;
  }

  nsCOMPtr<nsILoadContext> loadContext = do_GetInterface(mWindow);
  uint32_t cookieBehavior = loadContext
                                ? nsICookieManager::GetCookieBehavior(
                                      loadContext->UsePrivateBrowsing())
                                : nsICookieManager::GetCookieBehavior(false);
  bool cookieEnabled = cookieBehavior != nsICookieService::BEHAVIOR_REJECT;

  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();
  if (!doc) {
    return cookieEnabled;
  }

  uint32_t rejectedReason = 0;
  bool granted = false;
  nsresult rv = doc->NodePrincipal()->HasFirstpartyStorageAccess(
      mWindow, &rejectedReason, &granted);
  if (NS_FAILED(rv)) {
    return cookieEnabled;
  }

  if (!granted &&
      StoragePartitioningEnabled(rejectedReason, doc->CookieJarSettings())) {
    granted = true;
  }

  ContentBlockingNotifier::OnDecision(
      mWindow,
      granted ? ContentBlockingNotifier::BlockingDecision::eAllow
              : ContentBlockingNotifier::BlockingDecision::eBlock,
      rejectedReason);
  return granted;
}

bool Navigator::OnLine() {
  if (nsContentUtils::ShouldResistFingerprinting(
          GetDocShell(), RFPTarget::NetworkConnection)) {
    return true;
  }

  if (mWindow) {
    BrowsingContext* bc = mWindow->GetBrowsingContext();
    if (bc && bc->Top()->GetForceOffline()) {
      return false;
    }
  }
  return !NS_IsOffline();
}

void Navigator::GetBuildID(nsAString& aBuildID, CallerType aCallerType,
                           ErrorResult& aRv) const {
  if (aCallerType != CallerType::System) {
    if (nsContentUtils::ShouldResistFingerprinting(
            GetDocShell(), RFPTarget::NavigatorBuildID)) {
      aBuildID.AssignLiteral(LEGACY_BUILD_ID);
      return;
    }

    nsAutoString override;
    nsresult rv = Preferences::GetString("general.buildID.override", override);
    if (NS_SUCCEEDED(rv)) {
      aBuildID = std::move(override);
      return;
    }

    nsAutoCString host;
    bool isHTTPS = false;
    if (mWindow) {
      nsCOMPtr<Document> doc = mWindow->GetDoc();
      if (doc) {
        nsIURI* uri = doc->GetDocumentURI();
        if (uri) {
          isHTTPS = uri->SchemeIs("https");
          if (isHTTPS) {
            MOZ_ALWAYS_SUCCEEDS(uri->GetHost(host));
          }
        }
      }
    }

    if (!isHTTPS || !StringEndsWith(host, ".mozilla.org"_ns)) {
      aBuildID.AssignLiteral(LEGACY_BUILD_ID);
      return;
    }
  }

  nsCOMPtr<nsIXULAppInfo> appInfo =
      do_GetService("@mozilla.org/xre/app-info;1");
  if (!appInfo) {
    aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
    return;
  }

  nsAutoCString buildID;
  nsresult rv = appInfo->GetAppBuildID(buildID);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  aBuildID.Truncate();
  AppendASCIItoUTF16(buildID, aBuildID);
}

void Navigator::GetDoNotTrack(nsAString& aResult) {
  if (StaticPrefs::privacy_donottrackheader_enabled()) {
    aResult.AssignLiteral("1");
  } else {
    aResult.AssignLiteral("unspecified");
  }
}

bool Navigator::GlobalPrivacyControl() {
  bool gpcStatus = StaticPrefs::privacy_globalprivacycontrol_enabled();
  if (!gpcStatus) {
    nsCOMPtr<nsILoadContext> loadContext = do_GetInterface(mWindow);
    gpcStatus = loadContext && loadContext->UsePrivateBrowsing() &&
                StaticPrefs::privacy_globalprivacycontrol_pbmode_enabled();
  }
  return StaticPrefs::privacy_globalprivacycontrol_functionality_enabled() &&
         gpcStatus;
}

uint64_t Navigator::HardwareConcurrency() {
  workerinternals::RuntimeService* rts =
      workerinternals::RuntimeService::GetOrCreateService();
  if (!rts) {
    return 1;
  }

  return rts->ClampedHardwareConcurrency(
      nsGlobalWindowInner::Cast(mWindow)->ShouldResistFingerprinting(
          RFPTarget::NavigatorHWConcurrency),
      nsGlobalWindowInner::Cast(mWindow)->ShouldResistFingerprinting(
          RFPTarget::NavigatorHWConcurrencyTiered));
}

namespace {

class VibrateWindowListener : public nsIDOMEventListener {
 public:
  VibrateWindowListener(nsPIDOMWindowInner* aWindow, Document* aDocument)
      : mWindow(do_GetWeakReference(aWindow)), mDocument(aDocument) {
    constexpr auto visibilitychange = u"visibilitychange"_ns;
    aDocument->AddSystemEventListener(visibilitychange, this, 
                                      true,                   
                                      false );
  }

  void RemoveListener();

  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMEVENTLISTENER

 private:
  virtual ~VibrateWindowListener() = default;

  nsWeakPtr mWindow;
  WeakPtr<Document> mDocument;
};

NS_IMPL_ISUPPORTS(VibrateWindowListener, nsIDOMEventListener)

StaticRefPtr<VibrateWindowListener> gVibrateWindowListener;

static bool MayVibrate(Document* doc) {
  return (doc && !doc->Hidden());
}

NS_IMETHODIMP
VibrateWindowListener::HandleEvent(Event* aEvent) {
  nsCOMPtr<Document> doc = do_QueryInterface(aEvent->GetTarget());

  if (!MayVibrate(doc)) {
    nsCOMPtr<nsPIDOMWindowInner> window = do_QueryReferent(mWindow);
    hal::CancelVibrate(window);
    RemoveListener();
    gVibrateWindowListener = nullptr;
  }

  return NS_OK;
}

void VibrateWindowListener::RemoveListener() {
  nsCOMPtr<Document> target(mDocument);
  if (!target) {
    return;
  }
  constexpr auto visibilitychange = u"visibilitychange"_ns;
  target->RemoveSystemEventListener(visibilitychange, this,
                                    true );
}

}  

void Navigator::SetVibrationPermission(bool aPermitted, bool aPersistent) {
  MOZ_ASSERT(NS_IsMainThread());

  nsTArray<uint32_t> pattern = std::move(mRequestedVibrationPattern);

  if (!mWindow) {
    return;
  }

  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();

  if (!MayVibrate(doc)) {
    return;
  }

  if (aPermitted) {
    if (!gVibrateWindowListener) {
      ClearOnShutdown(&gVibrateWindowListener);
    } else {
      gVibrateWindowListener->RemoveListener();
    }
    gVibrateWindowListener = new VibrateWindowListener(mWindow, doc);
    hal::Vibrate(pattern, mWindow);
  }

  if (aPersistent) {
    nsCOMPtr<nsIPermissionManager> permMgr =
        components::PermissionManager::Service();
    if (!permMgr) {
      return;
    }
    permMgr->AddFromPrincipal(doc->NodePrincipal(), kVibrationPermissionType,
                              aPermitted ? nsIPermissionManager::ALLOW_ACTION
                                         : nsIPermissionManager::DENY_ACTION,
                              nsIPermissionManager::EXPIRE_SESSION, 0);
  }
}

bool Navigator::Vibrate(uint32_t aDuration) {
  AutoTArray<uint32_t, 1> pattern;
  pattern.AppendElement(aDuration);
  return Vibrate(pattern);
}

nsTArray<uint32_t> SanitizeVibratePattern(const nsTArray<uint32_t>& aPattern) {
  nsTArray<uint32_t> pattern(aPattern.Clone());

  if (pattern.Length() > StaticPrefs::dom_vibrator_max_vibrate_list_len()) {
    pattern.SetLength(StaticPrefs::dom_vibrator_max_vibrate_list_len());
  }

  for (size_t i = 0; i < pattern.Length(); ++i) {
    pattern[i] =
        std::min(StaticPrefs::dom_vibrator_max_vibrate_ms(), pattern[i]);
  }

  return pattern;
}

bool Navigator::Vibrate(const nsTArray<uint32_t>& aPattern) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!mWindow) {
    return false;
  }

  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();

  if (!MayVibrate(doc)) {
    return false;
  }

  nsTArray<uint32_t> pattern = SanitizeVibratePattern(aPattern);

  mRequestedVibrationPattern = std::move(pattern);

  PermissionDelegateHandler* permissionHandler =
      doc->GetPermissionDelegateHandler();
  if (NS_WARN_IF(!permissionHandler)) {
    return false;
  }

  uint32_t permission = nsIPermissionManager::UNKNOWN_ACTION;

  permissionHandler->GetPermission(kVibrationPermissionType, &permission,
                                   false);

  if (permission == nsIPermissionManager::DENY_ACTION) {
    SetVibrationPermission(false , false );
    return false;
  }

  if (permission == nsIPermissionManager::ALLOW_ACTION ||
      mRequestedVibrationPattern.IsEmpty() ||
      (mRequestedVibrationPattern.Length() == 1 &&
       mRequestedVibrationPattern[0] == 0)) {
    SetVibrationPermission(true , false );
    return true;
  }

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return true;
  }

  obs->NotifyObservers(ToSupports(this), "Vibration:Request", nullptr);

  return true;
}


uint32_t Navigator::MaxTouchPoints(CallerType aCallerType) {
  nsIDocShell* docshell = GetDocShell();
  BrowsingContext* bc = docshell ? docshell->GetBrowsingContext() : nullptr;

  if (bc && bc->Top()->InRDMPane()) {
    return bc->Top()->GetMaxTouchPointsOverride();
  }

  if (aCallerType != CallerType::System &&
      nsContentUtils::ShouldResistFingerprinting(GetDocShell(),
                                                 RFPTarget::MaxTouchPoints)) {
    return SPOOFED_MAX_TOUCH_POINTS;
  }

  nsCOMPtr<nsIWidget> widget =
      widget::WidgetUtils::DOMWindowToWidget(mWindow->GetOuterWindow());

  NS_ENSURE_TRUE(widget, 0);
  uint32_t maxTouchPoints = widget->GetMaxTouchPoints();

  if (aCallerType != CallerType::System &&
      nsContentUtils::ShouldResistFingerprinting(
          GetDocShell(), RFPTarget::MaxTouchPointsCollapse)) {
    return nsRFPService::CollapseMaxTouchPoints(maxTouchPoints);
  }
  return maxTouchPoints;
}



class BeaconStreamListener final : public nsIStreamListener {
  ~BeaconStreamListener() = default;

 public:
  BeaconStreamListener() : mLoadGroup(nullptr) {}

  void SetLoadGroup(nsILoadGroup* aLoadGroup) { mLoadGroup = aLoadGroup; }

  NS_DECL_ISUPPORTS
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSIREQUESTOBSERVER

 private:
  nsCOMPtr<nsILoadGroup> mLoadGroup;
};

NS_IMPL_ISUPPORTS(BeaconStreamListener, nsIStreamListener, nsIRequestObserver)

NS_IMETHODIMP
BeaconStreamListener::OnStartRequest(nsIRequest* aRequest) {
  mLoadGroup = nullptr;

  return NS_ERROR_ABORT;
}

NS_IMETHODIMP
BeaconStreamListener::OnStopRequest(nsIRequest* aRequest, nsresult aStatus) {
  return NS_OK;
}

NS_IMETHODIMP
BeaconStreamListener::OnDataAvailable(nsIRequest* aRequest,
                                      nsIInputStream* inStr,
                                      uint64_t sourceOffset, uint32_t count) {
  MOZ_ASSERT(false);
  return NS_OK;
}

bool Navigator::SendBeacon(const nsAString& aUrl,
                           const Nullable<fetch::BodyInit>& aData,
                           ErrorResult& aRv) {
  if (aData.IsNull()) {
    return SendBeaconInternal(aUrl, nullptr, eBeaconTypeOther, aRv);
  }

  if (aData.Value().IsArrayBuffer()) {
    BodyExtractor<const ArrayBuffer> body(&aData.Value().GetAsArrayBuffer());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeArrayBuffer, aRv);
  }

  if (aData.Value().IsArrayBufferView()) {
    BodyExtractor<const ArrayBufferView> body(
        &aData.Value().GetAsArrayBufferView());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeArrayBuffer, aRv);
  }

  if (aData.Value().IsBlob()) {
    BodyExtractor<const Blob> body(&aData.Value().GetAsBlob());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeBlob, aRv);
  }

  if (aData.Value().IsFormData()) {
    BodyExtractor<const FormData> body(&aData.Value().GetAsFormData());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeOther, aRv);
  }

  if (aData.Value().IsUSVString()) {
    BodyExtractor<const nsAString> body(&aData.Value().GetAsUSVString());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeOther, aRv);
  }

  if (aData.Value().IsURLSearchParams()) {
    BodyExtractor<const URLSearchParams> body(
        &aData.Value().GetAsURLSearchParams());
    return SendBeaconInternal(aUrl, &body, eBeaconTypeOther, aRv);
  }

  MOZ_CRASH("Invalid data type.");
  return false;
}

bool Navigator::SendBeaconInternal(const nsAString& aUrl,
                                   BodyExtractorBase* aBody, BeaconType aType,
                                   ErrorResult& aRv) {
  if (!mWindow) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return false;
  }

  nsCOMPtr<Document> doc = mWindow->GetDoc();
  if (!doc) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return false;
  }

  nsIURI* documentURI = doc->GetDocumentURI();
  if (!documentURI) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return false;
  }

  nsCOMPtr<nsIURI> uri;
  nsresult rv = nsContentUtils::NewURIWithDocumentCharset(
      getter_AddRefs(uri), aUrl, doc, doc->GetDocBaseURI());
  if (NS_FAILED(rv)) {
    aRv.ThrowTypeError<MSG_INVALID_URL>(NS_ConvertUTF16toUTF8(aUrl));
    return false;
  }

  if (!net::SchemeIsHttpOrHttps(uri)) {
    aRv.ThrowTypeError<MSG_INVALID_URL_SCHEME>("Beacon",
                                               uri->GetSpecOrDefault());
    return false;
  }

  nsCOMPtr<nsIInputStream> in;
  nsAutoCString contentTypeWithCharset;
  nsAutoCString charset;
  uint64_t length = 0;
  if (aBody) {
    aRv = aBody->GetAsStream(getter_AddRefs(in), &length,
                             contentTypeWithCharset, charset);
    if (NS_WARN_IF(aRv.Failed())) {
      return false;
    }
  }

  nsSecurityFlags securityFlags = nsILoadInfo::SEC_COOKIES_INCLUDE;
  if (aBody && !contentTypeWithCharset.IsVoid() &&
      !nsContentUtils::IsCORSSafelistedRequestHeader("content-type"_ns,
                                                     contentTypeWithCharset)) {
    securityFlags |= nsILoadInfo::SEC_REQUIRE_CORS_INHERITS_SEC_CONTEXT;
  } else {
    securityFlags |= nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT;
  }

  nsCOMPtr<nsIChannel> channel;
  rv = NS_NewChannel(getter_AddRefs(channel), uri, doc, securityFlags,
                     nsIContentPolicy::TYPE_BEACON);

  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return false;
  }

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(channel);
  if (!httpChannel) {
    aRv.Throw(NS_ERROR_DOM_BAD_URI);
    return false;
  }

  auto referrerInfo = MakeRefPtr<ReferrerInfo>(*doc);
  rv = httpChannel->SetReferrerInfoWithoutClone(referrerInfo);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  if (aBody) {
    nsCOMPtr<nsIUploadChannel2> uploadChannel = do_QueryInterface(channel);
    if (!uploadChannel) {
      aRv.Throw(NS_ERROR_FAILURE);
      return false;
    }

    uploadChannel->ExplicitSetUploadStream(in, contentTypeWithCharset, length,
                                           "POST"_ns);
  } else {
    rv = httpChannel->SetRequestMethod("POST"_ns);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  nsCOMPtr<nsISupportsPriority> p = do_QueryInterface(channel);
  if (p) {
    p->SetPriority(nsISupportsPriority::PRIORITY_LOWEST);
  }

  nsCOMPtr<nsIClassOfService> cos(do_QueryInterface(channel));
  if (cos) {
    cos->AddClassFlags(nsIClassOfService::Background);
  }

  nsCOMPtr<nsILoadGroup> loadGroup = do_CreateInstance(NS_LOADGROUP_CONTRACTID);
  nsCOMPtr<nsIInterfaceRequestor> callbacks =
      do_QueryInterface(mWindow->GetDocShell());
  loadGroup->SetNotificationCallbacks(callbacks);
  channel->SetLoadGroup(loadGroup);

  RefPtr<BeaconStreamListener> beaconListener = new BeaconStreamListener();
  rv = channel->AsyncOpen(beaconListener);
  NS_ENSURE_SUCCESS(rv, false);

  beaconListener->SetLoadGroup(loadGroup);

  return true;
}


static bool ShouldResistFingerprinting(const Document* aDoc,
                                       RFPTarget aTarget) {
  return aDoc ? aDoc->ShouldResistFingerprinting(aTarget)
              : nsContentUtils::ShouldResistFingerprinting("Fallback", aTarget);
}

already_AddRefed<LegacyMozTCPSocket> Navigator::MozTCPSocket() {
  RefPtr<LegacyMozTCPSocket> socket = new LegacyMozTCPSocket(GetWindow());
  return socket.forget();
}

network::Connection* Navigator::GetConnection(ErrorResult& aRv) {
  if (!mConnection) {
    if (!mWindow) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return nullptr;
    }
    mConnection = network::Connection::CreateForWindow(
        mWindow, nsGlobalWindowInner::Cast(mWindow)->ShouldResistFingerprinting(
                     RFPTarget::NetworkConnection));
  }

  return mConnection;
}

already_AddRefed<ServiceWorkerContainer> Navigator::ServiceWorker() {
  MOZ_ASSERT(mWindow);

  if (!mServiceWorkerContainer) {
    mServiceWorkerContainer =
        ServiceWorkerContainer::Create(mWindow->AsGlobal());
  }

  RefPtr<ServiceWorkerContainer> ref = mServiceWorkerContainer;
  return ref.forget();
}

size_t Navigator::SizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t n = aMallocSizeOf(this);


  return n;
}

JSObject* Navigator::WrapObject(JSContext* cx,
                                JS::Handle<JSObject*> aGivenProto) {
  return Navigator_Binding::Wrap(cx, this, aGivenProto);
}

already_AddRefed<nsPIDOMWindowInner> Navigator::GetWindowFromGlobal(
    JSObject* aGlobal) {
  nsCOMPtr<nsPIDOMWindowInner> win = xpc::WindowOrNull(aGlobal);
  return win.forget();
}

void Navigator::ClearPlatformCache() {
  Navigator_Binding::ClearCachedPlatformValue(this);
}

void Navigator::ClearLanguageCache() {
  Navigator_Binding::ClearCachedLanguageValue(this);
  Navigator_Binding::ClearCachedLanguagesValue(this);
}

nsresult Navigator::GetPlatform(nsAString& aPlatform, Document* aCallerDoc,
                                bool aUsePrefOverriddenValue) {
  MOZ_ASSERT(NS_IsMainThread());

  if (aUsePrefOverriddenValue &&
      !ShouldResistFingerprinting(aCallerDoc, RFPTarget::NavigatorPlatform)) {
    nsAutoString override;
    nsresult rv =
        mozilla::Preferences::GetString("general.platform.override", override);

    if (NS_SUCCEEDED(rv)) {
      aPlatform = std::move(override);
      return NS_OK;
    }
  }

  aPlatform.AssignLiteral("Linux x86_64");

  return NS_OK;
}

nsresult Navigator::GetAppVersion(nsAString& aAppVersion, Document* aCallerDoc,
                                  bool aUsePrefOverriddenValue) {
  MOZ_ASSERT(NS_IsMainThread());

  if (aUsePrefOverriddenValue) {
    if (ShouldResistFingerprinting(aCallerDoc,
                                   RFPTarget::NavigatorAppVersion)) {
      aAppVersion.AssignLiteral(SPOOFED_APPVERSION);
      return NS_OK;
    }
    nsAutoString override;
    nsresult rv = mozilla::Preferences::GetString("general.appversion.override",
                                                  override);

    if (NS_SUCCEEDED(rv)) {
      aAppVersion = std::move(override);
      return NS_OK;
    }
  }

  nsresult rv;

  nsCOMPtr<nsIHttpProtocolHandler> service(
      do_GetService(NS_NETWORK_PROTOCOL_CONTRACTID_PREFIX "http", &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString str;
  rv = service->GetAppVersion(str);
  CopyASCIItoUTF16(str, aAppVersion);
  NS_ENSURE_SUCCESS(rv, rv);

  aAppVersion.AppendLiteral(" (");

  rv = service->GetPlatform(str);
  NS_ENSURE_SUCCESS(rv, rv);

  AppendASCIItoUTF16(str, aAppVersion);
  aAppVersion.Append(char16_t(')'));

  return rv;
}

void Navigator::ClearUserAgentCache() {
  Navigator_Binding::ClearCachedUserAgentValue(this);
}

nsresult Navigator::GetUserAgent(nsPIDOMWindowInner* aWindow,
                                 Document* aCallerDoc,
                                 Maybe<bool> aShouldResistFingerprinting,
                                 nsAString& aUserAgent) {
  MOZ_ASSERT(NS_IsMainThread());


  bool shouldResistFingerprinting =
      aShouldResistFingerprinting.isSome()
          ? aShouldResistFingerprinting.value()
          : ShouldResistFingerprinting(aCallerDoc,
                                       RFPTarget::NavigatorUserAgent);

  if (!shouldResistFingerprinting) {
    nsAutoString override;
    nsresult rv =
        mozilla::Preferences::GetString("general.useragent.override", override);

    if (NS_SUCCEEDED(rv)) {
      aUserAgent = std::move(override);
      return NS_OK;
    }
  }

  if (shouldResistFingerprinting) {
    nsAutoCString spoofedUA;
    nsRFPService::GetSpoofedUserAgent(spoofedUA);
    CopyASCIItoUTF16(spoofedUA, aUserAgent);
    return NS_OK;
  }

  nsresult rv;
  nsCOMPtr<nsIHttpProtocolHandler> service(
      do_GetService(NS_NETWORK_PROTOCOL_CONTRACTID_PREFIX "http", &rv));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoCString ua;
  rv = service->GetUserAgent(ua);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  CopyASCIItoUTF16(ua, aUserAgent);

  if (!aWindow) {
    return NS_OK;
  }

  nsCOMPtr<Document> doc = aWindow->GetExtantDoc();
  if (!doc) {
    return NS_OK;
  }
  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(doc->GetChannel());
  if (httpChannel) {
    bool IsUserAgentHeaderOutdated;
    (void)httpChannel->GetIsUserAgentHeaderOutdated(&IsUserAgentHeaderOutdated);

    if (!IsUserAgentHeaderOutdated) {
      nsAutoCString userAgent;
      rv = httpChannel->GetRequestHeader("User-Agent"_ns, userAgent);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      CopyASCIItoUTF16(userAgent, aUserAgent);
    }
  }
  return NS_OK;
}

dom::MediaSession* Navigator::MediaSession() {
  if (!mMediaSession) {
    mMediaSession = new dom::MediaSession(GetWindow());
  }
  return mMediaSession;
}

dom::AudioSession* Navigator::AudioSession() {
  if (!mAudioSession) {
    mAudioSession = new dom::AudioSession(GetWindow());
  }
  return mAudioSession;
}

bool Navigator::HasCreatedMediaSession() const {
  return mMediaSession != nullptr;
}

Clipboard* Navigator::Clipboard() {
  if (!mClipboard) {
    mClipboard = new dom::Clipboard(GetWindow());
  }
  return mClipboard;
}

webgpu::Instance* Navigator::Gpu() {
  if (!mWebGpu) {
    mWebGpu = webgpu::Instance::Create(GetWindow()->AsGlobal());
  }
  return mWebGpu;
}

dom::LockManager* Navigator::Locks() {
  if (!mLocks) {
    mLocks = dom::LockManager::Create(*GetWindow()->AsGlobal());
  }
  return mLocks;
}

bool Navigator::Webdriver() {

  return false;
}

AutoplayPolicy Navigator::GetAutoplayPolicy(AutoplayPolicyMediaType aType) {
  if (!mWindow) {
    return AutoplayPolicy::Disallowed;
  }
  nsCOMPtr<Document> doc = mWindow->GetExtantDoc();
  if (!doc) {
    return AutoplayPolicy::Disallowed;
  }
  return media::AutoplayPolicy::GetAutoplayPolicy(aType, *doc);
}

AutoplayPolicy Navigator::GetAutoplayPolicy(HTMLMediaElement& aElement) {
  return media::AutoplayPolicy::GetAutoplayPolicy(aElement);
}

AutoplayPolicy Navigator::GetAutoplayPolicy(AudioContext& aContext) {
  return media::AutoplayPolicy::GetAutoplayPolicy(aContext);
}

already_AddRefed<dom::UserActivation> Navigator::UserActivation() {
  if (!mUserActivation) {
    mUserActivation = new dom::UserActivation(GetWindow());
  }
  return do_AddRef(mUserActivation);
}

}  
