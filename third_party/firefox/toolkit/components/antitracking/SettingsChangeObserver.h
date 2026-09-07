/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_settingschangeobserver_h
#define mozilla_settingschangeobserver_h

#include "nsIObserver.h"

#include <functional>

namespace mozilla {

class SettingsChangeObserver final : public nsIObserver {
  ~SettingsChangeObserver() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  using AntiTrackingSettingsChangedCallback = std::function<void()>;
  static void OnAntiTrackingSettingsChanged(
      const AntiTrackingSettingsChangedCallback& aCallback);

  static void PrivacyPrefChanged(const char* aPref = nullptr, void* = nullptr);

 private:
  static void RunAntiTrackingSettingsChangedCallbacks();
};

}  

#endif  // mozilla_settingschangeobserver_h
