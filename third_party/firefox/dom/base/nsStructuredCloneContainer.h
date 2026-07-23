/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsStructuredCloneContainer_h_
#define nsStructuredCloneContainer_h_

#include "mozilla/dom/ipc/StructuredCloneData.h"
#include "nsIStructuredCloneContainer.h"
#include "nsISupports.h"

#define NS_STRUCTUREDCLONECONTAINER_CONTRACTID \
  "@mozilla.org/docshell/structured-clone-container;1"
#define NS_STRUCTUREDCLONECONTAINER_CID       \
  { \
   0x38bd0634,                                \
   0x0fd4,                                    \
   0x46f0,                                    \
   {0xb8, 0x5f, 0x13, 0xce, 0xd8, 0x89, 0xee, 0xec}}

class nsStructuredCloneContainer final
    : public nsIStructuredCloneContainer,
      public mozilla::dom::ipc::StructuredCloneData {
 public:
  nsStructuredCloneContainer();

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSISTRUCTUREDCLONECONTAINER

 private:
  friend struct IPC::ParamTraits<nsStructuredCloneContainer*>;

  ~nsStructuredCloneContainer();
};

namespace IPC {

template <>
struct ParamTraits<nsStructuredCloneContainer*> {
  using paramType = nsStructuredCloneContainer;
  static void Write(IPC::MessageWriter* aWriter, paramType* aParam);
  static bool Read(IPC::MessageReader* aReader, RefPtr<paramType>* aResult);
};

}  

#endif
