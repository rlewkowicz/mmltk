/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define PANGO_ENABLE_BACKEND
#define PANGO_ENABLE_ENGINE

#include "gfxPlatformGtk.h"

#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>

#include "base/task.h"
#include "base/thread.h"
#include "base/message_loop.h"
#include "cairo.h"
#include "gfx2DGlue.h"
#include "gfxFcPlatformFontList.h"
#include "gfxConfig.h"
#include "gfxContext.h"
#include "gfxImageSurface.h"
#include "gfxUserFontSet.h"
#include "gfxUtils.h"
#include "gfxFT2FontBase.h"
#include "gfxTextRun.h"
#include "GLContextProvider.h"
#include "mozilla/Components.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/Monitor.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/StaticPrefs_media.h"
#include "nsAppRunner.h"
#include "nsIGfxInfo.h"
#include "nsMathUtils.h"
#include "nsUnicharUtils.h"
#include "nsUnicodeProperties.h"
#include "prenv.h"
#include "VsyncSource.h"
#include "mozilla/WidgetUtilsGtk.h"


#ifdef MOZ_WAYLAND
#  include <gdk/gdkwayland.h>
#  include "mozilla/widget/nsWaylandDisplay.h"
#endif
#ifdef MOZ_WIDGET_GTK
#  include "mozilla/widget/DMABufDevice.h"
#  include "mozilla/StaticPrefs_widget.h"
#endif

#define GDK_PIXMAP_SIZE_MAX 32767

#define GFX_PREF_MAX_GENERIC_SUBSTITUTIONS \
  "gfx.font_rendering.fontconfig.max_generic_substitutions"

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::unicode;
using namespace mozilla::widget;

static FT_Library gPlatformFTLibrary = nullptr;


gfxPlatformGtk::gfxPlatformGtk() {
  if (!gtk_init_check(nullptr, nullptr)) {
    gfxCriticalNote << "Failed to init Gtk, missing display? DISPLAY="
                    << getenv("DISPLAY")
                    << " WAYLAND_DISPLAY=" << getenv("WAYLAND_DISPLAY")
                    << "\n";
    abort();
  }

  mIsX11Display = GdkIsX11Display();
  if (XRE_IsParentProcess()) {
    InitX11EGLConfig();
    if (IsWaylandDisplay() || gfxConfig::IsEnabled(Feature::X11_EGL)) {
      gfxVars::SetUseEGL(true);
    }
    InitDmabufConfig();
    if (gfxConfig::IsEnabled(Feature::DMABUF)) {
      gfxVars::SetUseDMABuf(true);
    }
  }

  InitBackendPrefs(GetBackendPrefs());

  gPlatformFTLibrary = Factory::NewFTLibrary();
  MOZ_RELEASE_ASSERT(gPlatformFTLibrary);
  Factory::SetFTLibrary(gPlatformFTLibrary);

  PR_SetEnv("__GL_ALLOW_FXAA_USAGE=0");

  InitMesaThreading();
}

gfxPlatformGtk::~gfxPlatformGtk() {
  Factory::ReleaseFTLibrary(gPlatformFTLibrary);
  gPlatformFTLibrary = nullptr;
}

void gfxPlatformGtk::InitAcceleration() {
  gfxPlatform::InitAcceleration();

  if (XRE_IsContentProcess()) {
    ImportCachedContentDeviceData();
  }
}

void gfxPlatformGtk::InitX11EGLConfig() {
  FeatureState& feature = gfxConfig::GetFeature(Feature::X11_EGL);
  feature.DisableByDefault(FeatureStatus::Unavailable, "X11 support missing",
                           "FEATURE_FAILURE_NO_X11"_ns);
}

