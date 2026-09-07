/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AsyncPlatformPipes_h
#define mozilla_AsyncPlatformPipes_h

#include "mozilla/UniquePtrExtensions.h"
#include "nsIAsyncInputStream.h"

namespace mozilla {

namespace platform_pipe_detail {

class PlatformPipeLink;

}  


class PlatformPipeReader final : public nsIAsyncInputStream {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIINPUTSTREAM
  NS_DECL_NSIASYNCINPUTSTREAM

  PlatformPipeReader(UniqueFileHandle aHandle, uint32_t aBufferSize);

 private:
  ~PlatformPipeReader();

  RefPtr<platform_pipe_detail::PlatformPipeLink> mLink;
};

}  

#endif  // mozilla_AsyncPlatformPipes_h
