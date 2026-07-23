/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WidgetUtilsGtk_h_
#define WidgetUtilsGtk_h_

#include "nsString.h"
#include "nsTArray.h"
#include "mozilla/MozPromise.h"
#include <stdint.h>

typedef struct _GdkDisplay GdkDisplay;
typedef struct _GdkDevice GdkDevice;
typedef struct _GError GError;
typedef union _GdkEvent GdkEvent;
typedef struct _GdkSeat GdkSeat;
class nsWindow;

namespace mozilla::widget {

class WidgetUtilsGTK {
 public:
  static int32_t IsTouchDeviceSupportPresent();
};

bool IsMainWindowTransparent();

bool GdkIsWaylandDisplay(GdkDisplay* display);
bool GdkIsX11Display(GdkDisplay* display);

bool GdkIsWaylandDisplay();
bool GdkIsX11Display();

bool IsXWaylandProtocol();

GdkDevice* GdkGetPointer();

GdkSeat* GdkDeviceGetSeat(GdkDevice* device);
void GdkSeatUngrab(GdkSeat* seat);

void SetLastPointerDownEvent(GdkEvent*);
GdkEvent* GetLastPointerDownEvent();

const char* GetSnapInstanceName();
bool IsRunningUnderSnap();
bool IsRunningUnderFlatpak();
bool IsPackagedAppFileExists();
inline bool IsRunningUnderFlatpakOrSnap() {
  return IsRunningUnderFlatpak() || IsRunningUnderSnap();
}
#ifdef MOZ_ENABLE_DBUS
void RegisterHostApp();
#endif

enum class PortalKind {
  FilePicker,
  MimeHandler,
  NativeMessaging,
  Settings,
  Location,
  OpenUri,
};
bool ShouldUsePortal(PortalKind);

const nsCString& GetDesktopEnvironmentIdentifier();
bool IsGnomeDesktopEnvironment();
bool IsKdeDesktopEnvironment();

nsTArray<nsCString> ParseTextURIList(const nsACString& data);

using FocusRequestPromise = MozPromise<nsCString, bool, false>;
RefPtr<FocusRequestPromise> RequestWaylandFocusPromise();

bool IsCancelledGError(GError* aGError);


}  

#endif  // WidgetUtilsGtk_h_