void gfxPlatformGtk::InitDmabufConfig() {
  FeatureState& feature = gfxConfig::GetFeature(Feature::DMABUF);
  feature.EnableByDefault();

  nsCString failureId;
  int32_t status;
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  if (NS_FAILED(gfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_DMABUF, failureId,
                                          &status))) {
    feature.Disable(FeatureStatus::BlockedNoGfxInfo, "gfxInfo is broken",
                    "FEATURE_FAILURE_NO_GFX_INFO"_ns);
  } else if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
    feature.Disable(FeatureStatus::Blocklisted, "Blocklisted by gfxInfo",
                    failureId);
  }

  if (StaticPrefs::widget_dmabuf_force_enabled_AtStartup()) {
    feature.UserForceEnable("Force enabled by pref");
  } else if (!StaticPrefs::widget_dmabuf_enabled_AtStartup()) {
    feature.UserDisable("Force disable by pref",
                        "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
  }

  if (!gfxVars::UseEGL()) {
    feature.ForceDisable(FeatureStatus::Unavailable, "Requires EGL",
                         "FEATURE_FAILURE_REQUIRES_EGL"_ns);
  }

  if (!gfxVars::WebglUseHardware()) {
    feature.Disable(FeatureStatus::Blocklisted,
                    "DMABuf disabled with software rendering", failureId);
  }

  nsAutoCString drmRenderDevice;
  gfxInfo->GetDrmRenderDevice(drmRenderDevice);
  gfxVars::SetDrmRenderDevice(drmRenderDevice);

  if (feature.IsEnabled()) {
    DMABufDeviceLock device;
    if (!device.GetDMABufDevice()->IsEnabled(failureId)) {
      feature.ForceDisable(FeatureStatus::Failed, "Failed to configure",
                           failureId);
    }
    (void)GetGlobalDMABufFormats();
  }
}

void gfxPlatformGtk::InitPlatformHardwareVideoConfig() {
  FeatureState& featureDec =
      gfxConfig::GetFeature(Feature::HARDWARE_VIDEO_DECODING);
  if (!gfxVars::UseEGL()) {
    featureDec.ForceDisable(FeatureStatus::Unavailable, "Requires EGL",
                            "FEATURE_FAILURE_REQUIRES_EGL"_ns);
    gfxConfig::ForceDisable(Feature::HARDWARE_VIDEO_ENCODING,
                            FeatureStatus::Unavailable, "Requires EGL",
                            "FEATURE_FAILURE_REQUIRES_EGL"_ns);
  }

  if (!featureDec.IsEnabled()) {
    return;
  }

  FeatureState& featureZeroCopy =
      gfxConfig::GetFeature(Feature::HW_DECODED_VIDEO_ZERO_COPY);

  featureZeroCopy.EnableByDefault();
  uint32_t state =
      StaticPrefs::media_ffmpeg_vaapi_force_surface_zero_copy_AtStartup();
  if (state == 0) {
    featureZeroCopy.UserDisable("Force disable by pref",
                                "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
  } else if (state == 1) {
    featureZeroCopy.UserEnable("Force enabled by pref");
  } else {
    nsCString failureId;
    int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
    nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
    if (NS_FAILED(gfxInfo->GetFeatureStatus(
            nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY, failureId,
            &status))) {
      featureZeroCopy.Disable(FeatureStatus::BlockedNoGfxInfo,
                              "gfxInfo is broken",
                              "FEATURE_FAILURE_NO_GFX_INFO"_ns);
    } else if (status == nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST) {
      featureZeroCopy.ForceDisable(FeatureStatus::Unavailable,
                                   "Force disabled by gfxInfo", failureId);
    } else if (status != nsIGfxInfo::FEATURE_ALLOW_ALWAYS) {
      featureZeroCopy.Disable(FeatureStatus::Blocklisted,
                              "Blocklisted by gfxInfo", failureId);
    }
  }
  if (featureZeroCopy.IsEnabled()) {
    gfxVars::SetHwDecodedVideoZeroCopy(true);
  }
}

void gfxPlatformGtk::InitWebRenderConfig() {
  gfxPlatform::InitWebRenderConfig();

  if (!XRE_IsParentProcess()) {
    return;
  }

  FeatureState& feature = gfxConfig::GetFeature(Feature::WEBRENDER_COMPOSITOR);
#if defined(MOZ_WAYLAND)
  if (feature.IsEnabled()) {
    if (!IsWaylandDisplay()) {
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Wayland support missing",
                           "FEATURE_FAILURE_NO_WAYLAND"_ns);
    } else if (gfxConfig::IsEnabled(Feature::WEBRENDER) &&
               !gfxConfig::IsEnabled(Feature::DMABUF)) {
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Hardware Webrender requires DMAbuf support",
                           "FEATURE_FAILURE_NO_DMABUF"_ns);
    } else if (!widget::WaylandDisplayGet()->GetViewporter()) {
      feature.ForceDisable(FeatureStatus::Unavailable,
                           "Requires wp_viewporter protocol support",
                           "FEATURE_FAILURE_REQUIRES_WPVIEWPORTER"_ns);
    }
  }
