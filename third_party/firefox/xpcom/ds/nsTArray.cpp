/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsTArray.h"
#include "nsXPCOM.h"
#include "nsCycleCollectionNoteChild.h"
#include "nsDebug.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/IntegerPrintfMacros.h"

alignas(8) const nsTArrayHeader sEmptyTArrayHeader = {0, 0, 0};

bool IsTwiceTheRequiredBytesRepresentableAsUint32(size_t aCapacity,
                                                  size_t aElemSize) {
  using mozilla::CheckedUint32;
  return ((CheckedUint32(aCapacity) * aElemSize) * 2).isValid();
}
