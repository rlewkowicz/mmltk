/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_InitData_h_
#define mozilla_widget_InitData_h_

#include <cstdint>
#include "mozilla/TypedEnumBits.h"

namespace mozilla::widget {

enum class WindowType : uint8_t {
  TopLevel,   
  Dialog,     
  Popup,      
  Invisible,  
};

enum class PopupType : uint8_t {
  Panel,
  Menu,
  Tooltip,
  Any,  
};

enum class PopupLevel : uint8_t {
  Parent,
  Top,
};

enum class BorderStyle : int16_t {
  None = 0,           
  All = 1 << 0,       
  Border = 1 << 1,    
  ResizeH = 1 << 2,   
  Title = 1 << 3,     
  Menu = 1 << 4,      
  Minimize = 1 << 5,  
  Maximize = 1 << 6,  
  Close = 1 << 7,     
  Default = -1        
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(BorderStyle)

enum class TransparencyMode : uint8_t {
  Opaque = 0,   
  Transparent,  
};

struct InitData {
  WindowType mWindowType = WindowType::TopLevel;
  BorderStyle mBorderStyle = BorderStyle::Default;
  PopupType mPopupHint = PopupType::Panel;
  PopupLevel mPopupLevel = PopupLevel::Top;
  TransparencyMode mTransparencyMode = TransparencyMode::Opaque;
  bool mClipChildren = false;
  bool mClipSiblings = false;
  bool mRTL = false;
  bool mIsDragPopup = false;  
  bool mIsAnimationSuppressed = false;
  bool mHasRemoteContent = false;
  bool mAlwaysOnTop = false;
  bool mResizable = false;
  bool mIsPrivate = false;
  bool mIsAlert = false;
};

}  

#endif  // mozilla_widget_InitData
