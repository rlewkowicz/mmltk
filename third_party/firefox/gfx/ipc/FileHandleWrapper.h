/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _include_gfx_ipc_FileHandleWrapper_h_
#define _include_gfx_ipc_FileHandleWrapper_h_

#include "mozilla/UniquePtrExtensions.h"
#include "nsISupportsImpl.h"

namespace IPC {
template <typename P>
struct ParamTraits;
}

namespace mozilla {
namespace gfx {

class FileHandleWrapper {
  friend struct IPC::ParamTraits<gfx::FileHandleWrapper*>;

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FileHandleWrapper);

  explicit FileHandleWrapper(mozilla::UniqueFileHandle&& aHandle);

  mozilla::detail::FileHandleType GetHandle();

  mozilla::UniqueFileHandle ClonePlatformHandle();

 protected:
  ~FileHandleWrapper();

  const mozilla::UniqueFileHandle mHandle;
};

}  
}  

#endif  // _include_gfx_ipc_FileHandleWrapper_h_
