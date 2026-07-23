/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScreenHelperGTK.h"

#ifdef MOZ_WAYLAND
#  include <gdk/gdkwayland.h>
#endif /* MOZ_WAYLAND */
#include <dlfcn.h>
#include <gtk/gtk.h>

#include "gfxPlatformGtk.h"
#include "mozilla/dom/DOMTypes.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ToString.h"
#include "mozilla/WidgetUtilsGtk.h"
#include "nsGtkUtils.h"
#include "nsTArray.h"
#include "nsWindow.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_widget.h"

struct wl_registry;

#ifdef MOZ_WAYLAND
#  include "nsWaylandDisplay.h"
#endif

namespace mozilla::widget {

#ifdef MOZ_LOGGING
static LazyLogModule sScreenLog("WidgetScreen");
#  define LOG_SCREEN(...) MOZ_LOG(sScreenLog, LogLevel::Debug, (__VA_ARGS__))
#else
#  define LOG_SCREEN(...)
#endif /* MOZ_LOGGING */

using GdkMonitor = struct _GdkMonitor;
class WaylandMonitor;

GdkWindow* ScreenHelperGTK::sRootWindow = nullptr;
StaticRefPtr<ScreenGetterGtk> ScreenHelperGTK::gLastScreenGetter;
int ScreenHelperGTK::gLastSerial = 0;

static GdkMonitor* GdkDisplayGetMonitor(GdkDisplay* aDisplay,
                                        unsigned int aMonitor) {
  static auto s_gdk_display_get_monitor = (GdkMonitor * (*)(GdkDisplay*, int))
      dlsym(RTLD_DEFAULT, "gdk_display_get_monitor");
  if (!s_gdk_display_get_monitor) {
    return nullptr;
  }
  return s_gdk_display_get_monitor(aDisplay, aMonitor);
}

static uint32_t GetGTKPixelDepth() {
  GdkVisual* visual = gdk_screen_get_system_visual(gdk_screen_get_default());
  return gdk_visual_get_depth(visual);
}

#ifdef MOZ_WAYLAND
static already_AddRefed<Screen> MakeDummyScreen(unsigned int aMonitor) {
  LOG_SCREEN("MakeScreenGtk() create dummy screen for monitor [%d]", aMonitor);
  return MakeAndAddRef<Screen>(LayoutDeviceIntRect(), LayoutDeviceIntRect(), 0,
                               0, 0, DesktopToLayoutDeviceScale(1.0),
                               CSSToLayoutDeviceScale(1.0), 1,
                               Screen::IsPseudoDisplay::No, Screen::IsHDR(0));
}
#endif

static already_AddRefed<Screen> MakeScreenGtk(unsigned int aMonitor,
                                              bool aIsHDR) {
  gint geometryScaleFactor =
      ScreenHelperGTK::GetGTKMonitorScaleFactor(aMonitor);

  LOG_SCREEN("MakeScreenGtk() Monitor [%d] scale %d aIsHDR %d", aMonitor,
             geometryScaleFactor, aIsHDR);

  GdkRectangle workarea;
  GdkScreen* defaultScreen = gdk_screen_get_default();
  gdk_screen_get_monitor_workarea(defaultScreen, aMonitor, &workarea);
  LayoutDeviceIntRect availRect(workarea.x * geometryScaleFactor,
                                workarea.y * geometryScaleFactor,
                                workarea.width * geometryScaleFactor,
                                workarea.height * geometryScaleFactor);

  LOG_SCREEN("  workarea [%d, %d] -> [%d x %d]", availRect.x, availRect.y,
             availRect.width, availRect.height);

  DesktopToLayoutDeviceScale contentsScale(1.0);
  CSSToLayoutDeviceScale defaultCssScale(geometryScaleFactor);
  contentsScale.scale = geometryScaleFactor;

#ifdef MOZ_WAYLAND
  if (GdkIsWaylandDisplay()) {
    if (StaticPrefs::widget_wayland_fractional_scale_enabled()) {
      nsWaylandDisplay::MonitorConfig* config =
          WaylandDisplayGet()->GetMonitorConfig(workarea.x, workarea.y);
      (void)NS_WARN_IF(!config || config->pendingChanges);
      if (config && !config->pendingChanges) {
        LOG_SCREEN("  MonitorConfig pixel size [%d, %d] -> [%d x %d]",
                   config->x, config->y, config->pixelWidth,
                   config->pixelHeight);
        if (workarea.width > config->pixelWidth / geometryScaleFactor &&
            workarea.height > config->pixelHeight / geometryScaleFactor) {
          float fractionalScale = (float)config->pixelWidth / workarea.width;
          LOG_SCREEN("Monitor %d uses fractional scale %f", aMonitor,
                     fractionalScale);
          availRect.width = config->pixelWidth;
          availRect.height = config->pixelHeight;
          defaultCssScale = CSSToLayoutDeviceScale(fractionalScale);
          contentsScale.scale = fractionalScale;
        } else if (!workarea.width || !workarea.height) {
          LOG_SCREEN("We're missing workarea, use monitor size.");
          availRect.width = config->pixelWidth;
          availRect.height = config->pixelHeight;
        }
      }
    }
    availRect.MoveTo(0, 0);
  }
#endif

  LayoutDeviceIntRect rect;
  if (GdkIsX11Display()) {
    GdkRectangle monitor;
    gdk_screen_get_monitor_geometry(defaultScreen, aMonitor, &monitor);
    rect = LayoutDeviceIntRect(monitor.x * geometryScaleFactor,
                               monitor.y * geometryScaleFactor,
                               monitor.width * geometryScaleFactor,
                               monitor.height * geometryScaleFactor);
  } else {
    rect = availRect;
  }

  if (!rect.width || !rect.height) {
    NS_WARNING("Reporting screen with zero size!");
  }

  uint32_t pixelDepth = GetGTKPixelDepth();
  if (pixelDepth == 32) {
    pixelDepth = 24;
  }

  float dpi = 96.0f;
  gint heightMM = gdk_screen_get_monitor_height_mm(defaultScreen, aMonitor);
  if (heightMM > 0) {
    dpi = rect.height / (heightMM / MM_PER_INCH_FLOAT);
  }

  gint refreshRate = [&] {
    static auto s_gdk_monitor_get_refresh_rate = (int (*)(GdkMonitor*))dlsym(
        RTLD_DEFAULT, "gdk_monitor_get_refresh_rate");
    if (!s_gdk_monitor_get_refresh_rate) {
      return 0;
    }
    GdkMonitor* monitor =
        GdkDisplayGetMonitor(gdk_display_get_default(), aMonitor);
    if (!monitor) {
      return 0;
    }
    return NSToIntRound(s_gdk_monitor_get_refresh_rate(monitor) / 1000.0f);
  }();

  LOG_SCREEN(
      "New monitor %d size [%d,%d -> %d x %d] depth %d scale %f CssScale %f  "
      "DPI %f refresh %d HDR %d]",
      aMonitor, rect.x, rect.y, rect.width, rect.height, pixelDepth,
      contentsScale.scale, defaultCssScale.scale, dpi, refreshRate, aIsHDR);
  return MakeAndAddRef<Screen>(
      rect, availRect, pixelDepth, pixelDepth, refreshRate, contentsScale,
      defaultCssScale, dpi, Screen::IsPseudoDisplay::No, Screen::IsHDR(aIsHDR));
}

#ifdef MOZ_WAYLAND
class WaylandMonitor {
 public:
  NS_INLINE_DECL_REFCOUNTING(WaylandMonitor)

