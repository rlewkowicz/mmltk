/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_GamepadPlatformService_h_
#define mozilla_dom_GamepadPlatformService_h_

#include <map>

#include "mozilla/Mutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/GamepadBinding.h"
#include "mozilla/dom/GamepadEventTypes.h"
#include "mozilla/dom/GamepadHandle.h"

namespace mozilla::dom {

class GamepadEventChannelParent;
enum class GamepadLightIndicatorType : uint8_t;
struct GamepadPoseState;
struct GamepadTouchState;
class GamepadPlatformService;

class GamepadPlatformService final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(GamepadPlatformService)
 public:
  static already_AddRefed<GamepadPlatformService> GetParentService();

  GamepadHandle AddGamepad(const char* aID, GamepadMappingType aMapping,
                           GamepadHand aHand, uint32_t aNumButtons,
                           uint32_t aNumAxes, uint32_t aNumHaptics,
                           uint32_t aNumLightIndicator,
                           uint32_t aNumTouchEvents) MOZ_EXCLUDES(mMutex);
  void RemoveGamepad(GamepadHandle aHandle) MOZ_EXCLUDES(mMutex);

  void NewButtonEvent(GamepadHandle aHandle, uint32_t aButton, bool aPressed,
                      bool aTouched, double aValue) MOZ_EXCLUDES(mMutex);
  void NewButtonEvent(GamepadHandle aHandle, uint32_t aButton, bool aPressed)
      MOZ_EXCLUDES(mMutex);
  void NewButtonEvent(GamepadHandle aHandle, uint32_t aButton, bool aPressed,
                      bool aTouched) MOZ_EXCLUDES(mMutex);
  void NewButtonEvent(GamepadHandle aHandle, uint32_t aButton, bool aPressed,
                      double aValue) MOZ_EXCLUDES(mMutex);
  void NewAxisMoveEvent(GamepadHandle aHandle, uint32_t aAxis, double aValue)
      MOZ_EXCLUDES(mMutex);
  void NewPoseEvent(GamepadHandle aHandle, const GamepadPoseState& aState)
      MOZ_EXCLUDES(mMutex);
  void NewLightIndicatorTypeEvent(GamepadHandle aHandle, uint32_t aLight,
                                  GamepadLightIndicatorType aType)
      MOZ_EXCLUDES(mMutex);
  void NewMultiTouchEvent(GamepadHandle aHandle, uint32_t aTouchArrayIndex,
                          const GamepadTouchState& aState) MOZ_EXCLUDES(mMutex);

  void ResetGamepadIndexes() MOZ_EXCLUDES(mMutex);

  void AddChannelParent(GamepadEventChannelParent* aParent)
      MOZ_EXCLUDES(mMutex);

  void RemoveChannelParent(GamepadEventChannelParent* aParent)
      MOZ_EXCLUDES(mMutex);

  void MaybeShutdown() MOZ_EXCLUDES(mMutex);

  nsTArray<GamepadAdded> GetAllGamePads() MOZ_EXCLUDES(mMutex) {
    MutexAutoLock autoLock(mMutex);
    nsTArray<GamepadAdded> gamepads;

    for (const auto& elem : mGamepadAdded) {
      gamepads.AppendElement(elem.second);
    }
    return gamepads;
  }

 private:
  GamepadPlatformService();
  ~GamepadPlatformService();
  template <class T>
  void NotifyGamepadChange(GamepadHandle aHandle, const T& aInfo)
      MOZ_REQUIRES(mMutex);

  void Cleanup() MOZ_EXCLUDES(mMutex);

  Mutex mMutex;

  uint32_t mNextGamepadHandleValue MOZ_GUARDED_BY(mMutex);

  nsTArray<RefPtr<GamepadEventChannelParent>> mChannelParents
      MOZ_GUARDED_BY(mMutex);

  std::map<GamepadHandle, GamepadAdded> mGamepadAdded MOZ_GUARDED_BY(mMutex);
};

}  

#endif
