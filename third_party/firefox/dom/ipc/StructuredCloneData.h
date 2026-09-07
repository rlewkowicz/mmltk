/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ipc_StructuredCloneData_h
#define mozilla_dom_ipc_StructuredCloneData_h

#include "mozilla/dom/StructuredCloneHolder.h"
#include "nsISupportsImpl.h"

namespace IPC {
class MessageReader;
class MessageWriter;
template <typename T>
struct ParamTraits;
}  

namespace mozilla::dom::ipc {

class StructuredCloneData : public StructuredCloneHolder {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(StructuredCloneData);

  StructuredCloneData();

  StructuredCloneData(StructuredCloneScope aScope,
                      TransferringSupport aSupportsTransferring);

  bool CopyExternalData(const char* aData, size_t aDataLength,
                        uint32_t aVersion = JS_STRUCTURED_CLONE_VERSION);

  void WriteIPCParams(IPC::MessageWriter* aWriter);
  bool ReadIPCParams(IPC::MessageReader* aReader);

 protected:
  ~StructuredCloneData();
};

}  

namespace IPC {

template <>
struct ParamTraits<mozilla::dom::ipc::StructuredCloneData*> {
  using paramType = mozilla::dom::ipc::StructuredCloneData;
  static void Write(MessageWriter* aWriter, paramType* aParam);
  static bool Read(MessageReader* aReader, RefPtr<paramType>* aResult);
};

}  

#endif  // mozilla_dom_ipc_StructuredCloneData_h
