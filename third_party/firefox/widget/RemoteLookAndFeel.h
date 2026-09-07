/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_RemoteLookAndFeel_h_
#define mozilla_widget_RemoteLookAndFeel_h_

#include "mozilla/widget/nsXPLookAndFeel.h"
#include "mozilla/widget/LookAndFeelTypes.h"

namespace mozilla::widget {

class RemoteLookAndFeel final : public nsXPLookAndFeel {
 public:
  explicit RemoteLookAndFeel(FullLookAndFeel&& aTables);

  virtual ~RemoteLookAndFeel();

  void NativeInit() override {}

  nsresult NativeGetInt(IntID, int32_t& aResult) override;
  nsresult NativeGetFloat(FloatID, float& aResult) override;
  nsresult NativeGetColor(ColorID, ColorScheme, nscolor& aResult) override;
  bool NativeGetFont(FontID aID, nsString& aFontName,
                     gfxFontStyle& aFontStyle) override;

  char16_t GetPasswordCharacterImpl() override;
  bool GetEchoPasswordImpl() override;

  void SetDataImpl(FullLookAndFeel&& aTables) override;

  static const FullLookAndFeel* ExtractData();

  static void ClearCachedData();

 private:
  LookAndFeelTables mTables;
};

}  

#endif  // mozilla_widget_RemoteLookAndFeel_h_