  WaylandMonitor(ScreenGetterGtk* aScreenGetter, unsigned int aMonitor,
                 wl_output* aWlOutput);

  unsigned int GetMonitor() const { return mMonitor; }

  void SetHDR(bool aIsHDR) { mIsHDR = aIsHDR; }

  void ImageDescriptionReady();
  void ImageDescriptionDone();

  void Finish();

 private:
  ~WaylandMonitor();

  RefPtr<ScreenGetterGtk> mScreenGetter;
  unsigned int mMonitor = 0;

  wp_color_management_output_v1* mOutput = nullptr;
  wp_image_description_v1* mDescription = nullptr;

  bool mIsHDR = false;
};
#endif

class ScreenGetterGtk final {
 public:
  NS_INLINE_DECL_REFCOUNTING(ScreenGetterGtk)

  explicit ScreenGetterGtk(int aSerial, bool aHDRInfoOnly);
  bool CheckGetterSerial() const;
  void AddScreen(RefPtr<Screen> aScreen);
  bool AddScreenHDRAsync(unsigned int aMonitor);
  void Finish();

 protected:
  ~ScreenGetterGtk();

 private:
  AutoTArray<RefPtr<Screen>, 4> mScreenList;
#ifdef MOZ_WAYLAND
  AutoTArray<RefPtr<WaylandMonitor>, 4> mWaylandMonitors;
#endif
  int mSerial = 0;
  unsigned int mMonitorNum = 0;
  bool mHDRInfoOnly = false;
};

#ifdef MOZ_WAYLAND
void image_description_info_done(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1) {
  RefPtr monitor = dont_AddRef(static_cast<WaylandMonitor*>(data));
  LOG_SCREEN("WaylandMonitor() [%p] image_description_info_done monitor %d",
             (void*)monitor, monitor->GetMonitor());
  monitor->ImageDescriptionDone();
}

void image_description_info_icc_file(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    int32_t icc, uint32_t icc_size) {}
void image_description_info_primaries(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x,
    int32_t b_y, int32_t w_x, int32_t w_y) {}
void image_description_info_primaries_named(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t primaries) {}

void image_description_info_tf_power(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t eexp) {}
void image_description_info_tf_named(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t tf) {}
void image_description_info_luminances(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum) {
  auto* monitor = static_cast<WaylandMonitor*>(data);
  LOG_SCREEN(
      "WaylandMonitor() [%p] num [%d] Luminance min %d max %d reference %d",
      monitor, monitor->GetMonitor(), min_lum, max_lum, reference_lum);
  monitor->SetHDR(max_lum > reference_lum);
}
void image_description_info_target_primaries(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x,
    int32_t b_y, int32_t w_x, int32_t w_y) {}
void image_description_info_target_luminance(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t min_lum, uint32_t max_lum) {}
void image_description_info_target_max_cll(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t max_cll) {}
void image_description_info_target_max_fall(
    void* data,
    struct wp_image_description_info_v1* wp_image_description_info_v1,
    uint32_t max_fall) {}

static const struct wp_image_description_info_v1_listener
    image_description_info_listener{image_description_info_done,
                                    image_description_info_icc_file,
                                    image_description_info_primaries,
                                    image_description_info_primaries_named,
                                    image_description_info_tf_power,
                                    image_description_info_tf_named,
                                    image_description_info_luminances,
                                    image_description_info_target_primaries,
                                    image_description_info_target_luminance,
                                    image_description_info_target_max_cll,
                                    image_description_info_target_max_fall};

void WaylandMonitor::ImageDescriptionDone() {
  LOG_SCREEN("WaylandMonitor() [%p] ImageDescriptionDone HDR %d", this, mIsHDR);
  if (mScreenGetter) {
    bool dummyScreen = !mScreenGetter->CheckGetterSerial();
    mScreenGetter->AddScreen(dummyScreen ? MakeDummyScreen(mMonitor)
                                         : MakeScreenGtk(mMonitor, mIsHDR));
  }
}

void WaylandMonitor::ImageDescriptionReady() {
  LOG_SCREEN("WaylandMonitor() [%p] ImageDescriptionReady monitor %d", this,
             GetMonitor());

  AddRef();
  wp_image_description_info_v1_add_listener(
      wp_image_description_v1_get_information(mDescription),
      &image_description_info_listener, this);
}

void image_description_failed(void* aData,
                              struct wp_image_description_v1* aImageDescription,
                              uint32_t aCause, const char* aMsg) {
  LOG_SCREEN("imageDescriptionFailed [%p]", aData);
  RefPtr waylandMonitor = dont_AddRef(static_cast<WaylandMonitor*>(aData));
  waylandMonitor->ImageDescriptionDone();
}

void image_description_ready(void* aData,
                             struct wp_image_description_v1* aImageDescription,
                             uint32_t aIdentity) {
  RefPtr waylandMonitor = dont_AddRef(static_cast<WaylandMonitor*>(aData));
  waylandMonitor->ImageDescriptionReady();
}

WaylandMonitor::WaylandMonitor(ScreenGetterGtk* aScreenGetter,
                               unsigned int aMonitor, wl_output* aWlOutput)
    : mScreenGetter(aScreenGetter), mMonitor(aMonitor) {
  MOZ_COUNT_CTOR(WaylandMonitor);

  LOG_SCREEN("WaylandMonitor()[%p] monitor %d", this, mMonitor);

  mOutput = wp_color_manager_v1_get_output(
      WaylandDisplayGet()->GetColorManager(), aWlOutput);

  static const struct wp_color_management_output_v1_listener listener{
      [](void* data,
         struct wp_color_management_output_v1* wp_color_management_output_v1) {
#  if MOZ_LOGGING
        auto* monitor = static_cast<WaylandMonitor*>(data);
        LOG_SCREEN("WaylandMonitor() [%p] image_description_changed %d",
                   monitor, monitor->GetMonitor());
#  endif
        ScreenHelperGTK::RequestRefreshScreens();
      }};
  wp_color_management_output_v1_add_listener(mOutput, &listener, this);

  AddRef();
  mDescription = wp_color_management_output_v1_get_image_description(mOutput);

  static const struct wp_image_description_v1_listener
      monitor_image_description_listener{image_description_failed,
                                         image_description_ready};
  wp_image_description_v1_add_listener(
      mDescription, &monitor_image_description_listener, this);
}

void WaylandMonitor::Finish() {
  LOG_SCREEN("WaylandMonitor::Finish() [%p]", this);

  MozClearPointer(mOutput, wp_color_management_output_v1_destroy);
  MozClearPointer(mDescription, wp_image_description_v1_destroy);

  AddRef();
  static const struct wl_callback_listener listener{
      [](void* aData, struct wl_callback* callback, uint32_t time) {
        RefPtr monitor = dont_AddRef(static_cast<WaylandMonitor*>(aData));
        LOG_SCREEN("WaylandMonitor::FinishCallback() [%p] ", aData);
      }};
  wl_callback_add_listener(wl_display_sync(WaylandDisplayGetWLDisplay()),
                           &listener, this);
  mScreenGetter = nullptr;
}

WaylandMonitor::~WaylandMonitor() {
  LOG_SCREEN("WaylandMonitor::~WaylandMonitor() [%p]", this);
  MOZ_COUNT_DTOR(WaylandMonitor);
  MOZ_DIAGNOSTIC_ASSERT(!mScreenGetter);
  MOZ_DIAGNOSTIC_ASSERT(!mDescription);
  MOZ_DIAGNOSTIC_ASSERT(!mOutput);
}

bool ScreenGetterGtk::AddScreenHDRAsync(unsigned int aMonitor) {
  MOZ_DIAGNOSTIC_ASSERT(WaylandDisplayGet()->GetColorManager());
  GdkMonitor* monitor =
      GdkDisplayGetMonitor(gdk_display_get_default(), aMonitor);
  if (!monitor) {
    LOG_SCREEN(
        "ScreenGetterGtk::AddScreenHDRAsync() [%p] failed to get monitor %d",
        this, aMonitor);
    return false;
  }
  static auto s_gdk_wayland_monitor_get_wl_output =
      (struct wl_output * (*)(GdkMonitor*))
          dlsym(RTLD_DEFAULT, "gdk_wayland_monitor_get_wl_output");
  if (!s_gdk_wayland_monitor_get_wl_output) {
    LOG_SCREEN(
        "ScreenGetterGtk::AddScreenHDRAsync() missing "
        "gdk_wayland_monitor_get_wl_output");
    return false;
  }
  auto wlOutput = s_gdk_wayland_monitor_get_wl_output(monitor);
  if (!wlOutput) {
    LOG_SCREEN("ScreenGetterGtk::AddScreenHDRAsync() missing wl_output");
    return false;
  }

  LOG_SCREEN("ScreenGetterGtk::AddScreenHDR() [%p] monitor %d", this, aMonitor);
  mWaylandMonitors.AppendElement(new WaylandMonitor(this, aMonitor, wlOutput));
  return true;
}
#endif

void ScreenGetterGtk::Finish() {
#ifdef MOZ_WAYLAND
  LOG_SCREEN("ScreenGetterGtk::Finish() [%p]", this);
  for (auto& monitor : mWaylandMonitors) {
    monitor->Finish();
  }
  mWaylandMonitors.Clear();
#endif
}

RefPtr<Screen> ScreenHelperGTK::GetScreenForWindow(nsWindow* aWindow) {
  static auto s_gdk_display_get_monitor_at_window =
      (GdkMonitor * (*)(GdkDisplay*, GdkWindow*))
          dlsym(RTLD_DEFAULT, "gdk_display_get_monitor_at_window");

  if (!s_gdk_display_get_monitor_at_window) {
    LOG_SCREEN("  failed, missing Gtk helpers");
    return nullptr;
  }

  GdkWindow* gdkWindow = aWindow->GetToplevelGdkWindow();
  if (!gdkWindow) {
    LOG_SCREEN("  failed, can't get GdkWindow");
    return nullptr;
  }

  GdkDisplay* display = gdk_display_get_default();
  GdkMonitor* monitor = s_gdk_display_get_monitor_at_window(display, gdkWindow);
  if (!monitor) {
    LOG_SCREEN("  failed, can't get monitor for GdkWindow");
    return nullptr;
  }

  int index = -1;
  while (GdkMonitor* m = GdkDisplayGetMonitor(display, ++index)) {
    if (m == monitor) {
      RefPtr<Screen> screen =
          ScreenManager::GetSingleton().CurrentScreenList().SafeElementAt(
              index);
      if (!screen) {
        LOG_SCREEN(
            "GetScreenForWindow() [%p] [%d] found monitor %p but no screen",
            aWindow, index, monitor);
        return nullptr;
      }
      LOG_SCREEN("GetScreenForWindow() [%p] [%d] screen %s", aWindow, index,
                 ToString(screen->GetRect()).c_str());
      return screen.forget();
    }
  }

  LOG_SCREEN("  Couldn't find monitor %p", monitor);
  return nullptr;
}

bool ScreenGetterGtk::CheckGetterSerial() const {
  if (mSerial != ScreenHelperGTK::GetLastSerial()) {
    MOZ_DIAGNOSTIC_ASSERT(mSerial <= ScreenHelperGTK::GetLastSerial());
    LOG_SCREEN(
        "[%p] ScreenGetterGtk::CheckGetterSerial(): rejected, old serial %d "
        "latest %d",
        this, mSerial, ScreenHelperGTK::GetLastSerial());
    return false;
  }
  return true;
}

void ScreenGetterGtk::AddScreen(RefPtr<Screen> aScreen) {
  mScreenList.AppendElement(std::move(aScreen));
  MOZ_DIAGNOSTIC_ASSERT(mScreenList.Length() <= mMonitorNum);

  if (mScreenList.Length() < mMonitorNum) {
    return;
  }

  auto finish = MakeScopeExit([&] { Finish(); });

  if (!CheckGetterSerial()) {
    return;
  }

  if (mHDRInfoOnly) {
    bool supportsHDR = false;
    for (const auto& screen : mScreenList) {
      supportsHDR |= screen->GetIsHDR();
    }
    if (!supportsHDR) {
      LOG_SCREEN("ScreenGetterGtk::AddScreen() [%p]: no HDR support", this);
      return;
    }
  }

  LOG_SCREEN(
      "ScreenGetterGtk::AddScreen() [%p]: Set screens, serial %d HDR only %d",
      this, mSerial, mHDRInfoOnly);

  ScreenManager::Refresh(std::move(mScreenList));
}

ScreenGetterGtk::ScreenGetterGtk(int aSerial, bool aHDRInfoOnly)
    : mSerial(aSerial),
      mMonitorNum(gdk_screen_get_n_monitors(gdk_screen_get_default())),
      mHDRInfoOnly(aHDRInfoOnly) {
  LOG_SCREEN(
      "ScreenGetterGtk()::ScreenGetterGtk() [%p] HDR only [%d] monitor num %d",
      this, aHDRInfoOnly, mMonitorNum);
#ifdef MOZ_WAYLAND
  LOG_SCREEN("HDR Protocol %s",
             GdkIsWaylandDisplay() && WaylandDisplayGet()->IsHDREnabled()
                 ? "present"
                 : "missing");
#endif

  for (unsigned int i = 0; i < mMonitorNum; i++) {
#ifdef MOZ_WAYLAND
    if (GdkIsWaylandDisplay() && WaylandDisplayGet()->IsHDREnabled()) {
      if (AddScreenHDRAsync(i)) {
        continue;
      }
    }
#endif
    AddScreen(MakeScreenGtk(i,  false));
  }
}

ScreenGetterGtk::~ScreenGetterGtk() {
  LOG_SCREEN("ScreenGetterGtk::~ScreenGetterGtk() [%p]", this);
}

void ScreenHelperGTK::RequestRefreshScreens(bool aInitialRefresh) {
  LOG_SCREEN("ScreenHelperGTK::RequestRefreshScreens()");

  gLastSerial++;

  if (gLastScreenGetter) {
    gLastScreenGetter->Finish();
  }
  gLastScreenGetter =
      new ScreenGetterGtk(gLastSerial,  aInitialRefresh);
}

gint ScreenHelperGTK::GetGTKMonitorScaleFactor(gint aMonitor) {
  MOZ_ASSERT(NS_IsMainThread());
  GdkScreen* screen = gdk_screen_get_default();
  return aMonitor < gdk_screen_get_n_monitors(screen)
             ? gdk_screen_get_monitor_scale_factor(screen, aMonitor)
             : 1;
}

float ScreenHelperGTK::GetGTKMonitorFractionalScaleFactor(gint aMonitor) {
#ifdef MOZ_WAYLAND
  if (GdkIsWaylandDisplay()) {
    auto& screens = widget::ScreenManager::GetSingleton().CurrentScreenList();
    auto scale = (size_t)aMonitor < screens.Length()
                     ? screens[aMonitor]->GetContentsScaleFactor()
                     : 1.0f;
    LOG_SCREEN(
        "ScreenHelperGTK::GetGTKMonitorFractionalScaleFactor(%d) scale %f",
        aMonitor, scale);
    return scale;
  }
#endif
  return GetGTKMonitorScaleFactor(aMonitor);
}

static void monitors_changed(GdkScreen* aScreen, gpointer unused) {
  LOG_SCREEN("Received monitors-changed event");
  ScreenHelperGTK::RequestRefreshScreens();
}


#ifdef MOZ_WAYLAND
void ScreenHelperGTK::ScreensPrefChanged(const char* aPrefIgnored,
                                         void* aDataIgnored) {
  LOG_SCREEN("ScreenHelperGTK::ScreensPrefChanged()");
  MOZ_RELEASE_ASSERT(XRE_IsParentProcess());
  ScreenHelperGTK::RequestRefreshScreens();
}
#endif

ScreenHelperGTK::ScreenHelperGTK() {
  LOG_SCREEN("ScreenHelperGTK::ScreenHelperGTK() created");
  GdkScreen* defaultScreen = gdk_screen_get_default();
  if (!defaultScreen) {
    MOZ_LOG(sScreenLog, LogLevel::Debug,
            ("defaultScreen is nullptr, running headless"));
    return;
  }
  g_signal_connect(defaultScreen, "monitors-changed",
                   G_CALLBACK(monitors_changed), nullptr);


  AutoTArray<RefPtr<Screen>, 4> screenList;
  gint numScreens = gdk_screen_get_n_monitors(defaultScreen);
  for (gint i = 0; i < numScreens; i++) {
    screenList.AppendElement(MakeScreenGtk(i,  false));
  }
  ScreenManager::Refresh(std::move(screenList));

#ifdef MOZ_WAYLAND
  if (GdkIsWaylandDisplay() && WaylandDisplayGet()->IsHDREnabled()) {
    LOG_SCREEN("ScreenHelperGTK() query HDR Wayland display");
    RequestRefreshScreens( true);
  }
  Preferences::RegisterCallback(
      ScreenHelperGTK::ScreensPrefChanged,
      nsDependentCString(
          StaticPrefs::GetPrefName_widget_wayland_fractional_scale_enabled()));
#endif
}

int ScreenHelperGTK::GetMonitorCount() {
  return gdk_screen_get_n_monitors(gdk_screen_get_default());
}

ScreenHelperGTK::~ScreenHelperGTK() {
  LOG_SCREEN("ScreenHelperGTK::~ScreenHelperGTK() deleted");
  if (gLastScreenGetter) {
    gLastScreenGetter->Finish();
  }
  gLastScreenGetter = nullptr;
}

}  