#else  // MOZ_WAYLAND
  feature.ForceDisable(FeatureStatus::Unavailable, "Not available on X11",
                       "FEATURE_FAILURE_NO_WAYLAND"_ns);
#endif

  gfxVars::SetUseWebRenderCompositor(feature.IsEnabled());
}

void gfxPlatformGtk::InitPlatformGPUProcessPrefs() {
#ifdef MOZ_WAYLAND
  if (IsWaylandDisplay()) {
    FeatureState& gpuProc = gfxConfig::GetFeature(Feature::GPU_PROCESS);
    gpuProc.ForceDisable(FeatureStatus::Blocked,
                         "Wayland does not work in the GPU process",
                         "FEATURE_FAILURE_WAYLAND"_ns);
  }
#endif
}

void gfxPlatformGtk::InitMesaThreading() {
  FeatureState& featureMesaThreading =
      gfxConfig::GetFeature(Feature::MESA_THREADING);
  featureMesaThreading.EnableByDefault();

  nsCString failureId;
  int32_t status;
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  if (NS_FAILED(gfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_MESA_THREADING,
                                          failureId, &status))) {
    featureMesaThreading.Disable(FeatureStatus::BlockedNoGfxInfo,
                                 "gfxInfo is broken",
                                 "FEATURE_FAILURE_NO_GFX_INFO"_ns);
  } else if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
    featureMesaThreading.Disable(FeatureStatus::Blocklisted,
                                 "Blocklisted by gfxInfo", failureId);
  }

  if (gfxConfig::IsEnabled(Feature::X11_EGL) && IsX11Display()) {
    featureMesaThreading.Disable(FeatureStatus::Failed,
                                 "No glthread with EGL and X11",
                                 "FEATURE_FAILURE_EGL_X11"_ns);
  }

  if (!featureMesaThreading.IsEnabled()) {
    PR_SetEnv("mesa_glthread=false");
  }
}

already_AddRefed<gfxASurface> gfxPlatformGtk::CreateOffscreenSurface(
    const IntSize& aSize, gfxImageFormat aFormat) {
  if (!Factory::AllowedSurfaceSize(aSize)) {
    return nullptr;
  }

  RefPtr<gfxASurface> newSurface;
  bool needsClear = true;
  GdkScreen* gdkScreen = gdk_screen_get_default();
  if (gdkScreen) {
    newSurface = new gfxImageSurface(aSize, aFormat);
    needsClear = false;
  }

  if (!newSurface) {
    newSurface = new gfxImageSurface(aSize, aFormat);
  }

  if (newSurface->CairoStatus()) {
    newSurface = nullptr;  
  }

  if (newSurface && needsClear) {
    gfxUtils::ClearThebesSurface(newSurface);
  }

  return newSurface.forget();
}

nsresult gfxPlatformGtk::GetFontList(nsAtom* aLangGroup,
                                     const nsACString& aGenericFamily,
                                     nsTArray<nsString>& aListOfFonts) {
  gfxPlatformFontList::PlatformFontList()->GetFontList(
      aLangGroup, aGenericFamily, aListOfFonts);
  return NS_OK;
}

