/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MediaResourceCallback_h_
#define MediaResourceCallback_h_

#include "DecoderDoctorLogger.h"
#include "MediaResult.h"
#include "nsError.h"
#include "nsISupportsImpl.h"

namespace mozilla {

class AbstractThread;
class MediaDecoderOwner;
class MediaResource;

DDLoggedTypeDeclName(MediaResourceCallback);

class MediaResourceCallback
    : public DecoderDoctorLifeLogger<MediaResourceCallback> {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MediaResourceCallback);

  virtual AbstractThread* AbstractMainThread() const { return nullptr; }

  virtual MediaDecoderOwner* GetMediaOwner() const { return nullptr; }

  virtual void NotifyNetworkError(const MediaResult& aError) {}

  virtual void NotifyDataArrived() {}

  virtual void NotifyDataEnded(nsresult aStatus) {}

  virtual void NotifyPrincipalChanged() {}

  virtual void NotifySuspendedStatusChanged(bool aSuspendedByCache) {}

 protected:
  virtual ~MediaResourceCallback() = default;
};

}  

#endif  // MediaResourceCallback_h_
