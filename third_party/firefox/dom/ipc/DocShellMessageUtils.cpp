/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/DocShellMessageUtils.h"

#include "mozilla/dom/DOMTypes.h"
#include "nsSerializationHelper.h"

namespace IPC {

void ParamTraits<nsDocShellLoadState*>::Write(IPC::MessageWriter* aWriter,
                                              nsDocShellLoadState* aParam) {
  MOZ_RELEASE_ASSERT(aParam);
  WriteParam(aWriter, aParam->Serialize(aWriter->GetActor()));
}

bool ParamTraits<nsDocShellLoadState*>::Read(
    IPC::MessageReader* aReader, RefPtr<nsDocShellLoadState>* aResult) {
  mozilla::dom::DocShellLoadStateInit loadState;
  if (!ReadParam(aReader, &loadState)) {
    return false;
  }

  if (!loadState.URI()) {
    MOZ_ASSERT_UNREACHABLE("no URI in load state from IPC");
    return false;
  }

  bool readSuccess = false;
  RefPtr result =
      new nsDocShellLoadState(loadState, aReader->GetActor(), &readSuccess);
  if (readSuccess) {
    *aResult = result.forget();
  }
  return readSuccess;
}

}  