static const char kFontDejaVuSans[] = "DejaVu Sans";
static const char kFontDejaVuSerif[] = "DejaVu Serif";
static const char kFontFreeSans[] = "FreeSans";
static const char kFontFreeSerif[] = "FreeSerif";
static const char kFontTakaoPGothic[] = "TakaoPGothic";
static const char kFontTwemojiMozilla[] = "Twemoji Mozilla";
static const char kFontDroidSansFallback[] = "Droid Sans Fallback";
static const char kFontWenQuanYiMicroHei[] = "WenQuanYi Micro Hei";
static const char kFontNanumGothic[] = "NanumGothic";
static const char kFontSymbola[] = "Symbola";
static const char kFontNotoSansSymbols[] = "Noto Sans Symbols";
static const char kFontNotoSansSymbols2[] = "Noto Sans Symbols2";

void gfxPlatformGtk::GetCommonFallbackFonts(uint32_t aCh, Script aRunScript,
                                            FontPresentation aPresentation,
                                            nsTArray<const char*>& aFontList) {
  if (PrefersColor(aPresentation)) {
    aFontList.AppendElement(kFontTwemojiMozilla);
  }

  aFontList.AppendElement(kFontDejaVuSerif);
  aFontList.AppendElement(kFontFreeSerif);
  aFontList.AppendElement(kFontDejaVuSans);
  aFontList.AppendElement(kFontFreeSans);
  aFontList.AppendElement(kFontSymbola);
  aFontList.AppendElement(kFontNotoSansSymbols);
  aFontList.AppendElement(kFontNotoSansSymbols2);

  if (aCh >= 0x3000 && ((aCh < 0xe000) || (aCh >= 0xf900 && aCh < 0xfff0) ||
                        ((aCh >> 16) == 2))) {
    aFontList.AppendElement(kFontTakaoPGothic);
    aFontList.AppendElement(kFontDroidSansFallback);
    aFontList.AppendElement(kFontWenQuanYiMicroHei);
    aFontList.AppendElement(kFontNanumGothic);
  }
}

void gfxPlatformGtk::ReadSystemFontList(
    mozilla::dom::SystemFontList* retValue) {
  gfxFcPlatformFontList::PlatformFontList()->ReadSystemFontList(retValue);
}

bool gfxPlatformGtk::CreatePlatformFontList() {
  return gfxPlatformFontList::Initialize(new gfxFcPlatformFontList);
}

gfxImageFormat gfxPlatformGtk::GetOffscreenFormat() {
  GdkScreen* screen = gdk_screen_get_default();
  if (screen && gdk_visual_get_depth(gdk_visual_get_system()) == 16) {
    return SurfaceFormat::R5G6B5_UINT16;
  }

  return SurfaceFormat::X8R8G8B8_UINT32;
}

void gfxPlatformGtk::FontsPrefsChanged(const char* aPref) {
  if (strcmp(GFX_PREF_MAX_GENERIC_SUBSTITUTIONS, aPref) != 0) {
    gfxPlatform::FontsPrefsChanged(aPref);
    return;
  }

  gfxFcPlatformFontList* pfl = gfxFcPlatformFontList::PlatformFontList();
  pfl->ClearGenericMappings();
  FlushFontAndWordCaches();
}

bool gfxPlatformGtk::AccelerateLayersByDefault() { return true; }


nsTArray<uint8_t> gfxPlatformGtk::GetPlatformCMSOutputProfileData() {
  return nsTArray<uint8_t>();
}


bool gfxPlatformGtk::CheckVariationFontSupport() {
  FT_Int major, minor, patch;
  FT_Library_Version(Factory::GetFTLibrary(), &major, &minor, &patch);
  return major * 1000000 + minor * 1000 + patch >= 2007001;
}


already_AddRefed<gfx::VsyncSource>
gfxPlatformGtk::CreateGlobalHardwareVsyncSource() {
  return GetSoftwareVsyncSource();
}

void gfxPlatformGtk::BuildContentDeviceData(ContentDeviceData* aOut) {
  gfxPlatform::BuildContentDeviceData(aOut);

  aOut->cmsOutputProfileData() = GetPlatformCMSOutputProfileData();
}

namespace mozilla::gfx {
bool IsDMABufEnabled() { return gfxVars::UseDMABuf(); }
}  
