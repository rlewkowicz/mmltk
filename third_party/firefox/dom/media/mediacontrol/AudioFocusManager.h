/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIACONTROL_AUDIOFOCUSMANAGER_H_
#define DOM_MEDIA_MEDIACONTROL_AUDIOFOCUSMANAGER_H_

#include "VideoUtils.h"
#include "base/basictypes.h"
#include "nsTArray.h"

namespace mozilla::dom {

class IMediaController;
class MediaControlService;

class AudioFocusManager {
 public:
  void RequestAudioFocus(IMediaController* aController);
  void RevokeAudioFocus(IMediaController* aController);

  explicit AudioFocusManager() = default;
  ~AudioFocusManager() = default;

  uint32_t GetAudioFocusNums() const;

 private:
  friend class MediaControlService;
  void ClearFocusControllersIfNeeded();

  nsTArray<RefPtr<IMediaController>> mOwningFocusControllers;
};

}  

#endif  //  DOM_MEDIA_MEDIACONTROL_AUDIOFOCUSMANAGER_H_
