/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SMIL_SMILTIMEVALUESPECPARAMS_H_
#define DOM_SMIL_SMILTIMEVALUESPECPARAMS_H_

#include "mozilla/SMILTimeValue.h"
#include "nsAtom.h"

namespace mozilla {


class SMILTimeValueSpecParams {
 public:
  SMILTimeValueSpecParams() = default;

  SMILTimeValue mOffset;

  RefPtr<nsAtom> mDependentElemID;

  RefPtr<nsAtom> mEventSymbol;

  uint32_t mRepeatIteration = 0;

  enum class Type : uint8_t {
    Offset,
    Syncbase,
    Event,
    Repeat,
    Wallclock,
    Indefinite
  } mType = Type::Indefinite;

  bool mSyncBegin = false;
};

}  

#endif  // DOM_SMIL_SMILTIMEVALUESPECPARAMS_H_
