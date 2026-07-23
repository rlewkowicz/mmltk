/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_KeyboardMap_h
#define mozilla_layers_KeyboardMap_h

#include <stdint.h>  // for uint32_t

#include "InputData.h"             // for KeyboardInput
#include "nsTArray.h"              // for nsTArray
#include "mozilla/Maybe.h"         // for mozilla::Maybe
#include "KeyboardScrollAction.h"  // for KeyboardScrollAction

namespace mozilla {

struct IgnoreModifierState;

namespace layers {

class KeyboardMap;

class KeyboardShortcut final {
 public:
  KeyboardShortcut();

  KeyboardShortcut(KeyboardInput::KeyboardEventType aEventType,
                   uint32_t aKeyCode, uint32_t aCharCode, Modifiers aModifiers,
                   Modifiers aModifiersMask,
                   const KeyboardScrollAction& aAction);

  KeyboardShortcut(KeyboardInput::KeyboardEventType aEventType,
                   uint32_t aKeyCode, uint32_t aCharCode, Modifiers aModifiers,
                   Modifiers aModifiersMask);

  static void AppendHardcodedShortcuts(nsTArray<KeyboardShortcut>& aShortcuts);

 protected:
  friend mozilla::layers::KeyboardMap;

  bool Matches(const KeyboardInput& aInput, const IgnoreModifierState& aIgnore,
               uint32_t aOverrideCharCode = 0) const;

 private:
  bool MatchesKey(const KeyboardInput& aInput,
                  uint32_t aOverrideCharCode) const;
  bool MatchesModifiers(const KeyboardInput& aInput,
                        const IgnoreModifierState& aIgnore) const;

 public:
  KeyboardScrollAction mAction;

  uint32_t mKeyCode;
  uint32_t mCharCode;

  Modifiers mModifiers;
  Modifiers mModifiersMask;

  KeyboardInput::KeyboardEventType mEventType;

  bool mDispatchToContent;
};

class KeyboardMap final {
 public:
  KeyboardMap();
  explicit KeyboardMap(nsTArray<KeyboardShortcut>&& aShortcuts);

  const nsTArray<KeyboardShortcut>& Shortcuts() const { return mShortcuts; }

  Maybe<KeyboardShortcut> FindMatch(const KeyboardInput& aEvent) const;

 private:
  Maybe<KeyboardShortcut> FindMatchInternal(
      const KeyboardInput& aEvent, const IgnoreModifierState& aIgnore,
      uint32_t aOverrideCharCode = 0) const;

  CopyableTArray<KeyboardShortcut> mShortcuts;
};

}  
}  

#endif  // mozilla_layers_KeyboardMap_h
