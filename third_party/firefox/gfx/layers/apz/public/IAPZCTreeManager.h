/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_IAPZCTreeManager_h
#define mozilla_layers_IAPZCTreeManager_h

#include <stdint.h>  // for uint64_t, uint32_t

#include "mozilla/layers/LayersTypes.h"          // for TouchBehaviorFlags
#include "mozilla/layers/ScrollableLayerGuid.h"  // for ScrollableLayerGuid, etc
#include "mozilla/layers/ZoomConstraints.h"      // for ZoomConstraints
#include "nsTArrayForwardDeclare.h"  // for nsTArray, nsTArray_Impl, etc
#include "nsISupportsImpl.h"         // for MOZ_COUNT_CTOR, etc
#include "Units.h"                   // for CSSRect, etc

namespace mozilla {
namespace layers {

class APZInputBridge;
class KeyboardMap;
struct ZoomTarget;

enum AllowedTouchBehavior {
  NONE = 0,
  VERTICAL_PAN = 1 << 0,
  HORIZONTAL_PAN = 1 << 1,
  PINCH_ZOOM = 1 << 2,
  ANIMATING_ZOOM = 1 << 3,
  UNKNOWN = 1 << 4
};

enum ZoomToRectBehavior : uint32_t {
  DEFAULT_BEHAVIOR = 0,
  DISABLE_ZOOM_OUT = 1 << 0,
  PAN_INTO_VIEW_ONLY = 1 << 1,
  ONLY_ZOOM_TO_DEFAULT_SCALE = 1 << 2,
  ZOOM_TO_FOCUSED_INPUT = 1 << 3,
  ZOOM_TO_FOCUSED_INPUT_ON_RESIZES_VISUAL = 1 << 4,
};

enum class BrowserGestureResponse : bool;

class AsyncDragMetrics;
struct APZHandledResult;

class IAPZCTreeManager {
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

 public:
  virtual void SetKeyboardMap(const KeyboardMap& aKeyboardMap) = 0;

  virtual void ZoomToRect(const ScrollableLayerGuid& aGuid,
                          const ZoomTarget& aZoomTarget,
                          const uint32_t aFlags = DEFAULT_BEHAVIOR) = 0;

  virtual void ContentReceivedInputBlock(uint64_t aInputBlockId,
                                         bool aPreventDefault) = 0;

  virtual void SetTargetAPZC(uint64_t aInputBlockId,
                             const nsTArray<ScrollableLayerGuid>& aTargets) = 0;

  virtual void UpdateZoomConstraints(
      const ScrollableLayerGuid& aGuid,
      const Maybe<ZoomConstraints>& aConstraints) = 0;

  virtual void SetDPI(float aDpiValue) = 0;

  virtual void SetAllowedTouchBehavior(
      uint64_t aInputBlockId, const nsTArray<TouchBehaviorFlags>& aValues) = 0;

  virtual void SetBrowserGestureResponse(uint64_t aInputBlockId,
                                         BrowserGestureResponse aResponse) = 0;

  virtual void StartScrollbarDrag(const ScrollableLayerGuid& aGuid,
                                  const AsyncDragMetrics& aDragMetrics) = 0;

  virtual bool StartAutoscroll(const ScrollableLayerGuid& aGuid,
                               const ScreenPoint& aAnchorLocation) = 0;

  virtual void StopAutoscroll(const ScrollableLayerGuid& aGuid) = 0;

  virtual void SetLongTapEnabled(bool aTapGestureEnabled) = 0;

  virtual void NotifyApzAwareListenerAdded(
      const ScrollableLayerGuid& aGuid) = 0;

  virtual APZInputBridge* InputBridge() = 0;

 protected:

  virtual ~IAPZCTreeManager() = default;
};

}  
}  

#endif  // mozilla_layers_IAPZCTreeManager_h
