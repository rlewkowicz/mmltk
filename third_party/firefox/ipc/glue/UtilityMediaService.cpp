/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ProcInfo.h"
#include "mozilla/ipc/UtilityMediaService.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"

namespace mozilla::ipc {

UtilityActorName GetAudioActorName(const UtilityProcessKind aKind) {
  switch (aKind) {
    case GENERIC_UTILITY:
      return UtilityActorName::AudioDecoder_Generic;
    default:
      MOZ_CRASH("Unexpected mKind for GetActorName()");
  }
}

nsCString GetChildAudioActorName() {
  RefPtr<ipc::UtilityProcessChild> s = ipc::UtilityProcessChild::Get();
  MOZ_ASSERT(s, "Has UtilityProcessChild");
  return dom::GetEnumString(GetAudioActorName(s->mKind));
}

}  
