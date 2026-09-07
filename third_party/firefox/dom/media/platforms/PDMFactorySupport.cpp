/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PDMFactorySupport.h"

#include <mutex>

#include "mozilla/AppShutdown.h"
#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/SyncRunnable.h"
#include "mozilla/gfx/gfxVars.h"
#include "nsIXULRuntime.h"
#include "nsThreadUtils.h"

namespace mozilla {

namespace {


constexpr const char* kInvalidatingPrefs_CrossPlatform[] = {
    "media.use-blank-decoder",
    "media.gpu-process-decoder",
    "media.rdd-process.enabled",
    "media.utility-process.enabled",
    "media.allow-audio-non-utility",
    "media.prefer-non-ffvpx",
    "media.ffvpx-hw.enabled",
    "media.av1.enabled",
    "media.hevc.enabled",
    nullptr,
};




#if defined(MOZ_FFMPEG)
constexpr const char* kInvalidatingPrefs_FFmpeg[] = {
    "media.ffmpeg.enabled",
    "media.rdd-ffmpeg.enabled",
    nullptr,
};
#endif

static StaticMutex sInstanceMutex MOZ_UNANNOTATED;
static StaticRefPtr<PDMFactorySupport> sInstance;

static Atomic<bool> sStale{false};

}  

PDMFactorySupport::PDMFactorySupport() : mFactory(new PDMFactory()) {}

media::DecodeSupportSet PDMFactorySupport::IsTypeSupported(
    const nsACString& aMimeType) {
  RefPtr<PDMFactorySupport> support = Instance();
  if (!support) {
    return media::DecodeSupportSet{};
  }
  return support->SupportsMimeType(aMimeType);
}

media::DecodeSupportSet PDMFactorySupport::IsSupported(
    const SupportDecoderParams& aParams,
    DecoderDoctorDiagnostics* aDiagnostics) {
  RefPtr<PDMFactorySupport> support = Instance();
  if (!support) {
    return media::DecodeSupportSet{};
  }
  return support->Supports(aParams, aDiagnostics);
}

RefPtr<PDMFactorySupport> PDMFactorySupport::Instance() {
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    return nullptr;
  }

  if (!EnsureInvalidationListenersRegistered()) {
    return nullptr;
  }

  StaticMutexAutoLock lock(sInstanceMutex);
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    return nullptr;
  }
  if (sStale.exchange(false)) {
    sInstance = nullptr;
  }
  if (!sInstance) {
    MOZ_ASSERT(gfx::gfxVars::IsInitialized());
    sInstance = new PDMFactorySupport();

    static std::once_flag sShutdownRegistered;
    std::call_once(sShutdownRegistered, []() {
      if (NS_IsMainThread()) {
        ClearOnShutdown(&sInstance);
      } else {
        NS_DispatchToMainThread(
            NS_NewRunnableFunction("PDMFactorySupport::ClearOnShutdown",
                                   []() { ClearOnShutdown(&sInstance); }));
      }
    });
  }
  return sInstance.get();
}

void PDMFactorySupport::Invalidate() {
  sStale = true;
}

void PDMFactorySupport::OnInvalidatingPrefChanged(const char* ,
                                                  void* ) {
  Invalidate();
}

void PDMFactorySupport::OnInvalidatingGfxVarChanged() { Invalidate(); }

bool PDMFactorySupport::EnsureInvalidationListenersRegistered() {
  static Atomic<bool> sListenersRegisteredComplete{false};
  if (sListenersRegisteredComplete) {
    return true;
  }

  static std::once_flag sListenersRegistered;

  auto registerOnMain = []() {
    MOZ_ASSERT(NS_IsMainThread());

    if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
      return;
    }

    if (!gfx::gfxVars::IsInitialized()) {
      gfx::gfxVars::Initialize();
      (void)BrowserTabsRemoteAutostart();
    }

    Preferences::RegisterCallbacks(OnInvalidatingPrefChanged,
                                   kInvalidatingPrefs_CrossPlatform);
#if defined(MOZ_FFMPEG)
    Preferences::RegisterCallbacks(OnInvalidatingPrefChanged,
                                   kInvalidatingPrefs_FFmpeg);
#endif
    gfx::gfxVars::SetCanUseHardwareVideoDecodingListener(
        OnInvalidatingGfxVarChanged);
    gfx::gfxVars::SetUseAV1HwDecodeListener(OnInvalidatingGfxVarChanged);
    gfx::gfxVars::SetUseVP8HwDecodeListener(OnInvalidatingGfxVarChanged);
    gfx::gfxVars::SetUseVP9HwDecodeListener(OnInvalidatingGfxVarChanged);
    gfx::gfxVars::SetUseH264HwDecodeListener(OnInvalidatingGfxVarChanged);
    gfx::gfxVars::SetUseHEVCHwDecodeListener(OnInvalidatingGfxVarChanged);

    sListenersRegisteredComplete = true;
  };

  if (NS_IsMainThread()) {
    std::call_once(sListenersRegistered, registerOnMain);
    return sListenersRegisteredComplete;
  }

  nsCOMPtr<nsIEventTarget> mainTarget = GetMainThreadSerialEventTarget();
  if (!mainTarget) {
    return sListenersRegisteredComplete;
  }
  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "PDMFactorySupport::EnsureInvalidationListenersRegistered",
      [registerOnMain]() {
        std::call_once(sListenersRegistered, registerOnMain);
      });
  SyncRunnable::DispatchToThread(mainTarget, runnable);
  return sListenersRegisteredComplete;
}

}  
