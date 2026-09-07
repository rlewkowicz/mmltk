/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_HeadlessLookAndFeel_h
#define mozilla_widget_HeadlessLookAndFeel_h

#include "nsXPLookAndFeel.h"
#include "nsLookAndFeel.h"

namespace mozilla {
namespace widget {

#if defined(MOZ_WIDGET_GTK)


class HeadlessLookAndFeel : public nsXPLookAndFeel {
 public:
  explicit HeadlessLookAndFeel();
  virtual ~HeadlessLookAndFeel();

  void NativeInit() final {};
  nsresult NativeGetInt(IntID, int32_t& aResult) override;
  nsresult NativeGetFloat(FloatID, float& aResult) override;
  nsresult NativeGetColor(ColorID, ColorScheme, nscolor& aResult) override;
  bool NativeGetFont(FontID, nsString& aFontName, gfxFontStyle&) override;

  char16_t GetPasswordCharacterImpl() override;
};

#else


typedef nsLookAndFeel HeadlessLookAndFeel;

#endif

}  
}  

#endif
