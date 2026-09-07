/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_PLATFORMS_FFMPEG_VULKANDEVICEHOLDER_H_
#define DOM_MEDIA_PLATFORMS_FFMPEG_VULKANDEVICEHOLDER_H_

#include "mozilla/ThreadSafeWeakPtr.h"
#include "nsISupportsImpl.h"

struct AVBufferRef;

namespace mozilla {

struct FFmpegLibWrapper;

class VulkanDeviceHolder final
    : public SupportsThreadSafeWeakPtr<VulkanDeviceHolder> {
 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(VulkanDeviceHolder)

  static RefPtr<VulkanDeviceHolder> GetOrCreate(const FFmpegLibWrapper* aLib,
                                                const char* aDeviceName,
                                                const char* aDeviceExtensions);

  AVBufferRef* Ref() const;

  ~VulkanDeviceHolder();

 private:
  VulkanDeviceHolder(const FFmpegLibWrapper* aLib, AVBufferRef* aDeviceContext,
                     const char* aDeviceName);

  const FFmpegLibWrapper* mLib;
  AVBufferRef* mDeviceContext;
  char mDeviceName[256] = {'\0'};
};

}  

#endif  // DOM_MEDIA_PLATFORMS_FFMPEG_VULKANDEVICEHOLDER_H_
