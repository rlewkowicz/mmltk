/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_dom_gamepad_GamepadHandle_h
#define mozilla_dom_gamepad_GamepadHandle_h

#include <type_traits>

#include "PLDHashTable.h"

namespace IPC {

template <class>
struct ParamTraits;

}  

namespace mozilla::dom {

class GamepadPlatformService;

enum class GamepadHandleKind : uint8_t {
  GamepadPlatformManager,
};

class GamepadHandle {
 public:
  GamepadHandle() = default;
  GamepadHandle(const GamepadHandle&) = default;
  GamepadHandle& operator=(const GamepadHandle&) = default;

  GamepadHandleKind GetKind() const;

  friend bool operator==(const GamepadHandle& a, const GamepadHandle& b);
  friend bool operator!=(const GamepadHandle& a, const GamepadHandle& b);
  friend bool operator<(const GamepadHandle& a, const GamepadHandle& b);

  PLDHashNumber Hash() const;

 private:
  explicit GamepadHandle(uint32_t aValue, GamepadHandleKind aKind);
  uint32_t GetValue() const { return mValue; }

  uint32_t mValue{0};
  GamepadHandleKind mKind{GamepadHandleKind::GamepadPlatformManager};

  friend class mozilla::dom::GamepadPlatformService;

  friend struct IPC::ParamTraits<mozilla::dom::GamepadHandle>;
};

static_assert(std::is_trivially_copyable_v<GamepadHandle>,
              "GamepadHandle must be trivially copyable");

}  

#endif  // mozilla_dom_gamepad_GamepadHandle_h
