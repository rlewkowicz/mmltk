/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_IPCBlobUtils_h
#define mozilla_dom_IPCBlobUtils_h

#include "mozilla/RefPtr.h"
#include "mozilla/dom/File.h"


namespace mozilla::dom {

class IPCBlob;

namespace IPCBlobUtils {

already_AddRefed<BlobImpl> Deserialize(const IPCBlob& aIPCBlob);

nsresult Serialize(BlobImpl* aBlobImpl, IPCBlob& aIPCBlob);

}  
}  

namespace IPC {

template <>
struct ParamTraits<mozilla::dom::BlobImpl*> {
  static void Write(IPC::MessageWriter* aWriter,
                    mozilla::dom::BlobImpl* aParam);
  static bool Read(IPC::MessageReader* aReader,
                   RefPtr<mozilla::dom::BlobImpl>* aResult);
};

}  

#endif  // mozilla_dom_IPCBlobUtils_h
