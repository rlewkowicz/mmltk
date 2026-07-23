/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StaticAtomSet_h
#define mozilla_dom_StaticAtomSet_h

#include "nsAtom.h"
#include "nsTHashSet.h"

namespace mozilla::dom {

class StaticAtomSet : public nsTHashSet<const nsStaticAtom*> {
 public:
  StaticAtomSet() = default;
  explicit StaticAtomSet(uint32_t aLength)
      : nsTHashSet<const nsStaticAtom*>(aLength) {}

  bool Contains(nsAtom* aAtom) {
    return aAtom->IsStatic() && GetEntry(aAtom->AsStatic());
  }
};

}  

#endif  // ifndef mozilla_dom_StaticAtomSet_h
