/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_widget_NativeMenuSupport_h)
#define mozilla_widget_NativeMenuSupport_h

#include "mozilla/AlreadyAddRefed.h"

class nsIWidget;

#if 0 || defined(MOZ_WIDGET_GTK)
#  define HAS_NATIVE_MENU_SUPPORT 1
#endif

namespace mozilla {

namespace dom {
class Element;
}

namespace widget {

class NativeMenu;

class NativeMenuSupport final {
 public:
  static void CreateNativeMenuBar(nsIWidget* aParent,
                                  dom::Element* aMenuBarElement);

  static already_AddRefed<NativeMenu> CreateNativePopupMenu(
      dom::Element* aPopup);

  static bool ShouldUseNativeAnchoredMenus();

  static bool ShouldUseNativeContextMenus();
};

}  
}  

#endif
