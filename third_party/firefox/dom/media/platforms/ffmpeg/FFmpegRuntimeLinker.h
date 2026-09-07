/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef FFmpegRuntimeLinker_h_
#define FFmpegRuntimeLinker_h_

#include "PlatformDecoderModule.h"
#include "PlatformEncoderModule.h"
#include "mozilla/StaticMutex.h"

namespace mozilla {

class FFmpegRuntimeLinker {
 public:
  static bool Init() MOZ_EXCLUDES(sMutex);
  static already_AddRefed<PlatformDecoderModule> CreateDecoder();
  static already_AddRefed<PlatformEncoderModule> CreateEncoder();
  enum LinkStatus {
    LinkStatus_INIT = 0,   
    LinkStatus_SUCCEEDED,  
    LinkStatus_INVALID_FFMPEG_CANDIDATE,  
    LinkStatus_UNUSABLE_LIBAV57,         
    LinkStatus_INVALID_LIBAV_CANDIDATE,  
    LinkStatus_OBSOLETE_FFMPEG,
    LinkStatus_OBSOLETE_LIBAV,
    LinkStatus_INVALID_CANDIDATE,  
    LinkStatus_NOT_FOUND,  
  };
  static LinkStatus LinkStatusCode() {
    StaticMutexAutoLock lock(sMutex);
    return sLinkStatus;
  }
  static const char* LinkStatusString();
  static const char* LinkStatusLibraryName() {
    StaticMutexAutoLock lock(sMutex);
    return sLinkStatusLibraryName;
  }

 private:
  static void PrefCallbackLogLevel(const char* aPref, void* aData);

  static StaticMutex sMutex;
  static LinkStatus sLinkStatus MOZ_GUARDED_BY(sMutex);
  static const char* sLinkStatusLibraryName MOZ_GUARDED_BY(sMutex);
};

}  

#endif  // FFmpegRuntimeLinker_h_
